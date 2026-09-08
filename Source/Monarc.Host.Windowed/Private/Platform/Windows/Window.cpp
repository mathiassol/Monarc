// Raw Win32: the window class, `WndProc`, the message pump, DPI awareness, and the
// translation of Win32 messages into Monarc events.
//
// **The only file in Monarc.Host.Windowed that includes `<Windows.h>`, and no test in the
// repository includes it either** -- which is what `Detail::WindowTestHooks` at the foot of this
// file is for. Checkable, and it has to match the *include* rather than the word, because three
// other files in this module mention `<Windows.h>` in a comment:
//
//     grep -rniE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<windows\.h>' Source/
//
// -- one line in this module, this file's, and five in Monarc.Core's own platform layer.
//
// ADR-0016 selects it by *directory*: monarc_module() hands the compiler only the current
// platform's Private/Platform/<Platform>/ tree, so a macOS build never sees this file rather
// than compiling it out of a conditional.
//
// **No `#ifdef` on a platform macro anywhere in it**, which is what gate 10 in
// Tools/check_architecture.py polices. The gate exempts this directory; the exemption is not
// what keeps it quiet here, because there is nothing to exempt.
//
// Raw Win32 and not a windowing library, per the phase plan's tech stack. What that costs is
// this file; what it buys is that there is no third-party message loop deciding when Monarc
// gets to see an event, and no dependency in ADR-0014's table for something a game engine has
// to own anyway.

#include <Monarc/Host/Window.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Log.h>

#include <WindowPlatform.h>

#include <iterator>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace Monarc::Host::Detail {

namespace LogCategories {

/// Nested in a namespace of its own for one mechanical reason, the same one
/// Monarc.RHI.Vulkan/Private/VulkanDeviceState.h records: MONARC_LOG_CATEGORY declares a
/// *variable* named after the category, and a variable called `Window` in this namespace would
/// hide the class of that name -- so `Result<Window>` and `Window window;` below would both
/// stop compiling, naming a variable where a type belongs. The nesting keeps the category's
/// spelling, which is what appears in a log line.
MONARC_LOG_CATEGORY(Window, Info);

}  // namespace LogCategories

namespace {

/// The window class Monarc registers, once, for every window it makes.
///
/// Wide, because `RegisterClassExW`/`CreateWindowExW` are what this file calls: the W entry
/// points are the real ones on modern Windows and the A ones are a code-page-dependent shim
/// over them, which is not a conversion a game engine should inherit by accident.
constexpr wchar_t kClassName[] = L"Monarc.Host.Windowed.Window";

/// Longest window title this file will convert, including the terminator. A title longer than
/// this is truncated rather than refused -- a truncated title is still recognisable in a task
/// bar, where an empty one is not, which is `CopyAdapterName`'s argument in
/// Monarc/RHI/Adapter.h.
constexpr int kMaxTitleWide = 256;

/// How many live windows the registered class is serving.
///
/// **What the count decides is whether Monarc *asks* to unregister -- not whether it is safe
/// to, because Windows decides that itself.** `UnregisterClassW` refuses with
/// `ERROR_CLASS_HAS_WINDOWS` (1412) while any window of the class exists, and measuring that is
/// what corrected this comment: with the count dropped from `ReleaseClassIfUnused`, the window
/// suite stays entirely green -- 18 cases, 98 assertions -- and logs eighteen of those
/// warnings, one per attempt. So the count buys a quiet log
/// and an honest "release when the last one goes", and **no assertion in the suite can tell it
/// from Windows' own refusal.** That is stated rather than dressed up as coverage.
///
/// What the count is genuinely load-bearing for is the *other* direction, and that has reds:
/// with the release removed entirely, 13 of 18 window cases go red and `RegisterClassExW`
/// starts failing with `ERROR_CLASS_ALREADY_EXISTS` (1410).
///
/// A plain `u32` and not an atomic, because window creation and destruction are single-thread
/// operations by construction: Win32 delivers a window's messages to the thread that created
/// it, so a window created on another thread would have a queue nobody pumps.
/// `Window`'s class comment states that.
u32 g_liveWindows = 0;

/// Whether Monarc's window class is registered right now.
///
/// **Not derivable from `g_liveWindows`, which is why it is a second variable.** The count
/// reaches zero while the class is still registered: `UnregisterClassW` refuses while a window
/// of the class exists, so the release happens in `WindowPlatform::Destroy` after
/// `DestroyWindow` has returned, and the count was decremented back in the message handler. So
/// there is a window in which the count is zero and the registration is live, and without this
/// flag every `Destroy` on an already-closed window would attempt an unregister that fails
/// with `ERROR_CLASS_DOES_NOT_EXIST` (1411) and log a warning about it -- which is exactly what
/// the first version of this did, five times per suite run.
///
/// `Detail::WindowPlatform::IsClassRegistered()` deliberately asks the *platform* rather than
/// reading this, so the test that watches registration is measuring Windows and not Monarc's
/// bookkeeping.
bool g_classRegistered = false;

/// Whether `SetProcessDpiAwarenessContext` has been attempted. Process-wide and one-shot: the
/// call fails once awareness has been set, by us or by anything else, and re-attempting it
/// every window would put a spurious failure in the log per window.
bool g_dpiAwarenessAttempted = false;

/// The `Window` a native window belongs to, or null.
[[nodiscard]] Window* OwnerOf(HWND handle) {
    // GWLP_USERDATA rather than a map keyed by HWND: the pointer travels with the window, so
    // there is no second structure to keep in step with window lifetime, and the message
    // handler's lookup is a single read.
    return reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
}

/// Converts a UTF-8 title into `buffer`, truncating to fit and always null-terminating.
///
/// A third copy of the shape Monarc.Core's Windows File.cpp and Library.cpp each have, and
/// their own comment already says why: both are anonymous-namespace internals of their own
/// translation unit, and ADR-0016 leaves a shared home for small cross-file helpers as a step
/// nothing has needed to take. This one differs from theirs in what it does when the input does
/// not fit -- they refuse a path, this truncates a title -- so sharing would have needed a
/// policy parameter before it needed a home.
void ToWideTitle(const char* utf8, wchar_t (&buffer)[kMaxTitleWide]) {
    buffer[0] = L'\0';
    if (utf8 == nullptr || utf8[0] == '\0') {
        return;
    }
    const int written =
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buffer, kMaxTitleWide);
    if (written == 0) {
        // Either the input is not valid UTF-8 or it does not fit. Truncation needs the
        // explicit-length form, because the -1 form above refuses rather than truncating.
        const int truncated =
            MultiByteToWideChar(CP_UTF8, 0, utf8, kMaxTitleWide - 1, buffer, kMaxTitleWide - 1);
        buffer[truncated >= 0 ? truncated : 0] = L'\0';
        MONARC_LOG(LogCategories::Window, Warning,
                   "the window title did not convert whole and was truncated to {} character(s)",
                   truncated >= 0 ? truncated : 0);
    }
}

/// Records `event` into `events`/`count`, coalescing onto an entry of the same kind.
///
/// `kMaxWindowEvents` in Monarc/Host/Window.h says why coalescing rather than a queue: a corner
/// drag produces hundreds of WM_SIZE per pump and only the last describes a size a swapchain
/// could be created at.
///
/// **The buffer and the count rather than the `Window`, because friendship does not reach a
/// free function.** `WindowPlatform` is what `Window` befriends, so its static members below
/// pull the two members out and hand them here -- which also keeps this function testable in
/// principle and ignorant of what a window is.
void RecordEvent(WindowEvent* events, usize& count, const WindowEvent& event) {
    for (usize i = 0; i < count; ++i) {
        if (events[i].kind == event.kind) {
            events[i] = event;
            return;
        }
    }
    if (count < kMaxWindowEvents) {
        events[count] = event;
        ++count;
        return;
    }
    // Unreachable while there are two event kinds and two slots, and it is a check rather than
    // a silent drop because the day a third kind is added is the day this stops being
    // unreachable -- and a dropped close request is a program that will not quit.
    MONARC_CHECK(false, "a window event was dropped: kMaxWindowEvents is too small");
}

/// The client rectangle's size, as the platform reports it now.
[[nodiscard]] RHI::Extent2D QueryClientSize(HWND handle) {
    RECT client{};
    if (GetClientRect(handle, &client) == 0) {
        return RHI::Extent2D{};
    }
    // GetClientRect's rect always starts at (0, 0), so right and bottom *are* the size. Written
    // as a subtraction anyway, because reading it as a size is a property of this function's
    // input rather than of the arithmetic, and a rect that did not start at the origin would
    // otherwise be silently wrong.
    return RHI::Extent2D{static_cast<u32>(client.right - client.left),
                         static_cast<u32>(client.bottom - client.top)};
}

LRESULT CALLBACK WindowProc(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        // **The back-pointer is installed here and not in WM_CREATE**, because messages
        // arrive before WM_CREATE -- WM_GETMINMAXINFO and WM_NCCALCSIZE among them -- and a
        // handler that found no owner for those would have to have a null branch that is only
        // ever taken during creation.
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(handle, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(handle, message, wParam, lParam);
    }

    Window* window = OwnerOf(handle);
    if (window == nullptr) {
        return DefWindowProcW(handle, message, wParam, lParam);
    }

    switch (message) {
        case WM_SIZE:
            // Read back from the platform rather than decoded from lParam. The two agree for
            // WM_SIZE, and the read is what makes `WindowEvent::size` honest by construction:
            // there is one function that answers "how big is the client area", and both the
            // event and `Window::ClientSize()` go through it.
            //
            // A minimised window reports 0 x 0 here, which is the state the frame loop parks
            // on -- see `Window::ClientSize()`. It is reported as an ordinary resize because
            // that is what it is: the client area changed size, to nothing.
            WindowPlatform::OnResized(*window, QueryClientSize(handle));
            return 0;

        case WM_CLOSE:
            // **DefWindowProcW is deliberately not called**, because its WM_CLOSE handler
            // calls DestroyWindow -- and a window that destroyed itself here would take a live
            // `VkSurfaceKHR` with it while a frame was in flight. Closing is the caller's
            // decision; this records the request. `WindowEventKind::CloseRequested` says so
            // from the other side.
            WindowPlatform::OnCloseRequested(*window);
            return 0;

        case WM_DPICHANGED: {
            // The documented correct response: move and resize to the rectangle Windows
            // suggests, which is the window's rect scaled for the new DPI. lParam is a RECT*.
            //
            // **Untested on this machine, and saying so is the extent of the claim.** Both
            // monitors here are 1920x1080 and report the same DPI
            // (`WindowTestHooks::Monitors` reads it, and the window tests print both), so
            // dragging a window between them does not send this message. What would exercise
            // it is a display whose scaling differs -- or changing one monitor's scale in
            // Settings while the program runs. Neither is something a test on this machine can
            // arrange.
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested != nullptr) {
                SetWindowPos(handle, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            // Non-zero means "handled, do not erase". The swapchain owns every pixel of the
            // client area, so letting GDI paint the class brush first is a frame of grey before
            // each present -- visible as flicker while resizing.
            return 1;

        case WM_DESTROY:
            // The handle is about to become invalid, so the owner must stop naming it. Reached
            // both from `Destroy` -- which calls DestroyWindow itself -- and from a system
            // shutdown that destroys the window under us; clearing it here covers both, and
            // makes `Destroy` idempotent by construction rather than by a second flag.
            WindowPlatform::OnDestroyed(*window);
            SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
            return 0;

        default:
            // A `default` here rather than the exhaustive switch this codebase prefers, and it
            // is not the same kind of switch: `message` is a `UINT` with several hundred
            // system-defined values and no enumeration, so there is no set to be exhaustive
            // over. Everything Monarc does not handle is Windows' to handle, which is what
            // DefWindowProcW is.
            return DefWindowProcW(handle, message, wParam, lParam);
    }
}

/// Registers the window class if this is the first window. Returns false and logs on failure.
[[nodiscard]] bool EnsureClassRegistered() {
    if (g_classRegistered) {
        return true;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize        = sizeof(windowClass);
    // OWNDC is not asked for: nothing here draws through GDI, and a swapchain does not need a
    // device context of its own. HREDRAW/VREDRAW would ask Windows to invalidate the whole
    // client area on every resize, which is a repaint the swapchain is going to overwrite
    // anyway.
    windowClass.style         = 0;
    windowClass.lpfnWndProc   = WindowProc;
    windowClass.hInstance     = GetModuleHandleW(nullptr);
    // **`IDC_ARROW` needs the cast, and it is not a cast between unrelated things.** The
    // constant is `MAKEINTRESOURCE(32512)`, and in a build that does not define `UNICODE`
    // `MAKEINTRESOURCE` is the A-flavoured macro -- so it is an ordinal wearing an `LPSTR`,
    // which `LoadCursorW` will not take. Re-wearing the same ordinal as an `LPCWSTR` is what
    // `MAKEINTRESOURCEW` would do, and this keeps the constant's name instead of open-coding
    // 32512. Defining `UNICODE` for this translation unit was the alternative, and it would
    // change the meaning of every T-macro in `<Windows.h>` for the sake of one cursor.
    windowClass.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    // No background brush. With WM_ERASEBKGND answered above this changes nothing on its own;
    // it is here so that a future edit removing that handler does not silently reintroduce the
    // grey flash.
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        MONARC_LOG(LogCategories::Window, Error, "RegisterClassExW failed with Win32 error {}",
                   GetLastError());
        return false;
    }
    g_classRegistered = true;
    return true;
}

/// Unregisters the window class once the last window has gone.
///
/// Called from `WindowPlatform::Destroy` and nowhere else, and both of its paths call it --
/// including the one where the window was already gone, which is how a window the *system*
/// destroyed still releases its class. See `g_classRegistered` for why both variables exist.
void ReleaseClassIfUnused() {
    if (g_liveWindows != 0 || !g_classRegistered) {
        return;
    }
    if (UnregisterClassW(kClassName, GetModuleHandleW(nullptr)) == 0) {
        // Worth a log and not a check: the class can legitimately still be in use by a window
        // this module did not make, and there is nothing to do about it either way.
        MONARC_LOG(LogCategories::Window, Warning, "UnregisterClassW failed with Win32 error {}",
                   GetLastError());
        return;
    }
    g_classRegistered = false;
}

/// Makes the process per-monitor DPI aware, once.
void EnsureDpiAwareness() {
    if (g_dpiAwarenessAttempted) {
        return;
    }
    g_dpiAwarenessAttempted = true;

    // **PER_MONITOR_AWARE_V2 and not the older values**, because V2 is the one that scales the
    // non-client area -- title bar, borders, menus -- and sends WM_DPICHANGED before the move
    // rather than after. Without awareness, Windows lies to the process about every size it
    // reports: `GetClientRect` returns virtualised pixels on a scaled display, and a swapchain
    // created from them does not match its surface.
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
        // Not a failure to report upward. Awareness is process-wide and can only be set once,
        // so this is what happens when something else -- an application manifest, or a host
        // process -- has already chosen. Info rather than Warning: the common case is a
        // manifest that already asked for the same thing.
        MONARC_LOG(LogCategories::Window, Info,
                   "SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) was refused with "
                   "Win32 error {}; the process keeps whatever awareness it already had",
                   GetLastError());
    }
}

/// `window`'s HWND, or null.
///
/// Through `WindowPlatform::NativeHandle` and not `window.m_nativeHandle`, because this is a
/// free function and friendship does not reach one. The cast is the only thing this adds.
[[nodiscard]] HWND HandleOf(const Window& window) {
    return static_cast<HWND>(WindowPlatform::NativeHandle(window));
}

/// The window style Monarc's windows use: an ordinary resizable top-level window.
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;

}  // namespace

void* WindowPlatform::NativeHandle(const Window& window) { return window.m_nativeHandle; }

void WindowPlatform::OnResized(Window& window, RHI::Extent2D size) {
    window.m_clientSize = size;
    RecordEvent(window.m_events, window.m_eventCount,
                WindowEvent{WindowEventKind::Resized, size});
}

void WindowPlatform::OnCloseRequested(Window& window) {
    window.m_closeRequested = true;
    RecordEvent(window.m_events, window.m_eventCount,
                WindowEvent{WindowEventKind::CloseRequested, RHI::Extent2D{}});
}

void WindowPlatform::OnDestroyed(Window& window) {
    window.m_nativeHandle = nullptr;
    window.m_clientSize   = RHI::Extent2D{};

    // **The class reference count is decremented here and not in `Destroy`, and a mutation is
    // what moved it.** `Destroy` is one of two ways a window's `WM_DESTROY` arrives; the other
    // is the system destroying it -- a logoff, or a parent going away -- and a count that only
    // `Destroy` decremented would drift upwards by one on that path and leave the class
    // registered for the life of the process. Here it moves exactly once per destruction,
    // whoever started it.
    //
    // Found by making the `WM_CLOSE` handler call `DefWindowProcW`, whose `WM_CLOSE` calls
    // `DestroyWindow`: the case that says closing is a request went red as intended, and
    // `CHECK_FALSE(IsClassRegistered())` in a *later* case went red as well -- which was the
    // drift rather than the mutation.
    //
    // **`ReleaseClassIfUnused` is deliberately *not* called from here**, and the first attempt
    // at this fix did call it. `UnregisterClassW` refuses with `ERROR_CLASS_HAS_WINDOWS` while
    // any window of the class exists, and during `WM_DESTROY` one still does -- the handle
    // stays valid until after `WM_NCDESTROY` returns. So the unregister failed, the class
    // stayed registered with a count of zero, and the next `RegisterClassExW` failed with
    // `ERROR_CLASS_ALREADY_EXISTS` (1410): every remaining case in the suite went red. It is
    // `Destroy` that releases, because the earliest moment the platform will accept the
    // unregister is after `DestroyWindow` has returned.
    MONARC_CHECK(g_liveWindows > 0, "a window was destroyed that was never counted as live");
    if (g_liveWindows > 0) {
        --g_liveWindows;
    }
}

Result<Window> WindowPlatform::Create(const WindowDescription& description) {
    if (description.size.IsEmpty()) {
        return Err(ErrorCode::InvalidArgument,
                   "a window cannot be created with an empty client size");
    }
    if (!HasInteractiveSession()) {
        // Refused rather than created, because a window on a non-visible window station is a
        // window nobody can see or interact with -- see `HasInteractiveSession` for the
        // mechanical meaning. `CreateWindowExW` would succeed, which is exactly why this is
        // checked rather than left to fail.
        return Err(ErrorCode::Unsupported,
                   "this process has no interactive session, so it cannot open a window");
    }

    EnsureDpiAwareness();
    if (!EnsureClassRegistered()) {
        return Err(ErrorCode::IoFailure, "the window class could not be registered");
    }

    // The client size is what the caller asked for, so the *window* rect has to be computed
    // from it -- borders and a title bar are outside the client area. ForDpi and not the plain
    // AdjustWindowRectEx, because the plain one assumes the system DPI of a process that is
    // not per-monitor aware, which this one now is.
    RECT rect{0, 0, static_cast<LONG>(description.size.width),
              static_cast<LONG>(description.size.height)};
    const UINT dpi = GetDpiForSystem();
    if (AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, 0, dpi) == 0) {
        MONARC_LOG(LogCategories::Window, Warning,
                   "AdjustWindowRectExForDpi failed with Win32 error {}; asking for a window "
                   "rect of the client size instead",
                   GetLastError());
    }

    wchar_t title[kMaxTitleWide];
    ToWideTitle(description.title, title);

    Window window;

    // **Counted before the call and not after it, because `WM_NCCREATE` installs the
    // back-pointer from inside `CreateWindowExW`** -- so every message from that moment on
    // reaches `WindowPlatform`'s handlers, `WM_DESTROY` included. A creation Windows abandons
    // after `WM_CREATE` delivers one for a window that had not been counted yet, and
    // `OnDestroyed`'s `MONARC_CHECK(g_liveWindows > 0)` then aborts a Debug build on a failure
    // the caller is entitled to receive as an `Err`.
    //
    // **Measured, and the two abort points differ.** With the count taken after the call, a
    // `WM_CREATE` answered `-1` gives
    // `[assert] (g_liveWindows > 0) a window was destroyed that was never counted as live`
    // and the suite stops. A `WM_NCCREATE` answered `FALSE` does **not**: Windows sends
    // `WM_NCDESTROY` without a `WM_DESTROY`, so `OnDestroyed` never runs and the count reads 0
    // before and after -- probed, one line per attempted window. So the reachable half is the
    // `WM_CREATE` one, and nothing in this file handles `WM_CREATE`: the abort would have to be
    // Windows' own.
    //
    // Restored rather than decremented on the failure path below, which is what makes it right
    // for both: whether or not the handler ran, the count ends where it started.
    const u32 liveBefore = g_liveWindows;
    ++g_liveWindows;

    const HWND handle = CreateWindowExW(
        0, kClassName, title, kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
        GetModuleHandleW(nullptr), &window);
    if (handle == nullptr) {
        const DWORD error = GetLastError();
        MONARC_LOG(LogCategories::Window, Error, "CreateWindowExW failed with Win32 error {}", error);
        // Restored rather than decremented, which is what makes it right whether or not a
        // `WM_DESTROY` arrived: the handler's decrement and this assignment cannot double up.
        g_liveWindows = liveBefore;
        // The class was registered above for a window that does not exist, so it is released
        // again here -- the count is back to whatever it was, so this unregisters only if there
        // were no windows before this attempt.
        ReleaseClassIfUnused();
        return Err(ErrorCode::IoFailure, "the platform refused to create the window");
    }

    window.m_nativeHandle = handle;

    ShowWindow(handle, SW_SHOW);
    // The size *after* showing, and read from the platform: a window manager decides the final
    // client size, and this is the number a swapchain is created from.
    window.m_clientSize = QueryClientSize(handle);

    // **Creation and showing send WM_SIZE synchronously, so the handler has already recorded a
    // `Resized` event by now -- and it is dropped here.** `Window::Events()` promises "what
    // happened during the most recent pump", and a window that reported a resize before its
    // owner had pumped once would make that promise false; worse, a frame loop reading it would
    // recreate the swapchain on frame one, at the size it had just been created with. The size
    // itself is not lost: `ClientSize()` above is where a caller reads it.
    window.m_eventCount = 0;

    MONARC_LOG(LogCategories::Window, Info,
               "window opened | asked for {}x{} client | became {}x{} | system DPI {} | style "
               "0x{:x}",
               description.size.width, description.size.height, window.m_clientSize.width,
               window.m_clientSize.height, dpi, static_cast<u32>(kWindowStyle));

    // Moved out, which re-points the native window's back-pointer at the destination --
    // `Window`'s move constructor calls `Rebind` for exactly this. The local `window` was the
    // owner while `CreateWindowExW` ran, because the messages it sends arrive before this
    // function has anywhere else to put it.
    return window;
}

void WindowPlatform::Destroy(Window& window) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        // Already closed, moved from, never opened -- **or destroyed by the system**, which is
        // the case worth the call below. `OnDestroyed` has decremented the count by then and
        // could not unregister the class, because a window of it still existed at the time; so
        // this is the first moment the platform will accept the unregister. `~Window` always
        // reaches here, which is what makes it happen at all.
        ReleaseClassIfUnused();
        return;
    }

    // DestroyWindow sends WM_DESTROY synchronously, so `OnDestroyed` has already cleared
    // `window.m_nativeHandle` and decremented the count by the time this returns -- which is
    // what makes a second `Destroy` a no-op without a second flag to keep in step.
    if (DestroyWindow(handle) == 0) {
        MONARC_LOG(LogCategories::Window, Warning, "DestroyWindow failed with Win32 error {}",
                   GetLastError());
        // WM_DESTROY never arrived, so the bookkeeping it would have done has to happen here.
        // Clearing the handle is not optional either: holding one that is not usable would
        // mean a destructor retrying the same failing call, which is the shape of loop a
        // teardown path must not have.
        //
        // **Guarded on the handle, because `DestroyWindow` can fail *after* delivering
        // `WM_DESTROY`** -- it is documented to return zero on failure and says nothing about
        // how far it got. `OnDestroyed` is what clears the handle, so a non-null one here is
        // exactly "the handler has not run". Without the guard the class reference count is
        // decremented twice for one window; `MONARC_CHECK(g_liveWindows > 0)` catches that only
        // when the count was 1, so with a second window alive it would silently drift down and
        // release the class while a window of it still existed.
        if (HandleOf(window) != nullptr) {
            OnDestroyed(window);
        }
    }

    // After the window is gone, which is the only point `UnregisterClassW` accepts.
    ReleaseClassIfUnused();
}

void WindowPlatform::Rebind(Window& window) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return;
    }
    SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
}

void WindowPlatform::Pump(Window& window, bool wait) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        // Nothing to pump, and nothing to clear either: a closed window's last events stay
        // readable, which is what lets a loop see the close request that closed it.
        //
        // **Load-bearing, and two experiments say how.** Forced past -- `if (false && handle
        // == nullptr)` -- the window suite hangs rather than failing: `WaitMessage` on a
        // closed window blocks in the kernel forever, and nothing in the process wakes it.
        // That is why both test suites now carry a CTest `TIMEOUT` (CMake/MonarcTest.cmake);
        // without one, CTest's own default is 1500 seconds and a hang costs 25 minutes per
        // preset before it is even reported.
        //
        // Deleting the guard outright does not compile, which is a small guarantee nobody
        // asked for: `handle` is not referenced again in this function -- `PeekMessageW` is
        // deliberately given `nullptr` -- so `/W4 /WX` answers with `warning C4189: 'handle':
        // local variable is initialized but not referenced`, fatal through `error C2220`.
        // Measured on MSVC 19.51.
        return;
    }

    // Cleared before draining, so `Events()` describes this pump and not an accumulation.
    // `Window::PumpEvents` documents that.
    window.m_eventCount = 0;

    if (wait) {
        // Blocks in the kernel until there is a message. Returns false only on error, in which
        // case falling through to the drain is right: PeekMessageW will find nothing and the
        // caller comes back.
        if (WaitMessage() == 0) {
            MONARC_LOG(LogCategories::Window, Warning, "WaitMessage failed with Win32 error {}",
                       GetLastError());
        }
    }

    // **`nullptr` and not `handle`, deliberately.** Filtering by window would leave every
    // message for every *other* window this thread owns in the queue forever, and Win32 sends
    // a thread messages that belong to no window at all. A process with one window is the case
    // today; a queue that grows without bound is not a bug worth leaving for the second one.
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        // TranslateMessage turns key-down pairs into WM_CHAR. Nothing reads WM_CHAR yet --
        // input beyond closing and resizing is excluded from A3 by name -- and it is called
        // because the alternative is a text field in a later phase that silently receives no
        // characters, with the cause four phases back.
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

RHI::SurfaceDescription WindowPlatform::Surface(const Window& window) {
    RHI::SurfaceDescription surface{};
    const HWND              handle = HandleOf(window);
    if (handle == nullptr) {
        return surface;
    }
    // The module the window class was registered with, which is what
    // `VkWin32SurfaceCreateInfoKHR::hinstance` is specified to be. Read from the window rather
    // than from `GetModuleHandleW(nullptr)` again, so the two cannot disagree if a later change
    // registers the class from a DLL.
    surface.nativeDisplay = reinterpret_cast<void*>(GetWindowLongPtrW(handle, GWLP_HINSTANCE));
    surface.nativeWindow  = handle;
    return surface;
}

bool WindowPlatform::HasInteractiveSession() {
    const HWINSTA station = GetProcessWindowStation();
    if (station == nullptr) {
        return false;
    }

    USEROBJECTFLAGS flags{};
    DWORD           returned = 0;
    if (GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &returned) == 0) {
        MONARC_LOG(LogCategories::Window, Warning,
                   "GetUserObjectInformationW(UOI_FLAGS) failed with Win32 error {}; assuming "
                   "no interactive session",
                   GetLastError());
        return false;
    }
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

bool WindowPlatform::IsClassRegistered() {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    return GetClassInfoExW(GetModuleHandleW(nullptr), kClassName, &existing) != 0;
}

// ---------------------------------------------------------------------------------------
// WindowTestHooks
//
// Nothing in shipped code calls anything below this line. WindowPlatform.h says why they are
// here rather than in a test directory, and the short version is that it keeps `<Windows.h>`
// out of every test file in the repository.
// ---------------------------------------------------------------------------------------

void WindowTestHooks::RequestClientSize(Window& window, RHI::Extent2D size) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return;
    }

    RECT rect{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
    const UINT dpi = GetDpiForWindow(handle);
    if (AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, 0, dpi) == 0) {
        MONARC_LOG(LogCategories::Window, Warning, "AdjustWindowRectExForDpi failed with Win32 error {}",
                   GetLastError());
    }
    if (SetWindowPos(handle, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == 0) {
        MONARC_LOG(LogCategories::Window, Warning, "SetWindowPos (resize) failed with Win32 error {}",
                   GetLastError());
    }
}

void WindowTestHooks::MoveTo(Window& window, i32 x, i32 y) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return;
    }
    if (SetWindowPos(handle, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) == 0) {
        MONARC_LOG(LogCategories::Window, Warning, "SetWindowPos (move) failed with Win32 error {}",
                   GetLastError());
    }
}

void WindowTestHooks::Minimise(Window& window) {
    if (const HWND handle = HandleOf(window); handle != nullptr) {
        ShowWindow(handle, SW_MINIMIZE);
    }
}

void WindowTestHooks::Maximise(Window& window) {
    if (const HWND handle = HandleOf(window); handle != nullptr) {
        ShowWindow(handle, SW_MAXIMIZE);
    }
}

void WindowTestHooks::Restore(Window& window) {
    if (const HWND handle = HandleOf(window); handle != nullptr) {
        ShowWindow(handle, SW_RESTORE);
    }
}

void WindowTestHooks::DestroyNatively(Window& window) {
    if (const HWND handle = HandleOf(window); handle != nullptr) {
        // Straight to the platform, so the `WM_DESTROY` arrives without `WindowPlatform::Destroy`
        // having been involved -- which is the whole point of the hook.
        if (DestroyWindow(handle) == 0) {
            MONARC_LOG(LogCategories::Window, Warning,
                       "DestroyWindow failed with Win32 error {}", GetLastError());
        }
    }
}

void WindowTestHooks::RequestClose(Window& window) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return;
    }
    // Posted rather than sent, so it goes through the queue the next pump drains -- which is
    // the path the close button takes. `SendMessageW` would dispatch it inline and prove
    // nothing about the pump.
    if (PostMessageW(handle, WM_CLOSE, 0, 0) == 0) {
        MONARC_LOG(LogCategories::Window, Warning, "PostMessageW(WM_CLOSE) failed with Win32 "
                                                   "error {}",
                   GetLastError());
    }
}

WindowTestHooks::Foreground WindowTestHooks::BringToForeground(Window& window) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return Foreground::NotOnTop;
    }

    // TOPMOST first, then the foreground request. The order matters: `SetForegroundWindow` can
    // be refused outright -- Windows locks the foreground against processes that have not had
    // recent user input -- where the z-order change cannot, so putting the window on top is
    // the part that reliably works and the activation is the part that may not.
    if (SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW) == 0) {
        MONARC_LOG(LogCategories::Window, Warning, "SetWindowPos (topmost) failed with Win32 error {}",
                   GetLastError());
        return Foreground::NotOnTop;
    }
    if (SetForegroundWindow(handle) == 0) {
        MONARC_LOG(LogCategories::Window, Warning,
                   "SetForegroundWindow was refused; the window is topmost but not activated");
        // Topmost is what a screen capture actually needs, so this is reported and not fatal:
        // the caller decides whether to trust the capture, and the outcome says which happened.
        // **Reported as its own outcome and not as the same answer success gives**, which is
        // what a bool made it -- see the declaration.
        return Foreground::TopmostOnly;
    }
    BringWindowToTop(handle);
    return Foreground::Activated;
}

Status WindowTestHooks::CaptureScreenPixel(const Window& window, i32 x, i32 y, u8 (&out)[4]) {
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;

    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "a screen pixel cannot be captured for a window that is not open");
    }

    RECT client{};
    if (GetClientRect(handle, &client) == 0) {
        return Err(ErrorCode::IoFailure, "GetClientRect failed");
    }
    if (x < 0 || y < 0 || x >= client.right - client.left || y >= client.bottom - client.top) {
        return Err(ErrorCode::InvalidArgument,
                   "the requested pixel is outside the window's client area");
    }

    POINT point{x, y};
    if (ClientToScreen(handle, &point) == 0) {
        return Err(ErrorCode::IoFailure, "ClientToScreen failed");
    }

    // Anything the GPU or GDI has queued has to have reached the screen before it is read.
    // GdiFlush covers this thread's own batched GDI; the swapchain's frames are the caller's
    // problem, and the swapchain test presents several before it captures.
    GdiFlush();

    // The *screen* DC. See `CaptureScreenPixel`'s declaration for why not the window's.
    const HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return Err(ErrorCode::IoFailure, "GetDC(nullptr) returned no screen device context");
    }

    // A one-pixel DIB section rather than GetPixel(screen, ...) straight off the screen DC:
    // GetPixel goes through the same path but returns a COLORREF with no alpha, and a
    // BitBlt into memory is what CAPTUREBLT can be asked for.
    BITMAPINFO info{};
    info.bmiHeader.biSize     = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth    = 1;
    // Negative height for a top-down bitmap. With a positive height the single row is the same
    // row either way, and it is written negative so that this does not become wrong the day
    // someone captures more than one pixel with it.
    info.bmiHeader.biHeight      = -1;
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void*         pixels = nullptr;
    const HDC     memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap =
        memory != nullptr
            ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0)
            : nullptr;

    Status result{};
    if (memory == nullptr || bitmap == nullptr || pixels == nullptr) {
        result = Err(ErrorCode::IoFailure,
                     "a one-pixel device-independent bitmap could not be created");
    } else {
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        // CAPTUREBLT so that layered windows -- and the composited desktop -- are included.
        // Without it the read can miss anything the compositor draws, which is exactly what a
        // Vulkan swapchain's contents are.
        if (BitBlt(memory, 0, 0, 1, 1, screen, point.x, point.y, SRCCOPY | CAPTUREBLT) == 0) {
            result = Err(ErrorCode::IoFailure, "BitBlt from the screen device context failed");
        } else {
            const auto* bytes = static_cast<const u8*>(pixels);
            out[0]            = bytes[0];
            out[1]            = bytes[1];
            out[2]            = bytes[2];
            out[3]            = bytes[3];
        }
        SelectObject(memory, previous);
    }

    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    ReleaseDC(nullptr, screen);
    return result;
}

WindowTestHooks::PointOwner WindowTestHooks::WindowAtClientPoint(const Window& window, i32 x,
                                                                 i32 y) {
    PointOwner owner{};

    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return owner;
    }

    POINT point{x, y};
    if (ClientToScreen(handle, &point) == 0) {
        return owner;
    }

    // `WindowFromPoint` and not `GetForegroundWindow`: the question is what is *drawn* at that
    // pixel, and a topmost layered overlay can be over the window without being the foreground
    // one. That is precisely the case this hook was written for.
    const HWND at = WindowFromPoint(point);
    if (at == nullptr) {
        return owner;
    }
    owner.isOurs = at == handle;

    wchar_t   wide[64]  = {};
    const int wideChars = GetClassNameW(at, wide, static_cast<int>(std::size(wide)));
    if (wideChars > 0) {
        // Truncating rather than refusing, and always null-terminating: this is a log line, and
        // a truncated class name still identifies the tenant where an empty one does not.
        //
        // **The loop is what makes that true, and the one-call form did not.** With `-1` as the
        // source length `WideCharToMultiByte` requires the whole string *and* its terminator to
        // fit: it returns 0 and sets `ERROR_INSUFFICIENT_BUFFER` rather than writing what it
        // can, so a class name whose UTF-8 form exceeded 62 bytes produced exactly the empty
        // string this comment says it avoids. Reachable for a non-ASCII class name, since
        // `GetClassNameW` hands back up to 63 UTF-16 code units and one can become four UTF-8
        // bytes. `ToWideTitle` above gets the same problem right with an explicit two-call
        // fallback; this needs a loop rather than a second call because the byte length is not
        // a function of the character count.
        //
        // Dropping one UTF-16 code unit per attempt cannot cut a UTF-8 sequence in half. It can
        // leave a lone surrogate at the end, which converts to the replacement character rather
        // than failing -- `WC_ERR_INVALID_CHARS` is deliberately not asked for, because a
        // question mark in a log line is better than no line.
        constexpr int kCapacity = static_cast<int>(std::size(owner.className)) - 1;
        int           attempt   = wideChars;
        int           written   = 0;
        while (attempt > 0) {
            written = WideCharToMultiByte(CP_UTF8, 0, wide, attempt, owner.className, kCapacity,
                                          nullptr, nullptr);
            if (written > 0) {
                break;
            }
            --attempt;
        }
        // Terminated here rather than by the call, which is why `attempt` is a character count
        // and not `-1`: the explicit-length form does not write a terminator.
        owner.className[written > 0 ? written : 0] = '\0';
    }
    return owner;
}

WindowTestHooks::MonitorInfo WindowTestHooks::Monitors(const Window& window) {
    MonitorInfo info{};
    info.monitorCount = static_cast<u32>(GetSystemMetrics(SM_CMONITORS));
    info.virtualLeft  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    info.virtualTop   = GetSystemMetrics(SM_YVIRTUALSCREEN);

    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return info;
    }

    const HMONITOR monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return info;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo) != 0) {
        info.left   = monitorInfo.rcMonitor.left;
        info.top    = monitorInfo.rcMonitor.top;
        info.right  = monitorInfo.rcMonitor.right;
        info.bottom = monitorInfo.rcMonitor.bottom;
    }

    // `GetDpiForWindow` and not `GetDpiForMonitor`: the latter lives in shellscalingapi.h and
    // needs Shcore.lib on the link line, where this one is in user32 and is already linked.
    // For a per-monitor-v2 window they answer the same thing -- the DPI of the monitor the
    // window is on -- which is the number the caller is after.
    info.dpi = GetDpiForWindow(handle);
    return info;
}

}  // namespace Monarc::Host::Detail
