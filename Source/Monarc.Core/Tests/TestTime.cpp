#include <doctest/doctest.h>

#include <Monarc/Core/Platform/Time.h>

using Monarc::f64;
using Monarc::u64;
namespace Time = Monarc::Platform::Time;

TEST_CASE("the monotonic clock advances and never goes backwards") {
    const u64 first = Time::Ticks();
    u64 last = first;
    for (int i = 0; i < 1000; ++i) {
        const u64 now = Time::Ticks();
        CHECK(now >= last);
        last = now;
    }
    CHECK(last >= first);
}

TEST_CASE("the tick frequency is a sane positive value") {
    const u64 frequency = Time::TicksPerSecond();
    CHECK(frequency > 0);
    // A clock coarser than a millisecond is unusable for frame pacing.
    CHECK(frequency >= 1000);
}

TEST_CASE("ticks convert to seconds consistently") {
    const u64 frequency = Time::TicksPerSecond();
    CHECK(Time::TicksToSeconds(0) == 0.0);
    CHECK(Time::TicksToSeconds(frequency) == doctest::Approx(1.0));
    CHECK(Time::TicksToSeconds(frequency / 2) == doctest::Approx(0.5));
}

TEST_CASE("a measured interval is positive and not absurd") {
    // Deliberately loose: this asserts the clock is wired up, not that the machine is fast.
    // A tight bound here would fail on a loaded CI runner, which is the classic flaky test.
    const u64 start = Time::Ticks();
    volatile u64 sink = 0;
    for (u64 i = 0; i < 2'000'000; ++i) { sink += i; }
    const f64 elapsed = Time::TicksToSeconds(Time::Ticks() - start);
    CHECK(elapsed > 0.0);
    CHECK(elapsed < 60.0);
}

TEST_CASE("the wall clock returns a plausible present-day value") {
    // Seconds since the Unix epoch. Bounded loosely at both ends so it neither passes on a
    // zeroed clock nor fails when this code is still running years from now.
    const u64 now = Time::UnixSeconds();
    CHECK(now > 1'700'000'000ull);   // late 2023
    CHECK(now < 4'000'000'000ull);   // 2096
}
