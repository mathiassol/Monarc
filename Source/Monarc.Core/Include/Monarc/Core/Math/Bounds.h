#pragma once

#include <Monarc/Core/Math/Mat.h>
#include <Monarc/Core/Math/Vec.h>
#include <Monarc/Core/Types.h>

#include <algorithm>
#include <limits>

namespace Monarc {

// AABB and its free functions live in namespace Monarc for the same ADL reason as the rest
// of Math -- see Vec.h.
//
// Passed by const&, like Mat3/Mat4 and Transform: two Vec3s together are larger than one
// SIMD register, so a reference is cheaper than a copy.

/// An axis-aligned bounding box, stored as its two extreme corners.
struct AABB {
    Vec3 min;
    Vec3 max;

    /// An inverted box: min holds the largest representable value, max the smallest, so
    /// that expanding an Empty() box by a single point produces a zero-size box at exactly
    /// that point, rather than needing a special case for the first Expand. Contains() on
    /// an Empty() box is false for every point -- including the origin, which a merely
    /// zero-initialized (but not Empty()) AABB would wrongly contain.
    [[nodiscard]] static constexpr AABB Empty() {
        constexpr f32 kMax    = std::numeric_limits<f32>::max();
        constexpr f32 kLowest = std::numeric_limits<f32>::lowest();
        return AABB{Vec3{kMax, kMax, kMax}, Vec3{kLowest, kLowest, kLowest}};
    }
};

/// Grows `box` to include `point`, leaving it unchanged if `point` is already inside.
[[nodiscard]] constexpr AABB Expand(const AABB& box, Vec3 point) {
    return AABB{
        Vec3{
            std::min(box.min.x, point.x),
            std::min(box.min.y, point.y),
            std::min(box.min.z, point.z),
        },
        Vec3{
            std::max(box.max.x, point.x),
            std::max(box.max.y, point.y),
            std::max(box.max.z, point.z),
        },
    };
}

/// Grows `box` to include all of `other`.
///
/// Computed as an independent componentwise min/max against `other`'s own min and max,
/// rather than as two calls to the point-taking Expand above with `other.min` and
/// `other.max` -- that shortcut looks equivalent but breaks when `other` is itself Empty():
/// its min/max are the most extreme values a component can hold, and folding them in one
/// at a time would blow `box` open to them instead of correctly leaving it unchanged.
[[nodiscard]] constexpr AABB Expand(const AABB& box, const AABB& other) {
    return AABB{
        Vec3{
            std::min(box.min.x, other.min.x),
            std::min(box.min.y, other.min.y),
            std::min(box.min.z, other.min.z),
        },
        Vec3{
            std::max(box.max.x, other.max.x),
            std::max(box.max.y, other.max.y),
            std::max(box.max.z, other.max.z),
        },
    };
}

/// True if `point` lies within `box`, inclusive of the faces. Always false for an Empty()
/// box: its inverted min/max make every one of the six comparisons below fail at once.
[[nodiscard]] constexpr bool Contains(const AABB& box, Vec3 point) {
    return point.x >= box.min.x && point.x <= box.max.x && point.y >= box.min.y &&
           point.y <= box.max.y && point.z >= box.min.z && point.z <= box.max.z;
}

/// True if `a` and `b` share at least one point, by the separating-axis test: two boxes
/// fail to overlap if and only if they are separated along some single axis. False
/// whenever either box is Empty(), for the same reason Contains() is: an inverted min/max
/// cannot satisfy the comparisons below against any real box.
[[nodiscard]] constexpr bool Intersects(const AABB& a, const AABB& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y &&
           a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

/// The box's midpoint.
[[nodiscard]] constexpr Vec3 Center(const AABB& box) { return (box.min + box.max) * 0.5f; }

/// Half the box's size along each axis: the distance from Center() to either face, not the
/// full width/height/depth.
[[nodiscard]] constexpr Vec3 Extents(const AABB& box) { return (box.max - box.min) * 0.5f; }

/// Transforms `box` by `m`, re-fitting around all eight transformed corners rather than
/// just the transformed min and max. Transforming only those two produces a wrong,
/// too-small box under any rotation, because the corners that end up extreme afterward are
/// not, in general, the images of the corners that were extreme before it.
[[nodiscard]] constexpr AABB Transformed(const AABB& box, const Mat4& m) {
    const Vec3 corners[8] = {
        Vec3{box.min.x, box.min.y, box.min.z}, Vec3{box.max.x, box.min.y, box.min.z},
        Vec3{box.min.x, box.max.y, box.min.z}, Vec3{box.max.x, box.max.y, box.min.z},
        Vec3{box.min.x, box.min.y, box.max.z}, Vec3{box.max.x, box.min.y, box.max.z},
        Vec3{box.min.x, box.max.y, box.max.z}, Vec3{box.max.x, box.max.y, box.max.z},
    };

    AABB result = AABB::Empty();
    for (const Vec3& corner : corners) {
        result = Expand(result, TransformPoint(m, corner));
    }
    return result;
}

}  // namespace Monarc
