#include <doctest/doctest.h>

#include <Monarc/Core/Math/Vec.h>

using Monarc::f32;
using Monarc::Vec2;
using Monarc::Vec3;
using Monarc::Vec4;
using Monarc::Math::ApproxEqual;

TEST_CASE("vectors construct and compare componentwise") {
    CHECK(Vec3{1.0f, 2.0f, 3.0f}.x == 1.0f);
    CHECK(Vec3{1.0f, 2.0f, 3.0f} == Vec3{1.0f, 2.0f, 3.0f});
    CHECK(Vec3{1.0f, 2.0f, 3.0f} != Vec3{1.0f, 2.0f, 4.0f});
    CHECK(Vec3{2.0f}.y == 2.0f);   // broadcast
}

TEST_CASE("arithmetic is componentwise") {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};
    CHECK(a + b == Vec3{5.0f, 7.0f, 9.0f});
    CHECK(b - a == Vec3{3.0f, 3.0f, 3.0f});
    CHECK(a * 2.0f == Vec3{2.0f, 4.0f, 6.0f});
    CHECK(2.0f * a == Vec3{2.0f, 4.0f, 6.0f});
    CHECK(a * b == Vec3{4.0f, 10.0f, 18.0f});
    CHECK(-a == Vec3{-1.0f, -2.0f, -3.0f});
}

TEST_CASE("compound assignment matches the binary operators") {
    Vec3 v{1.0f, 2.0f, 3.0f};
    v += Vec3{1.0f, 1.0f, 1.0f};
    CHECK(v == Vec3{2.0f, 3.0f, 4.0f});
    v *= 2.0f;
    CHECK(v == Vec3{4.0f, 6.0f, 8.0f});
}

TEST_CASE("Dot and Length agree with each other") {
    CHECK(ApproxEqual(Dot(Vec3{1.0f, 2.0f, 3.0f}, Vec3{4.0f, 5.0f, 6.0f}), 32.0f));
    CHECK(ApproxEqual(Length(Vec3{3.0f, 4.0f, 0.0f}), 5.0f));
    CHECK(ApproxEqual(LengthSquared(Vec3{3.0f, 4.0f, 0.0f}), 25.0f));
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(ApproxEqual(Dot(v, v), LengthSquared(v)));
}

TEST_CASE("Normalize produces a unit vector and leaves direction intact") {
    const Vec3 n = Normalize(Vec3{0.0f, 3.0f, 0.0f});
    CHECK(ApproxEqual(Length(n), 1.0f));
    CHECK(n == Vec3{0.0f, 1.0f, 0.0f});
}

TEST_CASE("the basis is right-handed: X cross Y is Z") {
    // ADR-0015. If this ever fails, something has flipped handedness and every normal,
    // every winding order and every camera in the engine is now wrong.
    CHECK(Cross(Vec3::UnitX(), Vec3::UnitY()) == Vec3::UnitZ());
    CHECK(Cross(Vec3::UnitY(), Vec3::UnitZ()) == Vec3::UnitX());
    CHECK(Cross(Vec3::UnitZ(), Vec3::UnitX()) == Vec3::UnitY());
}

TEST_CASE("Cross is antisymmetric and orthogonal to both inputs") {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};
    const Vec3 c = Cross(a, b);
    CHECK(ApproxEqual(Dot(c, a), 0.0f));
    CHECK(ApproxEqual(Dot(c, b), 0.0f));
    CHECK(Cross(b, a) == -c);
}

TEST_CASE("Vec2 and Vec4 support the same vocabulary") {
    CHECK(ApproxEqual(Length(Vec2{3.0f, 4.0f}), 5.0f));
    CHECK(Vec2{1.0f, 2.0f} + Vec2{1.0f, 1.0f} == Vec2{2.0f, 3.0f});
    CHECK(ApproxEqual(Dot(Vec4{1.0f, 0.0f, 0.0f, 0.0f}, Vec4{1.0f, 2.0f, 3.0f, 4.0f}), 1.0f));
}

TEST_CASE("Vec3 converts to and from Vec4 explicitly") {
    CHECK(Vec4{Vec3{1.0f, 2.0f, 3.0f}, 1.0f} == Vec4{1.0f, 2.0f, 3.0f, 1.0f});
    CHECK(Vec4{1.0f, 2.0f, 3.0f, 1.0f}.XYZ() == Vec3{1.0f, 2.0f, 3.0f});
}

TEST_CASE("vectors are usable in constant expressions") {
    static_assert(Vec3{1.0f, 2.0f, 3.0f}.x == 1.0f);
    static_assert(Dot(Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}) == 1.0f);
    CHECK(true);
}

TEST_CASE("Vec4 is aligned so SIMD can be added without changing its layout") {
    // ADR-0015 defers SIMD but requires the layout not to change when it arrives.
    static_assert(sizeof(Vec4) == 16);
    static_assert(alignof(Vec4) == 16);
    static_assert(sizeof(Vec3) == 12);
    CHECK(true);
}
