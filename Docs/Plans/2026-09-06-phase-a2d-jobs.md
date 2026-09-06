# Phase A2d — `Monarc.Jobs`

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A job scheduler — a thread pool, a dependency graph, priorities and instrumentation — that the cooker can drive in Phase C, and that [Frame-Model](../Runtime/Frame-Model.md) can later use to overlap simulation with rendering.

**Architecture:** A thread pool with a dependency graph, not fibers ([Threading](../Runtime/Threading.md)). Jobs are referenced by generation-checked handles ([ADR-0002](../Architecture/Decisions/ADR-0002-handles-not-pointers.md)), never pointers. Job records live in a fixed-capacity pool sized at construction, so submitting allocates nothing.

**Tech Stack:** C++23, MSVC + Clang (`clang-cl`), CMake + Ninja, doctest.

**Plan style:** headers and complete tests, not implementation bodies — as A2a onward.

---

## Two things make this phase different

**This is the first module beyond `Monarc.Core`.** The module graph has had exactly one node
since A1, so the kind and tier rules and gate 2's cycle detection have never run against a
real second module — only against scratch fixtures in the gate tests. `Monarc.Jobs` is
`Runtime`, tier 0, and depends on `Monarc.Core`. Task 1 confirms `module-graph.json` records
that edge and the gates still pass.

**There is no ThreadSanitizer on Windows.** `clang-cl` rejects `-fsanitize=thread` and LLVM
ships no runtime for it, so nothing mechanical will find a data race here. ASan finds
use-after-free; UBSan finds undefined arithmetic; neither sees two threads touching the same
word. That is a real gap, and the plan compensates in three ways rather than pretending
otherwise:

1. **Keep the shared mutable surface tiny.** All cross-thread state is `std::atomic` or
   guarded by one mutex, and the header names which is which. A design small enough to
   enumerate is a design that can be reasoned about.
2. **Tests assert invariants a race would violate**, not merely that work completed — an
   exact completion count, a strict ordering, a value no interleaving could produce. A test
   that only checks "it didn't crash" tells you nothing about a scheduler.
3. **Stress tests repeat.** A race that shows up one run in fifty is invisible in a single
   run and near-certain across two hundred. Each concurrency test runs its scenario many
   times inside one case.

None of that equals a sanitizer. It is what is available, and the limitation belongs in
[Status.md](../Status.md) rather than in an assumption.

---

## File Structure

```
Source/Monarc.Jobs/
  CMakeLists.txt
  Include/Monarc/Jobs/
    JobHandle.h      generation-checked handle
    JobSystem.h      the scheduler: submit, depend, wait, priorities
    JobProfile.h     completed-job records for instrumentation
  Private/
    JobSystem.cpp
  Tests/
    TestJobHandle.cpp  TestJobSystem.cpp  TestJobDependencies.cpp
    TestJobPriority.cpp  TestJobProfile.cpp
```

---

## Decisions this plan makes, with reasons

**`Wait` may not be called from a worker thread.** A worker that blocks waiting for another
job removes itself from the pool, and enough of them deadlock it outright. The two ways out
are helping-while-waiting (a waiting worker runs other jobs) or forbidding it. Helping is the
more capable answer and is the reason engines end up with fibers; it also introduces
re-entrancy into every job, which is a large complication to accept before anything needs it.

So: **express ordering with dependencies, not by waiting inside a job.** `Wait` is for the
thread that owns the work — the main thread, or the cooker's driver — and calling it from a
worker is a `MONARC_CHECK` violation with a message saying why. Helping-while-waiting is
recorded as deferred, not forgotten; the interface does not change if it arrives.

**Job records live in a fixed-capacity pool.** Sized at construction. `Submit` returns an
error when the pool is exhausted rather than allocating, so the hot path never touches the
allocator and a runaway submitter fails loudly instead of consuming memory. The generation
counter in the handle is what makes a recycled slot safe.

**A job's callable is stored inline, up to a fixed size.** A `Function`-style type-erased
callable that heap-allocates per submit would put an allocation on the hot path. Inline
storage with a static assert on the callable's size keeps submission allocation-free and
makes an over-large capture a compile error rather than a silent slow path.

**Dependencies are counted, not traversed.** Each job holds an atomic count of unfinished
dependencies; completing a job decrements each dependent's count, and the job becomes
runnable at zero. No graph walk at schedule time.

---

## Task 1: The module, `JobHandle`, and a pool that runs one job

**Files:** create `Source/Monarc.Jobs/CMakeLists.txt`, `Include/Monarc/Jobs/JobHandle.h`, `Include/Monarc/Jobs/JobSystem.h`, `Private/JobSystem.cpp`, `Tests/TestJobHandle.cpp`, `Tests/TestJobSystem.cpp`; modify root `CMakeLists.txt`

- [ ] **Step 1: Declare the module**

`Source/Monarc.Jobs/CMakeLists.txt`:

```cmake
monarc_module(
    NAME        Monarc.Jobs
    KIND        Runtime
    TIER        0
    PUBLIC_DEPS Monarc.Core)

monarc_test_module(Monarc.Jobs)
```

In the root `CMakeLists.txt`, add `add_subdirectory(Source/Monarc.Jobs)` immediately after
the existing `add_subdirectory(Source/Monarc.Core)`.

Configure and confirm the graph now has two modules:

```
python -c "import json;d=json.load(open('Build/msvc-debug/module-graph.json'));print([(m['name'],m['tier'],m['publicDeps']) for m in d['modules']])"
```

Expected: `Monarc.Jobs` present, tier 0, with `publicDeps` `['Monarc.Core']`. Then run the
gates — this is the first time gate 2 has seen a real edge rather than a test fixture.

- [ ] **Step 2: Write the failing tests**

`Tests/TestJobHandle.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/HashMap.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Jobs/JobHandle.h>

using Monarc::JobHandle;

TEST_CASE("a default handle is invalid") {
    CHECK_FALSE(JobHandle{}.IsValid());
}

TEST_CASE("handles compare by both index and generation") {
    const JobHandle a = JobHandle::ForTesting(3, 1);
    const JobHandle b = JobHandle::ForTesting(3, 1);
    const JobHandle c = JobHandle::ForTesting(3, 2);   // same slot, later generation
    const JobHandle d = JobHandle::ForTesting(4, 1);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.IsValid());
}

TEST_CASE("a handle is hashable so it can key a map") {
    Monarc::SystemAllocator allocator;
    Monarc::HashMap<JobHandle, int> map(allocator);
    const JobHandle handle = JobHandle::ForTesting(7, 3);
    map.Insert(handle, 1);
    REQUIRE(map.Find(handle) != nullptr);
    CHECK(map.Find(JobHandle::ForTesting(7, 4)) == nullptr);
}
```

`Tests/TestJobSystem.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Jobs/JobSystem.h>

#include <atomic>

using Monarc::JobHandle;
using Monarc::JobSystem;
using Monarc::SystemAllocator;
using Monarc::u32;

TEST_CASE("a job system reports the worker count it was asked for") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 3, .maxJobs = 64});
    CHECK(jobs.WorkerCount() == 3);
}

TEST_CASE("a submitted job runs exactly once") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64});

    std::atomic<int> runs{0};
    const auto handle = jobs.Submit("once", [&runs] { runs.fetch_add(1, std::memory_order_acq_rel); });
    REQUIRE(handle.has_value());
    jobs.Wait(*handle);
    CHECK(runs.load(std::memory_order_acquire) == 1);
}

TEST_CASE("every one of many jobs runs exactly once") {
    // An exact count, not "at least one" -- a scheduler that drops or double-runs work is
    // the failure this catches, and both are invisible to a "did it finish" check.
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 4, .maxJobs = 2048});

    constexpr int kJobs = 1000;
    std::atomic<int> runs{0};
    Monarc::Array<JobHandle> handles(allocator);
    for (int i = 0; i < kJobs; ++i) {
        const auto handle = jobs.Submit("many", [&runs] { runs.fetch_add(1, std::memory_order_acq_rel); });
        REQUIRE(handle.has_value());
        handles.Push(*handle);
    }
    for (const JobHandle& handle : handles) { jobs.Wait(handle); }
    CHECK(runs.load(std::memory_order_acquire) == kJobs);
}

TEST_CASE("waiting on an already-finished job returns immediately") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64});
    const auto handle = jobs.Submit("done", [] {});
    REQUIRE(handle.has_value());
    jobs.Wait(*handle);
    jobs.Wait(*handle);   // second wait must not hang or fault
    CHECK(jobs.IsComplete(*handle));
}

TEST_CASE("waiting on a stale handle is safe and reports complete") {
    // A recycled slot must not make an old handle appear to name live work.
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 8});
    const auto first = jobs.Submit("first", [] {});
    REQUIRE(first.has_value());
    jobs.Wait(*first);

    for (int i = 0; i < 64; ++i) {
        const auto handle = jobs.Submit("churn", [] {});
        REQUIRE(handle.has_value());
        jobs.Wait(*handle);
    }
    CHECK(jobs.IsComplete(*first));
    jobs.Wait(*first);
}

TEST_CASE("submitting beyond capacity fails rather than allocating or blocking") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 1, .maxJobs = 4});

    // Hold every worker busy so nothing completes and slots cannot recycle.
    std::atomic<bool> release{false};
    Monarc::Array<JobHandle> handles(allocator);
    Monarc::Result<JobHandle> overflow = Monarc::Err(Monarc::ErrorCode::Unknown);
    for (int i = 0; i < 64; ++i) {
        auto handle = jobs.Submit("blocked", [&release] {
            while (!release.load(std::memory_order_acquire)) {}
        });
        if (!handle.has_value()) { overflow = handle; break; }
        handles.Push(*handle);
    }
    release.store(true, std::memory_order_release);
    for (const JobHandle& handle : handles) { jobs.Wait(handle); }

    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == Monarc::ErrorCode::OutOfMemory);
}

TEST_CASE("destroying the system drains outstanding work") {
    // Nothing may still be running when the destructor returns, or the callables' captured
    // state is destroyed underneath live threads.
    SystemAllocator allocator;
    std::atomic<int> runs{0};
    {
        JobSystem jobs(allocator, JobSystem::Config{.workerCount = 4, .maxJobs = 512});
        for (int i = 0; i < 200; ++i) {
            (void)jobs.Submit("drain", [&runs] { runs.fetch_add(1, std::memory_order_acq_rel); });
        }
    }
    CHECK(runs.load(std::memory_order_acquire) == 200);
}
```

- [ ] **Step 3: Build, confirm the tests fail for the right reason**

- [ ] **Step 4: Write the headers and implementation**

`JobHandle.h` in `namespace Monarc`: a trivially copyable struct of `u32 index` and
`u32 generation`, default-constructed invalid, with `IsValid()`, `operator==`/`!=`, a
`Hasher<JobHandle>` specialisation, and a `static JobHandle ForTesting(u32, u32)` — named so
that nothing in the engine constructs one by hand without it being obvious.

`JobSystem.h` in `namespace Monarc`, for this task:

- `struct Config { u32 workerCount; u32 maxJobs; }`
- `JobSystem(IAllocator&, const Config&)`, non-copyable, non-movable
- `~JobSystem()` — drains outstanding work, then stops and joins every worker
- `[[nodiscard]] Result<JobHandle> Submit(StringView name, Callable&&)`
- `void Wait(JobHandle)` — **`MONARC_CHECK`s that the caller is not a worker thread**, with a
  message explaining that a worker blocking here can deadlock the pool and that ordering
  belongs in dependencies
- `[[nodiscard]] bool IsComplete(JobHandle) const` — true for a stale handle
- `[[nodiscard]] u32 WorkerCount() const`

The callable is stored inline up to a fixed size; static-assert on it so an over-large
capture is a compile error, not a silent heap allocation.

- [ ] **Step 5: Verify on all six presets, then commit**

```bash
git add Source/Monarc.Jobs CMakeLists.txt
git commit -m "jobs: the module, generation-checked handles, and a running thread pool"
```

---

## Task 2: Dependencies

**Files:** modify `Include/Monarc/Jobs/JobSystem.h`, `Private/JobSystem.cpp`; create `Tests/TestJobDependencies.cpp`

- [ ] **Step 1: Write the failing test**

`Tests/TestJobDependencies.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Jobs/JobSystem.h>

#include <atomic>

using Monarc::Array;
using Monarc::JobHandle;
using Monarc::JobSystem;
using Monarc::SystemAllocator;

TEST_CASE("a dependent job runs strictly after its dependency") {
    // Repeated, because a scheduler that gets ordering wrong one run in twenty looks
    // correct in a single run. With no ThreadSanitizer available, repetition is the
    // substitute -- see this plan's opening note.
    SystemAllocator allocator;
    for (int attempt = 0; attempt < 200; ++attempt) {
        JobSystem jobs(allocator, JobSystem::Config{.workerCount = 4, .maxJobs = 64});
        std::atomic<int> order{0};
        std::atomic<int> firstFinishedAt{-1};
        std::atomic<int> secondStartedAt{-1};

        const auto first = jobs.Submit("first", [&] {
            firstFinishedAt.store(order.fetch_add(1, std::memory_order_acq_rel),
                                  std::memory_order_release);
        });
        REQUIRE(first.has_value());

        const auto second = jobs.SubmitAfter("second", *first, [&] {
            secondStartedAt.store(order.fetch_add(1, std::memory_order_acq_rel),
                                  std::memory_order_release);
        });
        REQUIRE(second.has_value());

        jobs.Wait(*second);
        CHECK(firstFinishedAt.load(std::memory_order_acquire) <
              secondStartedAt.load(std::memory_order_acquire));
    }
}

TEST_CASE("a job waits for all of several dependencies") {
    SystemAllocator allocator;
    for (int attempt = 0; attempt < 100; ++attempt) {
        JobSystem jobs(allocator, JobSystem::Config{.workerCount = 4, .maxJobs = 256});
        constexpr int kDeps = 16;
        std::atomic<int> completed{0};
        std::atomic<int> seenByDependent{-1};

        Array<JobHandle> deps(allocator);
        for (int i = 0; i < kDeps; ++i) {
            auto handle = jobs.Submit("dep", [&completed] {
                completed.fetch_add(1, std::memory_order_acq_rel);
            });
            REQUIRE(handle.has_value());
            deps.Push(*handle);
        }

        const auto joiner = jobs.SubmitAfter("joiner", deps, [&] {
            seenByDependent.store(completed.load(std::memory_order_acquire),
                                  std::memory_order_release);
        });
        REQUIRE(joiner.has_value());
        jobs.Wait(*joiner);
        CHECK(seenByDependent.load(std::memory_order_acquire) == kDeps);
    }
}

TEST_CASE("a chain of dependencies runs in order") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 4, .maxJobs = 256});
    constexpr int kLength = 64;
    std::atomic<int> counter{0};
    std::atomic<bool> outOfOrder{false};

    JobHandle previous;
    for (int i = 0; i < kLength; ++i) {
        const int expected = i;
        auto step = [&counter, &outOfOrder, expected] {
            if (counter.fetch_add(1, std::memory_order_acq_rel) != expected) {
                outOfOrder.store(true, std::memory_order_release);
            }
        };
        auto handle = previous.IsValid() ? jobs.SubmitAfter("chain", previous, step)
                                         : jobs.Submit("chain", step);
        REQUIRE(handle.has_value());
        previous = *handle;
    }
    jobs.Wait(previous);
    CHECK_FALSE(outOfOrder.load(std::memory_order_acquire));
    CHECK(counter.load(std::memory_order_acquire) == kLength);
}

TEST_CASE("depending on an already-finished job runs immediately") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64});
    const auto first = jobs.Submit("first", [] {});
    REQUIRE(first.has_value());
    jobs.Wait(*first);

    std::atomic<bool> ran{false};
    const auto second = jobs.SubmitAfter("second", *first,
                                         [&ran] { ran.store(true, std::memory_order_release); });
    REQUIRE(second.has_value());
    jobs.Wait(*second);
    CHECK(ran.load(std::memory_order_acquire));
}

TEST_CASE("a wide fan-out then fan-in completes every branch") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 4, .maxJobs = 1024});
    constexpr int kWidth = 128;
    std::atomic<int> branchRuns{0};

    const auto root = jobs.Submit("root", [] {});
    REQUIRE(root.has_value());

    Array<JobHandle> branches(allocator);
    for (int i = 0; i < kWidth; ++i) {
        auto handle = jobs.SubmitAfter("branch", *root, [&branchRuns] {
            branchRuns.fetch_add(1, std::memory_order_acq_rel);
        });
        REQUIRE(handle.has_value());
        branches.Push(*handle);
    }

    std::atomic<int> seen{-1};
    const auto join = jobs.SubmitAfter("join", branches, [&] {
        seen.store(branchRuns.load(std::memory_order_acquire), std::memory_order_release);
    });
    REQUIRE(join.has_value());
    jobs.Wait(*join);
    CHECK(seen.load(std::memory_order_acquire) == kWidth);
}
```

- [ ] **Step 2: Build, confirm it fails**

- [ ] **Step 3: Extend `JobSystem`**

Add:

- `[[nodiscard]] Result<JobHandle> SubmitAfter(StringView name, JobHandle dependency, Callable&&)`
- `[[nodiscard]] Result<JobHandle> SubmitAfter(StringView name, Span<const JobHandle> dependencies, Callable&&)`

A dependency that is already complete — or stale — must count as satisfied, so a job whose
dependencies all finished before it was submitted becomes runnable immediately rather than
waiting forever. That is the single most likely defect in this design, and two tests cover it.

Each job holds an atomic unfinished-dependency count; completing a job decrements each
dependent's count and enqueues it at zero. No graph traversal at schedule time.

- [ ] **Step 4: Verify on all six presets, then commit**

```bash
git add Source/Monarc.Jobs
git commit -m "jobs: a counted dependency graph"
```

---

## Task 3: Priorities and instrumentation

**Files:** modify `Include/Monarc/Jobs/JobSystem.h`, `Private/JobSystem.cpp`; create `Include/Monarc/Jobs/JobProfile.h`, `Tests/TestJobPriority.cpp`, `Tests/TestJobProfile.cpp`

- [ ] **Step 1: Write the failing tests**

`Tests/TestJobPriority.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Jobs/JobSystem.h>

#include <atomic>

using Monarc::Array;
using Monarc::JobHandle;
using Monarc::JobPriority;
using Monarc::JobSystem;
using Monarc::SystemAllocator;

TEST_CASE("priority is honoured when work is queued behind a busy pool") {
    // With one worker, everything queues. A high-priority job submitted last must still run
    // before the low-priority backlog. Asserting an exact position would be flaky; asserting
    // it beats the *tail* of the backlog is the real guarantee.
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 1, .maxJobs = 256});

    std::atomic<bool> release{false};
    std::atomic<int>  order{0};
    std::atomic<int>  highAt{-1};
    std::atomic<int>  lastLowAt{-1};

    // Occupy the single worker so the rest genuinely queue.
    const auto blocker = jobs.Submit("blocker", [&release] {
        while (!release.load(std::memory_order_acquire)) {}
    });
    REQUIRE(blocker.has_value());

    Array<JobHandle> lows(allocator);
    for (int i = 0; i < 64; ++i) {
        auto handle = jobs.SubmitWithPriority("low", JobPriority::Low, [&] {
            lastLowAt.store(order.fetch_add(1, std::memory_order_acq_rel),
                            std::memory_order_release);
        });
        REQUIRE(handle.has_value());
        lows.Push(*handle);
    }
    const auto high = jobs.SubmitWithPriority("high", JobPriority::High, [&] {
        highAt.store(order.fetch_add(1, std::memory_order_acq_rel), std::memory_order_release);
    });
    REQUIRE(high.has_value());

    release.store(true, std::memory_order_release);
    jobs.Wait(*blocker);
    for (const JobHandle& handle : lows) { jobs.Wait(handle); }
    jobs.Wait(*high);

    CHECK(highAt.load(std::memory_order_acquire) <
          lastLowAt.load(std::memory_order_acquire));
}

TEST_CASE("a job defaults to normal priority") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64});
    std::atomic<bool> ran{false};
    const auto handle = jobs.Submit("default", [&ran] { ran.store(true, std::memory_order_release); });
    REQUIRE(handle.has_value());
    jobs.Wait(*handle);
    CHECK(ran.load(std::memory_order_acquire));
}
```

`Tests/TestJobProfile.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Jobs/JobProfile.h>
#include <Monarc/Jobs/JobSystem.h>

using Monarc::Array;
using Monarc::JobProfileRecord;
using Monarc::JobSystem;
using Monarc::SystemAllocator;

TEST_CASE("completed jobs are recorded with a name, a worker and a duration") {
    // Threading.md calls visibility a requirement, not a nicety: a scheduler you cannot see
    // into is one you cannot tune.
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64,
                                                .profileCapacity = 128});
    for (int i = 0; i < 8; ++i) {
        const auto handle = jobs.Submit("profiled", [] {});
        REQUIRE(handle.has_value());
        jobs.Wait(*handle);
    }

    Array<JobProfileRecord> records(allocator);
    jobs.CollectProfile(records);
    CHECK(records.Size() == 8);
    for (const JobProfileRecord& record : records) {
        CHECK(record.name == "profiled");
        CHECK(record.workerIndex < jobs.WorkerCount());
        CHECK(record.endTicks >= record.startTicks);
    }
}

TEST_CASE("profiling is off when no capacity is configured") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64});
    const auto handle = jobs.Submit("unprofiled", [] {});
    REQUIRE(handle.has_value());
    jobs.Wait(*handle);

    Array<JobProfileRecord> records(allocator);
    jobs.CollectProfile(records);
    CHECK(records.Size() == 0);
}

TEST_CASE("collecting profile records clears them") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 1, .maxJobs = 64,
                                                .profileCapacity = 64});
    const auto handle = jobs.Submit("once", [] {});
    REQUIRE(handle.has_value());
    jobs.Wait(*handle);

    Array<JobProfileRecord> first(allocator);
    jobs.CollectProfile(first);
    CHECK(first.Size() == 1);

    Array<JobProfileRecord> second(allocator);
    jobs.CollectProfile(second);
    CHECK(second.Size() == 0);
}
```

- [ ] **Step 2: Build, confirm both fail**

- [ ] **Step 3: Extend `JobSystem` and add `JobProfile.h`**

- `enum class JobPriority : u8 { High, Normal, Low }`
- `Result<JobHandle> SubmitWithPriority(StringView name, JobPriority, Callable&&)`, and a
  priority-taking overload of `SubmitAfter`
- `Config` gains `u32 profileCapacity = 0` — zero disables recording entirely, so
  instrumentation costs nothing when unwanted
- `struct JobProfileRecord { StringView name; u32 workerIndex; u64 startTicks; u64 endTicks; }`
- `void CollectProfile(Array<JobProfileRecord>& out)` — moves records out and clears

Workers drain `High` before `Normal` before `Low`. Note in the header that this is strict
priority with no ageing, so a saturated high-priority queue can starve low — acceptable while
the only producer is the cooker, and worth revisiting when gameplay submits work.

- [ ] **Step 4: Verify on all six presets, then commit**

```bash
git add Source/Monarc.Jobs
git commit -m "jobs: priorities and completed-job instrumentation"
```

---

## Task 4: The cooker-shaped stress test, and close-out

**Files:** modify `Tests/TestJobDependencies.cpp`; modify `Docs/Status.md`, `Docs/Runtime/Threading.md`

- [ ] **Step 1: Write the shape Phase C will actually use**

Append to `Tests/TestJobDependencies.cpp`:

```cpp
TEST_CASE("the cooker's shape: import many, then one job that needs them all") {
    // Phase C's cooker submits an import per asset and waits for the lot before writing a
    // manifest. This is that graph, repeated, with an invariant no interleaving can satisfy
    // unless every import really finished first.
    SystemAllocator allocator;
    for (int attempt = 0; attempt < 50; ++attempt) {
        JobSystem jobs(allocator, JobSystem::Config{.workerCount = 4, .maxJobs = 1024});
        constexpr int kAssets = 100;

        std::atomic<int> imported{0};
        Array<JobHandle> imports(allocator);
        for (int i = 0; i < kAssets; ++i) {
            auto handle = jobs.Submit("import", [&imported] {
                imported.fetch_add(1, std::memory_order_acq_rel);
            });
            REQUIRE(handle.has_value());
            imports.Push(*handle);
        }

        std::atomic<int> manifestSaw{-1};
        const auto manifest = jobs.SubmitAfter("manifest", imports, [&] {
            manifestSaw.store(imported.load(std::memory_order_acquire),
                              std::memory_order_release);
        });
        REQUIRE(manifest.has_value());

        jobs.Wait(*manifest);
        CHECK(manifestSaw.load(std::memory_order_acquire) == kAssets);
        CHECK(imported.load(std::memory_order_acquire) == kAssets);
    }
}
```

- [ ] **Step 2: Verify `Wait` from a worker is rejected**

Confirm the guard fires rather than deadlocking. Install a non-breaking assert handler as
`TestAssert.cpp` does, submit a job that calls `Wait` on another handle, and confirm the
handler is invoked. **Report exactly what happens after the handler declines to break** — if
the implementation then deadlocks anyway, say so, because a guard that only works under a
breaking handler is a guard that fails in Release.

- [ ] **Step 3: Clean verification on all six presets**

Record doctest totals. `clang-asan` matters most here — it runs the whole concurrent suite
under instrumentation.

Then run the job tests **repeatedly** to shake out low-probability races:

```
for /L %i in (1,1,20) do Build\msvc-debug\Source\Monarc.Jobs\Monarc.Jobs.Tests.exe
```

Report how many runs passed. A single green run proves very little about a scheduler.

- [ ] **Step 4: Update the docs**

`Docs/Status.md`: mark A2d complete, add `### A2d delivered`, note that the module graph now
has two modules so gate 2 sees a real edge.

`Docs/Runtime/Threading.md`: its "Where the job system is actually used in M0" section says
"mostly the cooker" and describes jobs as not yet built. Update it to describe what exists,
and record the `Wait`-from-a-worker rule and the deferred helping-while-waiting alternative.

- [ ] **Step 5: Commit**

```bash
python Tools/check_doc_links.py Docs README.md
git add Source/Monarc.Jobs Docs/Status.md Docs/Runtime/Threading.md
git commit -m "jobs: the cooker's job shape under stress; record Phase A2d complete"
```

---

## Definition of done

1. All six presets configure, build clean, and pass.
2. `module-graph.json` records two modules and the `Monarc.Jobs → Monarc.Core` edge, and the
   gates pass against it.
3. The job test binary passes 20 consecutive runs.
4. `Wait` from a worker is rejected, and the report says what happens if the guard is declined.
5. `Docs/Status.md` and `Docs/Runtime/Threading.md` reflect reality.

## What A2d deliberately excludes

- **No fibers.** [Threading](../Runtime/Threading.md) records the reasoning.
- **No work stealing.** A shared per-priority queue is enough at this scale; stealing is a
  tuning change behind the same interface.
- **No helping-while-waiting.** See the decisions section — deferred, with `Wait` guarded so
  its absence is a loud error rather than a deadlock.
- **No cancellation.** M0 defers it; the cooker is the first plausible consumer.
- **No thread affinity.** Nothing needs a specific thread until the RHI wants main-thread
  submission, which is A3 at the earliest.
- **No priority ageing.** Strict priority, documented, revisited when something other than
  the cooker submits work.
