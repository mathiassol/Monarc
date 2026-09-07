#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/RHI/Types.h>

namespace Monarc::Host {

/// What a window should be created as.
///
/// The size is an RHI::Extent2D rather than a pair of integers of its own, because it is the
/// same quantity a swapchain is created from -- a window's client area and its swapchain
/// extent must agree, and giving them two unrelated types would invite a conversion where
/// there should be an identity.
struct WindowDescription {
    /// Client-area size in physical pixels, not DPI-scaled units. Task 4 makes the process
    /// per-monitor DPI aware, so a request here is what the client area actually becomes.
    RHI::Extent2D size = {1280, 720};

    /// Non-owning; must outlive the Create call. A string literal at the call site is the
    /// intended use, matching Error::message in Monarc/Core/Error.h.
    const char* title = "Monarc";
};

/// An operating-system window.
///
/// Move-only and owning, in the shape Platform::Library uses for the same reason: the thing
/// held is an OS resource that must be released exactly once, and a copy would release it
/// twice.
///
/// **Phase A3 Task 1 creates no windows.** Create reports ErrorCode::Unsupported with a
/// message saying so; the message pump, resize and close events, and the native surface
/// handle Vulkan needs all arrive in Task 4. A stub that fails honestly is what "not yet"
/// looks like from a caller's side -- the alternative is handing back a Window that is not
/// one, which every caller would then have to be told about separately.
class Window {
public:
    Window() noexcept = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    /// Creates a window and shows it.
    [[nodiscard]] static Result<Window> Create(const WindowDescription& description);

    /// Closes the window if it is open. Safe to call unconditionally, and more than once.
    void Destroy();

    [[nodiscard]] bool IsOpen() const { return m_nativeHandle != nullptr; }

private:
    // A Win32 HWND on Windows, held as void* so this header names no platform type and its
    // layout does not depend on which platform is building it (ADR-0016). It is also what
    // Task 4 hands to vkCreateWin32SurfaceKHR, so keeping it opaque is what allows the one
    // file that includes <windows.h> to be a per-platform .cpp in the backend rather than a
    // header anybody could include.
    void* m_nativeHandle = nullptr;
};

}  // namespace Monarc::Host
