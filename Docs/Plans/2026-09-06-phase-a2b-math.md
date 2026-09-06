# Phase A2b — Math

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vectors, matrices, quaternions, transforms and bounds — enough for a camera, a projection and a scene graph, with the conventions of [ADR-0015](../Architecture/Decisions/ADR-0015-math-conventions.md) locked in by tests rather than by comment.

**Architecture:** Header-only and `constexpr` where possible; scalar implementations behind types aligned so SIMD can be added later without changing layout or API. No allocation anywhere — none of these types own memory, so none of them take an `IAllocator`.

**Tech Stack:** C++23, MSVC + Clang (`clang-cl`), CMake + Ninja, doctest.

**Plan style:** as with A2a, this specifies **headers and complete tests, not implementation bodies**. Implementers derive the bodies. See A2a's plan for why.

---

## The conventions this phase makes real

Every one of these is from [ADR-0015](../Architecture/Decisions/ADR-0015-math-conventions.md),
and every one has a test whose whole purpose is to fail if it is ever quietly changed:

| Convention | The test that locks it |
|---|---|
| Right-handed, +Y up, −Z forward | `Cross(UnitX, UnitY) == UnitZ`; `LookAt` looks down −Z |
| Column vectors, `M * v` | Transforming a point applies translation from `columns[3]` |
| `A * B` applies **B first** | Translate-then-rotate differs from rotate-then-translate, in a stated direction |
| Column-major storage | `columns[3].xyz` is the translation |
| Depth 0..1 | `Perspective` maps near to 0 and far to 1 — **not** −1 |

A convention that only lives in a comment drifts. These make drift a build failure.

---

## File Structure

```
Source/Monarc.Core/Include/Monarc/Core/Math/
  Scalar.h        constants, radians/degrees, ApproxEqual, Clamp, Lerp
  Vec.h           Vec2, Vec3, Vec4
  Mat.h           Mat3, Mat4, projections, LookAt
  Quat.h          Quat
  Transform.h     Transform (translation, rotation, scale)
  Bounds.h        AABB
Source/Monarc.Core/Tests/
  TestMathScalar.cpp  TestVec.cpp  TestMat.cpp  TestQuat.cpp  TestTransform.cpp
```

Header-only: these are small, `constexpr`-heavy, and used everywhere, so out-of-line
definitions would cost more in call overhead than they save in compile time.

---

## Task 1: `Scalar` and vectors

**Files:** create `Math/Scalar.h`, `Math/Vec.h`, `Tests/TestMathScalar.cpp`, `Tests/TestVec.cpp`

- [ ] **Step 1: Write the failing tests**

`Tests/TestMathScalar.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Math/Scalar.h>

using Monarc::f32;

TEST_CASE("degrees and radians round-trip") {
    using namespace Monarc::Math;
    CHECK(ApproxEqual(Radians(180.0f), kPi));
    CHECK(ApproxEqual(Degrees(kPi), 180.0f));
    CHECK(ApproxEqual(Degrees(Radians(37.5f)), 37.5f));
}

TEST_CASE("ApproxEqual tolerates small error and rejects large") {
    using namespace Monarc::Math;
    CHECK(ApproxEqual(1.0f, 1.0f + 1e-7f));
    CHECK_FALSE(ApproxEqual(1.0f, 1.01f));
    CHECK(ApproxEqual(0.0f, 0.0f));
}

TEST_CASE("Clamp bounds in both directions and passes through the middle") {
    using namespace Monarc::Math;
    CHECK(Clamp(5.0f, 0.0f, 10.0f) == 5.0f);
    CHECK(Clamp(-1.0f, 0.0f, 10.0f) == 0.0f);
    CHECK(Clamp(11.0f, 0.0f, 10.0f) == 10.0f);
}

TEST_CASE("Lerp hits both endpoints exactly") {
    using namespace Monarc::Math;
    CHECK(Lerp(2.0f, 6.0f, 0.0f) == 2.0f);
    CHECK(Lerp(2.0f, 6.0f, 1.0f) == 6.0f);
    CHECK(ApproxEqual(Lerp(2.0f, 6.0f, 0.5f), 4.0f));
}

TEST_CASE("the scalar helpers are usable in a constant expression") {
    using namespace Monarc::Math;
    static_assert(Clamp(5.0f, 0.0f, 10.0f) == 5.0f);
    static_assert(Lerp(0.0f, 10.0f, 0.5f) == 5.0f);
    CHECK(true);
}
```

`Tests/TestVec.cpp`:

```cpp
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
```

- [ ] **Step 2: Build and confirm both files fail for the right reason**

- [ ] **Step 3: Write `Math/Scalar.h`**

Provide, in `namespace Monarc::Math`: `kPi`, `kTwoPi`, `kHalfPi`, `kEpsilon`;
`constexpr f32 Radians(f32 degrees)`, `Degrees(f32 radians)`, `Clamp(v, lo, hi)`,
`Lerp(a, b, t)`; and `ApproxEqual(a, b, tolerance = kEpsilon)`. Templated on the scalar type
where it costs nothing. `Clamp` and `Lerp` must be `constexpr`.

- [ ] **Step 4: Write `Math/Vec.h`**

`Vec2`, `Vec3`, `Vec4` in `namespace Monarc`, each an aggregate with public `x`, `y`, `z`,
`w` as appropriate — no accessor ceremony, these are values.

Required surface per type: a broadcast constructor from one scalar; `operator==`/`!=`
(exact, componentwise); `+`, `-`, `*` (by scalar both ways, and componentwise), unary `-`;
`+=`, `-=`, `*=`; free functions `Dot`, `Length`, `LengthSquared`, `Normalize`; `Cross` for
`Vec3` only; static `Zero()`, `One()`, `UnitX()`, `UnitY()`, `UnitZ()` (the last for `Vec3`
and `Vec4`). `Vec4` needs a `Vec3 + w` constructor and `XYZ()`.

Everything `constexpr` that can be — `Length` and `Normalize` cannot be, since `sqrt` is not
`constexpr` before C++26.

`Vec4` must be 16-byte aligned; `Vec3` must stay 12 bytes (do **not** pad it — arrays of
`Vec3` are vertex data and must match the GPU layout).

- [ ] **Step 5: Build, test, verify on all five presets, commit**

```bash
git add Source/Monarc.Core
git commit -m "core: scalar helpers and Vec2/Vec3/Vec4"
```

---

## Task 2: Matrices

**Files:** create `Math/Mat.h`, `Tests/TestMat.cpp`

- [ ] **Step 1: Write the failing test**

`Tests/TestMat.cpp`:

```cpp
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
```

- [ ] **Step 2: Build and confirm it fails**

- [ ] **Step 3: Write `Math/Mat.h`**

`Mat4` stores `Vec4 columns[4]`, so `columns[3]` is the translation. `Mat3` likewise stores
`Vec3 columns[3]` and exists for normal matrices and rotation-only work.

Required: `Identity()`, `Translation(Vec3)`, `Scale(Vec3)`, `RotationX/Y/Z(f32 radians)`,
`Perspective(fovYRadians, aspect, near, far)`, `Orthographic(l, r, b, t, near, far)`,
`LookAt(eye, target, up)`; `operator*` for `Mat4 * Mat4` and `Mat4 * Vec4`; free
`TransformPoint(Mat4, Vec3)` (w = 1), `TransformDirection(Mat4, Vec3)` (w = 0),
`Transpose`, `Inverse`.

`Inverse` must handle a general affine matrix, not just rigid ones — one test uses
non-uniform scale.

**Both projections must produce 0..1 depth**, matching Vulkan, D3D12 and Metal.
Vulkan's inverted clip-space Y is *not* handled here — that belongs in the backend, per
ADR-0015.

- [ ] **Step 4: Build, test, verify on all five presets, commit**

```bash
git add Source/Monarc.Core
git commit -m "core: Mat3/Mat4 with 0..1-depth projections and LookAt"
```

---

## Task 3: Quaternions and transforms

**Files:** create `Math/Quat.h`, `Math/Transform.h`, `Tests/TestQuat.cpp`, `Tests/TestTransform.cpp`

- [ ] **Step 1: Write the failing tests**

`Tests/TestQuat.cpp`:

```cpp
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
```

`Tests/TestTransform.cpp`:

```cpp
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
```

- [ ] **Step 2: Build and confirm both fail**

- [ ] **Step 3: Write the headers**

`Quat` stores `x, y, z, w` with `w` the scalar part, 16-byte aligned. Required:
`Identity()`, `FromAxisAngle(axis, radians)`, `operator*` (quaternion composition, matching
matrix order: the right operand applies first), `Rotate(Quat, Vec3)`, `Conjugate`,
`Normalize`, `Length`, `Dot`, `Slerp`, `ToMat4`, `FromMat4` if cheap.

**`Slerp` must take the shorter path** — negate one operand when the dot product is
negative. A test covers it, because without it a rotating object lurches.

`Transform` is a plain aggregate of `Vec3 translation`, `Quat rotation`, `Vec3 scale`,
defaulting to identity. Required: `ToMat4`, `operator*` (composition), `Inverse`.

`Inverse` may assume uniform scale and should say so in its doc comment — non-uniform scale
does not invert to a `Transform`, only to a `Mat4`. Do not silently produce a wrong answer;
assert on it.

- [ ] **Step 4: Build, test, verify on all five presets, commit**

```bash
git add Source/Monarc.Core
git commit -m "core: quaternions and Transform"
```

---

## Task 4: Bounds and close-out

**Files:** create `Math/Bounds.h`, add cases to `Tests/TestTransform.cpp`; modify `Docs/Status.md`

- [ ] **Step 1: Write the failing tests**

Append to `Tests/TestTransform.cpp`:

```cpp
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
```

- [ ] **Step 2: Write `Math/Bounds.h`**

`AABB` is an aggregate of `Vec3 min, max`. Required: `Empty()` (an inverted box, so the
first `Expand` sets both bounds), `Expand(AABB, Vec3)`, `Expand(AABB, AABB)`, `Contains`,
`Intersects`, `Center`, `Extents`, `Transformed(AABB, Mat4)`.

`Transformed` must transform all eight corners and re-fit. Transforming only `min` and `max`
gives a wrong box under any rotation.

- [ ] **Step 3: Clean verification on all five presets**

Including `clang-asan`. Record doctest totals.

- [ ] **Step 4: Update `Docs/Status.md`**

Mark A2b complete in the progress table and add an `### A2b delivered` subsection listing
what landed, the test totals, and a pointer to
[ADR-0015](../Architecture/Decisions/ADR-0015-math-conventions.md) as the record of the
conventions.

- [ ] **Step 5: Commit**

```bash
python Tools/check_doc_links.py Docs README.md
git add Source/Monarc.Core Docs/Status.md
git commit -m "core: AABB bounds; record Phase A2b complete"
```

---

## Definition of done

1. All five presets configure, build clean, and pass — including `clang-asan`.
2. Every build is warning-free (`/WX`).
3. The convention tests pass: right-handed cross products, translation in `columns[3]`,
   `A * B` applying B first, both projections mapping near to **0** and far to **1**, and
   `LookAt` looking down −Z.
4. `sizeof`/`alignof` static assertions hold, so SIMD can be added later without changing
   layout.
5. `Docs/Status.md` reflects reality.

## What A2b deliberately excludes

- **No SIMD.** Scalar implementations behind aligned types, so it can be added without an
  API or layout change. Deferred until there is profiling data.
- **No Euler angles.** They invite gimbal-lock bugs and ordering ambiguity. If an authoring
  boundary needs them, they belong there, converted once.
- **No `Mat2`, no swizzling, no colour types.** Add them when something needs them.
- **No frustum, sphere, ray, or plane types.** They arrive with culling, in the renderer.
- **No `f64` variants.** The types are templated only where it costs nothing; a double
  precision path arrives if and when large-world coordinates demand it.
