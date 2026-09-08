// `Detail::WindowTestHooks`: the Win32 that drives a window from code and reads back what is
// actually on the screen.
//
// **A test-only translation unit, and the directory is what says so.** `Private/**/TestSupport/`
// is compiled into each of a module's test binaries and into *nothing else* --
// `monarc_module()` does not glob it and `_monarc_add_test_binary()` does. See
// MONARC_TEST_SUPPORT_DIR in CMake/MonarcModule.cmake for the convention and for the
// measurement that motivated it.
//
// **The measurement, because this file exists for a number.** These ten functions have no
// shipped caller and sat at the foot of `../Window.cpp`, so the linker pulled them into every
// app that links the module: a static library's members are selected whole, everything
// references `Window.cpp.obj`, and both MSVC configurations link with `/INCREMENTAL`, which
// disables `/OPT:REF`. `dumpbin /imports` on the Release `Monarc.FirstLight.exe` listed
// `GDI32.dll` -- an entire extra system DLL, for `BitBlt`, `CreateCompatibleDC`,
// `CreateDIBSection`, `SelectObject`, `DeleteDC`, `DeleteObject` and `GdiFlush`, all from
// `CaptureScreenPixel` -- plus twelve USER32 imports and one KERNEL32 import no shipped path
// calls. It now lists none of them. Docs/Status.md has both tables.
//
// **Being its own translation unit is what removes the imports; being outside the module is
// what keeps them out.** Measured both ways: with this file added back to the module and a
// *full* link, `GDI32.dll` still does not appear, because no symbol in this member is
// referenced and the linker never selects it from the archive. That is a property of nothing
// referencing it, which lasts exactly as long as nothing does. Not compiling it into the
// library at all does not depend on `/OPT:REF`, on the configuration, or on a future shipped
// object leaving it alone.
//
// **Still under `Private/Platform/<Platform>/`, which is the point of nesting TestSupport/
// inside it rather than beside `Private/`.** Two rules keep applying unchanged: the build
// selects this file by directory, so a macOS build never hands it to a compiler; and gate 10's
// exemption is the path `Private/Platform/<Platform>/`, so a platform conditional here would be
// judged by exactly the same rule as one in the shipping half. Moving these into `Tests/` and
// `TestsDevice/` would have satisfied neither -- and would have put `<Windows.h>` in two test
// files, since both of this module's suites need them.
//
// **What is *not* here.** `WindowPlatform`'s own members, all of which have a shipped caller,
// stay in `Private/Platform/Windows/Window.cpp`. The split is greppable by design: everything
// in that struct ships and everything in this one does not. `WindowPlatform::NativeHandle` is
// declared in `Private/WindowPlatform.h` and is the whole of the access this file needs, so
// nothing here is a friend of `Window` -- `HandleOf` below casts and adds nothing else, exactly
// as its twin in Window.cpp does.
//
// The window style is read through `WindowPlatform::WindowStyle()` rather than redeclared:
// `RequestClientSize` computes an outer size from a client size, and a second `constexpr DWORD`
// here would compute the wrong one the day `Create`'s style changed.

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

/// This file's own category, distinct from `Window` in the shipping half.
///
/// Nested in a namespace of its own for the mechanical reason Window.cpp records:
/// MONARC_LOG_CATEGORY declares a *variable* named after the category, and a variable called
/// `Window` at this scope would hide the class of that name. A separate spelling from
/// `Window` rather than a second declaration of it, so that a warning from a hook is
/// attributable to a hook -- these lines exist to diagnose a bad capture or a refused
/// `SetWindowPos`, and which half emitted one is the first thing a reader wants.
MONARC_LOG_CATEGORY(WindowTestHooks, Info);

}  // namespace LogCategories

namespace {

/// `window`'s HWND, or null.
///
/// Through `WindowPlatform::NativeHandle` and not `window.m_nativeHandle`, because this is a
/// free function and friendship does not reach one. The cast is the only thing this adds. Its
/// twin in Window.cpp is identical and deliberately not shared: it is three tokens, and a
/// header to hold it would have to name `HWND`.
[[nodiscard]] HWND HandleOf(const Window& window) {
    return static_cast<HWND>(WindowPlatform::NativeHandle(window));
}

}  // namespace

void WindowTestHooks::RequestClientSize(Window& window, RHI::Extent2D size) {
    const HWND handle = HandleOf(window);
    if (handle == nullptr) {
        return;
    }

    RECT rect{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
    const UINT dpi = GetDpiForWindow(handle);
    if (AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(WindowPlatform::WindowStyle()),
                                 FALSE, 0, dpi) == 0) {
        MONARC_LOG(LogCategories::WindowTestHooks, Warning, "AdjustWindowRectExForDpi failed with Win32 error {}",
                   GetLastError());
    }
    if (SetWindowPos(handle, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) == 0) {
        MONARC_LOG(LogCategories::WindowTestHooks, Warning, "SetWindowPos (resize) failed with Win32 error {}",
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
        MONARC_LOG(LogCategories::WindowTestHooks, Warning, "SetWindowPos (move) failed with Win32 error {}",
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
            MONARC_LOG(LogCategories::WindowTestHooks, Warning,
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
        MONARC_LOG(LogCategories::WindowTestHooks, Warning, "PostMessageW(WM_CLOSE) failed with Win32 "
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
        MONARC_LOG(LogCategories::WindowTestHooks, Warning, "SetWindowPos (topmost) failed with Win32 error {}",
                   GetLastError());
        return Foreground::NotOnTop;
    }
    if (SetForegroundWindow(handle) == 0) {
        MONARC_LOG(LogCategories::WindowTestHooks, Warning,
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
