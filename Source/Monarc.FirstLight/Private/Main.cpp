// Monarc.FirstLight -- the runnable proof that the module graph links end to end.
//
// Phase A3 Task 1 has no Vulkan backend and no window, so this asks for both, reports what
// each one said, and exits non-zero. That is the deliverable, not a placeholder for one: an
// app printing "ok" here would be claiming something no code in the repository does, and the
// first thing it would prove is that nobody reads it.
//
// It is also the only thing in the build that references a symbol from all four of
// Monarc.Core, Monarc.RHI, Monarc.RHI.Vulkan and Monarc.Host.Windowed at once. A declared
// dependency nothing calls links happily and proves nothing, so every edge below is a real
// call into the module it names.
//
// Runtime kind, so ADR-0003's restricted library subset applies: MONARC_LOG rather than
// <iostream>, which is banned in shipping code and would be the easy thing to reach for in a
// diagnostic like this one.

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Host/Window.h>
#include <Monarc/RHI/Types.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>

namespace {

MONARC_LOG_CATEGORY(FirstLight, Info);

/// What A3 will ask a swapchain for once there is one. B8G8R8A8_UNORM with FIFO present is
/// what the phase plan commits to: FIFO is the only present mode Vulkan guarantees, and UNORM
/// rather than an sRGB format keeps A3 out of deciding clear-value semantics on sRGB images.
/// It lives here, in the app, because choosing what to ask for is the app's job -- Task 4
/// moves the choice into swapchain creation, where it can be negotiated against what the
/// surface actually supports.
constexpr Monarc::RHI::Format kSurfaceFormat = Monarc::RHI::Format::B8G8R8A8_UNORM;

/// The size is left at WindowDescription's own default rather than restated here: the number
/// belongs in one place, and the log line below reads it back from the description so that
/// what is printed is what will be asked for.
constexpr Monarc::Host::WindowDescription kWindow{.title = "Monarc -- First Light"};

void Report(const char* what, const Monarc::Error& error) {
    MONARC_LOG(FirstLight, Error, "{}: {} -- {}", what, Monarc::ToString(error.code),
               error.message);
}

}  // namespace

int main() {
    MONARC_LOG(FirstLight, Info, "Monarc.FirstLight | {}x{} | surface {} at {} bytes/pixel",
               kWindow.size.width, kWindow.size.height,
               Monarc::RHI::ToString(kSurfaceFormat),
               Monarc::RHI::BytesPerPixel(kSurfaceFormat));

    int unimplemented = 0;

    if (const Monarc::Status backend = Monarc::RHI::CreateVulkanBackend(); !backend) {
        Report("Vulkan backend", backend.error());
        ++unimplemented;
    }

    if (const Monarc::Result<Monarc::Host::Window> window =
            Monarc::Host::Window::Create(kWindow);
        !window) {
        Report("window", window.error());
        ++unimplemented;
    }

    if (unimplemented != 0) {
        MONARC_LOG(FirstLight, Error,
                   "first light is not lit: {} of 2 steps are not implemented yet", unimplemented);
        return 1;
    }

    MONARC_LOG(FirstLight, Info, "first light");
    return 0;
}
