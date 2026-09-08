#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/Host/Window.h>
#include <Monarc/RHI/Types.h>

namespace Monarc::Host::Detail {

/// Everything about a window that needs the platform's own headers.
///
/// **A private header naming no platform type, so that `Private/Window.cpp` can be the
/// platform-neutral half the A3 plan asks for.** `Private/Platform/Windows/Window.cpp` defines
/// every member below and is the only file in this module that includes `<Windows.h>`; the
/// neutral half forwards to them and owns the ownership semantics -- the destructor, the moves
/// and the accessors -- which are the parts that are the same on every platform.
///
/// **Why these are static members of a befriended struct rather than free functions.** They
/// need `Window`'s private state: the platform's message handler writes the client size, the
/// close flag and the event buffer, and `Create` fills in the native handle. `Window` names
/// this one struct as a friend, which is the house form -- `VulkanDeviceFactory` is
/// befriended by `VulkanDevice` for exactly the same reason, and it keeps "who may reach into
/// a Window" answerable by grepping for one name.
struct WindowPlatform {
    /// Creates a native window and returns a `Window` owning it. `Window::Create` forwards
    /// here and does nothing else.
    [[nodiscard]] static Result<Window> Create(const WindowDescription& description);

    /// Destroys `window`'s native window, if it has one, and clears its state.
    static void Destroy(Window& window);

    /// Re-points the native window's back-pointer at `window`.
    ///
    /// **The one thing a move has to do that is not moving members, and it is not optional.**
    /// The platform's message handler finds its `Window` through a pointer stored on the native
    /// window; a moved-to `Window` lives at a different address, so without this the handler
    /// would go on writing the client size and the close flag into the *moved-from* object --
    /// which is empty, and whose storage the caller may have reused. A defaulted move would
    /// compile and produce exactly that.
    ///
    /// A no-op on a window with no native handle, which is what a moved-from one is.
    static void Rebind(Window& window);

    /// Drains the message queue and fills `window`'s event buffer. `wait` blocks until at
    /// least one message arrives before draining.
    static void Pump(Window& window, bool wait);

    /// `window`'s native handles, as a surface description.
    [[nodiscard]] static RHI::SurfaceDescription Surface(const Window& window);

    /// `window`'s native window handle as an opaque pointer, or null.
    ///
    /// **The whole of the read access this struct's friendship is used for, and it is here
    /// rather than local to the platform file because friendship does not reach a free
    /// function.** Every Win32 helper in Private/Platform/Windows/Window.cpp that needs the
    /// `HWND` goes through this and casts, including `WindowTestHooks` below -- which is not a
    /// friend of `Window` and does not need to be.
    [[nodiscard]] static void* NativeHandle(const Window& window);

    // The three things a platform's message handler reports into a window.
    //
    // **Static members rather than the handler writing the fields directly, because a
    // handler is a free function and friendship does not reach one.** Naming them also means
    // the entire mutable surface of a `Window` is these three plus `Create` and `Pump` --
    // which is a shorter list to audit than "whatever the platform file touches", and it is
    // where the invariants live: `OnResized` is the only writer of the client size, and it
    // records the event from the same value, so `Window::ClientSize()` and
    // `WindowEvent::size` cannot disagree.

    /// The client area became `size`. Records a coalesced `WindowEventKind::Resized`.
    static void OnResized(Window& window, RHI::Extent2D size);

    /// The platform asked for the window to close. Sets the sticky flag and records a
    /// `WindowEventKind::CloseRequested`.
    static void OnCloseRequested(Window& window);

    /// The native window has been destroyed and its handle is no longer valid.
    static void OnDestroyed(Window& window);

    /// Whether this process can put a window on a screen at all.
    ///
    /// **The mechanical meaning on Windows: the process's window station has `WSF_VISIBLE`.**
    /// `GetProcessWindowStation` plus `GetUserObjectInformationW(..., UOI_FLAGS, ...)` is the
    /// documented way to ask, and a *service* -- or anything else running in a non-interactive
    /// session -- gets a window station without that flag. Such a process can still call
    /// `CreateWindowExW` successfully, which is precisely why the test needs this: the window
    /// would exist on a desktop nobody is looking at, so a case asserting anything about what
    /// is on screen would be asserting about nothing.
    ///
    /// It is not a claim about CI. GitHub's Windows runners were not measured for this, and
    /// whichever way they answer is a finding rather than an assumption -- the window tests
    /// skip with 77 when this is false, which puts the answer in CTest's output.
    [[nodiscard]] static bool HasInteractiveSession();

    /// Whether Monarc's window class is currently registered with the platform.
    ///
    /// **A query with no shipped caller, and its caller is the test the plan names**: "a
    /// window created and destroyed leaves no registered class behind". Registration is
    /// reference-counted against the number of live windows, and nothing else in the process
    /// can observe that counter -- so without this the test would be asserting on a state it
    /// cannot see, which is the shape of assertion this branch has already deleted 45 of.
    ///
    /// In `Private/` rather than in the public header for that reason: it is not part of what
    /// a window is, and a test target has this directory on its include path
    /// (CMake/MonarcTest.cmake) where nothing outside the module does. `LoaderTables.h` in
    /// Monarc.RHI.Vulkan is the precedent.
    [[nodiscard]] static bool IsClassRegistered();
};

/// Drives a window from code, and reads back what is actually on the screen.
///
/// **Nothing in shipped code calls any of these, and that is stated rather than left to be
/// noticed.** They exist because Phase A3 Task 4's verification has to be mechanical: the
/// plan's own verification step is "resize it, maximise it, minimise and restore it, and drag
/// it between the two monitors", which is a person at a keyboard. A person cannot be a
/// regression test, and "I ran it and it looked right" is not the standard the rest of this
/// branch holds.
///
/// **They live here, beside the shipping platform functions, rather than in the test
/// directories -- and that buys something specific: no test file in this repository includes
/// `<Windows.h>`.** `SetWindowPos`, `ShowWindow` and a screen `BitBlt` all need it, and
/// putting them in a test would put Win32 in two places in this module and outside the
/// per-platform directory ADR-0016 confines it to. Two test binaries need them (the
/// device-free window tests and the device-required swapchain tests), so a header in
/// `Private/` is also the only place both can reach -- `LoaderTables.h`'s argument, one module
/// over.
///
/// A separate struct from `WindowPlatform` above so the distinction is greppable: everything
/// in that one has a shipped caller and nothing in this one does.
struct WindowTestHooks {
    /// Asks the platform for a client area of `size`. The window may become another size, and
    /// `Window::ClientSize()` after a pump is what it actually became -- which is the fact the
    /// resize test exists to assert.
    static void RequestClientSize(Window& window, RHI::Extent2D size);

    /// Moves the window's top-left corner to `x, y` in virtual-screen coordinates, keeping its
    /// size. Negative coordinates are how the second monitor is reached on this machine:
    /// `\\.\DISPLAY5` sits at `(-1920, 0)`.
    static void MoveTo(Window& window, i32 x, i32 y);

    static void Minimise(Window& window);
    static void Maximise(Window& window);
    static void Restore(Window& window);

    /// Destroys the native window behind `window`'s back, the way the system does at a logoff
    /// or when a parent goes away -- rather than through `Window::Destroy`.
    ///
    /// **The only way to reach the other of the two paths a `WM_DESTROY` arrives by.** A
    /// window's class registration is reference-counted, and until a mutation found otherwise
    /// the count was decremented in `Window::Destroy` -- so a system-initiated destruction
    /// drifted the count upwards and left the class registered for the life of the process.
    /// The count moved into the message handler; this is what tests that it did.
    static void DestroyNatively(Window& window);

    /// Asks the platform to close the window, exactly as its close button does.
    ///
    /// **A posted message and not a direct call into `Window`, which is what makes it worth
    /// having**: the close path a person takes goes through the platform's message queue and
    /// the handler that decides *not* to destroy the window, and a test that set the flag
    /// itself would exercise neither. It is what proves close is a request.
    static void RequestClose(Window& window);

    /// Brings the window to the front and makes it topmost, so a screen capture of its client
    /// rect reads its pixels rather than whatever was over it. Returns false when the platform
    /// refused, which it may: a console process is not always allowed to take the foreground.
    [[nodiscard]] static bool BringToForeground(Window& window);

    /// Reads one pixel from the **screen**, at `x, y` within `window`'s client area, into
    /// `out` as blue, green, red, alpha.
    ///
    /// **From the screen's device context and not the window's, which is the trap worth
    /// naming.** `PrintWindow` and a `BitBlt` from the window's own DC return black for a
    /// Vulkan window on Windows: the swapchain's contents never enter the window's GDI
    /// surface, because the desktop compositor puts them on screen directly. Capturing from
    /// the screen DC reads the composited result, which is the thing a person would see.
    ///
    /// The bytes are blue, green, red, alpha because that is what a 32-bit `BI_RGB` DIB holds
    /// -- `0x00RRGGBB` little-endian -- which is the same order a `B8G8R8A8_UNORM` swapchain
    /// image reads back in. That is a coincidence of two conventions agreeing, not one fact,
    /// and both assertions state their order.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for a window that is not open or a point
    /// outside its client area, and `ErrorCode::IoFailure` when the platform's capture failed.
    [[nodiscard]] static Status CaptureScreenPixel(const Window& window, i32 x, i32 y,
                                                   u8 (&out)[4]);

    /// Whose pixels are actually on screen at `x, y` within `window`'s client area.
    struct PointOwner {
        /// True when the topmost window at that screen point is `window` itself.
        bool isOurs = false;

        /// The window class of whatever is there, null-terminated and truncated to fit. For a
        /// log line, so a capture that read someone else's pixels can say whose.
        char className[64] = {};
    };

    /// Asks the platform which window is on top at `x, y` within `window`'s client area.
    ///
    /// **This exists because a screen capture is an assertion about the desktop, and the
    /// desktop has other tenants.** The capture below read a stable
    /// `(106, 71, 35)` -- the expected bytes at 55% -- on every run for a while, and the cause
    /// was not Monarc: `WindowFromPoint` named a layered, topmost `Shell_SystemDim`, which is
    /// the overlay Windows puts over the whole desktop while a system security dialog is open.
    /// A test that asserted through that would be asserting about Windows' dimming.
    ///
    /// So the capture case asks this first, and reports rather than asserts when the answer is
    /// somebody else. That is not a widened tolerance: the bytes it does assert are still
    /// exact, and the reason it declined is named in the log.
    [[nodiscard]] static PointOwner WindowAtClientPoint(const Window& window, i32 x, i32 y);

    /// How many monitors the platform reports, and the bounding rectangle of the one `window`
    /// is currently on, in virtual-screen coordinates.
    ///
    /// What the monitor-drag case reads to decide whether there is a second monitor to drag to,
    /// and to assert that the drag actually landed on it. `monitorCount` of one means the case
    /// reports that it had nothing to test rather than asserting something vacuous.
    struct MonitorInfo {
        u32 monitorCount = 0;
        i32 left         = 0;
        i32 top          = 0;
        i32 right        = 0;
        i32 bottom       = 0;

        /// The DPI the platform reports for this monitor. 96 is unscaled. On this machine both
        /// monitors report the same value, which is why a drag between them does **not**
        /// exercise the DPI-change path -- the window tests read this and say so rather than
        /// implying otherwise.
        u32 dpi = 0;

        /// The top-left of the *virtual screen* -- the bounding box of every monitor. On a
        /// machine with one monitor this is that monitor's origin; on this one it is
        /// `(-1920, 0)`, because `\\.\DISPLAY5` sits to the left of the primary.
        ///
        /// **Here so that a test can move a window to a different monitor without naming
        /// one.** The screen-capture case needs that: Windows' `Shell_SystemDim` overlay
        /// covers the monitor a system dialog is on and not the others, so trying a second
        /// position is the difference between reading Monarc's pixels and reading Windows'.
        i32 virtualLeft = 0;
        i32 virtualTop  = 0;
    };

    [[nodiscard]] static MonitorInfo Monitors(const Window& window);
};

}  // namespace Monarc::Host::Detail
