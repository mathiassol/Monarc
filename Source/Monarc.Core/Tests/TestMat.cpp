#include <doctest/doctest.h>

#include <Monarc/Core/Math/Mat.h>

using Monarc::f32;
using Monarc::Mat4;
using Monarc::Vec3;
using Monarc::Vec4;
using Monarc::Math::ApproxEqual;
using Monarc::Math::kPi;

namespace {

bool Approx(const Vec3& a, const Vec3& b, f32 tol = 1e-4f) {
    return ApproxEqual(a.x, b.x, tol) && ApproxEqual(a.y, b.y, tol) &&
           ApproxEqual(a.z, b.z, tol);
}

bool Approx(const Mat4& a, const Mat4& b, f32 tol = 1e-4f) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!ApproxEqual(a.columns[c][r], b.columns[c][r], tol)) { return false; }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("identity leaves points and matrices alone") {
    const Mat4 i = Mat4::Identity();
    CHECK(Approx(TransformPoint(i, Vec3{1.0f, 2.0f, 3.0f}), Vec3{1.0f, 2.0f, 3.0f}));
    CHECK(Approx(i * i, i));
}

TEST_CASE("translation lives in the fourth column") {
    // ADR-0015: column-major storage, column vectors. If translation ever appears in the
    // fourth *row*, the storage convention has silently flipped.
    const Mat4 t = Mat4::Translation(Vec3{4.0f, 5.0f, 6.0f});
    CHECK(ApproxEqual(t.columns[3][0], 4.0f));
    CHECK(ApproxEqual(t.columns[3][1], 5.0f));
    CHECK(ApproxEqual(t.columns[3][2], 6.0f));
    CHECK(ApproxEqual(t.columns[3][3], 1.0f));
}

TEST_CASE("points are translated and directions are not") {
    const Mat4 t = Mat4::Translation(Vec3{4.0f, 5.0f, 6.0f});
    CHECK(Approx(TransformPoint(t, Vec3{1.0f, 1.0f, 1.0f}), Vec3{5.0f, 6.0f, 7.0f}));
    CHECK(Approx(TransformDirection(t, Vec3{1.0f, 0.0f, 0.0f}), Vec3{1.0f, 0.0f, 0.0f}));
}

TEST_CASE("scale multiplies each axis") {
    const Mat4 s = Mat4::Scale(Vec3{2.0f, 3.0f, 4.0f});
    CHECK(Approx(TransformPoint(s, Vec3{1.0f, 1.0f, 1.0f}), Vec3{2.0f, 3.0f, 4.0f}));
}

TEST_CASE("rotation about Z by 90 degrees sends +X to +Y") {
    // Right-handed: a positive rotation about +Z turns +X toward +Y.
    const Mat4 r = Mat4::RotationZ(kPi * 0.5f);
    CHECK(Approx(TransformPoint(r, Vec3{1.0f, 0.0f, 0.0f}), Vec3{0.0f, 1.0f, 0.0f}));
}

TEST_CASE("A * B applies B first") {
    // ADR-0015's composition order. Translate-then-rotate must differ from
    // rotate-then-translate, and in a stated direction -- this is the test that catches a
    // silently reversed multiply.
    const Mat4 t = Mat4::Translation(Vec3{1.0f, 0.0f, 0.0f});
    const Mat4 r = Mat4::RotationZ(kPi * 0.5f);

    // r * t: translate first, then rotate. (1,0,0) -> (2,0,0) -> (0,2,0)
    CHECK(Approx(TransformPoint(r * t, Vec3{1.0f, 0.0f, 0.0f}), Vec3{0.0f, 2.0f, 0.0f}));

    // t * r: rotate first, then translate. (1,0,0) -> (0,1,0) -> (1,1,0)
    CHECK(Approx(TransformPoint(t * r, Vec3{1.0f, 0.0f, 0.0f}), Vec3{1.0f, 1.0f, 0.0f}));
}

TEST_CASE("Transpose swaps rows and columns and is its own inverse") {
    const Mat4 m = Mat4::Translation(Vec3{1.0f, 2.0f, 3.0f}) * Mat4::Scale(Vec3{2.0f, 3.0f, 4.0f});
    CHECK(ApproxEqual(Transpose(m).columns[0][3], m.columns[3][0]));
    CHECK(Approx(Transpose(Transpose(m)), m));
}

TEST_CASE("Inverse undoes a rigid transform") {
    const Mat4 m   = Mat4::Translation(Vec3{3.0f, -2.0f, 5.0f}) * Mat4::RotationZ(0.7f);
    const Mat4 inv = Inverse(m);
    CHECK(Approx(m * inv, Mat4::Identity()));
    CHECK(Approx(TransformPoint(inv, TransformPoint(m, Vec3{1.0f, 2.0f, 3.0f})),
                 Vec3{1.0f, 2.0f, 3.0f}));
}

TEST_CASE("Inverse undoes a transform with non-uniform scale too") {
    const Mat4 m   = Mat4::Translation(Vec3{1.0f, 2.0f, 3.0f}) *
                     Mat4::RotationZ(0.3f) * Mat4::Scale(Vec3{2.0f, 4.0f, 0.5f});
    CHECK(Approx(m * Inverse(m), Mat4::Identity()));
}

TEST_CASE("perspective maps near to 0 and far to 1") {
    // ADR-0015: depth range is 0..1, which all three target APIs use. A -1 here means the
    // OpenGL convention has crept in, and everything will z-fight or vanish.
    const f32  nearZ = 0.1f;
    const f32  farZ  = 100.0f;
    const Mat4 p     = Mat4::Perspective(kPi * 0.25f, 16.0f / 9.0f, nearZ, farZ);

    // A point on the near plane, in view space, looking down -Z.
    const Vec4 atNear = p * Vec4{0.0f, 0.0f, -nearZ, 1.0f};
    CHECK(ApproxEqual(atNear.z / atNear.w, 0.0f, 1e-3f));

    const Vec4 atFar = p * Vec4{0.0f, 0.0f, -farZ, 1.0f};
    CHECK(ApproxEqual(atFar.z / atFar.w, 1.0f, 1e-3f));
}

TEST_CASE("orthographic also maps near to 0 and far to 1") {
    const Mat4 o = Mat4::Orthographic(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f);
    const Vec4 atNear = o * Vec4{0.0f, 0.0f, -0.1f, 1.0f};
    const Vec4 atFar  = o * Vec4{0.0f, 0.0f, -100.0f, 1.0f};
    CHECK(ApproxEqual(atNear.z / atNear.w, 0.0f, 1e-3f));
    CHECK(ApproxEqual(atFar.z / atFar.w, 1.0f, 1e-3f));
}

TEST_CASE("LookAt puts the target down -Z in view space") {
    // ADR-0015: -Z is forward. A camera at the origin looking at (0,0,-5) must see it
    // straight ahead, at negative Z in view space.
    const Mat4 view = Mat4::LookAt(Vec3::Zero(), Vec3{0.0f, 0.0f, -5.0f}, Vec3::UnitY());
    const Vec3 seen = TransformPoint(view, Vec3{0.0f, 0.0f, -5.0f});
    CHECK(ApproxEqual(seen.x, 0.0f, 1e-4f));
    CHECK(ApproxEqual(seen.y, 0.0f, 1e-4f));
    CHECK(seen.z < 0.0f);
    CHECK(ApproxEqual(seen.z, -5.0f, 1e-4f));
}

TEST_CASE("LookAt places the eye at the view-space origin") {
    const Vec3 eye{3.0f, 4.0f, 5.0f};
    const Mat4 view = Mat4::LookAt(eye, Vec3::Zero(), Vec3::UnitY());
    CHECK(Approx(TransformPoint(view, eye), Vec3::Zero()));
}

TEST_CASE("Mat4 is 16-byte aligned for later SIMD") {
    static_assert(sizeof(Mat4) == 64);
    static_assert(alignof(Mat4) == 16);
    CHECK(true);
}

TEST_CASE("a small but well-conditioned matrix still inverts") {
    // A determinant is a cubed length, so a plain uniform scale of 0.01 has det = 1e-6 --
    // below a tolerance tuned for unit-magnitude comparisons. Testing the threshold against
    // kEpsilon declares this perfectly invertible matrix singular and silently returns
    // identity: a wrong answer with no diagnostic, which is the worst kind.
    const Mat4 m   = Mat4::Scale(Vec3{0.01f, 0.01f, 0.01f});
    const Mat4 inv = Inverse(m);
    CHECK(Approx(m * inv, Mat4::Identity()));
    CHECK(Approx(TransformPoint(inv, Vec3{1.0f, 2.0f, 3.0f}),
                 Vec3{100.0f, 200.0f, 300.0f}, 1e-2f));
}

TEST_CASE("a short but nonzero vector still normalizes to a unit direction") {
    // Same class of error on the vector side: a length floor tuned for unit magnitudes
    // discards the direction of a legitimately small vector.
    const Vec3 tiny = Normalize(Vec3{1e-5f, 0.0f, 0.0f});
    CHECK(ApproxEqual(tiny.x, 1.0f, 1e-4f));
    CHECK(ApproxEqual(Monarc::Length(tiny), 1.0f, 1e-4f));
}
