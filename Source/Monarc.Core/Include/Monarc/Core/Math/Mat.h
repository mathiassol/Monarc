#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Math/Scalar.h>
#include <Monarc/Core/Math/Vec.h>
#include <Monarc/Core/Types.h>

#include <cmath>

namespace Monarc {

// Free functions here live in namespace Monarc for the same reason Vec.h's do: they are
// found by argument-dependent lookup from template code instantiated inside namespace
// Monarc, and ADL only looks at a type's own namespace. See Vec.h.
//
// Mat3/Mat4 are passed by const& (unlike Vec2/3/4, passed by value) -- large enough that a
// reference is cheaper than a copy regardless of what eventually lives behind the layout.

namespace Detail {

/// Floor for the determinant below which Inverse refuses to divide.
///
/// A determinant is a *cubed* length, so it needs its own floor rather than Vec.h's
/// kMinNormalizableLength or Scalar.h's Math::kEpsilon, both tuned for linear,
/// roughly-unit-magnitude quantities: a legitimate small uniform scale of 0.01 already has a
/// determinant of 1e-6, and either of those would wrongly reject it as singular. This floor
/// exists only to stop `1 / det` from overflowing to infinity (needs |det| below roughly
/// `1 / FLT_MAX` (~2.9e-39)); 1e-20 sits comfortably above that and comfortably below any
/// determinant a physically plausible transform produces.
inline constexpr f32 kMinInvertibleDeterminant = 1e-20f;

}  // namespace Detail

/// Column-major 3x3 matrix: rotation-only work, and normal matrices (the inverse-transpose
/// of a model matrix's upper-left 3x3, needed to keep normals correct under non-uniform
/// scale). Deliberately minimal next to Mat4 -- nothing yet needs more than this, and
/// nothing here is exercised by a test; extend it when the renderer phase needs to.
struct Mat3 {
    Vec3 columns[3];

    [[nodiscard]] static constexpr Mat3 Identity() {
        return Mat3{
            Vec3{1.0f, 0.0f, 0.0f},
            Vec3{0.0f, 1.0f, 0.0f},
            Vec3{0.0f, 0.0f, 1.0f},
        };
    }
};

[[nodiscard]] constexpr Vec3 operator*(const Mat3& m, Vec3 v) {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z;
}

[[nodiscard]] constexpr Mat3 operator*(const Mat3& a, const Mat3& b) {
    return Mat3{
        a * b.columns[0],
        a * b.columns[1],
        a * b.columns[2],
    };
}

[[nodiscard]] constexpr Mat3 Transpose(const Mat3& m) {
    Mat3 result{};
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            result.columns[row][col] = m.columns[col][row];
        }
    }
    return result;
}

/// Column-major 4x4 matrix. `columns[3]` is the translation column (ADR-0015): storage is
/// column-major, vectors are columns, and a transform is applied as `M * v`.
///
/// 16-byte aligned so a later SIMD backend can treat each column as one register, without
/// changing this layout or the API built on top of it.
struct alignas(16) Mat4 {
    Vec4 columns[4];

    [[nodiscard]] static constexpr Mat4 Identity() {
        return Mat4{
            Vec4{1.0f, 0.0f, 0.0f, 0.0f},
            Vec4{0.0f, 1.0f, 0.0f, 0.0f},
            Vec4{0.0f, 0.0f, 1.0f, 0.0f},
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
        };
    }

    [[nodiscard]] static constexpr Mat4 Translation(Vec3 t) {
        return Mat4{
            Vec4{1.0f, 0.0f, 0.0f, 0.0f},
            Vec4{0.0f, 1.0f, 0.0f, 0.0f},
            Vec4{0.0f, 0.0f, 1.0f, 0.0f},
            Vec4{t, 1.0f},
        };
    }

    [[nodiscard]] static constexpr Mat4 Scale(Vec3 s) {
        return Mat4{
            Vec4{s.x, 0.0f, 0.0f, 0.0f},
            Vec4{0.0f, s.y, 0.0f, 0.0f},
            Vec4{0.0f, 0.0f, s.z, 0.0f},
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
        };
    }

    /// Right-handed: a positive angle turns +Y toward +Z -- the cyclic continuation of
    /// ADR-0015's Cross(UnitY(), UnitZ()) == UnitX().
    [[nodiscard]] static Mat4 RotationX(f32 radians) {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        return Mat4{
            Vec4{1.0f, 0.0f, 0.0f, 0.0f},
            Vec4{0.0f, c, s, 0.0f},
            Vec4{0.0f, -s, c, 0.0f},
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
        };
    }

    /// Right-handed: a positive angle turns +Z toward +X.
    [[nodiscard]] static Mat4 RotationY(f32 radians) {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        return Mat4{
            Vec4{c, 0.0f, -s, 0.0f},
            Vec4{0.0f, 1.0f, 0.0f, 0.0f},
            Vec4{s, 0.0f, c, 0.0f},
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
        };
    }

    /// Right-handed: a positive angle turns +X toward +Y -- the rotation ADR-0015's own
    /// handedness test exercises directly.
    [[nodiscard]] static Mat4 RotationZ(f32 radians) {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        return Mat4{
            Vec4{c, s, 0.0f, 0.0f},
            Vec4{-s, c, 0.0f, 0.0f},
            Vec4{0.0f, 0.0f, 1.0f, 0.0f},
            Vec4{0.0f, 0.0f, 0.0f, 1.0f},
        };
    }

    /// Right-handed perspective projection, depth range 0..1: near maps to 0, far to 1
    /// (ADR-0015 -- Vulkan, D3D12 and Metal all want this; a -1 would mean OpenGL's
    /// convention crept in). `fovYRadians` is the full vertical field of view.
    ///
    /// Vulkan's inverted clip-space Y is deliberately not handled here -- ADR-0015 puts that
    /// in exactly one function per backend, not in shared code every camera and every asset
    /// would otherwise have to know about.
    ///
    /// Preconditions -- checked, but not recovered from: `0 < fovYRadians < pi`;
    /// `aspect > 0`; `0 < nearZ < farZ`, both finite. There is no "safe" projection matrix to
    /// substitute for a violated precondition, so unlike Normalize this does not fall back
    /// to a defined value -- it is a caller bug, and MONARC_CHECK exists to surface exactly
    /// that.
    [[nodiscard]] static Mat4 Perspective(f32 fovYRadians, f32 aspect, f32 nearZ, f32 farZ) {
        MONARC_CHECK(fovYRadians > 0.0f && fovYRadians < Math::kPi,
                     "Perspective: fovYRadians must be in (0, pi)");
        MONARC_CHECK(aspect > 0.0f, "Perspective: aspect must be positive");
        MONARC_CHECK(nearZ > 0.0f, "Perspective: nearZ must be positive");
        // An infinite farZ is not supported: the depth-mapping coefficients below divide by
        // (nearZ - farZ), and farZ - or that difference - being infinite turns them into
        // Inf/Inf, i.e. NaN. Checked explicitly rather than merely by `farZ > nearZ`, which
        // an infinite farZ would pass while still silently producing a NaN-filled matrix.
        MONARC_CHECK(std::isfinite(farZ) && farZ > nearZ,
                     "Perspective: farZ must be finite and greater than nearZ");

        const f32 focalLength = 1.0f / std::tan(fovYRadians * 0.5f);
        return Mat4{
            Vec4{focalLength / aspect, 0.0f, 0.0f, 0.0f},
            Vec4{0.0f, focalLength, 0.0f, 0.0f},
            Vec4{0.0f, 0.0f, farZ / (nearZ - farZ), -1.0f},
            Vec4{0.0f, 0.0f, (nearZ * farZ) / (nearZ - farZ), 0.0f},
        };
    }

    /// Right-handed orthographic projection, depth range 0..1. See Perspective for the
    /// depth-range rationale. There is no perspective divide in an orthographic projection --
    /// w stays 1 -- so the 0..1 mapping is exact, not merely after a divide.
    [[nodiscard]] static Mat4 Orthographic(f32 left, f32 right, f32 bottom, f32 top,
                                           f32 nearZ, f32 farZ) {
        MONARC_CHECK(right != left, "Orthographic: right must differ from left");
        MONARC_CHECK(top != bottom, "Orthographic: top must differ from bottom");
        MONARC_CHECK(farZ != nearZ, "Orthographic: farZ must differ from nearZ");

        return Mat4{
            Vec4{2.0f / (right - left), 0.0f, 0.0f, 0.0f},
            Vec4{0.0f, 2.0f / (top - bottom), 0.0f, 0.0f},
            Vec4{0.0f, 0.0f, -1.0f / (farZ - nearZ), 0.0f},
            Vec4{
                -(right + left) / (right - left),
                -(top + bottom) / (top - bottom),
                -nearZ / (farZ - nearZ),
                1.0f,
            },
        };
    }

    /// Right-handed view matrix: the inverse of the camera's world transform, built directly
    /// from `eye`/`target`/`up` rather than assembled and run through Inverse. `eye` maps to
    /// the view-space origin and `target` ends up down -Z (ADR-0015) -- both are tested
    /// directly. `up` need not be exactly perpendicular to the view direction; it is only a
    /// hint used to construct the right axis, which is then re-orthogonalised.
    [[nodiscard]] static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up) {
        const Vec3 zAxis = Normalize(eye - target);
        const Vec3 xAxis = Normalize(Cross(up, zAxis));
        const Vec3 yAxis = Cross(zAxis, xAxis);

        return Mat4{
            Vec4{xAxis.x, yAxis.x, zAxis.x, 0.0f},
            Vec4{xAxis.y, yAxis.y, zAxis.y, 0.0f},
            Vec4{xAxis.z, yAxis.z, zAxis.z, 0.0f},
            Vec4{-Dot(xAxis, eye), -Dot(yAxis, eye), -Dot(zAxis, eye), 1.0f},
        };
    }
};

static_assert(sizeof(Mat4) == 64, "Mat4 must stay four SIMD registers wide");
static_assert(alignof(Mat4) == 16, "Mat4 must stay 16-byte aligned for later SIMD loads");

[[nodiscard]] constexpr Vec4 operator*(const Mat4& m, Vec4 v) {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z + m.columns[3] * v.w;
}

[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    return Mat4{
        a * b.columns[0],
        a * b.columns[1],
        a * b.columns[2],
        a * b.columns[3],
    };
}

/// Transforms a point: w = 1, so translation applies. Not a projection -- there is no
/// perspective divide here. A matrix produced by Perspective is deliberately never fed
/// through TransformPoint in this engine; divide by the result's own w explicitly instead
/// (see TestMat.cpp's projection tests), because only the caller knows whether a divide is
/// wanted.
[[nodiscard]] constexpr Vec3 TransformPoint(const Mat4& m, Vec3 p) {
    return (m * Vec4{p, 1.0f}).XYZ();
}

/// Transforms a direction: w = 0, so translation does not apply.
[[nodiscard]] constexpr Vec3 TransformDirection(const Mat4& m, Vec3 d) {
    return (m * Vec4{d, 0.0f}).XYZ();
}

[[nodiscard]] constexpr Mat4 Transpose(const Mat4& m) {
    Mat4 result{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result.columns[row][col] = m.columns[col][row];
        }
    }
    return result;
}

/// Inverts a general affine matrix: any invertible upper-left 3x3 (rotation combined with
/// non-uniform scale or shear, not only rotation alone) plus an arbitrary translation, with
/// the bottom row assumed to be exactly [0 0 0 1]. This is *not* a general projective 4x4
/// inverse -- it is never applied to a matrix produced by Perspective or Orthographic, and
/// nothing in this engine does that.
///
/// Computed as the textbook block inverse of [R t; 0 1], namely [R^-1  -R^-1 t; 0 1], with
/// R^-1 built from cross products (each row of R^-1 is a cross product of two of R's columns,
/// divided by det(R)) rather than general Gaussian elimination -- cheaper, and exact for
/// exactly the affine case this function documents.
///
/// A singular or near-singular R has no inverse. MONARC_CHECK flags that (a caller bug, same
/// as Perspective's preconditions) and, since some defined value has to come back, this
/// returns Identity() rather than a matrix built from a division by ~0 -- consistent with
/// Normalize's choice to return a defined value instead of propagating NaN.
[[nodiscard]] constexpr Mat4 Inverse(const Mat4& m) {
    const Vec3 linX        = m.columns[0].XYZ();
    const Vec3 linY        = m.columns[1].XYZ();
    const Vec3 linZ        = m.columns[2].XYZ();
    const Vec3 translation = m.columns[3].XYZ();

    const f32 det           = Dot(linX, Cross(linY, linZ));
    const bool nearSingular = det <= Detail::kMinInvertibleDeterminant &&
                              det >= -Detail::kMinInvertibleDeterminant;
    MONARC_CHECK(!nearSingular,
                 "Inverse: matrix is singular or too close to singular to invert safely");
    if (nearSingular) {
        return Mat4::Identity();
    }
    const f32 invDet = 1.0f / det;

    // Rows of the inverse of the 3x3 linear part. Correct for any invertible 3x3 -- rotation
    // combined with non-uniform scale or shear -- not only an orthogonal, rotation-only one.
    const Vec3 row0 = Cross(linY, linZ) * invDet;
    const Vec3 row1 = Cross(linZ, linX) * invDet;
    const Vec3 row2 = Cross(linX, linY) * invDet;

    // New translation is -(R^-1 * translation), taken as dot products against the rows
    // above rather than assembling R^-1 as a matrix and then multiplying it through.
    const Vec3 invTranslation{
        -Dot(row0, translation),
        -Dot(row1, translation),
        -Dot(row2, translation),
    };

    return Mat4{
        Vec4{row0.x, row1.x, row2.x, 0.0f},
        Vec4{row0.y, row1.y, row2.y, 0.0f},
        Vec4{row0.z, row1.z, row2.z, 0.0f},
        Vec4{invTranslation, 1.0f},
    };
}

}  // namespace Monarc
