#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>

#include <string>

namespace {

struct CapturedAssert {
    bool        fired = false;
    std::string expression;
    std::string message;
    int         line = 0;
};

CapturedAssert g_captured;

bool CaptureHandler(const char* expr, const char* /*file*/, int line, const char* message) {
    g_captured.fired      = true;
    g_captured.expression = expr;
    g_captured.message    = message ? message : "";
    g_captured.line       = line;
    return false;   // false: do not break into the debugger
}

// Installs the capturing handler and restores the previous one on scope exit.
struct ScopedCapture {
    Monarc::AssertHandler previous;
    ScopedCapture() : previous(Monarc::SetAssertHandler(&CaptureHandler)) {
        g_captured = CapturedAssert{};
    }
    ~ScopedCapture() { Monarc::SetAssertHandler(previous); }
};

}  // namespace

TEST_CASE("MONARC_CHECK does nothing when the condition holds") {
    ScopedCapture capture;
    MONARC_CHECK(1 + 1 == 2, "arithmetic still works");
    CHECK_FALSE(g_captured.fired);
}

TEST_CASE("MONARC_CHECK reports expression and message when it fails") {
    ScopedCapture capture;
    MONARC_CHECK(1 + 1 == 3, "arithmetic broke");
    REQUIRE(g_captured.fired);
    CHECK(g_captured.expression == "1 + 1 == 3");
    CHECK(g_captured.message == "arithmetic broke");
    CHECK(g_captured.line > 0);
}

TEST_CASE("MONARC_CHECK evaluates its condition exactly once") {
    ScopedCapture capture;
    int calls = 0;
    auto bump = [&calls] { ++calls; return true; };
    MONARC_CHECK(bump(), "should evaluate once");
    CHECK(calls == 1);
}

TEST_CASE("SetAssertHandler returns the handler it replaced") {
    Monarc::AssertHandler first  = Monarc::SetAssertHandler(&CaptureHandler);
    Monarc::AssertHandler second = Monarc::SetAssertHandler(first);
    CHECK(second == &CaptureHandler);
}

TEST_CASE("MONARC_ASSERT keeps its expression type-checked when disabled") {
    int unusedOutsideAssert = 42;
    MONARC_ASSERT(unusedOutsideAssert == 42, "type-checked either way");
    CHECK(true);
}

TEST_CASE("a disabled MONARC_ASSERT still counts both operands as used") {
    // Both operands come from locals referenced nowhere else. With MONARC_ENABLE_ASSERTS=0
    // the macro expands to unevaluated sizeof, which must still mark them used -- otherwise
    // this fails /WX in Release only, while Debug stays green.
    const int   expected = 42;
    const char* reason   = "message built from a local, not a literal";
    MONARC_ASSERT(expected == 42, reason);
    CHECK(true);
}
