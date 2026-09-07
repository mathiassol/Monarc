// The one translation unit in Monarc.Host.Windowed.Tests that provides doctest's main().
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Monarc/Core/Error.h>
#include <Monarc/Host/Window.h>

#include <type_traits>

using Monarc::Host::Window;
using Monarc::Host::WindowDescription;

// Task 1's contract. Task 4 replaces the first case with real window creation.
TEST_CASE("creating a window reports Unsupported, and says why") {
    const Monarc::Result<Window> window = Window::Create(WindowDescription{});

    REQUIRE_FALSE(window.has_value());
    CHECK(window.error().code == Monarc::ErrorCode::Unsupported);
    CHECK_FALSE(window.error().message.empty());
}

TEST_CASE("a default-constructed window is not open") {
    const Window window;
    CHECK_FALSE(window.IsOpen());
}

TEST_CASE("destroying a window that was never opened is harmless, and repeatable") {
    Window window;
    window.Destroy();
    window.Destroy();
    CHECK_FALSE(window.IsOpen());
}

TEST_CASE("a window description defaults to a size a swapchain could use") {
    const WindowDescription description;
    CHECK_FALSE(description.size.IsEmpty());
}

// There is deliberately no run-time test of the move operations. Create never returns an
// open window, so m_nativeHandle is null before and after every move -- a test asserting
// IsOpen() on either side of one would pass just as happily against a move constructor that
// copied the handle instead of taking it, which is the exact bug those operations exist to
// prevent. Task 4 is the first point at which such a test can fail, and is where it belongs.
// What can be asserted now is the type's shape, which is what these do: a window owns an OS
// resource, so copying it must not compile.
static_assert(!std::is_copy_constructible_v<Window>);
static_assert(!std::is_copy_assignable_v<Window>);
static_assert(std::is_nothrow_move_constructible_v<Window>);
static_assert(std::is_nothrow_move_assignable_v<Window>);
