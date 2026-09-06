#include <doctest/doctest.h>

#include <Monarc/Core/Math/Quat.h>

using Monarc::f32;
using Monarc::Mat4;
using Monarc::Quat;
using Monarc::Vec3;
using Monarc::Math::ApproxEqual;
using Monarc::Math::kPi;

namespace {
bool Approx(const Vec3& a, const Vec3& b, f32 tol = 1e-4f) {
    return ApproxEqual(a.x, b.x, tol) && ApproxEqual(a.y, b.y, tol) &&
           ApproxEqual(a.z, b.z, tol);
}
}  // namespace

TEST_CASE("the identity quaternion rotates nothing") {
    CHECK(Approx(Rotate(Quat::Identity(), Vec3{1.0f, 2.0f, 3.0f}), Vec3{1.0f, 2.0f, 3.0f}));
}

TEST_CASE("a quaternion about Z by 90 degrees sends +X to +Y") {
    const Quat q = Quat::FromAxisAngle(Vec3::UnitZ(), kPi * 0.5f);
    CHECK(Approx(Rotate(q, Vec3::UnitX()), Vec3::UnitY()));
}

TEST_CASE("rotation preserves length") {
    const Quat q = Quat::FromAxisAngle(Normalize(Vec3{1.0f, 2.0f, 3.0f}), 1.1f);
    const Vec3 v{4.0f, -5.0f, 6.0f};
    CHECK(ApproxEqual(Length(Rotate(q, v)), Length(v), 1e-4f));
}

TEST_CASE("composition applies the right-hand operand first, matching matrices") {
    const Quat a = Quat::FromAxisAngle(Vec3::UnitZ(), kPi * 0.5f);
    const Quat b = Quat::FromAxisAngle(Vec3::UnitX(), kPi * 0.5f);
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(Approx(Rotate(a * b, v), Rotate(a, Rotate(b, v))));
}

TEST_CASE("Conjugate undoes a rotation") {
    const Quat q = Quat::FromAxisAngle(Normalize(Vec3{0.0f, 1.0f, 1.0f}), 0.9f);
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(Approx(Rotate(Conjugate(q), Rotate(q, v)), v));
}

TEST_CASE("a quaternion and its matrix agree") {
    const Quat q = Quat::FromAxisAngle(Normalize(Vec3{1.0f, 1.0f, 0.0f}), 0.6f);
    const Vec3 v{1.0f, -2.0f, 0.5f};
    CHECK(Approx(Rotate(q, v), TransformDirection(ToMat4(q), v)));
}

TEST_CASE("Normalize makes a quaternion unit length") {
    const Quat q = Normalize(Quat{1.0f, 2.0f, 3.0f, 4.0f});
    CHECK(ApproxEqual(Length(q), 1.0f));
}

TEST_CASE("Slerp reaches both endpoints and stays unit length") {
    const Quat a = Quat::Identity();
    const Quat b = Quat::FromAxisAngle(Vec3::UnitY(), kPi * 0.5f);
    CHECK(Approx(Rotate(Slerp(a, b, 0.0f), Vec3::UnitX()), Rotate(a, Vec3::UnitX())));
    CHECK(Approx(Rotate(Slerp(a, b, 1.0f), Vec3::UnitX()), Rotate(b, Vec3::UnitX())));
    CHECK(ApproxEqual(Length(Slerp(a, b, 0.37f)), 1.0f, 1e-4f));
}

TEST_CASE("Slerp takes the short way around") {
    // Without a sign fix on a negative dot product, this interpolates the long way and a
    // rotating object visibly lurches. Halfway between identity and a near-360 rotation
    // must stay near identity, not swing to the far side.
    const Quat a = Quat::Identity();
    const Quat b = Quat::FromAxisAngle(Vec3::UnitY(), kPi * 1.9f);
    const Vec3 mid = Rotate(Slerp(a, b, 0.5f), Vec3::UnitX());
    CHECK(Dot(mid, Vec3::UnitX()) > 0.0f);
}
