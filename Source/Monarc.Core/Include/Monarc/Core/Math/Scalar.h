#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc::Math {

/// Ratio of a circle's circumference to its diameter, to float precision.
inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = kPi * 2.0f;
inline constexpr f32 kHalfPi = kPi * 0.5f;

/// Default tolerance for ApproxEqual. An absolute difference, chosen to absorb ordinary
/// float round-off (a handful of ULPs at values near unit magnitude) while still catching a
/// computation that is actually wrong. Not suitable unmodified at very large or very small
/// magnitudes -- pass an explicit tolerance there.
inline constexpr f32 kEpsilon = 1e-5f;

/// Converts an angle from degrees to radians.
///
/// ADR-0015: every interface below an authoring boundary takes radians. This is the
/// conversion, meant to be used once, at that boundary -- not threaded through gameplay code.
template <typename T>
[[nodiscard]] constexpr T Radians(T degrees) {
    return degrees * (kPi / static_cast<T>(180));
}

/// Converts an angle from radians to degrees. See Radians -- friendlier to read in a
/// debugger or an authoring tool, not meant for use in an internal computation.
template <typename T>
[[nodiscard]] constexpr T Degrees(T radians) {
    return radians * (static_cast<T>(180) / kPi);
}

/// Bounds `value` to [low, high], passing it through unchanged in between.
template <typename T>
[[nodiscard]] constexpr T Clamp(T value, T low, T high) {
    return value < low ? low : (value > high ? high : value);
}

/// Linear interpolation. Exact at the endpoints: `Lerp(a, b, 0) == a` and
/// `Lerp(a, b, 1) == b`, with no accumulated error from the identity term cancelling out.
template <typename T>
[[nodiscard]] constexpr T Lerp(T a, T b, T t) {
    return a + (b - a) * t;
}

/// Componentwise-safe equality for floating point: exact equality is nearly always the
/// wrong question to ask of two floats that reached the same value by different
/// computations. `tolerance` is an absolute difference -- adequate at the roughly-unit
/// magnitudes this engine's math deals in; nothing here needs a relative/ULP comparison yet.
template <typename T>
[[nodiscard]] constexpr bool ApproxEqual(T a, T b, T tolerance = static_cast<T>(kEpsilon)) {
    const T diff = a > b ? a - b : b - a;
    return diff <= tolerance;
}

}  // namespace Monarc::Math
