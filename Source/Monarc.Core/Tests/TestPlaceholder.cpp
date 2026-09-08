// doctest's implementation used to live here together with its `main`. A3 Task 5 moved both to
// TestDeathGuards.cpp, which needs a `main` of its own: `--monarc-death-guard=<name>` has to be
// handled *before* doctest starts, because doctest's SEH filter would otherwise catch the
// debug break a fatal guard raises and report it as a failed assertion instead of letting the
// process die. Exactly one translation unit may define the implementation, and the file that
// owns `main` is the natural one.
#include <doctest/doctest.h>

TEST_CASE("the test harness runs") {
    CHECK(2 + 2 == 4);
}
