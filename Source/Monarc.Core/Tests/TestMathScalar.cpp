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
