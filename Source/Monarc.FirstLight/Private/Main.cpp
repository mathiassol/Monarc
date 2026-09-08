// Monarc.FirstLight -- the runnable proof that the module graph links end to end, and the
// program that opens a window and clears it.
//
// Two modes, and they answer different questions.
//
// `--adapters` brings the Vulkan backend up, prints the loader's status, the instance version,
// what happened to validation, and both adapter lists -- the raw one Vulkan reported and the
// deduplicated one a caller should use -- and **exits zero regardless of what it found**. That
// is not laxity: this mode is also registered with CTest as Monarc.RHI.Vulkan.Probe, whose job
// is to put the truth about each machine into the log rather than to gate anything. A machine
// with no Vulkan at all is a finding, and a finding is something a probe reports. The gate is
// Monarc.RHI.Vulkan.DeviceTests, which reports Skipped in exactly that case.
//
// "Regardless of what it found" is exact, and narrower than "always exits zero". This mode
// runs Monarc's own code, and in Debug it runs it with validation on and the fatal messenger
// installed -- so a VALIDATION-type error stops the process at 0x80000003 and the CTest entry
// reports Failed. What the probe declines to gate on is the machine, not Monarc's use of
// Vulkan while looking at it.
//
// **With no arguments this is first light: a window opens, every frame clears it to
// (64, 128, 192), and closing the window exits zero.** Phase A3 Task 4 is what made that true;
// through Tasks 1 to 3 this mode exited 1 because the window did not exist yet.
//
// `--frames=N` runs N presented frames and exits, which is what a test and a frame capture
// need: a loop that only ends when a person closes a window cannot be either. `--adapter=<n or
// name>` picks which GPU to run on, because this machine has two and a capture per adapter is
// what the phase plan asks for. Both are stated in `--help`.
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
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Capabilities.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Swapchain.h>
#include <Monarc/RHI/Types.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>
#include <Monarc/RHI/Vulkan/VulkanDevice.h>
#include <Monarc/RHI/Vulkan/VulkanSwapchain.h>

#include <charconv>
#include <string_view>

namespace {

MONARC_LOG_CATEGORY(FirstLight, Info);

/// What the swapchain asks for. `B8G8R8A8_UNORM` with FIFO and sRGB-non-linear is what the
/// phase plan commits to: FIFO is the only present mode Vulkan guarantees, and UNORM rather
/// than an sRGB format keeps A3 out of deciding clear-value semantics on sRGB images.
///
/// The format is named here because choosing what to ask for is the app's job;
/// `VulkanSwapchainFactory` is what negotiates it against the surface and refuses rather than
/// substituting.
constexpr Monarc::RHI::Format kSurfaceFormat = Monarc::RHI::Format::B8G8R8A8_UNORM;

/// The colour every frame clears to, and the same value Task 3's readback asserts.
///
/// 64, 128 and 192 over 255 each survive the round trip through `f32` and back through UNORM
/// quantisation, so the driver has no rounding decision to make -- which is what lets the
/// swapchain readback and the on-screen capture in the device tests assert exact bytes. They
/// are also far apart and asymmetric, so a channel order swapped between R and B is visible
/// rather than plausible.
constexpr Monarc::RHI::ClearColor kClearColor{64.0F / 255.0F, 128.0F / 255.0F, 192.0F / 255.0F,
                                              1.0F};

/// The size is left at WindowDescription's own default rather than restated here: the number
/// belongs in one place, and the log line below reads it back from the description so that
/// what is printed is what will be asked for.
constexpr Monarc::Host::WindowDescription kWindow{.title = "Monarc -- First Light"};

constexpr std::string_view kAdaptersOption = "--adapters";
constexpr std::string_view kFramesOption   = "--frames=";
constexpr std::string_view kAdapterOption  = "--adapter=";
constexpr std::string_view kHelpOption     = "--help";

/// How this program was asked to run.
struct Options {
    bool listAdapters = false;
    bool help         = false;

    /// Non-zero means "run this many presented frames and exit". Zero means "until the window
    /// is closed", which is the human-facing default.
    ///
    /// **Parked frames do not count**, and that is the whole reason the counter is of
    /// *presented* frames: `--frames=10` on a minimised window would otherwise exit having
    /// rendered nothing, which is exactly the shape of green this phase exists to avoid.
    Monarc::u32 frames = 0;

    /// Empty means "the first adapter that can present to this window". Otherwise a decimal
    /// index into the deduplicated adapter list, or a substring of an adapter's name.
    ///
    /// A substring and not an exact match, because the exact names on this machine are
    /// "NVIDIA GeForce RTX 3070 Ti" and "Intel(R) UHD Graphics 730" -- `--adapter=Intel` is
    /// what a person types, and it is what the RenderDoc capture commands use.
    std::string_view adapter = {};
};

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
               info.queueFamilyCount, capabilities.graphicsQueueFamilyCount);
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

/// Prints the loader's status, the instance, and both adapter lists. Returns 0 on every
/// finding, including "there is no Vulkan here" -- see the file comment: this is a report, not
/// an assertion. It can still be stopped by the fatal validation messenger, which is a
/// statement about Monarc rather than about the machine.
int ReportAdapters() {
    Monarc::SystemAllocator            allocator;
    Monarc::RHI::VulkanBackend::Config config{};
    config.applicationName = "Monarc.FirstLight";

    MONARC_LOG(FirstLight, Info, "probe: validation requested {}", config.validation);

    Monarc::Result<Monarc::RHI::VulkanBackend> created =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    if (!created) {
        // Error level, because a human running --adapters on their own machine wants this to
        // stand out, even though the exit code stays zero. The two are separate signals on
        // purpose: the log says what is wrong, the exit code says "this was a report".
        //
        // The message is a string literal; the backend's own log lines above this one carry
        // which library, which VkResult and which version it actually saw.
        Report("probe: the Vulkan backend did not come up", created.error());
        MONARC_LOG(FirstLight, Info,
                   "probe: no adapters can be listed on this machine. Exiting zero anyway -- "
                   "this mode reports, and Monarc.RHI.Vulkan.DeviceTests is the gate.");
        return 0;
    }
    Monarc::RHI::VulkanBackend& backend = *created;

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

void PrintHelp() {
    MONARC_LOG(FirstLight, Info, "Monarc.FirstLight");
    MONARC_LOG(FirstLight, Info,
               "  (no arguments)     open a window, clear it, and exit zero when it is closed");
    MONARC_LOG(FirstLight, Info,
               "  --frames=N         present N frames and exit zero; parked frames do not "
               "count");
    MONARC_LOG(FirstLight, Info,
               "  --adapter=<n|name> which GPU to run on: an index into the deduplicated "
               "adapter list, or part of its name");
    MONARC_LOG(FirstLight, Info,
               "  --adapters         print what Vulkan sees on this machine and exit zero");
    MONARC_LOG(FirstLight, Info, "  --help             this");
}

/// Parses the command line. Returns false and reports on anything it does not recognise.
///
/// **An unrecognised argument is a failure and not a warning.** This program's arguments are
/// how a test and a frame capture drive it, so `--frame=10` accepted as "no arguments" would
/// be a run that measured nothing while exiting zero.
[[nodiscard]] bool ParseOptions(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == kAdaptersOption) {
            out.listAdapters = true;
            continue;
        }
        if (argument == kHelpOption) {
            out.help = true;
            continue;
        }
        if (argument.starts_with(kFramesOption)) {
            const std::string_view value = argument.substr(kFramesOption.size());
            Monarc::u32            frames = 0;
            const auto* const      first  = value.data();
            const auto* const      last   = value.data() + value.size();
            // `from_chars` and not `atoi`: it reports what it could not parse, and it does not
            // accept a leading sign or trailing rubbish silently. `--frames=10x` is a mistake
            // worth naming.
            const std::from_chars_result parsed = std::from_chars(first, last, frames);
            if (parsed.ec != std::errc{} || parsed.ptr != last || frames == 0) {
                MONARC_LOG(FirstLight, Error,
                           "--frames needs a positive decimal count; got \"{}\"", value);
                return false;
            }
            out.frames = frames;
            continue;
        }
        if (argument.starts_with(kAdapterOption)) {
            out.adapter = argument.substr(kAdapterOption.size());
            if (out.adapter.empty()) {
                MONARC_LOG(FirstLight, Error,
                           "--adapter needs an index or part of an adapter's name");
                return false;
            }
            continue;
        }
        MONARC_LOG(FirstLight, Error, "unrecognised argument \"{}\"; try --help", argument);
        return false;
    }
    return true;
}

/// Whether `name` contains `needle`. A tiny substring test rather than `std::string::find`,
/// because `AdapterInfo::name` is a fixed char array and `std::string_view` over it is what
/// this has.
[[nodiscard]] bool NameContains(const char* name, std::string_view needle) {
    return std::string_view(name).find(needle) != std::string_view::npos;
}

/// Picks the adapter to run on.
///
/// `selector` empty means "the first that can present to this surface", which is the answer a
/// person wants and is not the same as "the first adapter": on a machine whose integrated part
/// cannot reach the display driving the window, the first adapter is the wrong one and the
/// error would be a swapchain failure rather than a choice.
///
/// **Presentation support is measured per adapter and reported for every one of them, not just
/// for the one chosen.** That list is the finding the phase asks for.
[[nodiscard]] Monarc::Result<Monarc::usize> ChooseAdapter(
    Monarc::RHI::VulkanBackend& backend, const Monarc::Array<Monarc::RHI::AdapterInfo>& adapters,
    const Monarc::RHI::SurfaceDescription& surface, std::string_view selector) {
    Monarc::usize chosen        = adapters.Size();
    bool          selectorFound = false;

    for (Monarc::usize i = 0; i < adapters.Size(); ++i) {
        const Monarc::RHI::AdapterInfo& adapter = adapters[i];

        const Monarc::Result<bool> canPresent = backend.AdapterCanPresent(adapter, surface);
        if (!canPresent) {
            // A query that failed is not the same as an adapter that cannot present, so it is
            // reported and the adapter is skipped rather than recorded as unable.
            Report("presentation support could not be queried", canPresent.error());
            continue;
        }

        MONARC_LOG(FirstLight, Info, "adapter [{}] \"{}\" | tier {} | can present: {}", i,
                   adapter.name, Monarc::RHI::ToString(adapter.tier), *canPresent);

        if (!*canPresent) {
            continue;
        }

        if (selector.empty()) {
            if (chosen == adapters.Size()) {
                chosen = i;
            }
            continue;
        }

        // An index if the whole selector is one, otherwise a name substring. Tried in that
        // order because "0" is a legitimate index and is not part of any adapter name here.
        Monarc::usize                index  = 0;
        const auto* const            first  = selector.data();
        const auto* const            last   = selector.data() + selector.size();
        const std::from_chars_result parsed = std::from_chars(first, last, index);
        const bool                   isIndex =
            parsed.ec == std::errc{} && parsed.ptr == last;

        if ((isIndex && index == i) || (!isIndex && NameContains(adapter.name, selector))) {
            chosen        = i;
            selectorFound = true;
        }
    }

    if (!selector.empty() && !selectorFound) {
        MONARC_LOG(FirstLight, Error,
                   "--adapter=\"{}\" matched no adapter that can present to this window",
                   selector);
        return Monarc::Err(Monarc::ErrorCode::NotFound,
                           "--adapter matched no adapter that can present to this window");
    }
    if (chosen == adapters.Size()) {
        return Monarc::Err(Monarc::ErrorCode::Unsupported,
                           "no adapter on this machine can present to this window");
    }
    return chosen;
}

/// Records and submits one frame: barrier into the colour-attachment layout, clear, barrier
/// into the present layout.
///
/// **The two barriers are the whole of A3's use of ADR-0005's model, and their synchronisation
/// scopes are chosen to chain with the swapchain's semaphores rather than filled in with
/// `AllCommands`.**
///
/// - The first names `PipelineStage::ColorAttachmentOutput` on *both* sides. The before scope
///   is not `None`, and that is deliberate: `VulkanDeviceState::SubmitList` waits on the
///   acquire semaphore at the colour-attachment-output stage, and a layout transition is a
///   write that must be ordered after that wait. Naming the same stage is what chains the two.
/// - The second names `PipelineStage::None` and `Access::None` on the after side, because
///   there is no *command* after it: what reads the image next is the presentation engine, and
///   the render-finished semaphore -- signalled at `ALL_COMMANDS`, so after this transition --
///   is the dependency that covers it.
[[nodiscard]] Monarc::Status RecordFrame(Monarc::RHI::ICommandList& list,
                                         Monarc::RHI::TextureHandle image,
                                         Monarc::RHI::Extent2D      extent) {
    if (Monarc::Status begun = list.Begin(); !begun) {
        return begun;
    }

    list.Barrier(Monarc::RHI::TextureBarrier(
        image, Monarc::RHI::TextureLayout::Undefined,
        Monarc::RHI::TextureLayout::ColorAttachment,
        Monarc::RHI::PipelineStage::ColorAttachmentOutput,
        Monarc::RHI::PipelineStage::ColorAttachmentOutput, Monarc::RHI::Access::None,
        Monarc::RHI::Access::ColorAttachmentWrite));

    const Monarc::RHI::ColorAttachment attachments[1] = {
        {image, Monarc::RHI::LoadOp::Clear, Monarc::RHI::StoreOp::Store, kClearColor}};

    Monarc::RHI::RenderingDescription rendering{};
    rendering.extent           = extent;
    rendering.colorAttachments = attachments;

    // No draw call, and none is needed: a load-op clear happens when rendering begins. There is
    // deliberately no separate clear command in this interface -- Device.h's `LoadOp::Clear`
    // says why, and this is the path A4's render graph will take too.
    if (Monarc::Status began = list.BeginRendering(rendering); !began) {
        return began;
    }
    list.EndRendering();

    list.Barrier(Monarc::RHI::TextureBarrier(
        image, Monarc::RHI::TextureLayout::ColorAttachment,
        Monarc::RHI::TextureLayout::PresentSource,
        Monarc::RHI::PipelineStage::ColorAttachmentOutput, Monarc::RHI::PipelineStage::None,
        Monarc::RHI::Access::ColorAttachmentWrite, Monarc::RHI::Access::None));

    return list.End();
}

/// The frame loop. Returns the process's exit code.
///
/// **Every step the phase plan's Task 4 checkbox names, in the order it names them**: acquire,
/// barrier undefined to colour attachment, begin rendering with a clear, end, barrier to
/// present, submit, present, advance the timeline. The timeline advance is
/// `IQueue::Submit`'s return value and `IDevice::BeginFrame`'s wait on it; there is no separate
/// call, which is the point of a timeline.
[[nodiscard]] int RunFrameLoop(Monarc::Host::Window& window, Monarc::RHI::IDevice& device,
                               Monarc::RHI::ISwapchain& swapchain, Monarc::u32 frameLimit) {
    Monarc::u32 presented = 0;
    Monarc::u32 parked    = 0;
    Monarc::u32 recreated = 0;
    Monarc::u32 skipped   = 0;

    while (window.IsOpen() && !window.CloseRequested() &&
           (frameLimit == 0 || presented < frameLimit)) {
        window.PumpEvents();
        if (window.CloseRequested()) {
            break;
        }

        bool resized = false;
        for (const Monarc::Host::WindowEvent& event : window.Events()) {
            // Logged by name, which is what gives `Host::ToString(WindowEventKind)` a shipped
            // caller. Not noisy: events are coalesced to at most one resize and one close per
            // pump, and a window that is not being dragged produces neither.
            MONARC_LOG(FirstLight, Info, "window event: {} ({}x{})",
                       Monarc::Host::ToString(event.kind), event.size.width, event.size.height);
            if (event.kind == Monarc::Host::WindowEventKind::Resized) {
                resized = true;
            }
        }

        const Monarc::RHI::Extent2D size = window.ClientSize();
        if (size.IsEmpty()) {
            // **Parked.** A minimised window reports 0 x 0 and a swapchain cannot be created
            // from it, so the loop pumps messages, presents nothing, and waits in the kernel
            // rather than spinning. This is the bug most first attempts have -- either a
            // swapchain created at 0 x 0, which is a validation error, or a loop that keeps
            // presenting to a surface with no area.
            //
            // `WaitForEvents` and not `PumpEvents`: there is no frame to pace the loop while
            // parked, so pumping in a tight loop would spend a core doing nothing.
            ++parked;
            window.WaitForEvents();
            continue;
        }

        if (resized || swapchain.NeedsRecreation() || swapchain.Extent() != size) {
            // Three conditions and each catches something the others do not: a resize event is
            // the window telling us, `NeedsRecreation()` is the swapchain telling us (an
            // `OUT_OF_DATE` or `SUBOPTIMAL` from either acquire or present), and the extent
            // comparison catches a size that changed while the loop was parked, where the
            // resize event has long since been overwritten.
            if (Monarc::Status recreatedNow = swapchain.Recreate(size); !recreatedNow) {
                // Logged and retried on the next iteration rather than fatal. A recreate can
                // fail transiently -- the window can be minimised between the size check above
                // and the surface query inside -- and the loop is still able to see a close
                // request, so it is not a hang.
                Report("the swapchain could not be recreated; retrying next frame",
                       recreatedNow.error());
                ++skipped;
                continue;
            }
            ++recreated;
        }

        // Before the acquire, and it stays before it: this waits on the frame slot's timeline
        // value and resets its command pool, which is about the pool and not about the image.
        const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
        if (!commands) {
            Report("IDevice::BeginFrame failed", commands.error());
            return 1;
        }

        const Monarc::Result<Monarc::RHI::AcquiredImage> acquired = swapchain.Acquire();
        if (!acquired) {
            Report("ISwapchain::Acquire failed", acquired.error());
            return 1;
        }
        if (acquired->outcome != Monarc::RHI::AcquireOutcome::Acquired) {
            // No image and nothing to present. The frame slot's pool has been reset and
            // nothing was recorded into it, which is a state `IQueue::Submit` refuses by name
            // -- so the loop must not try. `NeedsRecreation()` is set, so the next iteration
            // recreates.
            //
            // Named rather than counted silently, which is what gives
            // `RHI::ToString(AcquireOutcome)` a shipped caller: "the frame was skipped" and
            // "the swapchain went out of date" are the same event only for as long as
            // `AcquireOutcome` has two enumerators.
            MONARC_LOG(FirstLight, Info, "acquire reported {}; skipping this frame",
                       Monarc::RHI::ToString(acquired->outcome));
            ++skipped;
            continue;
        }

        if (Monarc::Status recorded =
                RecordFrame(**commands, acquired->texture, swapchain.Extent());
            !recorded) {
            Report("recording the frame failed", recorded.error());
            return 1;
        }

        const Monarc::Result<Monarc::u64> submitted =
            swapchain.SubmitForPresent(device.GraphicsQueue(), **commands);
        if (!submitted) {
            Report("ISwapchain::SubmitForPresent failed", submitted.error());
            return 1;
        }

        if (Monarc::Status shown = swapchain.Present(); !shown) {
            Report("ISwapchain::Present failed", shown.error());
            return 1;
        }
        ++presented;
    }

    // Clean shutdown, first half: wait for the timeline to drain before anything is destroyed.
    // The caller's destructors do the rest, in reverse order -- swapchain, device, backend,
    // window -- and each of those waits again, because each is documented as safe to call on
    // its own. This one is here so that a failure to drain is *reported* rather than logged
    // inside a destructor that cannot report.
    if (Monarc::Status idle = device.WaitIdle(); !idle) {
        Report("IDevice::WaitIdle failed during shutdown", idle.error());
        return 1;
    }

    MONARC_LOG(FirstLight, Info,
               "first light | {} frame(s) presented | {} recreation(s) | {} parked "
               "iteration(s) | {} skipped frame(s) | close requested: {}",
               presented, recreated, parked, skipped, window.CloseRequested());
    return 0;
}

/// Opens a window, brings up a device and a swapchain on it, and runs the frame loop.
[[nodiscard]] int RunFirstLight(const Options& options) {
    Monarc::SystemAllocator allocator;

    MONARC_LOG(FirstLight, Info,
               "Monarc.FirstLight | asking for a {}x{} window | surface {} at {} bytes/pixel | "
               "frames: {}",
               kWindow.size.width, kWindow.size.height, Monarc::RHI::ToString(kSurfaceFormat),
               Monarc::RHI::BytesPerPixel(kSurfaceFormat),
               options.frames == 0 ? 0 : options.frames);

    const Monarc::RHI::VulkanBackend::Config backendConfig{.applicationName =
                                                               "Monarc.FirstLight"};
    Monarc::Result<Monarc::RHI::VulkanBackend> backend =
        Monarc::RHI::VulkanBackend::Create(allocator, backendConfig);
    if (!backend) {
        Report("the Vulkan backend did not come up", backend.error());
        return 1;
    }

    const Monarc::RHI::ApiVersion instance = backend->InstanceApiVersion();
    MONARC_LOG(FirstLight, Info,
               "Vulkan backend up | instance {}.{}.{} | validation layer {} | debug messenger "
               "{}",
               instance.major, instance.minor, instance.patch,
               backend->ValidationLayerEnabled() ? "enabled" : "not enabled",
               backend->DebugMessengerInstalled() ? "installed" : "absent");

    Monarc::Array<Monarc::RHI::AdapterInfo> adapters(allocator);
    if (const Monarc::Status enumerated = backend->EnumerateAdapters(adapters); !enumerated) {
        Report("adapter enumeration failed", enumerated.error());
        return 1;
    }
    if (adapters.IsEmpty()) {
        MONARC_LOG(FirstLight, Error,
                   "the Vulkan instance came up and reports no physical devices; there is "
                   "nothing to render on");
        return 1;
    }

    // The window comes before the device, and it has to: which adapters can present is a fact
    // about a *surface*, so there is nothing to ask until there is a window to ask about.
    Monarc::Result<Monarc::Host::Window> window = Monarc::Host::Window::Create(kWindow);
    if (!window) {
        Report("the window could not be opened", window.error());
        return 1;
    }
    MONARC_LOG(FirstLight, Info, "window open | client area {}x{}", window->ClientSize().width,
               window->ClientSize().height);

    const Monarc::Result<Monarc::usize> index =
        ChooseAdapter(*backend, adapters, window->Surface(), options.adapter);
    if (!index) {
        Report("no adapter was chosen", index.error());
        return 1;
    }
    const Monarc::RHI::AdapterInfo& adapter = adapters[*index];
    MONARC_LOG(FirstLight, Info, "running on adapter [{}] \"{}\"", *index, adapter.name);

    Monarc::Result<Monarc::RHI::VulkanDevice> device =
        backend->CreateDevice(allocator, adapter, Monarc::RHI::DeviceConfig{});
    if (!device) {
        Report("the device could not be created", device.error());
        return 1;
    }

    Monarc::RHI::SwapchainDescription swapchainDescription{};
    swapchainDescription.surface = window->Surface();
    swapchainDescription.extent  = window->ClientSize();
    swapchainDescription.format  = kSurfaceFormat;
    // **This app does not ask for readback and the device tests do.** Readback needs
    // `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` on the swapchain images, which is a request a shipped
    // program has no reason to make: it costs nothing measurable and it is still a usage bit
    // asked for with no user. What proves the clear reaches the presented image is
    // Monarc.Host.Windowed's device suite, which asks for it because it copies one out.
    swapchainDescription.allowReadback = false;

    Monarc::Result<Monarc::RHI::VulkanSwapchain> swapchain =
        backend->CreateSwapchain(allocator, *device, swapchainDescription);
    if (!swapchain) {
        Report("the swapchain could not be created", swapchain.error());
        return 1;
    }
    MONARC_LOG(FirstLight, Info,
               "swapchain up | {}x{} | {} | {} image(s) | {} frame(s) in flight",
               swapchain->Extent().width, swapchain->Extent().height,
               Monarc::RHI::ToString(swapchain->ImageFormat()), swapchain->ImageCount(),
               Monarc::RHI::kFramesInFlight);

    const int exitCode = RunFrameLoop(*window, *device, *swapchain, options.frames);

    // **Destroyed explicitly and in reverse order, rather than left to scope exit.** The order
    // is not negotiable -- a swapchain names the device's entry points and the backend's
    // instance, and a window's native handle outlives neither safely -- and the declarations
    // above happen to already be in that order, so scope exit would do the same thing. Written
    // out because "the destructors happen to be in the right order" is a property of the
    // declaration order rather than a stated intent, and one inserted local would change it.
    swapchain->Shutdown();
    device->Shutdown();
    window->Destroy();
    backend->Shutdown();
    return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!ParseOptions(argc, argv, options)) {
        PrintHelp();
        // Two, not one: "you asked for something I do not understand" is a different outcome
        // from "what you asked for did not work", and a test driving this program should not
        // have them collapse into one code.
        return 2;
    }
    if (options.help) {
        PrintHelp();
        return 0;
    }
    if (options.listAdapters) {
        return ReportAdapters();
    }
    return RunFirstLight(options);
}
