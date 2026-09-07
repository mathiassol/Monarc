#include <Monarc/Host/Window.h>

#include <utility>

namespace Monarc::Host {

// The ownership shape is written now, ahead of the thing it owns. That is deliberate rather
// than premature: once Task 4's Private/Platform/Windows/Window.cpp starts returning a real
// HWND, a defaulted move would leave two Windows naming one window and the destructor would
// close it twice. Getting it right while there is nothing to break is cheaper than debugging
// it once there is.
//
// It is also, until then, unverified -- and Tests/TestWindow.cpp says so rather than covering
// it with a case that cannot fail. Nothing here can hold a non-null handle yet, so no test
// can tell a correct move from a copy.

Window::~Window() { Destroy(); }

Window::Window(Window&& other) noexcept
    : m_nativeHandle(std::exchange(other.m_nativeHandle, nullptr)) {}

Window& Window::operator=(Window&& other) noexcept {
    // Self-assignment would otherwise destroy the window and then adopt the handle it had
    // just released.
    if (this != &other) {
        Destroy();
        m_nativeHandle = std::exchange(other.m_nativeHandle, nullptr);
    }
    return *this;
}

Result<Window> Window::Create(const WindowDescription&) {
    // Unsupported, not Unknown: nothing failed, there is simply no windowing code yet. The
    // description is unnamed on purpose -- reading it here would suggest it is honoured.
    return Err(ErrorCode::Unsupported,
               "Monarc.Host.Windowed cannot create a window yet: the Win32 window class, "
               "message pump and native surface handle arrive in Task 4 of Phase A3");
}

void Window::Destroy() {
    // Nothing to release: Create never hands back an open window, so m_nativeHandle is
    // always null here. Task 4 destroys the native window from its per-platform source and
    // this stays the one place that clears the handle.
    m_nativeHandle = nullptr;
}

}  // namespace Monarc::Host
