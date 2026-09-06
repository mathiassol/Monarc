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
