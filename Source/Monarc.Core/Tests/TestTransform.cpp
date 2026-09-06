#include <doctest/doctest.h>

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
