#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Swapchain.h>
#include <Monarc/RHI/Types.h>

#include <span>

namespace Monarc::Host {

class Window;

namespace Detail {

/// Everything about a window that needs the platform's headers, and the one thing permitted to
/// reach into a `Window`'s private state.
///
/// Declared here and defined in Private/WindowPlatform.h so that this header names it in a
/// friend declaration without naming a Win32 type. `VulkanDeviceFactory` in
/// Monarc/RHI/Vulkan/VulkanDevice.h is the same arrangement for the same reason: a friend
/// *function* declaration would have to spell its parameters, and `HWND` may not appear here
/// (ADR-0016).
struct WindowPlatform;

}  // namespace Detail

/// What a window should be created as.
///
/// The size is an RHI::Extent2D rather than a pair of integers of its own, because it is the
/// same quantity a swapchain is created from -- a window's client area and its swapchain
/// extent must agree, and giving them two unrelated types would invite a conversion where
/// there should be an identity.
struct WindowDescription {
    /// Client-area size in physical pixels, not DPI-scaled units. Window creation makes the
    /// process per-monitor DPI aware, so a request here is what the client area actually
    /// becomes -- to within what the window manager is willing to give, which is why
    /// `Window::ClientSize()` is what a swapchain is created from and this is only a request.
    RHI::Extent2D size = {1280, 720};

    /// Non-owning; must outlive the Create call. A string literal at the call site is the
    /// intended use, matching Error::message in Monarc/Core/Error.h.
    const char* title = "Monarc";
};

/// What happened to a window.
enum class WindowEventKind : u32 {
    /// The client area changed size. `WindowEvent::size` is the size it became.
    Resized = 0,

    /// The platform asked for the window to close -- the close button, Alt+F4, or a system
    /// shutdown. **The window is still open**: this is a request, and closing is the caller's
    /// decision, made by calling `Destroy` or by letting the window go out of scope. A window
    /// that closed itself would take its swapchain's surface with it while a frame was in
    /// flight.
    CloseRequested,
};

/// The enumerator's own spelling, for logs and test failures. Never nullptr; a value outside
/// the enumerator set gets a name of its own, for the reason `RHI::ToString(RHI::Format)` in
/// Monarc/RHI/Types.h gives.
[[nodiscard]] const char* ToString(WindowEventKind kind);

/// One thing that happened to a window since the last pump.
struct WindowEvent {
    WindowEventKind kind = WindowEventKind::Resized;

    /// The client size the window **actually became**, read back from the platform rather than
    /// echoed from what was requested. Meaningful for `Resized` and zero otherwise.
    ///
    /// That distinction is the whole content of the event: a window manager decides the final
    /// size -- borders, snapping, minimum tracking size, a maximise to the monitor's work area
    /// -- so a resize event carrying the requested size would be a swapchain extent that does
    /// not match its surface, which is `VUID-VkSwapchainCreateInfoKHR-imageExtent-01274`.
    RHI::Extent2D size = {};
};

/// How many events one pump can report.
///
/// **Two, because resizes are coalesced and there are two kinds of event.** A window being
/// dragged by its corner produces hundreds of `WM_SIZE` messages per pump and only the last
/// one describes a size anything could be created at, so the pump keeps one `Resized` event
/// carrying the size the window ended at. Close is a single fact. So two is the whole set, not
/// a buffer that can overflow -- and being a fixed array is what keeps pumping allocation-free
/// on a path a frame runs.
///
/// Coalescing loses the intermediate sizes. Nothing wants them: a swapchain recreated at every
/// intermediate size during a drag is the naive implementation of resize and is visibly worse
/// than one recreated at the size the drag stopped at.
inline constexpr usize kMaxWindowEvents = 2;

/// An operating-system window.
///
/// Move-only and owning, in the shape Platform::Library uses for the same reason: the thing
/// held is an OS resource that must be released exactly once, and a copy would release it
/// twice.
///
/// **The frame loop this is shaped for.** `PumpEvents`, then read `CloseRequested()` and
/// `Events()`, then `ClientSize()`; a size of `0 x 0` means the window is minimised and the
/// loop must park -- pump messages, present nothing -- which is what `WaitForEvents` is for.
/// `Surface()` is what a swapchain is created from, once.
///
/// **Not thread-safe, and the platform makes that structural rather than a rule.** Win32
/// delivers a window's messages to the thread that created it, so `PumpEvents` on any other
/// thread would drain a queue that is never filled. Create and pump a window on one thread.
class Window {
public:
    Window() noexcept = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    /// Creates a window and shows it.
    ///
    /// Makes the process per-monitor DPI aware on the first call, which is why the returned
    /// client size is in physical pixels. That is a process-wide setting and is done once;
    /// a process that had already chosen its own awareness keeps it, and the log says so.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for an empty `description.size`,
    /// `ErrorCode::Unsupported` when there is no interactive session to put a window in --
    /// see `Detail::WindowPlatform::HasInteractiveSession` for what that means mechanically --
    /// and `ErrorCode::IoFailure` when the platform refused to register the class or make the
    /// window. Every message is a string literal; the platform's own error code goes to the
    /// log beside it.
    [[nodiscard]] static Result<Window> Create(const WindowDescription& description);

    /// Closes the window if it is open. Safe to call unconditionally, and more than once.
    ///
    /// **Anything created from `Surface()` must be destroyed first.** A `VkSurfaceKHR` names
    /// this window's native handle, and destroying the window under a live surface is
    /// undefined -- the same ordering rule `VulkanSwapchain`'s class comment states from the
    /// other side. Nothing here can enforce it: a window knows nothing about surfaces, which
    /// is what keeps this module free of any graphics API.
    void Destroy();

    [[nodiscard]] bool IsOpen() const { return m_nativeHandle != nullptr; }

    /// Drains the platform's message queue for this thread and translates what it finds into
    /// events. Never blocks. Clears the events the previous pump reported.
    ///
    /// A no-op on a window that is not open, so a loop does not have to guard it.
    void PumpEvents();

    /// Blocks until the platform has a message for this thread, then pumps.
    ///
    /// **What a parked frame loop calls, and the reason it exists rather than the loop
    /// spinning on `PumpEvents`.** A minimised window has no swapchain to present to, so the
    /// loop has nothing to do and no frame to pace it; pumping in a tight loop would spend a
    /// core doing nothing. This waits in the kernel and returns when something happens --
    /// including the restore that ends the parking.
    ///
    /// It blocks indefinitely, which is right for a program a person minimised and walked away
    /// from, and is the reason a *test* drives the restore itself rather than calling this.
    /// A no-op on a window that is not open, so it cannot become an unbreakable wait.
    void WaitForEvents();

    /// What happened during the most recent pump, in the order it happened. Valid until the
    /// next `PumpEvents` or `WaitForEvents`.
    ///
    /// **Exact for one window per thread, and approximate for two.** A pump drains the whole
    /// thread's message queue by design -- filtering by window would leave every other
    /// window's messages in it forever, and Win32 sends a thread messages belonging to no
    /// window at all -- but it clears only the pumped window's own event count. So with two
    /// windows on a thread the one that is not pumped accumulates events across the other's
    /// pumps, and an event posted to A can be consumed during B's pump and then cleared by A's
    /// next pump before A ever reads it.
    ///
    /// `kMaxWindowEvents` and the coalescing in the handler bound what that can cost: at most
    /// two entries, one per kind, latest of each. And the fact that actually matters is not
    /// lost either way -- `CloseRequested()` is sticky precisely so that a loop checking it
    /// once per frame cannot miss a request reported by a pump it did not look at. What a
    /// second window can lose is the *event entry*, not the request.
    ///
    /// Two windows exist in both of this module's suites, so this is a real configuration and
    /// not a hypothetical; what it is not is a bug worth a per-window queue while nothing needs
    /// the exactness.
    [[nodiscard]] std::span<const WindowEvent> Events() const;

    /// The client area's current size in physical pixels.
    ///
    /// **`0 x 0` while the window is minimised, and that is a state rather than an error.** A
    /// swapchain cannot be created from it (`ISwapchain::Recreate` refuses one), so this is
    /// the value a frame loop parks on. `RHI::Extent2D::IsEmpty()` is the test.
    [[nodiscard]] RHI::Extent2D ClientSize() const { return m_clientSize; }

    /// Whether the platform has asked for this window to close.
    ///
    /// Sticky: set by the first `WindowEventKind::CloseRequested` and never cleared, so a loop
    /// that checks it once per frame cannot miss one reported by a pump it has already looked
    /// at. Cleared only by `Destroy`, which makes the question moot.
    [[nodiscard]] bool CloseRequested() const { return m_closeRequested; }

    /// The native handles a graphics surface is created from.
    ///
    /// **An `RHI::SurfaceDescription` rather than a bare `void*` pair of this module's own,
    /// and it is the same argument `WindowDescription::size` makes for `RHI::Extent2D`**: the
    /// destination type is the one the RHI already has, so there is no place to copy two
    /// `void*` into two other `void*` and get them the wrong way round. This module already
    /// depends on `Monarc.RHI` publicly.
    ///
    /// Both handles are null on a window that is not open, which `SurfaceDescription::IsEmpty`
    /// reports and surface creation refuses.
    [[nodiscard]] RHI::SurfaceDescription Surface() const;

private:
    friend struct Detail::WindowPlatform;

    // A Win32 HWND on Windows, held as void* so this header names no platform type and its
    // layout does not depend on which platform is building it (ADR-0016). It is also what is
    // handed to vkCreateWin32SurfaceKHR, so keeping it opaque is what allows the one file in
    // the backend that includes <Windows.h> to be a per-platform .cpp rather than a header
    // anybody could include.
    void* m_nativeHandle = nullptr;

    /// The client size the platform last reported, in physical pixels. Written by the
    /// platform's message handler, never computed here.
    RHI::Extent2D m_clientSize = {};

    bool m_closeRequested = false;

    /// Coalesced, and refilled by every pump. `kMaxWindowEvents` says why two is the whole
    /// set.
    WindowEvent m_events[kMaxWindowEvents] = {};
    usize       m_eventCount               = 0;
};

}  // namespace Monarc::Host
