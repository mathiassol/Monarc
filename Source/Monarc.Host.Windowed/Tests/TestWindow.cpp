// Window tests: device-free, and **the first suite in the tree that can report Skipped without
// needing a GPU**.
//
// Everything here needs a screen to put a window on and nothing here needs Vulkan, a driver or
// a device. That is a third axis from the device/device-free split, so main() below makes the
// decision the device suites make -- before doctest runs a single case -- against a different
// question: `Detail::WindowPlatform::HasInteractiveSession()`, which on Windows is "this
// process's window station has WSF_VISIBLE". A service, or anything in a non-interactive
// session, gets a window station without that flag; `CreateWindowExW` would still *succeed*
// there, which is exactly why the check is explicit. See that function for the mechanism.
//
// **No `<Windows.h>` in this file, or in any other test in the repository.** Driving a window
// from code needs `SetWindowPos`, `ShowWindow` and a posted `WM_CLOSE`, and those live behind
// `Detail::WindowTestHooks` in the module's own per-platform source -- see WindowPlatform.h for
// why that is where they belong. What this file gets is a platform-neutral vocabulary for
// "resize it", "minimise it", "ask it to close".
//
// **Why these cases can fail, which is the standard this branch holds.** Task 1's version of
// this file said plainly that its move-operation cases could not: `Create` never returned an
// open window, so every assertion phrased in terms of `IsOpen()` was reading a value that could
// not be anything else, and it deleted them rather than keeping them for the look of coverage.
// Task 4 is the point at which they can, and the move cases below are written to fail against
// the specific bug the operations exist to prevent -- a move that copies the handle, or one that
// forgets to re-point the platform's back-pointer.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Host/Window.h>
#include <Monarc/RHI/Types.h>

#include <WindowPlatform.h>

#include <string_view>
#include <type_traits>
#include <utility>

using Monarc::Host::Window;
using Monarc::Host::WindowDescription;
using Monarc::Host::WindowEvent;
using Monarc::Host::WindowEventKind;
using Monarc::Host::Detail::WindowPlatform;
using Monarc::Host::Detail::WindowTestHooks;

namespace {

MONARC_LOG_CATEGORY(WindowTest, Info);

/// CTest's SKIP_RETURN_CODE for this binary, set in Source/Monarc.Host.Windowed/CMakeLists.txt.
/// 77 is the value the Phase A3 plan names and autotools' long-standing convention for the same
/// thing.
constexpr int kSkipReturnCode = 77;

/// A window small enough to be unobtrusive and large enough that a swapchain could be created
/// from it. Not `WindowDescription`'s default, because a case that resized the window would
/// otherwise have no size to compare against that was not also the default.
constexpr WindowDescription kTestWindow{.size = {480, 270}, .title = "Monarc test window"};

/// Everything `Settle` saw, accumulated across pumps.
struct SettledEvents {
    bool                  sawResize  = false;
    Monarc::RHI::Extent2D resizeSize = {};
    bool                  sawClose   = false;
};

void Collect(const Window& window, SettledEvents& settled) {
    for (const WindowEvent& event : window.Events()) {
        switch (event.kind) {
            case WindowEventKind::Resized:
                settled.sawResize  = true;
                settled.resizeSize = event.size;
                break;
            case WindowEventKind::CloseRequested:
                settled.sawClose = true;
                break;
        }
    }
}

/// Pumps `window` until the platform has nothing left to say, and returns everything that
/// happened along the way.
///
/// **It accumulates rather than reading `Events()` once, and it reads before the first pump as
/// well as after each one. Both are for the same reason, and it is a fact about Win32 rather
/// than about Monarc: not every message goes through the queue.** `SetWindowPos` and
/// `ShowWindow` *send* `WM_SIZE` synchronously from inside the call that provoked it, so the
/// handler has already recorded the event before a pump has run -- and `Window::Events()`
/// describes one pump, so the next pump clears it. A test that only looked afterwards would be
/// asserting about a buffer the pump had just emptied.
///
/// **What still proves the pump drains the queue is the close case**, which is the one thing
/// here that is genuinely *posted*: nothing but a pump can deliver `WM_CLOSE`, so
/// `sawClose` is false unless the drain ran.
///
/// Eight rounds, and it neither sleeps nor waits: everything these cases provoke is delivered
/// by the call that provoked it or posted to this thread's own queue, so a wait would be
/// covering for a race that is not there. More than one round because handling the first batch
/// can generate the next -- a restore sends activation before it sends the size.
///
/// Not `[[nodiscard]]`: several cases below pump only so that the window's *state* catches up
/// and have no interest in the events, which is a legitimate use rather than a discarded
/// result.
SettledEvents Settle(Window& window) {
    SettledEvents settled;
    Collect(window, settled);
    for (int i = 0; i < 8; ++i) {
        window.PumpEvents();
        Collect(window, settled);
    }
    return settled;
}

}  // namespace

TEST_CASE("a window opens at a size a swapchain could use, and reports what it became") {
    // Non-const, because pumping mutates. Held by value so the window outlives the reference.
    Monarc::Result<Window> created = Window::Create(kTestWindow);
    REQUIRE(created.has_value());
    Window& window = *created;

    CHECK(window.IsOpen());
    CHECK_FALSE(window.CloseRequested());
    CHECK_FALSE(window.ClientSize().IsEmpty());

    // On this machine the window manager grants the requested client size exactly, and that is
    // asserted rather than assumed: the interesting case is the one below, where it does not.
    MONARC_LOG(WindowTest, Info, "asked for {}x{} and became {}x{}", kTestWindow.size.width,
               kTestWindow.size.height, window.ClientSize().width, window.ClientSize().height);
    CHECK(window.ClientSize() == kTestWindow.size);
}

TEST_CASE("a resize event carries the size the window became, not the size requested") {
    // **The plan's own wording, and it needs a size the window manager will refuse.** A resize
    // to a size that is granted cannot tell an event carrying `ClientSize()` from one echoing
    // the request -- they are the same number. Windows enforces a minimum window width
    // (SM_CXMIN, around 136 physical pixels here), so a request for a 1x1 *client* area comes
    // back as something else, and the event has to carry that something else or a swapchain
    // created from it is VUID-VkSwapchainCreateInfoKHR-imageExtent-01274.
    Monarc::Result<Window> created = Window::Create(kTestWindow);
    REQUIRE(created.has_value());
    Window& window = *created;

    const Monarc::RHI::Extent2D refused{1, 1};
    WindowTestHooks::RequestClientSize(window, refused);
    const SettledEvents settled = Settle(window);
    REQUIRE(settled.sawResize);

    MONARC_LOG(WindowTest, Info, "asked for {}x{} and the event carries {}x{}", refused.width,
               refused.height, settled.resizeSize.width, settled.resizeSize.height);

    // The two halves of the claim. The first is what fails if the event is built from the
    // request; the second is what fails if the size is read from somewhere other than the
    // window -- there is one function in the platform file that answers "how big is the client
    // area", and both of these go through it.
    CHECK(settled.resizeSize != refused);
    CHECK(settled.resizeSize == window.ClientSize());

    // And the ordinary case, so that "the event carries something else" is not passing because
    // resizing is broken outright.
    const Monarc::RHI::Extent2D granted{640, 360};
    WindowTestHooks::RequestClientSize(window, granted);
    const SettledEvents second = Settle(window);
    REQUIRE(second.sawResize);
    CHECK(second.resizeSize == granted);
    CHECK(window.ClientSize() == granted);
}

TEST_CASE("a window created and destroyed leaves no registered class behind") {
    // The plan's own case. Registration is reference-counted against the number of live
    // windows, so this is three assertions and not one: before, during, and after.
    CHECK_FALSE(WindowPlatform::IsClassRegistered());

    {
        const Monarc::Result<Window> created = Window::Create(kTestWindow);
        REQUIRE(created.has_value());
        CHECK(WindowPlatform::IsClassRegistered());
    }

    CHECK_FALSE(WindowPlatform::IsClassRegistered());
}

TEST_CASE("the window class survives one of two windows closing") {
    // **This case's first draft claimed to catch the reference count going, and measuring said
    // otherwise.** With the count dropped from `ReleaseClassIfUnused` -- unregister on any
    // close -- this suite stays entirely green at 18 cases and 98 assertions, and logs
    // eighteen `ERROR_CLASS_HAS_WINDOWS`
    // warnings instead: Windows refuses to unregister a class while a window of it exists, so
    // it protects the surviving window whether Monarc's bookkeeping does or not.
    //
    // What is left is still worth asserting, and it is not the same thing: that a second window
    // does not need a second registration, that closing the inner one leaves the outer one
    // open and its class registered, and that closing the last one releases it. The *release*
    // is the part with a red: removing it turns 13 of 18 cases in this file red.
    CHECK_FALSE(WindowPlatform::IsClassRegistered());

    Monarc::Result<Window> first = Window::Create(kTestWindow);
    REQUIRE(first.has_value());

    {
        const Monarc::Result<Window> second = Window::Create(kTestWindow);
        REQUIRE(second.has_value());
        CHECK(WindowPlatform::IsClassRegistered());
    }

    CHECK(WindowPlatform::IsClassRegistered());
    CHECK(first->IsOpen());

    first->Destroy();
    CHECK_FALSE(WindowPlatform::IsClassRegistered());
}

TEST_CASE("moving a window takes the platform's back-pointer with it") {
    // **The case Task 1 said it could not write, and the bug it catches is specific.** The
    // platform's message handler finds its `Window` through a pointer stored on the native
    // window. A moved-to window lives at a different address, so a move that did not re-point
    // it -- a defaulted move, which compiles perfectly -- would leave the handler writing
    // resize events into the moved-from object.
    //
    // The resize is provoked *after* the move for exactly that reason: an assertion about the
    // state a move copied would pass either way.
    Monarc::Result<Window> created = Window::Create(kTestWindow);
    REQUIRE(created.has_value());

    Window moved(std::move(*created));
    CHECK(moved.IsOpen());
    CHECK_FALSE(created->IsOpen());

    const Monarc::RHI::Extent2D target{512, 288};
    WindowTestHooks::RequestClientSize(moved, target);
    const SettledEvents settled = Settle(moved);

    CHECK(moved.ClientSize() == target);
    CHECK(settled.sawResize);

    // And nothing reached the source. Its handle is null so it cannot be pumped, which is why
    // this reads the state rather than the event list: a handler still pointed at it would have
    // written the new size here.
    CHECK(created->ClientSize().IsEmpty());
}

TEST_CASE("move assignment closes what the destination held and rebinds what it took") {
    Monarc::Result<Window> source      = Window::Create(kTestWindow);
    Monarc::Result<Window> destination = Window::Create(kTestWindow);
    REQUIRE(source.has_value());
    REQUIRE(destination.has_value());
    CHECK(WindowPlatform::IsClassRegistered());

    *destination = std::move(*source);

    // The window the destination held is closed, so the count is back to one -- the class is
    // still registered because the moved window is live.
    CHECK(destination->IsOpen());
    CHECK_FALSE(source->IsOpen());
    CHECK(WindowPlatform::IsClassRegistered());

    // Rebound, by the same measurement the move-construction case makes.
    const Monarc::RHI::Extent2D target{576, 324};
    WindowTestHooks::RequestClientSize(*destination, target);
    Settle(*destination);
    CHECK(destination->ClientSize() == target);
    CHECK(source->ClientSize().IsEmpty());

    destination->Destroy();
    CHECK_FALSE(WindowPlatform::IsClassRegistered());
}

TEST_CASE("self-move-assignment leaves a window open") {
    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());

    // Without the `this != &other` guard this destroys the window and then adopts the handle it
    // has just released. The reference launder is what stops the compiler warning about a
    // self-move it can see; the operator's guard is what makes it safe.
    Window& alias = *window;
    *window       = std::move(alias);

    CHECK(window->IsOpen());
    CHECK_FALSE(window->ClientSize().IsEmpty());
}

TEST_CASE("closing is a request the window does not act on by itself") {
    // **The window must not destroy itself on WM_CLOSE**, because it would take a live
    // `VkSurfaceKHR` with it while a frame was in flight -- so the handler deliberately does
    // not call DefWindowProcW, whose WM_CLOSE handler calls DestroyWindow. Restoring that call
    // turns the third assertion below red.
    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());
    CHECK_FALSE(window->CloseRequested());

    WindowTestHooks::RequestClose(*window);
    const SettledEvents settled = Settle(*window);

    CHECK(window->CloseRequested());
    // **The one event in this suite that only a pump can deliver**, because
    // `WindowTestHooks::RequestClose` posts it. See `Settle`: everything else is sent
    // synchronously by the call that provoked it, so this is what proves the drain runs.
    CHECK(settled.sawClose);
    CHECK(window->IsOpen());

    // Sticky: a later pump that sees nothing does not clear it, which is what lets a frame loop
    // check it once per iteration.
    window->PumpEvents();
    CHECK(window->Events().empty());
    CHECK(window->CloseRequested());
}

TEST_CASE("a minimised window reports no area, and a restored one reports it again") {
    // **The state the frame loop parks on.** A swapchain cannot be created from 0 x 0, so this
    // is the fact `ISwapchain::Recreate` refuses and `Monarc.FirstLight`'s loop tests before it
    // does anything else.
    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());
    const Monarc::RHI::Extent2D before = window->ClientSize();
    REQUIRE_FALSE(before.IsEmpty());

    WindowTestHooks::Minimise(*window);
    Settle(*window);
    MONARC_LOG(WindowTest, Info, "minimised: client area is {}x{}", window->ClientSize().width,
               window->ClientSize().height);
    CHECK(window->ClientSize().IsEmpty());

    WindowTestHooks::Restore(*window);
    Settle(*window);
    MONARC_LOG(WindowTest, Info, "restored: client area is {}x{}", window->ClientSize().width,
               window->ClientSize().height);
    CHECK_FALSE(window->ClientSize().IsEmpty());
    CHECK(window->ClientSize() == before);
}

TEST_CASE("a maximised window reports the size the window manager gave it") {
    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());

    const WindowTestHooks::MonitorInfo monitor = WindowTestHooks::Monitors(*window);
    REQUIRE(monitor.monitorCount >= 1);

    WindowTestHooks::Maximise(*window);
    Settle(*window);

    const Monarc::RHI::Extent2D maximised = window->ClientSize();
    MONARC_LOG(WindowTest, Info,
               "maximised: client area is {}x{} on a monitor {} wide by {} tall",
               maximised.width, maximised.height, monitor.right - monitor.left,
               monitor.bottom - monitor.top);

    CHECK_FALSE(maximised.IsEmpty());
    // Bigger than it was, and no wider than the monitor. Both halves matter: the first fails if
    // maximising did nothing, and the second fails if the client size is being computed from
    // the *window* rect rather than the client rect -- a maximised window's frame extends past
    // the work area, so the two differ by the border width.
    CHECK(maximised.width > kTestWindow.size.width);
    CHECK(maximised.width <= static_cast<Monarc::u32>(monitor.right - monitor.left));

    WindowTestHooks::Restore(*window);
    Settle(*window);
    CHECK(window->ClientSize() == kTestWindow.size);
}

TEST_CASE("a window can be moved between monitors, and both monitors' DPI is reported") {
    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());

    const WindowTestHooks::MonitorInfo first = WindowTestHooks::Monitors(*window);
    MONARC_LOG(WindowTest, Info,
               "{} monitor(s); this window is on the one at ({}, {})-({}, {}) at {} DPI",
               first.monitorCount, first.left, first.top, first.right, first.bottom, first.dpi);

    // **What `Monitors()` reported, asserted on any machine, before the count is consulted at
    // all.** This used to `return` on `monitorCount < 2`, which meant a one-monitor machine
    // reported this case green having asserted one thing: that `Window::Create` succeeded.
    // Nothing about monitors and nothing about DPI, both of which are in the title. Measured,
    // by forcing `WindowTestHooks::Monitors` to report one monitor: the suite stayed 18 of 18
    // green while assertions fell 98 to 93, and run alone the case reported one passed.
    //
    // A zeroed `MONITORINFO` -- `GetMonitorInfoW` failing, or `MonitorFromWindow` finding
    // nothing -- is what these four turn red, and they are the half of this case that does not
    // need a second display.
    CHECK(first.monitorCount >= 1);
    CHECK(first.right > first.left);
    CHECK(first.bottom > first.top);
    CHECK(first.dpi > 0);
    // The monitor the window is on is inside the virtual screen, which is the only relation
    // between the two rectangles that holds however many displays there are.
    CHECK(first.left >= first.virtualLeft);
    CHECK(first.top >= first.virtualTop);

    // **A second monitor is declared as a *branch* here and as a requirement in the device
    // suite, and the asymmetry is deliberate.** `Monarc.Host.Windowed/TestsDevice`'s "a window
    // moved to another monitor keeps presenting" asserts `CHECK(monitorCount >= 2)` outright,
    // on the argument that a suite already requiring a GPU and an interactive session can
    // declare a second display alongside them. This suite is the one CI actually runs -- it
    // needs no device, only `HasInteractiveSession()` -- and a GitHub runner reports one
    // monitor. The same `CHECK` here would be a permanently red assertion about the runner's
    // hardware rather than about Monarc, which is the kind of red that teaches people to
    // ignore reds.
    //
    // So the count picks the branch and **both branches assert**. What is not on offer is the
    // third option this used to take.
    const bool crossMonitor = first.monitorCount >= 2;

    if (!crossMonitor) {
        // **And the count itself is cross-checked, which is what makes a wrong one catchable
        // on a machine with any number of displays.** The virtual screen is the bounding box
        // of every monitor, so on a machine with one it *is* that monitor and its origin is
        // that monitor's origin. Forcing `WindowTestHooks::Monitors` to report a count of 1 on
        // this two-monitor machine -- where `\\.\DISPLAY5` sits at `(-1920, 0)` -- turns this
        // red, which is the mutation a bare `return` and a bare `CHECK(monitorCount >= 2)`
        // would each have missed here for opposite reasons.
        CHECK(first.virtualLeft == first.left);
        CHECK(first.virtualTop == first.top);
    }

    // The development machine has \\.\DISPLAY5 at (-1920, 0) and \\.\DISPLAY1, the primary, at
    // (0, 0). A negative X is therefore how the second monitor is reached -- see Docs/Status.md.
    // Where there is only one, the move stays inside it, so the client-size and DPI assertions
    // below run on every machine.
    WindowTestHooks::MoveTo(*window, crossMonitor ? first.left - 1000 : first.left + 200,
                            first.top + 100);
    Settle(*window);

    const WindowTestHooks::MonitorInfo second = WindowTestHooks::Monitors(*window);
    MONARC_LOG(WindowTest, Info,
               "after the move it is on the one at ({}, {})-({}, {}) at {} DPI", second.left,
               second.top, second.right, second.bottom, second.dpi);

    if (crossMonitor) {
        CHECK(second.left != first.left);
    } else {
        MONARC_LOG(WindowTest, Warning,
                   "this machine reports one monitor, so the move above was within it and the "
                   "cross-monitor half of this case is unexercised here; what runs instead is "
                   "everything that does not need a second display");
        // The move stayed on the one monitor, which is the only thing there is to assert about
        // where it ended up -- and it is an assertion rather than a return.
        CHECK(second.left == first.left);
    }

    CHECK_FALSE(window->ClientSize().IsEmpty());
    // The window keeps its client size across the move. **On this machine that is also why the
    // case does not exercise the WM_DPICHANGED path**, and saying so is the whole of the claim:
    // a drag between displays at different scale factors would change the size, and neither
    // monitor here is scaled differently from the other. Within one monitor the equality is a
    // stronger statement, because no DPI change is even possible -- a `MoveTo` that resized the
    // window is what it catches.
    CHECK(second.dpi == first.dpi);
    CHECK(window->ClientSize() == kTestWindow.size);

    WindowTestHooks::MoveTo(*window, first.left + 100, first.top + 100);
    Settle(*window);
    CHECK(WindowTestHooks::Monitors(*window).left == first.left);
}

TEST_CASE("an open window reports both native handles, and a closed one reports neither") {
    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());

    const Monarc::RHI::SurfaceDescription surface = window->Surface();
    CHECK_FALSE(surface.IsEmpty());
    CHECK(surface.nativeWindow != nullptr);
    // The module handle as well, because Win32 surface creation needs both and refuses a
    // description missing either -- `Detail::CreatePlatformSurface` in Monarc.RHI.Vulkan names
    // which one was missing, and this is the assertion that the window supplies it.
    CHECK(surface.nativeDisplay != nullptr);

    window->Destroy();
    CHECK(window->Surface().IsEmpty());
    CHECK(window->Surface().nativeWindow == nullptr);
    CHECK(window->Surface().nativeDisplay == nullptr);
}

TEST_CASE("destroying a window twice is a no-op, and so is pumping a closed one") {
    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());

    window->Destroy();
    CHECK_FALSE(window->IsOpen());
    window->Destroy();
    CHECK_FALSE(window->IsOpen());

    // Neither of these may reach into a destroyed native window. There is no assertion that
    // catches a call through a freed HWND -- Windows returns failure for an invalid handle
    // rather than crashing -- so what this case actually covers is the guard: with the null
    // test removed from `WindowPlatform::Pump`, `WaitMessage` blocks forever and this suite
    // hangs instead of failing, which is a finding stated rather than an assertion claimed.
    window->PumpEvents();
    window->WaitForEvents();
    CHECK(window->Events().empty());
}

TEST_CASE("a window the system destroys still releases its class registration") {
    // **The other of the two paths a `WM_DESTROY` arrives by, and a mutation is what found
    // it.** Making the `WM_CLOSE` handler call `DefWindowProcW` -- whose `WM_CLOSE` calls
    // `DestroyWindow` -- turned the close case red as intended and *also* turned a later
    // case's `CHECK_FALSE(IsClassRegistered())` red, because the reference count was
    // decremented in `Window::Destroy` and nothing decremented it when the system did the
    // destroying. The count moved into the message handler; this is the case that says so.
    CHECK_FALSE(WindowPlatform::IsClassRegistered());

    Monarc::Result<Window> window = Window::Create(kTestWindow);
    REQUIRE(window.has_value());
    CHECK(WindowPlatform::IsClassRegistered());

    WindowTestHooks::DestroyNatively(*window);

    // No pump needed: `DestroyWindow` sends `WM_DESTROY` synchronously, so the handler has
    // already run and the window reads as closed.
    CHECK_FALSE(window->IsOpen());
    CHECK(window->ClientSize().IsEmpty());

    // **Still registered, and that is the mechanism rather than a gap.** `UnregisterClassW`
    // refuses while a window of the class exists, and one still did when `WM_DESTROY` was
    // handled -- so the count is at zero and the registration is waiting for a moment the
    // platform will accept. The first fix for the drift above tried to unregister from the
    // handler and turned this whole suite red; the note in `OnDestroyed` records what
    // happened.
    CHECK(WindowPlatform::IsClassRegistered());

    // `Destroy` is that moment, and `~Window` always reaches it -- so a window destroyed by
    // the system still releases its class when its owner goes away. It is also a no-op for the
    // count rather than a second decrement, which is what the cleared handle buys: without it
    // the count would go negative and the check in `OnDestroyed` would fire.
    window->Destroy();
    CHECK_FALSE(WindowPlatform::IsClassRegistered());
}

TEST_CASE("a window cannot be created with an empty client size") {
    const Monarc::Result<Window> zeroWidth =
        Window::Create(WindowDescription{.size = {0, 720}, .title = "no"});
    REQUIRE_FALSE(zeroWidth.has_value());
    CHECK(zeroWidth.error().code == Monarc::ErrorCode::InvalidArgument);

    const Monarc::Result<Window> zeroHeight =
        Window::Create(WindowDescription{.size = {1280, 0}, .title = "no"});
    REQUIRE_FALSE(zeroHeight.has_value());
    CHECK(zeroHeight.error().code == Monarc::ErrorCode::InvalidArgument);

    // And the refusal left nothing behind: an early return that had already registered the
    // class would leave it registered for the life of the process.
    CHECK_FALSE(WindowPlatform::IsClassRegistered());
}

TEST_CASE("a default-constructed window is not open and describes nothing") {
    const Window window;
    CHECK_FALSE(window.IsOpen());
    CHECK(window.ClientSize().IsEmpty());
    CHECK_FALSE(window.CloseRequested());
    CHECK(window.Events().empty());
    CHECK(window.Surface().IsEmpty());
}

TEST_CASE("a window description defaults to a size a swapchain could use") {
    const WindowDescription description;
    CHECK_FALSE(description.size.IsEmpty());
}

TEST_CASE("every WindowEventKind has its own spelling") {
    CHECK(std::string_view(Monarc::Host::ToString(WindowEventKind::Resized)) == "Resized");
    CHECK(std::string_view(Monarc::Host::ToString(WindowEventKind::CloseRequested)) ==
          "CloseRequested");
    // A value outside the enumerator set gets a name of its own rather than one of the two
    // above -- `RHI::ToString(RHI::Format)`'s rule, and the reason is the same: a log that
    // reported an invalid value as a real event kind would send its reader after the wrong
    // thing.
    CHECK(std::string_view(Monarc::Host::ToString(static_cast<WindowEventKind>(4242))) !=
          "Resized");
    CHECK(std::string_view(Monarc::Host::ToString(static_cast<WindowEventKind>(4242))) !=
          "CloseRequested");
}

// A window owns an OS resource, so copying it must not compile. Kept from Task 1, where these
// were the only assertions in this file that could fail.
static_assert(!std::is_copy_constructible_v<Window>);
static_assert(!std::is_copy_assignable_v<Window>);
static_assert(std::is_nothrow_move_constructible_v<Window>);
static_assert(std::is_nothrow_move_assignable_v<Window>);

int main(int argc, char** argv) {
    if (!WindowPlatform::HasInteractiveSession()) {
        // Info, not Error: a process with no interactive session is a legitimate place to run
        // a build, and CTest is about to print this as Skipped. The file comment says what the
        // check actually is.
        MONARC_LOG(WindowTest, Info,
                   "skipping the window tests: this process has no interactive session to put a "
                   "window in (returning {} so CTest reports Skipped rather than Passed)",
                   kSkipReturnCode);
        return kSkipReturnCode;
    }

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
