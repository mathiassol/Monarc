// Monarc.FirstLight -- the runnable proof that the module graph links end to end, and the
// program that prints what Vulkan actually sees on this machine.
//
// Two modes.
//
// `--adapters` brings the Vulkan backend up, prints the loader's status, the instance version,
// what happened to validation, and both adapter lists -- the raw one Vulkan reported and the
// deduplicated one a caller should use -- and **exits zero regardless of what it found**. That
// is not laxity: this mode is also registered with CTest as Monarc.RHI.Vulkan.Probe, whose job
// is to put the truth about each machine into the log rather than to gate anything. A machine
// with no Vulkan at all is a finding, and a finding is something a probe reports. The gate is
// Monarc.RHI.Vulkan.DeviceTests, which reports Skipped in exactly that case.
//
// With no arguments, this asks for a backend and a window, reports what each said, and exits
// non-zero while any of it is unimplemented. Phase A3 Task 2 has the backend; the window is
// Task 4's, so this still exits 1 -- deliberately. An app printing "ok" here would be claiming
// something no code in the repository does, and the first thing it would prove is that nobody
// reads it.
//
// It is also the only thing in the build that references a symbol from all four of
// Monarc.Core, Monarc.RHI, Monarc.RHI.Vulkan and Monarc.Host.Windowed at once. A declared
// dependency nothing calls links happily and proves nothing, so every edge below is a real
// call into the module it names.
//
// Runtime kind, so ADR-0003's restricted library subset applies: MONARC_LOG rather than
// <iostream>, which is banned in shipping code and would be the easy thing to reach for in a
// diagnostic like this one.

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Host/Window.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Capabilities.h>
#include <Monarc/RHI/Types.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>

#include <string_view>

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

constexpr std::string_view kAdaptersOption = "--adapters";

void Report(const char* what, const Monarc::Error& error) {
    MONARC_LOG(FirstLight, Error, "{}: {} -- {}", what, Monarc::ToString(error.code),
               error.message);
}

void PrintAdapter(const char* label, Monarc::usize index,
                  const Monarc::RHI::AdapterInfo& info) {
    const Monarc::RHI::Capabilities& capabilities = info.capabilities;
    MONARC_LOG(FirstLight, Info, "  {} [{}] {}", label, index, info.name);
    MONARC_LOG(FirstLight, Info, "        uuid {} | {} | vendor 0x{:04x} device 0x{:04x}",
               Monarc::RHI::ToString(info.uuid).text, Monarc::RHI::ToString(info.type),
               info.vendorId, info.deviceId);
    MONARC_LOG(FirstLight, Info,
               "        Vulkan {}.{}.{} | tier {} | queue families {} ({} graphics)",
               capabilities.apiVersion.major, capabilities.apiVersion.minor,
               capabilities.apiVersion.patch, Monarc::RHI::ToString(info.tier),
               capabilities.queueFamilyCount, capabilities.graphicsQueueFamilyCount);
    MONARC_LOG(FirstLight, Info,
               "        timeline {} | dynamic rendering {} | sync2 {} | bindless images {}",
               capabilities.timelineSemaphores, capabilities.dynamicRendering,
               capabilities.synchronization2, capabilities.maxBindlessSampledImages);
    MONARC_LOG(FirstLight, Info,
               "        non-uniform indexing {} | runtime array {} | partially bound {} | "
               "mesh {} | ray tracing {}",
               capabilities.nonUniformIndexing, capabilities.runtimeDescriptorArray,
               capabilities.partiallyBoundDescriptors, capabilities.meshShading,
               capabilities.rayTracing);
}

/// Prints the loader's status, the instance, and both adapter lists. Always returns 0 -- see
/// the file comment: this is a report, not an assertion.
int ReportAdapters() {
    Monarc::SystemAllocator            allocator;
    Monarc::RHI::VulkanBackend         backend(allocator);
    Monarc::RHI::VulkanBackend::Config config{};
    config.applicationName = "Monarc.FirstLight";

    MONARC_LOG(FirstLight, Info, "probe: validation requested {}", config.validation);

    if (const Monarc::Status initialized = backend.Initialize(config); !initialized) {
        // Error level, because a human running --adapters on their own machine wants this to
        // stand out, even though the exit code stays zero. The two are separate signals on
        // purpose: the log says what is wrong, the exit code says "this was a report".
        Report("probe: the Vulkan backend did not come up", initialized.error());
        MONARC_LOG(FirstLight, Info,
                   "probe: no adapters can be listed on this machine. Exiting zero anyway -- "
                   "this mode reports, and Monarc.RHI.Vulkan.DeviceTests is the gate.");
        return 0;
    }

    const Monarc::RHI::ApiVersion instance = backend.InstanceApiVersion();
    MONARC_LOG(FirstLight, Info,
               "probe: loader open | instance Vulkan {}.{}.{} | validation layer {} | debug "
               "messenger {}",
               instance.major, instance.minor, instance.patch,
               backend.ValidationLayerEnabled() ? "enabled" : "not enabled",
               backend.DebugMessengerInstalled() ? "installed" : "absent");

    Monarc::Array<Monarc::RHI::AdapterInfo> raw(allocator);
    if (const Monarc::Status enumerated = backend.EnumerateAdaptersRaw(raw); !enumerated) {
        Report("probe: raw enumeration failed", enumerated.error());
        return 0;
    }

    Monarc::Array<Monarc::RHI::AdapterInfo> deduplicated(allocator);
    if (const Monarc::Status enumerated = backend.EnumerateAdapters(deduplicated);
        !enumerated) {
        Report("probe: enumeration failed", enumerated.error());
        return 0;
    }

    MONARC_LOG(FirstLight, Info, "probe: raw enumeration -- {} physical device(s)", raw.Size());
    for (Monarc::usize i = 0; i < raw.Size(); ++i) {
        PrintAdapter("raw", i, raw[i]);
    }

    MONARC_LOG(FirstLight, Info, "probe: after deduplication on deviceUUID -- {} adapter(s)",
               deduplicated.Size());
    for (Monarc::usize i = 0; i < deduplicated.Size(); ++i) {
        PrintAdapter("adapter", i, deduplicated[i]);
    }

    MONARC_LOG(FirstLight, Info, "probe: {} raw entries collapsed to {}", raw.Size(),
               deduplicated.Size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == kAdaptersOption) {
            return ReportAdapters();
        }
    }

    MONARC_LOG(FirstLight, Info, "Monarc.FirstLight | {}x{} | surface {} at {} bytes/pixel",
               kWindow.size.width, kWindow.size.height,
               Monarc::RHI::ToString(kSurfaceFormat),
               Monarc::RHI::BytesPerPixel(kSurfaceFormat));

    // Both numbers in the summary below are counted here rather than one of them being
    // written into the message. Tasks 2 through 4 turn these steps green one at a time and
    // Task 5 adds more, and a hardcoded total is the kind of thing that survives that as
    // "1 of 2" long after there are three. The steps stay written out one per block, each
    // with its result type spelled in full, because being a visible call into the module it
    // names is this app's whole job -- see the file comment.
    int steps         = 0;
    int unimplemented = 0;

    Monarc::SystemAllocator allocator;

    ++steps;
    Monarc::RHI::VulkanBackend               backend(allocator);
    const Monarc::RHI::VulkanBackend::Config backendConfig{
        .applicationName = "Monarc.FirstLight"};
    if (const Monarc::Status brought = backend.Initialize(backendConfig); !brought) {
        Report("Vulkan backend", brought.error());
        ++unimplemented;
    } else {
        const Monarc::RHI::ApiVersion instance = backend.InstanceApiVersion();
        MONARC_LOG(FirstLight, Info,
                   "Vulkan backend up | instance {}.{}.{} | validation layer {} | debug "
                   "messenger {} | run with --adapters to list what it sees",
                   instance.major, instance.minor, instance.patch,
                   backend.ValidationLayerEnabled() ? "enabled" : "not enabled",
                   backend.DebugMessengerInstalled() ? "installed" : "absent");
    }

    ++steps;
    if (const Monarc::Result<Monarc::Host::Window> window =
            Monarc::Host::Window::Create(kWindow);
        !window) {
        Report("window", window.error());
        ++unimplemented;
    }

    if (unimplemented != 0) {
        MONARC_LOG(FirstLight, Error,
                   "first light is not lit: {} of {} steps are not implemented yet",
                   unimplemented, steps);
        return 1;
    }

    MONARC_LOG(FirstLight, Info, "first light");
    return 0;
}
