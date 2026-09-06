#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Platform/Thread.h>

#include <atomic>

using Monarc::u32;
namespace Platform = Monarc::Platform;

TEST_CASE("HardwareThreadCount reports something plausible") {
    const u32 count = Platform::HardwareThreadCount();
    CHECK(count >= 1);
    CHECK(count <= 4096);
}

TEST_CASE("a thread runs its entry point and Join waits for it") {
    std::atomic<int> ran{0};
    Platform::Thread thread;
    REQUIRE(thread.Start("monarc-test", [&ran] { ran.store(1, std::memory_order_release); })
                .has_value());
    thread.Join();
    CHECK(ran.load(std::memory_order_acquire) == 1);
    CHECK_FALSE(thread.IsRunning());
}

TEST_CASE("many threads all run to completion") {
    constexpr int kThreads = 8;
    std::atomic<int> completed{0};
    Platform::Thread threads[kThreads];
    for (auto& thread : threads) {
        REQUIRE(thread.Start("monarc-many", [&completed] {
                    completed.fetch_add(1, std::memory_order_acq_rel);
                }).has_value());
    }
    for (auto& thread : threads) { thread.Join(); }
    CHECK(completed.load(std::memory_order_acquire) == kThreads);
}

TEST_CASE("a mutex serialises access so a non-atomic counter stays correct") {
    // A plain int deliberately: if the mutex does not exclude, this loses increments and
    // the test fails. An atomic would pass even with a broken mutex.
    constexpr int kThreads = 4;
    constexpr int kPerThread = 20'000;
    Platform::Mutex mutex;
    int guarded = 0;

    Platform::Thread threads[kThreads];
    for (auto& thread : threads) {
        REQUIRE(thread.Start("monarc-lock", [&] {
                    for (int i = 0; i < kPerThread; ++i) {
                        Platform::ScopedLock lock(mutex);
                        ++guarded;
                    }
                }).has_value());
    }
    for (auto& thread : threads) { thread.Join(); }
    CHECK(guarded == kThreads * kPerThread);
}

TEST_CASE("a condition variable wakes a waiter") {
    Platform::Mutex mutex;
    Platform::ConditionVariable signal;
    bool ready = false;
    int  received = 0;

    Platform::Thread waiter;
    REQUIRE(waiter.Start("monarc-wait", [&] {
                Platform::ScopedLock lock(mutex);
                while (!ready) { signal.Wait(mutex); }
                received = 42;
            }).has_value());

    {
        Platform::ScopedLock lock(mutex);
        ready = true;
    }
    signal.NotifyAll();
    waiter.Join();
    CHECK(received == 42);
}

TEST_CASE("a condition variable tolerates a notify that arrives before the wait") {
    // The predicate loop must handle this; a Wait that assumes a notify is still pending
    // deadlocks. Notifying with no waiter must simply do nothing.
    Platform::Mutex mutex;
    Platform::ConditionVariable signal;
    signal.NotifyAll();   // nobody waiting

    bool ready = false;
    {
        Platform::ScopedLock lock(mutex);
        ready = true;     // predicate already true before any wait
    }

    Platform::Thread waiter;
    REQUIRE(waiter.Start("monarc-prewait", [&] {
                Platform::ScopedLock lock(mutex);
                while (!ready) { signal.Wait(mutex); }
            }).has_value());
    waiter.Join();
    CHECK(ready);
}

TEST_CASE("a default-constructed thread is safe to destroy and reports not running") {
    Platform::Thread thread;
    CHECK_FALSE(thread.IsRunning());
}

TEST_CASE("Join on a thread that was never started returns immediately") {
    Platform::Thread thread;
    thread.Join();
    CHECK_FALSE(thread.IsRunning());
}

TEST_CASE("a thread can be started again after being joined") {
    Platform::Thread thread;
    std::atomic<int> count{0};
    auto increment = [&count] { count.fetch_add(1, std::memory_order_acq_rel); };

    REQUIRE(thread.Start("monarc-reuse-1", increment).has_value());
    thread.Join();
    REQUIRE(thread.Start("monarc-reuse-2", increment).has_value());
    thread.Join();

    CHECK(count.load(std::memory_order_acquire) == 2);
}

TEST_CASE("starting an already-running thread fails without disturbing it") {
    // Start on a running Thread is a programming error and MONARC_CHECK fires by design.
    // A non-breaking handler is installed so this test can observe the graceful fallback
    // -- Err(AlreadyExists) -- rather than crash the process, mirroring TestAssert.cpp's
    // capture pattern. IsRunning() reflects "has an unjoined handle", not "the entry point
    // is still executing" (see Thread::IsRunning's doc comment), so this is deterministic
    // regardless of how quickly the first thread's callable actually finishes.
    Monarc::AssertHandler previous = Monarc::SetAssertHandler(
        [](const char*, const char*, int, const char*) { return false; });

    std::atomic<int> ran{0};
    Platform::Thread thread;
    REQUIRE(thread.Start("monarc-first", [&ran] { ran.store(1, std::memory_order_release); })
                .has_value());

    const auto second = thread.Start("monarc-second", [] {});

    Monarc::SetAssertHandler(previous);

    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == Monarc::ErrorCode::AlreadyExists);

    thread.Join();
    CHECK(ran.load(std::memory_order_acquire) == 1);
}
