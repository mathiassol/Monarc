#include <doctest/doctest.h>

#include <Monarc/Core/Math/Bounds.h>
#include <Monarc/Core/Math/Transform.h>

using Monarc::f32;
using Monarc::Mat4;
using Monarc::Quat;
using Monarc::Transform;
using Monarc::Vec3;
using Monarc::Math::ApproxEqual;
using Monarc::Math::kPi;

namespace {
bool Approx(const Vec3& a, const Vec3& b, f32 tol = 1e-4f) {
    return ApproxEqual(a.x, b.x, tol) && ApproxEqual(a.y, b.y, tol) &&
           ApproxEqual(a.z, b.z, tol);
}
}  // namespace

TEST_CASE("a default Transform is the identity") {
    const Transform t;
    CHECK(Approx(TransformPoint(ToMat4(t), Vec3{1.0f, 2.0f, 3.0f}), Vec3{1.0f, 2.0f, 3.0f}));
}

TEST_CASE("a Transform applies scale, then rotation, then translation") {
    // The order is the whole point: scaling after rotating shears a non-uniformly scaled
    // object, which is the classic scene-graph bug.
    Transform t;
    t.translation = Vec3{10.0f, 0.0f, 0.0f};
    t.rotation    = Quat::FromAxisAngle(Vec3::UnitZ(), kPi * 0.5f);
    t.scale       = Vec3{2.0f, 2.0f, 2.0f};

    // (1,0,0) -> scale -> (2,0,0) -> rotate -> (0,2,0) -> translate -> (10,2,0)
    CHECK(Approx(TransformPoint(ToMat4(t), Vec3{1.0f, 0.0f, 0.0f}), Vec3{10.0f, 2.0f, 0.0f}));
}

TEST_CASE("composing transforms matches composing their matrices") {
    Transform parent;
    parent.translation = Vec3{1.0f, 2.0f, 3.0f};
    parent.rotation    = Quat::FromAxisAngle(Vec3::UnitY(), 0.4f);
    parent.scale       = Vec3{2.0f, 2.0f, 2.0f};

    Transform child;
    child.translation = Vec3{0.0f, 1.0f, 0.0f};
    child.rotation    = Quat::FromAxisAngle(Vec3::UnitZ(), 0.9f);

    const Vec3 v{1.0f, 0.5f, -2.0f};
    CHECK(Approx(TransformPoint(ToMat4(parent * child), v),
                 TransformPoint(ToMat4(parent) * ToMat4(child), v)));
}

TEST_CASE("Inverse undoes a Transform") {
    Transform t;
    t.translation = Vec3{4.0f, -1.0f, 2.0f};
    t.rotation    = Quat::FromAxisAngle(Normalize(Vec3{1.0f, 1.0f, 0.0f}), 0.8f);
    t.scale       = Vec3{2.0f, 2.0f, 2.0f};

    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(Approx(TransformPoint(ToMat4(Inverse(t)), TransformPoint(ToMat4(t), v)), v));
}

TEST_CASE("an AABB from points contains them all") {
    const Vec3 points[] = {Vec3{1.0f, 2.0f, 3.0f}, Vec3{-4.0f, 0.0f, 5.0f},
                           Vec3{0.0f, -6.0f, 1.0f}};
    Monarc::AABB box = Monarc::AABB::Empty();
    for (const Vec3& p : points) { box = Expand(box, p); }

    for (const Vec3& p : points) { CHECK(Contains(box, p)); }
    CHECK(Approx(box.min, Vec3{-4.0f, -6.0f, 1.0f}));
    CHECK(Approx(box.max, Vec3{1.0f, 2.0f, 5.0f}));
}

TEST_CASE("an empty AABB contains nothing and expands correctly") {
    const Monarc::AABB empty = Monarc::AABB::Empty();
    CHECK_FALSE(Contains(empty, Vec3::Zero()));
    const Monarc::AABB one = Expand(empty, Vec3{1.0f, 1.0f, 1.0f});
    CHECK(Contains(one, Vec3{1.0f, 1.0f, 1.0f}));
}

TEST_CASE("AABB centre and extents describe the box") {
    const Monarc::AABB box{Vec3{-1.0f, -2.0f, -3.0f}, Vec3{1.0f, 2.0f, 3.0f}};
    CHECK(Approx(Center(box), Vec3::Zero()));
    CHECK(Approx(Extents(box), Vec3{1.0f, 2.0f, 3.0f}));
}

TEST_CASE("AABBs report overlap correctly") {
    const Monarc::AABB a{Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f}};
    const Monarc::AABB b{Vec3{1.0f, 1.0f, 1.0f}, Vec3{3.0f, 3.0f, 3.0f}};
    const Monarc::AABB c{Vec3{5.0f, 5.0f, 5.0f}, Vec3{6.0f, 6.0f, 6.0f}};
    CHECK(Intersects(a, b));
    CHECK_FALSE(Intersects(a, c));
    CHECK(Intersects(a, a));
}

TEST_CASE("a transformed AABB still contains the transformed corners") {
    // Transforming a box means transforming all eight corners and re-fitting -- not
    // transforming min and max, which produces a wrong box under rotation.
    const Monarc::AABB box{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
    const Mat4 m = Mat4::Translation(Vec3{5.0f, 0.0f, 0.0f}) * Mat4::RotationZ(kPi * 0.25f);
    const Monarc::AABB moved = Transformed(box, m);

    for (f32 x : {-1.0f, 1.0f}) {
        for (f32 y : {-1.0f, 1.0f}) {
            for (f32 z : {-1.0f, 1.0f}) {
                CHECK(Contains(moved, TransformPoint(m, Vec3{x, y, z})));
            }
        }
    }
}

TEST_CASE("an empty AABB stays empty and finite under transformation") {
    // Without a guard, Transformed feeds the sentinel corners of an empty box through the
    // matrix, and multiplying the largest finite floats overflows to infinity. Expanding a
    // parent by that result silently poisons the parent -- and a scene node with no
    // geometry yet is exactly what produces an empty box in the first place.
    const Monarc::AABB empty = Monarc::AABB::Empty();
    const Mat4 m = Mat4::Translation(Vec3{5.0f, 0.0f, 0.0f}) * Mat4::RotationZ(0.3f);
    const Monarc::AABB moved = Transformed(empty, m);

    CHECK(IsEmpty(moved));
    CHECK_FALSE(Contains(moved, Vec3::Zero()));

    // And it must not contaminate a real box it is folded into.
    const Monarc::AABB real{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
    const Monarc::AABB merged = Expand(real, moved);
    CHECK(Approx(merged.min, real.min));
    CHECK(Approx(merged.max, real.max));
}

TEST_CASE("an empty AABB reports zero extents rather than infinity") {
    CHECK(Approx(Extents(Monarc::AABB::Empty()), Vec3::Zero()));
    CHECK(IsEmpty(Monarc::AABB::Empty()));
    CHECK_FALSE(IsEmpty(Monarc::AABB{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}}));
}
