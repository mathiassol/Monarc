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
