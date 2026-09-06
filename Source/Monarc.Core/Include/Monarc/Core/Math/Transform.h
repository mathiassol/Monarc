#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Math/Mat.h>
#include <Monarc/Core/Math/Quat.h>
#include <Monarc/Core/Math/Scalar.h>
#include <Monarc/Core/Math/Vec.h>
#include <Monarc/Core/Types.h>

namespace Monarc {

// Transform and its free functions live in namespace Monarc for the same ADL reason as
// Vec.h, Mat.h and Quat.h -- see Vec.h.
//
// Passed by const&, like Mat3/Mat4 and unlike Vec2/3/4/Quat: a translation, a rotation and
// a second Vec3 together are larger than one SIMD register, so a reference is cheaper than
// a copy regardless of what eventually lives behind Quat's or Vec3's layout.

/// A local transform, stored as separate translation, rotation and scale rather than as a
/// matrix -- the representation a scene graph actually edits and interpolates (Slerp
/// between two Transforms' rotations means something; Slerp between two matrices' upper-left
/// 3x3 does not). Defaults to the identity transform: zero translation, no rotation, unit
/// scale.
///
/// Two 12-byte Vec3s either side of a 16-byte-aligned Quat cannot be packed into a multiple
/// of 16 bytes without a gap, which MSVC's /W4 flags as C4324. The padding is expected and
/// harmless -- unlike Vec3, Transform is never packed into a tightly laid out array or fed
/// to a GPU -- so it is silenced here rather than reordering members to hide it from the
/// compiler.
#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4324)
#endif
struct Transform {
    Vec3 translation = Vec3::Zero();
    Quat rotation     = Quat::Identity();
    Vec3 scale        = Vec3::One();
};
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

/// Builds the matrix this Transform represents: `T * R * S`, so that scale is applied
/// first, then rotation, then translation. Doing it in any other order shears a
/// non-uniformly scaled object under rotation -- the classic scene-graph bug.
[[nodiscard]] constexpr Mat4 ToMat4(const Transform& t) {
    return Mat4::Translation(t.translation) * ToMat4(t.rotation) * Mat4::Scale(t.scale);
}

/// Composes two transforms: `parent * child` places `child` relative to `parent`, matching
/// `ToMat4(parent * child) == ToMat4(parent) * ToMat4(child)` -- which, via Mat4's own
/// composition rule, applies `child` first and `parent` second.
///
/// Exact when `parent.scale` is uniform, which is the common case -- uniform scale is also
/// the only kind Inverse below accepts, for the same underlying reason. Under a
/// non-uniform parent scale this is the same well-known approximation every engine with a
/// translation/rotation/scale transform makes: composing through a non-uniform scale can
/// introduce shear that a plain scale vector cannot represent, and this does not correct
/// for it.
[[nodiscard]] constexpr Transform operator*(const Transform& parent, const Transform& child) {
    return Transform{
        parent.translation + Rotate(parent.rotation, parent.scale * child.translation),
        parent.rotation * child.rotation,
        parent.scale * child.scale,
    };
}

/// Inverts a Transform, assuming uniform scale (`t.scale.x == t.scale.y == t.scale.z`).
/// Checked, not corrected: a non-uniformly scaled Transform does not invert to another
/// Transform, only to a Mat4 -- for a non-uniform `t`, invert `ToMat4(t)` with Mat4's own
/// Inverse instead, for the same shear reason operator* above documents.
///
/// Does not separately guard against a zero or near-zero scale the way Mat4::Inverse
/// guards its determinant -- a degenerate Transform is treated as a caller bug this does
/// not yet special-case.
[[nodiscard]] constexpr Transform Inverse(const Transform& t) {
    MONARC_CHECK(Math::ApproxEqual(t.scale.x, t.scale.y) &&
                     Math::ApproxEqual(t.scale.y, t.scale.z),
                 "Transform::Inverse assumes uniform scale; invert ToMat4(t) directly for a "
                 "non-uniform Transform");

    const f32  invScale    = 1.0f / t.scale.x;
    const Quat invRotation = Conjugate(t.rotation);

    return Transform{
        Rotate(invRotation, t.translation) * -invScale,
        invRotation,
        Vec3{invScale, invScale, invScale},
    };
}

}  // namespace Monarc
