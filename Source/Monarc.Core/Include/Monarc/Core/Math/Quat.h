#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Math/Mat.h>
#include <Monarc/Core/Math/Scalar.h>
#include <Monarc/Core/Math/Vec.h>
#include <Monarc/Core/Types.h>

#include <cmath>

namespace Monarc {

// Quat and its free functions live in namespace Monarc, not Monarc::Math, for the same ADL
// reason as Vec.h and Mat.h: template code instantiated inside namespace Monarc finds
// Rotate, operator* and friends only through argument-dependent lookup on Quat's own
// namespace, not wherever the template happens to be written. See Vec.h.
//
// Passed by value throughout, like Vec2/3/4 and unlike Mat3/Mat4/Transform: a Quat is
// exactly one SIMD register wide (four f32, 16-byte aligned), so a reference would cost
// more than the copy it avoids, and ADR-0015 asks for a layout that does not have to change
// when a SIMD backend arrives.

namespace Detail {

/// Above this cosine of the angle between two quaternions, Slerp's exact formula would
/// divide by a sin(theta) too close to zero to trust -- exactly the common case of a
/// slowly animating rotation, where the two orientations are nearly identical, not a rare
/// edge case. 0.9995 corresponds to roughly a 1.8 degree separation: the same threshold
/// glm uses, far enough from 1 to keep the division stable in float32, and small enough
/// that the exact and linear-interpolation results are indistinguishable within it.
inline constexpr f32 kSlerpLinearThreshold = 0.9995f;

}  // namespace Detail

/// A rotation, stored as `x, y, z` (the vector part) and `w` (the scalar part) -- the
/// Hamilton convention. 16-byte aligned, matching Vec4, so a later SIMD backend can load
/// and store it directly without changing this layout or the API built on it.
struct alignas(16) Quat {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    constexpr Quat() = default;
    constexpr Quat(f32 inX, f32 inY, f32 inZ, f32 inW) : x(inX), y(inY), z(inZ), w(inW) {}

    /// The rotation that does nothing. Deliberately not what a default-constructed Quat
    /// holds -- see Mat4::Identity(), which draws the same distinction for the same reason:
    /// a type's zero value and its identity value are different things, and spelling out
    /// Identity() at every call site that needs it is cheaper than a bug from assuming a
    /// bare `Quat{}` is safe to rotate with (it is not: a zero quaternion has zero length).
    [[nodiscard]] static constexpr Quat Identity() { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }

    /// Builds the rotation of `radians` about `axis`.
    ///
    /// Precondition: `axis` must already be unit length -- checked, not corrected, the same
    /// choice Mat4's Perspective and Orthographic make for their own preconditions. A
    /// non-unit axis produces a non-unit quaternion (its length scales with the axis's),
    /// which would silently break every consumer downstream that assumes unit rotations,
    /// rather than failing where the actual mistake is.
    [[nodiscard]] static Quat FromAxisAngle(Vec3 axis, f32 radians) {
        MONARC_CHECK(Math::ApproxEqual(Length(axis), 1.0f),
                     "FromAxisAngle: axis must be a unit vector");
        const f32 half = radians * 0.5f;
        const f32 s    = std::sin(half);
        const f32 c    = std::cos(half);
        return Quat{axis.x * s, axis.y * s, axis.z * s, c};
    }
};

static_assert(sizeof(Quat) == 16, "Quat must stay exactly one SIMD register wide");
static_assert(alignof(Quat) == 16, "Quat must stay 16-byte aligned for later SIMD loads");

[[nodiscard]] constexpr f32 Dot(Quat a, Quat b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

/// Not constexpr: std::sqrt is not a constant expression before C++26.
[[nodiscard]] inline f32 Length(Quat q) { return std::sqrt(Dot(q, q)); }

/// Scales `q` to unit length. See Vec.h's Normalize: a degenerate input is flagged (in
/// every configuration MONARC_CHECK runs in), then mapped to a defined value rather than
/// left to produce NaN. The defined value here is Identity() rather than a zero quaternion
/// -- a zero quaternion is not a rotation at all (it has no unit-length representative),
/// where Identity() is exactly the "do nothing" rotation a caller that fed in a degenerate
/// quaternion almost certainly wanted.
///
/// Reuses Vec.h's Detail::kMinNormalizableLength rather than defining a second, identical
/// constant: the floor exists only to stop `1 / length` from overflowing to infinity, and
/// that reasoning does not depend on how many components the thing being normalized has.
[[nodiscard]] inline Quat Normalize(Quat q) {
    const f32 len = Length(q);
    MONARC_CHECK(len > Detail::kMinNormalizableLength,
                 "Normalize: quaternion is too close to zero length to normalize safely");
    if (len <= Detail::kMinNormalizableLength) {
        return Quat::Identity();
    }
    const f32 inv = 1.0f / len;
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

/// The Hamilton product. Matches ADR-0015's matrix composition convention exactly: `a * b`
/// applies `b` first, then `a` -- `Rotate(a * b, v) == Rotate(a, Rotate(b, v))`, the same
/// rule Mat4's operator* follows, and a test exists whose only purpose is to fail if the
/// two ever disagree.
[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

/// Rotates `v` by `q`. Assumes `q` is unit length, as every quaternion this file produces
/// is (FromAxisAngle checks its input; Normalize and Slerp both return unit results); a
/// non-unit `q` scales the result rather than triggering some well-defined failure, so
/// unlike Normalize there is no single bad value here for a MONARC_CHECK to usefully catch.
///
/// Computed as `v + 2w(u x v) + 2u x (u x v)` (u = q's vector part, w = q's scalar part) --
/// algebraically the same rotation as `q v q*` with `v` treated as a pure quaternion, but
/// without ever forming that intermediate quaternion product.
[[nodiscard]] constexpr Vec3 Rotate(Quat q, Vec3 v) {
    const Vec3 u = Vec3{q.x, q.y, q.z};
    const Vec3 t = Cross(u, v) * 2.0f;
    return v + q.w * t + Cross(u, t);
}

/// The inverse rotation, for a unit quaternion: negating the vector part reverses the
/// angle's sense while leaving the axis's line -- and the scalar part, hence the angle's
/// magnitude -- unchanged.
[[nodiscard]] constexpr Quat Conjugate(Quat q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

/// Converts to the equivalent rotation matrix. Assumes `q` is unit length -- see Rotate;
/// the same reasoning applies, and the derivation below is only a rotation matrix at all
/// when it does.
[[nodiscard]] constexpr Mat4 ToMat4(Quat q) {
    const f32 xx = q.x * q.x;
    const f32 yy = q.y * q.y;
    const f32 zz = q.z * q.z;
    const f32 xy = q.x * q.y;
    const f32 xz = q.x * q.z;
    const f32 yz = q.y * q.z;
    const f32 wx = q.w * q.x;
    const f32 wy = q.w * q.y;
    const f32 wz = q.w * q.z;

    return Mat4{
        Vec4{1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f},
        Vec4{2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f},
        Vec4{2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f},
        Vec4{0.0f, 0.0f, 0.0f, 1.0f},
    };
}

/// Spherical linear interpolation between two rotations, always taking the shorter of the
/// two paths around the 4D unit sphere.
///
/// `q` and `-q` represent the identical rotation, so before interpolating, `b` is negated
/// whenever it is the dot-farther of the two choices from `a` -- otherwise the exact
/// formula below still interpolates a mathematically valid path, just the long way around,
/// and a rotating object visibly lurches. A test exists whose only purpose is to fail if
/// this sign fix is ever dropped.
///
/// Falls back to a normalized linear interpolation when `a` and `b` (after that sign fix)
/// are nearly parallel: the exact formula divides by sin(theta), which approaches zero
/// exactly there -- the common case of a slowly animating rotation, not a rare one -- and
/// dividing by a near-zero sine is the numerically wrong way to compute what is,
/// geometrically, almost a straight-line interpolation anyway.
[[nodiscard]] inline Quat Slerp(Quat a, Quat b, f32 t) {
    f32  cosTheta = Dot(a, b);
    Quat to       = b;
    if (cosTheta < 0.0f) {
        cosTheta = -cosTheta;
        to       = Quat{-b.x, -b.y, -b.z, -b.w};
    }

    if (cosTheta > Detail::kSlerpLinearThreshold) {
        return Normalize(Quat{
            Math::Lerp(a.x, to.x, t),
            Math::Lerp(a.y, to.y, t),
            Math::Lerp(a.z, to.z, t),
            Math::Lerp(a.w, to.w, t),
        });
    }

    const f32 theta    = std::acos(Math::Clamp(cosTheta, -1.0f, 1.0f));
    const f32 sinTheta = std::sin(theta);
    const f32 wa       = std::sin((1.0f - t) * theta) / sinTheta;
    const f32 wb       = std::sin(t * theta) / sinTheta;

    return Quat{
        wa * a.x + wb * to.x,
        wa * a.y + wb * to.y,
        wa * a.z + wb * to.z,
        wa * a.w + wb * to.w,
    };
}

}  // namespace Monarc
