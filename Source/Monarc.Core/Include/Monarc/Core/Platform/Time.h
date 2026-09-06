#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc::Platform::Time {

/// Monotonic tick count from an unspecified origin. Never decreases, unaffected by wall
/// clock changes. This is the clock for frame pacing and profiling; UnixSeconds is not.
[[nodiscard]] u64 Ticks();

/// Ticks per second for the monotonic clock. Constant for the life of the process.
[[nodiscard]] u64 TicksPerSecond();

/// Converts a tick *interval* to seconds. Meaningless applied to an absolute Ticks()
/// value, whose origin is unspecified.
[[nodiscard]] f64 TicksToSeconds(u64 ticks);

/// Seconds since the Unix epoch. For logs and file timestamps -- not for measuring
/// intervals, because it can jump when the system clock is corrected.
[[nodiscard]] u64 UnixSeconds();

}  // namespace Monarc::Platform::Time
