// The half of a window that is the same on every platform: its ownership, its move
// operations, and the accessors over the state the platform's message handler writes.
//
// **The A3 plan names two files -- this one for "platform-neutral parts" and
// Private/Platform/Windows/Window.cpp for the Win32 -- and the split falls where it does
// because of the move operations.** Everything platform-specific is behind
// `Detail::WindowPlatform`, declared in Private/WindowPlatform.h, so nothing here names a Win32
// type. What is genuinely neutral is worth having on this side of the line: a window owns an OS
// resource that must be released exactly once, and getting that wrong is the same bug on every
// platform.
//
// `Move` is the interesting one. It is not "exchange the members and done": the native window
// holds a back-pointer to the `Window` that owns it, so a moved-to window at a new address has
// to re-point it. `Detail::WindowPlatform::Rebind` is that one platform call, and it is why the
// moves are written out rather than defaulted -- a defaulted move compiles perfectly and leaves
// the platform writing resize events into the object that was moved from.

#include <Monarc/Host/Window.h>

#include <WindowPlatform.h>

#include <utility>

namespace Monarc::Host {

const char* ToString(WindowEventKind kind) {
    // `default`-less, so an event kind added later is a compile error here rather than a
    // silent fall-through -- Monarc.RHI/Private/Types.cpp states the mechanism and
    // CMake/MonarcTargetOptions.cmake explains the /w44062 that makes MSVC agree with Clang
    // about it. The trailing return still has to exist: the enum has a fixed underlying type
    // and can hold a value outside its enumerator set.
    switch (kind) {
        case WindowEventKind::Resized:        return "Resized";
        case WindowEventKind::CloseRequested: return "CloseRequested";
    }
    return "<invalid WindowEventKind>";
}

Window::~Window() { Destroy(); }

Window::Window(Window&& other) noexcept
    : m_nativeHandle(std::exchange(other.m_nativeHandle, nullptr)),
      m_clientSize(other.m_clientSize),
      m_closeRequested(other.m_closeRequested),
      m_eventCount(other.m_eventCount) {
    for (usize i = 0; i < m_eventCount; ++i) {
        m_events[i] = other.m_events[i];
    }
    // The source keeps its size and its close flag as they were, which costs nothing and says
    // nothing: with no native handle its accessors describe a window it no longer has. What is
    // cleared is the handle, because that is the thing that must be released exactly once.
    other.m_clientSize = RHI::Extent2D{};
    other.m_eventCount = 0;

    // **Without this the platform would go on writing into `other`.** See
    // `Detail::WindowPlatform::Rebind`: the native window holds a pointer to its owner, and
    // that owner is now at this address.
    Detail::WindowPlatform::Rebind(*this);
}

Window& Window::operator=(Window&& other) noexcept {
    // Self-assignment would otherwise destroy the window and then adopt the handle it had just
    // released.
    if (this != &other) {
        Destroy();
        m_nativeHandle   = std::exchange(other.m_nativeHandle, nullptr);
        m_clientSize     = other.m_clientSize;
        m_closeRequested = other.m_closeRequested;
        m_eventCount     = other.m_eventCount;
        for (usize i = 0; i < m_eventCount; ++i) {
            m_events[i] = other.m_events[i];
        }
        other.m_clientSize = RHI::Extent2D{};
        other.m_eventCount = 0;

        Detail::WindowPlatform::Rebind(*this);
    }
    return *this;
}

Result<Window> Window::Create(const WindowDescription& description) {
    // Forwarded whole. Every check worth making needs the platform -- whether there is an
    // interactive session, whether the class registered, what size the window actually became
    // -- so a neutral pre-check here would either duplicate one of those or be the empty-size
    // test, which the platform has to make anyway before it can compute a window rect.
    return Detail::WindowPlatform::Create(description);
}

void Window::Destroy() { Detail::WindowPlatform::Destroy(*this); }

void Window::PumpEvents() { Detail::WindowPlatform::Pump(*this, false); }

void Window::WaitForEvents() { Detail::WindowPlatform::Pump(*this, true); }

std::span<const WindowEvent> Window::Events() const {
    return std::span<const WindowEvent>(m_events, m_eventCount);
}

RHI::SurfaceDescription Window::Surface() const {
    return Detail::WindowPlatform::Surface(*this);
}

}  // namespace Monarc::Host
