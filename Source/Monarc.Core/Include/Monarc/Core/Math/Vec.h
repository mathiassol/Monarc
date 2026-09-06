#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Math/Scalar.h>
#include <Monarc/Core/Types.h>

#include <cmath>

namespace Monarc {

// -----------------------------------------------------------------------------------------
// Every free function here -- operators included -- lives in namespace Monarc, the same
// namespace as the vector types themselves, rather than in Monarc::Math. This is not
// stylistic: template code elsewhere (a HashMap<Vec3, X>, a generic Array<T> algorithm) is
// instantiated from within namespace Monarc and finds `Dot`, `operator==`, and friends only
// through argument-dependent lookup. ADL looks at the associated namespace of the argument
// types -- Vec3's own namespace, Monarc -- not wherever the template happens to be written.
// MSVC's lenient lookup will find a misplaced operator anyway; clang-cl correctly will not.
// See Monarc::HashMap's test file for the case that first surfaced this.
//
// Vec2/Vec3/Vec4 are passed by value throughout, not by const&: they are exactly
// SIMD-register-sized (or, for Vec3, small enough that a reference buys nothing), and
// ADR-0015 asks for layout and API that do not have to change when a SIMD backend arrives.
// Mat3/Mat4 (Mat.h) are passed by const& instead -- large enough that a reference is
// cheaper than a copy regardless of what eventually lives behind it.
// -----------------------------------------------------------------------------------------

namespace Detail {

/// Floor for the length below which Normalize refuses to divide.
///
/// Deliberately far smaller than Math::kEpsilon, which is tuned for comparing
/// roughly-unit-magnitude quantities and is the wrong tool here: a legitimate small vector
/// like (1e-5, 0, 0) still has a perfectly well-defined direction, and Math::kEpsilon would
/// have discarded it. This floor exists only to stop `1 / length` from overflowing to
/// infinity, which needs a length below roughly `1 / FLT_MAX` (~2.9e-39); 1e-20 sits
/// comfortably above that and comfortably below any vector this engine plausibly computes.
inline constexpr f32 kMinNormalizableLength = 1e-20f;

}  // namespace Detail

/// Two-component vector: texture coordinates, 2D positions, screen-space offsets.
struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    constexpr Vec2() = default;
    constexpr explicit Vec2(f32 scalar) : x(scalar), y(scalar) {}
    constexpr Vec2(f32 inX, f32 inY) : x(inX), y(inY) {}

    [[nodiscard]] constexpr f32& operator[](usize index) {
        MONARC_CHECK(index < 2, "Vec2 subscript out of range");
        switch (index) {
            case 0:  return x;
            default: return y;
        }
    }

    [[nodiscard]] constexpr const f32& operator[](usize index) const {
        MONARC_CHECK(index < 2, "Vec2 subscript out of range");
        switch (index) {
            case 0:  return x;
            default: return y;
        }
    }

    constexpr Vec2& operator+=(Vec2 rhs) { x += rhs.x; y += rhs.y; return *this; }
    constexpr Vec2& operator-=(Vec2 rhs) { x -= rhs.x; y -= rhs.y; return *this; }
    constexpr Vec2& operator*=(f32 rhs) { x *= rhs; y *= rhs; return *this; }
    constexpr Vec2& operator*=(Vec2 rhs) { x *= rhs.x; y *= rhs.y; return *this; }

    [[nodiscard]] static constexpr Vec2 Zero() { return Vec2{0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec2 One() { return Vec2{1.0f, 1.0f}; }
    [[nodiscard]] static constexpr Vec2 UnitX() { return Vec2{1.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec2 UnitY() { return Vec2{0.0f, 1.0f}; }
};

[[nodiscard]] constexpr bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
[[nodiscard]] constexpr bool operator!=(Vec2 a, Vec2 b) { return !(a == b); }

[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 v) { return Vec2{-v.x, -v.y}; }
[[nodiscard]] constexpr Vec2 operator*(Vec2 a, Vec2 b) { return Vec2{a.x * b.x, a.y * b.y}; }
[[nodiscard]] constexpr Vec2 operator*(Vec2 v, f32 s) { return Vec2{v.x * s, v.y * s}; }
[[nodiscard]] constexpr Vec2 operator*(f32 s, Vec2 v) { return v * s; }

[[nodiscard]] constexpr f32 Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr f32 LengthSquared(Vec2 v) { return Dot(v, v); }

/// Not constexpr: std::sqrt is not a constant expression before C++26.
[[nodiscard]] inline f32 Length(Vec2 v) { return std::sqrt(LengthSquared(v)); }

/// Scales `v` to unit length. A vector too close to zero length has no meaningful
/// direction, so rather than propagate the NaN that dividing by ~0 would produce -- which
/// silently poisons every computation downstream of it -- this flags the violation (in every
/// configuration MONARC_CHECK runs in) and returns Zero(), a defined, inert value.
[[nodiscard]] inline Vec2 Normalize(Vec2 v) {
    const f32 len = Length(v);
    MONARC_CHECK(len > Detail::kMinNormalizableLength,
                 "Normalize: vector is too close to zero length to normalize safely");
    if (len <= Detail::kMinNormalizableLength) {
        return Vec2::Zero();
    }
    return v * (1.0f / len);
}

/// Three-component vector: positions, directions, normals, RGB.
///
/// Exactly 12 bytes, unpadded. Arrays of Vec3 are vertex data and must match the layout a
/// GPU expects -- do not add a fourth, hidden component "for alignment".
struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;

    constexpr Vec3() = default;
    constexpr explicit Vec3(f32 scalar) : x(scalar), y(scalar), z(scalar) {}
    constexpr Vec3(f32 inX, f32 inY, f32 inZ) : x(inX), y(inY), z(inZ) {}

    [[nodiscard]] constexpr f32& operator[](usize index) {
        MONARC_CHECK(index < 3, "Vec3 subscript out of range");
        switch (index) {
            case 0:  return x;
            case 1:  return y;
            default: return z;
        }
    }

    [[nodiscard]] constexpr const f32& operator[](usize index) const {
        MONARC_CHECK(index < 3, "Vec3 subscript out of range");
        switch (index) {
            case 0:  return x;
            case 1:  return y;
            default: return z;
        }
    }

    constexpr Vec3& operator+=(Vec3 rhs) {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }
    constexpr Vec3& operator-=(Vec3 rhs) {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        return *this;
    }
    constexpr Vec3& operator*=(f32 rhs) {
        x *= rhs;
        y *= rhs;
        z *= rhs;
        return *this;
    }
    constexpr Vec3& operator*=(Vec3 rhs) {
        x *= rhs.x;
        y *= rhs.y;
        z *= rhs.z;
        return *this;
    }

    [[nodiscard]] static constexpr Vec3 Zero() { return Vec3{0.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec3 One() { return Vec3{1.0f, 1.0f, 1.0f}; }
    [[nodiscard]] static constexpr Vec3 UnitX() { return Vec3{1.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec3 UnitY() { return Vec3{0.0f, 1.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec3 UnitZ() { return Vec3{0.0f, 0.0f, 1.0f}; }
};

static_assert(sizeof(Vec3) == 12, "Vec3 must match the GPU's tightly packed layout");

[[nodiscard]] constexpr bool operator==(Vec3 a, Vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
[[nodiscard]] constexpr bool operator!=(Vec3 a, Vec3 b) { return !(a == b); }

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 v) { return Vec3{-v.x, -v.y, -v.z}; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, Vec3 b) {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}
[[nodiscard]] constexpr Vec3 operator*(Vec3 v, f32 s) {
    return Vec3{v.x * s, v.y * s, v.z * s};
}
[[nodiscard]] constexpr Vec3 operator*(f32 s, Vec3 v) { return v * s; }

[[nodiscard]] constexpr f32 Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr f32 LengthSquared(Vec3 v) { return Dot(v, v); }

/// Not constexpr: std::sqrt is not a constant expression before C++26.
[[nodiscard]] inline f32 Length(Vec3 v) { return std::sqrt(LengthSquared(v)); }

/// See Vec2's Normalize: a degenerate input is flagged, then mapped to a defined zero
/// rather than left to produce NaN.
[[nodiscard]] inline Vec3 Normalize(Vec3 v) {
    const f32 len = Length(v);
    MONARC_CHECK(len > Detail::kMinNormalizableLength,
                 "Normalize: vector is too close to zero length to normalize safely");
    if (len <= Detail::kMinNormalizableLength) {
        return Vec3::Zero();
    }
    return v * (1.0f / len);
}

/// Right-handed cross product.
///
/// ADR-0015: Cross(UnitX(), UnitY()) == UnitZ() is this engine's working definition of
/// "right-handed". A test exists whose only purpose is to fail if this ever quietly flips --
/// which would mirror every normal, flip every winding order, and turn every camera around.
[[nodiscard]] constexpr Vec3 Cross(Vec3 a, Vec3 b) {
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

/// Four-component vector: homogeneous coordinates (w = 1 for a point, 0 for a direction),
/// RGBA, or a clip-space result.
///
/// 16-byte aligned so a later SIMD backend can load and store it directly, without changing
/// this layout or the API built on it -- ADR-0015 defers SIMD, not the alignment it needs.
struct alignas(16) Vec4 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    constexpr Vec4() = default;
    constexpr explicit Vec4(f32 scalar) : x(scalar), y(scalar), z(scalar), w(scalar) {}
    constexpr Vec4(f32 inX, f32 inY, f32 inZ, f32 inW) : x(inX), y(inY), z(inZ), w(inW) {}
    constexpr Vec4(Vec3 xyz, f32 inW) : x(xyz.x), y(xyz.y), z(xyz.z), w(inW) {}

    [[nodiscard]] constexpr f32& operator[](usize index) {
        MONARC_CHECK(index < 4, "Vec4 subscript out of range");
        switch (index) {
            case 0:  return x;
            case 1:  return y;
            case 2:  return z;
            default: return w;
        }
    }

    [[nodiscard]] constexpr const f32& operator[](usize index) const {
        MONARC_CHECK(index < 4, "Vec4 subscript out of range");
        switch (index) {
            case 0:  return x;
            case 1:  return y;
            case 2:  return z;
            default: return w;
        }
    }

    /// Drops w. Explicit and named rather than an implicit conversion, so truncating a
    /// homogeneous coordinate is always visible at the call site.
    [[nodiscard]] constexpr Vec3 XYZ() const { return Vec3{x, y, z}; }

    constexpr Vec4& operator+=(Vec4 rhs) {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        w += rhs.w;
        return *this;
    }
    constexpr Vec4& operator-=(Vec4 rhs) {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        w -= rhs.w;
        return *this;
    }
    constexpr Vec4& operator*=(f32 rhs) {
        x *= rhs;
        y *= rhs;
        z *= rhs;
        w *= rhs;
        return *this;
    }
    constexpr Vec4& operator*=(Vec4 rhs) {
        x *= rhs.x;
        y *= rhs.y;
        z *= rhs.z;
        w *= rhs.w;
        return *this;
    }

    [[nodiscard]] static constexpr Vec4 Zero() { return Vec4{0.0f, 0.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec4 One() { return Vec4{1.0f, 1.0f, 1.0f, 1.0f}; }
    [[nodiscard]] static constexpr Vec4 UnitX() { return Vec4{1.0f, 0.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec4 UnitY() { return Vec4{0.0f, 1.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vec4 UnitZ() { return Vec4{0.0f, 0.0f, 1.0f, 0.0f}; }
};

static_assert(sizeof(Vec4) == 16, "Vec4 must stay exactly one SIMD register wide");
static_assert(alignof(Vec4) == 16, "Vec4 must stay 16-byte aligned for later SIMD loads");

[[nodiscard]] constexpr bool operator==(Vec4 a, Vec4 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
[[nodiscard]] constexpr bool operator!=(Vec4 a, Vec4 b) { return !(a == b); }

[[nodiscard]] constexpr Vec4 operator+(Vec4 a, Vec4 b) {
    return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
[[nodiscard]] constexpr Vec4 operator-(Vec4 a, Vec4 b) {
    return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
[[nodiscard]] constexpr Vec4 operator-(Vec4 v) { return Vec4{-v.x, -v.y, -v.z, -v.w}; }
[[nodiscard]] constexpr Vec4 operator*(Vec4 a, Vec4 b) {
    return Vec4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
[[nodiscard]] constexpr Vec4 operator*(Vec4 v, f32 s) {
    return Vec4{v.x * s, v.y * s, v.z * s, v.w * s};
}
[[nodiscard]] constexpr Vec4 operator*(f32 s, Vec4 v) { return v * s; }

[[nodiscard]] constexpr f32 Dot(Vec4 a, Vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
[[nodiscard]] constexpr f32 LengthSquared(Vec4 v) { return Dot(v, v); }

/// Not constexpr: std::sqrt is not a constant expression before C++26.
[[nodiscard]] inline f32 Length(Vec4 v) { return std::sqrt(LengthSquared(v)); }

/// See Vec2's Normalize: a degenerate input is flagged, then mapped to a defined zero
/// rather than left to produce NaN.
[[nodiscard]] inline Vec4 Normalize(Vec4 v) {
    const f32 len = Length(v);
    MONARC_CHECK(len > Detail::kMinNormalizableLength,
                 "Normalize: vector is too close to zero length to normalize safely");
    if (len <= Detail::kMinNormalizableLength) {
        return Vec4::Zero();
    }
    return v * (1.0f / len);
}

}  // namespace Monarc
