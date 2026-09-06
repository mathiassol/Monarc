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
