#include <Monarc/Core/Platform/Time.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace Monarc::Platform::Time {

namespace {

u64 QueryFrequency() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<u64>(frequency.QuadPart);
}

}  // namespace

u64 Ticks() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<u64>(counter.QuadPart);
}

u64 TicksPerSecond() {
    // QueryPerformanceFrequency is fixed for the life of the process (and has been since
    // Windows XP), so it is queried once and cached rather than making a system call on
    // every use -- this is called from the middle of frame-pacing code.
    static const u64 frequency = QueryFrequency();
    return frequency;
}

f64 TicksToSeconds(u64 ticks) {
    return static_cast<f64>(ticks) / static_cast<f64>(TicksPerSecond());
}

u64 UnixSeconds() {
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);

    ULARGE_INTEGER wide;
    wide.LowPart  = fileTime.dwLowDateTime;
    wide.HighPart = fileTime.dwHighDateTime;

    // FILETIME counts 100ns intervals since 1601-01-01; the Unix epoch is 1970-01-01,
    // which is this many seconds later.
    constexpr u64 kIntervalsPerSecond    = 10'000'000ull;
    constexpr u64 kEpochDifferenceSeconds = 11'644'473'600ull;
    return static_cast<u64>(wide.QuadPart / kIntervalsPerSecond) - kEpochDifferenceSeconds;
}

}  // namespace Monarc::Platform::Time
