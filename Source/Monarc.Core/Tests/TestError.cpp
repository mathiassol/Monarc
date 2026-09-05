#include <doctest/doctest.h>

#include <Monarc/Core/Error.h>

#include <string_view>

namespace {

Monarc::Result<int> Divide(int numerator, int denominator) {
    if (denominator == 0) {
        return Monarc::Err(Monarc::ErrorCode::InvalidArgument, "division by zero");
    }
    return numerator / denominator;
}

Monarc::Status Validate(int value) {
    if (value < 0) {
        return Monarc::Err(Monarc::ErrorCode::InvalidArgument, "value must be non-negative");
    }
    return {};
}

}  // namespace

TEST_CASE("Result carries a value on success") {
    auto result = Divide(10, 2);
    REQUIRE(result.has_value());
    CHECK(*result == 5);
}

TEST_CASE("Result carries code and message on failure") {
    auto result = Divide(10, 0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(result.error().message == "division by zero");
}

TEST_CASE("Status expresses success with no value") {
    CHECK(Validate(3).has_value());
    auto bad = Validate(-1);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == Monarc::ErrorCode::InvalidArgument);
}

TEST_CASE("ToString names every error code") {
    using Monarc::ErrorCode;
    CHECK(std::string_view(Monarc::ToString(ErrorCode::Unknown)) == "Unknown");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::InvalidArgument)) == "InvalidArgument");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::OutOfMemory)) == "OutOfMemory");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::NotFound)) == "NotFound");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::AlreadyExists)) == "AlreadyExists");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::PermissionDenied)) == "PermissionDenied");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::IoFailure)) == "IoFailure");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::Unsupported)) == "Unsupported");
}

TEST_CASE("errors propagate through and_then without losing detail") {
    auto chained = Divide(10, 0).and_then(
        [](int v) -> Monarc::Result<int> { return v * 2; });
    REQUIRE_FALSE(chained.has_value());
    CHECK(chained.error().message == "division by zero");
}
