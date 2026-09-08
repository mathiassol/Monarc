// Device-required tests for the swapchain, against a real window.
//
// **This is where Phase A3 Task 4's evidence lives, and it is mechanical rather than visual.**
// The plan's own verification step reads "a window opens and is filled with the clear colour;
// resize it, maximise it, minimise and restore it, and drag it between the two monitors" -- a
// person at a keyboard. A person is not a regression test, so every one of those is driven from
// code here (`Detail::WindowTestHooks`), and the colour is proved two ways:
//
//   1. **The swapchain readback.** After the clear and *before* the present, the acquired image
//      is barriered to transfer-source, copied into a host-visible buffer, and asserted to the
//      byte. This is the strongest evidence available, because it proves the clear reached *the
//      image that gets presented* rather than an offscreen texture -- which is the one thing
//      Task 3's readback could not say.
//   2. **The on-screen capture.** The window is brought to the front, frames are presented, and
//      one pixel is read back out of the *screen's* device context with `BitBlt`. That is what
//      a person would see, and it is the only assertion here that crosses the desktop
//      compositor.
//
// Both assert exact bytes. A tolerance would hide the two bugs they exist to catch -- a channel
// order swapped and a colour space applied -- so where the bytes are not exact the actual values
// are reported as a finding rather than the assertion loosened to fit them.
//
// **Two skip conditions, and a missing one of either is a SKIP and never a PASS.** main() below
// returns 77 -- CTest's `SKIP_RETURN_CODE`, set in this module's CMakeLists.txt -- when there is
// no Vulkan runtime, no adapter, or no interactive session to put a window in. The decision is
// made before doctest runs a case, for the reason
// Monarc.RHI.Vulkan/TestsDevice/TestVulkanDevice.cpp gives: a filter inside a binary that had
// already started would report "0 tests, all passed", which is the shape of green this phase is
// built to avoid.
//
// **This suite lives in Monarc.Host.Windowed and not in Monarc.RHI.Vulkan**, where the swapchain
// is implemented. The CMakeLists.txt beside it explains the trade: a swapchain test needs both
// the backend and a window it can drive, and a tier-3 test reaching down to tier 2 is the
// direction the module graph already allows, where a tier-2 test reaching up to tier 3 is the
// one it exists to forbid.
//
// **The frame recording below is a second copy of `Monarc.FirstLight`'s, deliberately.** An app
// is a graph leaf, so nothing may link it, and a shared `RecordFrame` in a shipping module would
// be a function whose only caller is a test plus one app. What is shared is what matters: both
// go through the same `ICommandList` and the same two barriers, and this suite asserts the
// *result* rather than the code path.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Host/Window.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Swapchain.h>
#include <Monarc/RHI/Types.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>
#include <Monarc/RHI/Vulkan/VulkanDevice.h>
#include <Monarc/RHI/Vulkan/VulkanSwapchain.h>

#include <WindowPlatform.h>

#include <span>
#include <string_view>

using Monarc::Host::Window;
using Monarc::Host::WindowDescription;
using Monarc::Host::Detail::WindowPlatform;
using Monarc::Host::Detail::WindowTestHooks;

namespace {

MONARC_LOG_CATEGORY(SwapchainTest, Info);

/// CTest's SKIP_RETURN_CODE for this binary, set in Source/Monarc.Host.Windowed/CMakeLists.txt.
constexpr int kSkipReturnCode = 77;

constexpr std::string_view kLibraryOption = "--vulkan-library=";

/// How long a wait on the GPU timeline is given before it is called a deadlock. Five seconds,
/// matching the backend's own `kTimelineWaitTimeoutNanoseconds`, and for the same reason: a
/// suite that hangs forever tells CI nothing.
constexpr Monarc::u64 kWaitTimeoutNanoseconds = 5'000'000'000ULL;

/// The window every case opens.
///
/// 640x360, which is small enough to be unobtrusive on a 1080p display and large enough that
/// the readback's staging buffer stays under a megabyte. The size matters for one case: the
/// on-screen capture reads the *centre* pixel, so the window has to be big enough that its
/// centre is unambiguously inside the client area.
constexpr WindowDescription kHarnessWindow{.size  = {640, 360},
                                           .title = "Monarc -- swapchain test"};

constexpr Monarc::RHI::Format kSwapchainFormat = Monarc::RHI::Format::B8G8R8A8_UNORM;

/// The clear, as floats. The same value Task 3's readback asserts and the same value
/// Monarc.FirstLight clears to, so all three are statements about one colour.
constexpr Monarc::RHI::ClearColor kClearColor{64.0F / 255.0F, 128.0F / 255.0F, 192.0F / 255.0F,
                                              1.0F};

/// What the swapchain image reads back as, byte for byte.
///
/// **Blue, green, red, alpha -- and the swap is the point of asserting it here at all.** The
/// swapchain is `B8G8R8A8_UNORM`, and Vulkan's clear value is specified per *component*:
/// `VkClearColorValue::float32[0]` is the red component whatever order the format stores its
/// bytes in. So the same clear that reads back `(64, 128, 192, 255)` from Task 3's
/// `R8G8B8A8_UNORM` texture reads back `(192, 128, 64, 255)` here.
///
/// That makes this a channel-order test rather than a repeat of Task 3's: a backend that had
/// written the clear value into memory in component order rather than letting the format decide
/// would produce `(64, 128, 192, 255)`, and 64 and 192 are far enough apart that the swap is a
/// failure rather than a plausible near miss.
constexpr Monarc::u8 kExpectedSwapchainBytes[4] = {192, 128, 64, 255};

/// The backend main() brought up, and the adapters it found. Raw pointers to locals in main
/// rather than static objects, so nothing Vulkan-shaped is constructed during static
/// initialisation -- TestVulkanDevice.cpp's arrangement, for the same reason.
Monarc::RHI::VulkanBackend*              g_backend  = nullptr;
Monarc::Array<Monarc::RHI::AdapterInfo>* g_adapters = nullptr;

[[nodiscard]] Monarc::RHI::VulkanBackend& Backend() {
    MONARC_CHECK(g_backend != nullptr, "the swapchain test's backend was never brought up");
    return *g_backend;
}

[[nodiscard]] const Monarc::Array<Monarc::RHI::AdapterInfo>& Adapters() { return *g_adapters; }

/// Pumps `window` until the platform has nothing left to say.
///
/// Eight rounds and no wait, for TestWindow.cpp's reasons: some Win32 operations send their
/// messages synchronously from the call that provoked them, and handling one batch can generate
/// the next. This one discards the events -- what these cases read is `Window::ClientSize()`,
/// which the pump keeps up to date and does not clear.
void Settle(Window& window) {
    for (int i = 0; i < 8; ++i) {
        window.PumpEvents();
    }
}

/// A window, a device on one adapter, and a swapchain on both.
///
/// Opened by `Open` rather than by a constructor, because three fallible steps in a member
/// initialiser list cannot report which of them failed.
struct Harness {
    Monarc::SystemAllocator                      allocator;
    Monarc::Result<Window>                       window    = Monarc::Err(Monarc::ErrorCode::Unknown);
    Monarc::Result<Monarc::RHI::VulkanDevice>    device    = Monarc::Err(Monarc::ErrorCode::Unknown);
    Monarc::Result<Monarc::RHI::VulkanSwapchain> swapchain = Monarc::Err(Monarc::ErrorCode::Unknown);

    ~Harness() {
        // Reverse creation order, and it is not negotiable: a swapchain names the device's
        // entry points and the backend's instance, and a `VkSurfaceKHR` must not outlive the
        // window whose `HWND` it holds. Written out rather than left to member destruction
        // because the declaration order above would do it in the same order and that is a
        // property of the declarations rather than a stated intent.
        if (swapchain) {
            swapchain->Shutdown();
        }
        if (device) {
            device->Shutdown();
        }
        if (window) {
            window->Destroy();
        }
    }

    [[nodiscard]] Monarc::Status Open(const Monarc::RHI::AdapterInfo& adapter, bool readback) {
        window = Window::Create(kHarnessWindow);
        if (!window) {
            return Monarc::Status(std::unexpect, window.error());
        }
        Settle(*window);

        device = Backend().CreateDevice(allocator, adapter, Monarc::RHI::DeviceConfig{});
        if (!device) {
            return Monarc::Status(std::unexpect, device.error());
        }

        Monarc::RHI::SwapchainDescription description{};
        description.surface       = window->Surface();
        description.extent        = window->ClientSize();
        description.format        = kSwapchainFormat;
        description.allowReadback = readback;

        swapchain = Backend().CreateSwapchain(allocator, *device, description);
        if (!swapchain) {
            return Monarc::Status(std::unexpect, swapchain.error());
        }
        return {};
    }

    [[nodiscard]] bool Ready() const {
        return window.has_value() && device.has_value() && swapchain.has_value();
    }
};

/// Records the frame every case presents: barrier in, clear, optionally copy out, barrier to
/// present.
///
/// `staging` valid asks for the copy, which is what makes this the readback path. The extra
/// barriers it needs are the interesting part and each is commented where it is recorded.
[[nodiscard]] Monarc::Status RecordFrame(Monarc::RHI::ICommandList& list,
                                         Monarc::RHI::TextureHandle image,
                                         Monarc::RHI::Extent2D      extent,
                                         Monarc::RHI::BufferHandle  staging) {
    if (Monarc::Status begun = list.Begin(); !begun) {
        return begun;
    }

    // Barrier one. `ColorAttachmentOutput` on the *before* side and not `None`, and that is
    // paired with the acquire semaphore rather than free: `VulkanDeviceState::SubmitList` waits
    // on that semaphore at the colour-attachment-output stage, and a layout transition is a
    // write that has to be ordered after the wait. Naming the same stage is what chains them.
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

    // No draw call. A load-op clear happens when rendering begins, and it is the path A4's
    // render graph will take too -- Device.h's `LoadOp::Clear` says why there is no separate
    // clear command to test instead.
    if (Monarc::Status began = list.BeginRendering(rendering); !began) {
        return began;
    }
    list.EndRendering();

    Monarc::RHI::TextureLayout beforePresent = Monarc::RHI::TextureLayout::ColorAttachment;
    Monarc::RHI::PipelineStage presentSyncBefore =
        Monarc::RHI::PipelineStage::ColorAttachmentOutput;
    Monarc::RHI::Access presentAccessBefore = Monarc::RHI::Access::ColorAttachmentWrite;

    if (staging.IsValid()) {
        // **The readback, on the swapchain's own image, before it is presented.** This is the
        // step that makes the clear provable rather than visible: the bytes asserted are the
        // bytes the presentation engine is about to display.
        list.Barrier(Monarc::RHI::TextureBarrier(
            image, Monarc::RHI::TextureLayout::ColorAttachment,
            Monarc::RHI::TextureLayout::TransferSource,
            Monarc::RHI::PipelineStage::ColorAttachmentOutput, Monarc::RHI::PipelineStage::Copy,
            Monarc::RHI::Access::ColorAttachmentWrite, Monarc::RHI::Access::TransferRead));

        if (Monarc::Status copied = list.CopyTextureToBuffer(image, staging); !copied) {
            return copied;
        }

        // Waiting on the timeline makes the copy's writes *available*; it does not make them
        // visible to the host. A memory dependency into the host stage is what does, and
        // without it a mapped read is reading memory whose visibility nothing established --
        // Task 3's readback records the same barrier for the same reason.
        list.Barrier(Monarc::RHI::BufferBarrier{
            staging, Monarc::RHI::PipelineStage::Copy, Monarc::RHI::PipelineStage::Host,
            Monarc::RHI::Access::TransferWrite, Monarc::RHI::Access::HostRead});

        beforePresent       = Monarc::RHI::TextureLayout::TransferSource;
        presentSyncBefore   = Monarc::RHI::PipelineStage::Copy;
        presentAccessBefore = Monarc::RHI::Access::TransferRead;
    }

    // The last barrier. `PipelineStage::None` and `Access::None` on the after side because
    // there is no *command* after it: what reads the image next is the presentation engine, and
    // the render-finished semaphore -- signalled at ALL_COMMANDS, so after this transition --
    // is the dependency that covers it.
    list.Barrier(Monarc::RHI::TextureBarrier(
        image, beforePresent, Monarc::RHI::TextureLayout::PresentSource, presentSyncBefore,
        Monarc::RHI::PipelineStage::None, presentAccessBefore, Monarc::RHI::Access::None));

    return list.End();
}

/// What one frame did.
struct FrameResult {
    bool                        presented     = false;
    Monarc::RHI::AcquireOutcome outcome       = Monarc::RHI::AcquireOutcome::OutOfDate;
    Monarc::RHI::TextureHandle  texture       = {};
    Monarc::u32                 imageIndex    = 0;
    bool                        suboptimal    = false;
    Monarc::u64                 timelineValue = 0;
};

/// Runs one frame through the harness, asserting each step. `staging` valid asks for the
/// readback copy.
[[nodiscard]] FrameResult PresentOneFrame(Harness& harness, Monarc::RHI::BufferHandle staging) {
    FrameResult result{};

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = harness.device->BeginFrame();
    REQUIRE(commands.has_value());
    REQUIRE(*commands != nullptr);

    const Monarc::Result<Monarc::RHI::AcquiredImage> acquired = harness.swapchain->Acquire();
    REQUIRE(acquired.has_value());
    result.outcome = acquired->outcome;
    if (acquired->outcome != Monarc::RHI::AcquireOutcome::Acquired) {
        return result;
    }
    result.texture    = acquired->texture;
    result.imageIndex = acquired->index;
    result.suboptimal = acquired->suboptimal;

    REQUIRE(RecordFrame(**commands, acquired->texture, harness.swapchain->Extent(), staging)
                .has_value());

    const Monarc::Result<Monarc::u64> submitted =
        harness.swapchain->SubmitForPresent(harness.device->GraphicsQueue(), **commands);
    REQUIRE(submitted.has_value());
    result.timelineValue = *submitted;

    REQUIRE(harness.swapchain->Present().has_value());
    result.presented = true;
    return result;
}

/// The staging buffer a readback frame copies into, sized for `extent`.
[[nodiscard]] Monarc::u64 ReadbackByteCount(Monarc::RHI::Extent2D extent) {
    return static_cast<Monarc::u64>(extent.width) * extent.height *
           Monarc::RHI::BytesPerPixel(kSwapchainFormat);
}

void ReportBytes(const char* what, const char* adapterName, std::span<const Monarc::u8> bytes) {
    MONARC_LOG(SwapchainTest, Info,
               "{} on \"{}\": first pixel = ({}, {}, {}, {}) as blue, green, red, alpha; "
               "expected ({}, {}, {}, {})",
               what, adapterName, bytes[0], bytes[1], bytes[2], bytes[3],
               kExpectedSwapchainBytes[0], kExpectedSwapchainBytes[1],
               kExpectedSwapchainBytes[2], kExpectedSwapchainBytes[3]);
}

}  // namespace

TEST_CASE("a swapchain comes up on every adapter that can present, and says what it negotiated") {
    REQUIRE_FALSE(Adapters().IsEmpty());

    Monarc::usize presenting = 0;
    for (Monarc::usize i = 0; i < Adapters().Size(); ++i) {
        const Monarc::RHI::AdapterInfo& adapter = Adapters()[i];

        Harness harness;
        const Monarc::Status opened = harness.Open(adapter, false);
        if (!opened) {
            // **Reported and skipped, not asserted.** "This GPU cannot present to this window"
            // is a legitimate configuration on a machine with more than one -- see
            // `VulkanBackend::AdapterCanPresent` -- and a case that required every adapter to
            // present would fail on hardware Monarc is meant to support. What is not acceptable
            // is *assuming* it can, which is why the reason is logged.
            MONARC_LOG(SwapchainTest, Warning,
                       "adapter [{}] \"{}\" has no swapchain: {} -- {}", i, adapter.name,
                       Monarc::ToString(opened.error().code), opened.error().message);
            continue;
        }
        ++presenting;

        CHECK(harness.swapchain->IsInitialized());
        CHECK(harness.swapchain->ImageFormat() == kSwapchainFormat);
        CHECK(harness.swapchain->Extent() == harness.window->ClientSize());
        CHECK_FALSE(harness.swapchain->NeedsRecreation());
        // Readback was not asked for, so it must not be available. The other direction -- asked
        // for and available -- is the readback case's own first assertion.
        CHECK_FALSE(harness.swapchain->ReadbackAvailable());

        // **Swapchain images are not frames in flight**, and this is the assertion that says
        // so: `kFramesInFlight` is 2 and the surfaces on this machine report a minimum of 2, so
        // `minImageCount + 1` is 3. A backend that had tied the two counts together would
        // report 2 here and would still work, which is why the number is asserted rather than
        // left to a comment.
        CHECK(harness.swapchain->ImageCount() > Monarc::RHI::kFramesInFlight);

        MONARC_LOG(SwapchainTest, Info,
                   "adapter [{}] \"{}\": {}x{} | {} | {} image(s) | {} frame(s) in flight", i,
                   adapter.name, harness.swapchain->Extent().width,
                   harness.swapchain->Extent().height,
                   Monarc::RHI::ToString(harness.swapchain->ImageFormat()),
                   harness.swapchain->ImageCount(), Monarc::RHI::kFramesInFlight);
    }

    // At least one adapter must be able to present, or there is nothing this suite can test and
    // main() should have skipped rather than let it pass.
    CHECK(presenting >= 1);
}

TEST_CASE("ten frames are acquired and presented, with no out-of-date loop") {
    // The plan's own case: "create a swapchain against a real window, acquire and present ten
    // frames headlessly-but-really, and assert no validation output and no OUT_OF_DATE loop."
    //
    // No validation output is what the fatal messenger guarantees -- an error stops the process
    // -- and "a build that asked for validation actually has a messenger" is the case that keeps
    // that guarantee from being vacuous. What is left to assert here is the loop: with the
    // window untouched, every acquire must return an image, because a swapchain that reported
    // itself out of date every frame would present nothing and look exactly like one that
    // worked.
    REQUIRE_FALSE(Adapters().IsEmpty());

    for (const Monarc::RHI::AdapterInfo& adapter : Adapters()) {
        Harness harness;
        if (!harness.Open(adapter, false)) {
            continue;
        }

        Monarc::u32 presented = 0;
        Monarc::u32 outOfDate = 0;
        Monarc::u64 lastValue = 0;
        for (int frame = 0; frame < 10; ++frame) {
            harness.window->PumpEvents();
            const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
            if (result.outcome != Monarc::RHI::AcquireOutcome::Acquired) {
                ++outOfDate;
                REQUIRE(harness.swapchain
                            ->Recreate(harness.window->ClientSize())
                            .has_value());
                continue;
            }
            ++presented;
            // The timeline advances once per submission and never repeats -- which is what
            // `IDevice::BeginFrame`'s wait depends on, and what a swapchain submission signing
            // its own value alongside the binary semaphore must not break.
            CHECK(result.timelineValue > lastValue);
            lastValue = result.timelineValue;
        }

        MONARC_LOG(SwapchainTest, Info,
                   "\"{}\": {} frame(s) presented, {} out-of-date acquire(s), timeline at {}",
                   adapter.name, presented, outOfDate, lastValue);
        CHECK(presented == 10);
        CHECK(outOfDate == 0);
        CHECK(harness.device->GraphicsQueue().LastSubmittedValue() == lastValue);

        REQUIRE(harness.device->WaitIdle().has_value());
    }
}

TEST_CASE("every swapchain image is acquired in turn") {
    // **What the image count is actually for.** A swapchain that returned image 0 every time
    // would satisfy every other case here: the clear would land, the readback would pass, and
    // the picture would be right. It would also mean two of the three images were never used,
    // which is a present loop that has stopped rotating -- and the only way to see it is to
    // watch which index comes back.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    const Monarc::u32 imageCount = harness.swapchain->ImageCount();
    REQUIRE(imageCount > 0);
    REQUIRE(imageCount <= 16);

    bool seen[16] = {};
    for (Monarc::u32 frame = 0; frame < imageCount * 3; ++frame) {
        harness.window->PumpEvents();
        const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
        REQUIRE(result.outcome == Monarc::RHI::AcquireOutcome::Acquired);
        REQUIRE(result.imageIndex < imageCount);
        seen[result.imageIndex] = true;
    }

    Monarc::u32 distinct = 0;
    for (Monarc::u32 i = 0; i < imageCount; ++i) {
        if (seen[i]) {
            ++distinct;
        }
    }
    MONARC_LOG(SwapchainTest, Info, "{} of {} swapchain image(s) were acquired over {} frames",
               distinct, imageCount, imageCount * 3);
    CHECK(distinct == imageCount);

    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("THE SWAPCHAIN READBACK: the clear reaches the image that gets presented") {
    // **The headline of Task 4, and it runs on every adapter that can present.** Task 3 proved
    // a clear reached an offscreen texture; this proves it reaches the swapchain image the
    // presentation engine displays, which is a different claim and the one the phase is about.
    //
    // Every pixel is asserted and the bytes are logged per adapter rather than only on failure:
    // a result that differs between vendors is a finding and not a flake.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Monarc::usize adaptersRead = 0;
    for (const Monarc::RHI::AdapterInfo& adapter : Adapters()) {
        Harness harness;
        if (const Monarc::Status opened = harness.Open(adapter, true); !opened) {
            MONARC_LOG(SwapchainTest, Warning, "\"{}\" has no swapchain: {} -- {}",
                       adapter.name, Monarc::ToString(opened.error().code),
                       opened.error().message);
            continue;
        }

        if (!harness.swapchain->ReadbackAvailable()) {
            // **Stated and skipped, never silently asserted away.**
            // `VkSurfaceCapabilitiesKHR::supportedUsageFlags` is not required to offer
            // `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`; it is near-universal and not guaranteed. A
            // surface that does not offer it gets a working swapchain whose images cannot be
            // copied out of, and this case has nothing to measure on that adapter.
            MONARC_LOG(SwapchainTest, Warning,
                       "\"{}\" produced a swapchain whose surface does not offer transfer-source "
                       "images, so the readback did not run on it",
                       adapter.name);
            continue;
        }

        const Monarc::RHI::Extent2D extent = harness.swapchain->Extent();
        Monarc::RHI::BufferDescription bufferDescription{};
        bufferDescription.size     = ReadbackByteCount(extent);
        bufferDescription.usage    = Monarc::RHI::BufferUsage::TransferDestination;
        bufferDescription.location = Monarc::RHI::MemoryLocation::HostVisible;

        const Monarc::Result<Monarc::RHI::BufferHandle> staging =
            harness.device->CreateBuffer(bufferDescription);
        REQUIRE(staging.has_value());

        harness.window->PumpEvents();
        const FrameResult result = PresentOneFrame(harness, *staging);
        REQUIRE(result.outcome == Monarc::RHI::AcquireOutcome::Acquired);
        REQUIRE(result.presented);

        // The copy is part of the submission, so the timeline value the submission signals is
        // what says the bytes are there. Presenting first and waiting second is deliberate: the
        // image asserted below is the one that has been handed to the presentation engine.
        REQUIRE(harness.device->GraphicsQueue()
                    .Wait(result.timelineValue, kWaitTimeoutNanoseconds)
                    .has_value());

        const Monarc::Result<std::span<const Monarc::u8>> mapped =
            harness.device->MapBufferForRead(*staging);
        REQUIRE(mapped.has_value());
        REQUIRE(mapped->size() == ReadbackByteCount(extent));

        ReportBytes("swapchain readback", adapter.name, *mapped);

        CHECK((*mapped)[0] == kExpectedSwapchainBytes[0]);
        CHECK((*mapped)[1] == kExpectedSwapchainBytes[1]);
        CHECK((*mapped)[2] == kExpectedSwapchainBytes[2]);
        CHECK((*mapped)[3] == kExpectedSwapchainBytes[3]);

        // Every pixel, individually, which is what catches a copy whose row pitch was wrong: on
        // a 640-wide target a stride bug lands the second row's bytes somewhere inside this span
        // rather than past the end of it.
        Monarc::usize       matching = 0;
        const Monarc::usize pixels   = mapped->size() / 4;
        for (Monarc::usize pixel = 0; pixel < pixels; ++pixel) {
            const Monarc::u8* bytes = mapped->data() + pixel * 4;
            if (bytes[0] == kExpectedSwapchainBytes[0] &&
                bytes[1] == kExpectedSwapchainBytes[1] &&
                bytes[2] == kExpectedSwapchainBytes[2] &&
                bytes[3] == kExpectedSwapchainBytes[3]) {
                ++matching;
            }
        }
        MONARC_LOG(SwapchainTest, Info,
                   "swapchain readback on \"{}\": {} of {} pixel(s) exact", adapter.name,
                   matching, pixels);
        CHECK(matching == pixels);

        harness.device->UnmapBuffer(*staging);
        REQUIRE(harness.device->WaitIdle().has_value());
        harness.device->DestroyBuffer(*staging);
        ++adaptersRead;
    }

    // The suite must not pass having read back nothing. If every adapter's surface refused
    // transfer-source images this goes red, which is the finding rather than a silent skip.
    CHECK(adaptersRead >= 1);
}

TEST_CASE("THE SCREEN CAPTURE: the window's own pixels are the clear colour") {
    // **The only assertion in this suite that crosses the desktop compositor**, and the weakest
    // link in it is the desktop rather than Monarc: the window has to be visible, unobscured,
    // and composited before the pixel is read.
    //
    // Two traps are worth naming because they cost time. `PrintWindow`, and a `BitBlt` from the
    // *window's* device context, return black for a Vulkan window: the swapchain's contents
    // never enter the window's GDI surface, because the compositor puts them on screen
    // directly. So the capture is from the screen DC -- see
    // `WindowTestHooks::CaptureScreenPixel`. And the result can still be perturbed by occlusion
    // or by display colour management, which is why a failure here reports the bytes it got
    // rather than widening a tolerance until it fits.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    const bool foregrounded = WindowTestHooks::BringToForeground(*harness.window);
    MONARC_LOG(SwapchainTest, Info, "the window was brought to the front: {}", foregrounded);
    Settle(*harness.window);

    // **Presented repeatedly rather than once, and the reason is FIFO.** Each present waits for
    // a vertical blank, so forty frames is roughly two thirds of a second on a 60 Hz display --
    // long enough for the compositor to have put the newest frame on screen several times over.
    // A single frame followed by an immediate read is a race with the compositor, not a test.
    for (int frame = 0; frame < 40; ++frame) {
        harness.window->PumpEvents();
        const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
        if (result.outcome != Monarc::RHI::AcquireOutcome::Acquired) {
            REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());
        }
    }
    REQUIRE(harness.device->WaitIdle().has_value());
    Settle(*harness.window);

    const Monarc::RHI::Extent2D size = harness.window->ClientSize();
    REQUIRE_FALSE(size.IsEmpty());

    Monarc::u8 pixel[4] = {};
    const Monarc::Status captured =
        WindowTestHooks::CaptureScreenPixel(*harness.window,
                                            static_cast<Monarc::i32>(size.width / 2),
                                            static_cast<Monarc::i32>(size.height / 2), pixel);
    REQUIRE(captured.has_value());

    ReportBytes("screen capture", Adapters()[0].name, pixel);

    // Blue, green and red exactly. **The alpha byte is deliberately not asserted**: a 32-bit
    // `BI_RGB` device-independent bitmap has an unused fourth byte with no defined value, so
    // asserting it would be asserting about GDI's padding rather than about Monarc's clear. It
    // is logged above, so what it actually was is in the record either way.
    CHECK(pixel[0] == kExpectedSwapchainBytes[0]);
    CHECK(pixel[1] == kExpectedSwapchainBytes[1]);
    CHECK(pixel[2] == kExpectedSwapchainBytes[2]);
}

TEST_CASE("resizing the window and recreating the swapchain keeps presenting") {
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    const Monarc::RHI::Extent2D before = harness.swapchain->Extent();
    REQUIRE(before == harness.window->ClientSize());

    for (int frame = 0; frame < 3; ++frame) {
        harness.window->PumpEvents();
        REQUIRE(PresentOneFrame(harness, Monarc::RHI::BufferHandle{}).presented);
    }

    const Monarc::RHI::Extent2D target{800, 480};
    WindowTestHooks::RequestClientSize(*harness.window, target);
    Settle(*harness.window);
    REQUIRE(harness.window->ClientSize() == target);

    REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());
    CHECK(harness.swapchain->Extent() == target);
    CHECK(harness.swapchain->Extent() != before);
    CHECK_FALSE(harness.swapchain->NeedsRecreation());

    for (int frame = 0; frame < 5; ++frame) {
        harness.window->PumpEvents();
        const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
        CHECK(result.outcome == Monarc::RHI::AcquireOutcome::Acquired);
    }

    // Back down, so the shrink direction is covered too: a recreate that only ever grew would
    // hide an extent taken from a stale capability query.
    WindowTestHooks::RequestClientSize(*harness.window, before);
    Settle(*harness.window);
    REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());
    CHECK(harness.swapchain->Extent() == before);

    for (int frame = 0; frame < 5; ++frame) {
        harness.window->PumpEvents();
        CHECK(PresentOneFrame(harness, Monarc::RHI::BufferHandle{}).presented);
    }
    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("maximising the window and recreating the swapchain keeps presenting") {
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());
    const Monarc::RHI::Extent2D before = harness.swapchain->Extent();

    WindowTestHooks::Maximise(*harness.window);
    Settle(*harness.window);
    const Monarc::RHI::Extent2D maximised = harness.window->ClientSize();
    REQUIRE_FALSE(maximised.IsEmpty());
    CHECK(maximised.width > before.width);

    REQUIRE(harness.swapchain->Recreate(maximised).has_value());
    CHECK(harness.swapchain->Extent() == maximised);

    for (int frame = 0; frame < 5; ++frame) {
        harness.window->PumpEvents();
        CHECK(PresentOneFrame(harness, Monarc::RHI::BufferHandle{}).presented);
    }

    WindowTestHooks::Restore(*harness.window);
    Settle(*harness.window);
    REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());
    CHECK(harness.swapchain->Extent() == before);
    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("a minimised window parks the loop instead of producing a 0x0 swapchain") {
    // **The bug most first attempts have**, and the plan says so: a minimised window reports
    // `0 x 0`, and creating a swapchain with it is invalid. What this case asserts is the pair
    // of facts a frame loop needs -- the window reports no area, and `Recreate` refuses it by
    // name rather than producing a validation error -- and then that the loop resumes.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());
    const Monarc::RHI::Extent2D before = harness.swapchain->Extent();

    harness.window->PumpEvents();
    REQUIRE(PresentOneFrame(harness, Monarc::RHI::BufferHandle{}).presented);
    REQUIRE(harness.device->WaitIdle().has_value());

    WindowTestHooks::Minimise(*harness.window);
    Settle(*harness.window);
    REQUIRE(harness.window->ClientSize().IsEmpty());

    // The refusal, by code and not by crashing. `vkCreateSwapchainKHR` rejects a 0 x 0 extent
    // (VUID-VkSwapchainCreateInfoKHR-imageExtent-01689), which with the fatal messenger
    // installed would stop this process -- so a returned Status is the difference between a
    // suite that reports and a suite that dies.
    const Monarc::Status refused = harness.swapchain->Recreate(harness.window->ClientSize());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);

    // **And the swapchain it was asked about is untouched, which is the assertion this case was
    // written the wrong way round the first time.** `Recreate` refuses an empty extent *before*
    // it tears anything down, so a loop that asks at the wrong moment -- the window minimised
    // between its own size check and this call -- is left holding the swapchain it already had
    // rather than none at all. The first draft of this case asserted the opposite, and the run
    // is what corrected it: destroying on refusal would mean a transient race cost the caller a
    // working swapchain, which is strictly worse than nothing happening.
    CHECK(harness.swapchain->IsInitialized());
    CHECK(harness.swapchain->Extent() == before);

    WindowTestHooks::Restore(*harness.window);
    Settle(*harness.window);
    REQUIRE_FALSE(harness.window->ClientSize().IsEmpty());
    CHECK(harness.window->ClientSize() == before);

    // Resumed. This is the half that a loop which parked and never came back would fail, and
    // it recreates rather than carrying on with the swapchain it kept: a minimise-and-restore
    // is what a driver most often answers with `VK_ERROR_OUT_OF_DATE_KHR` on the next acquire.
    REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());
    CHECK(harness.swapchain->IsInitialized());
    CHECK(harness.swapchain->Extent() == before);

    for (int frame = 0; frame < 5; ++frame) {
        harness.window->PumpEvents();
        CHECK(PresentOneFrame(harness, Monarc::RHI::BufferHandle{}).presented);
    }
    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("a window moved to another monitor keeps presenting") {
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    const WindowTestHooks::MonitorInfo first = WindowTestHooks::Monitors(*harness.window);
    MONARC_LOG(SwapchainTest, Info,
               "{} monitor(s); the window starts on the one at ({}, {})-({}, {}) at {} DPI",
               first.monitorCount, first.left, first.top, first.right, first.bottom, first.dpi);

    if (first.monitorCount < 2) {
        MONARC_LOG(SwapchainTest, Warning,
                   "this machine reports one monitor, so the monitor-drag half of this case did "
                   "not run");
        return;
    }

    WindowTestHooks::MoveTo(*harness.window, first.left - 1000, first.top + 100);
    Settle(*harness.window);

    const WindowTestHooks::MonitorInfo second = WindowTestHooks::Monitors(*harness.window);
    MONARC_LOG(SwapchainTest, Info,
               "after the move it is on the one at ({}, {})-({}, {}) at {} DPI", second.left,
               second.top, second.right, second.bottom, second.dpi);
    REQUIRE(second.left != first.left);

    // **Both monitors report the same DPI here, so this does not exercise WM_DPICHANGED**, and
    // that is stated rather than implied: the window keeps its client size, so the swapchain
    // does not even need recreating. What the case does cover is a present to a surface whose
    // window has moved to a different display -- which some drivers answer with
    // `VK_SUBOPTIMAL_KHR`, and the loop below handles that the way a frame loop must.
    CHECK(second.dpi == first.dpi);

    Monarc::u32 recreations = 0;
    for (int frame = 0; frame < 10; ++frame) {
        harness.window->PumpEvents();
        if (harness.swapchain->NeedsRecreation() ||
            harness.swapchain->Extent() != harness.window->ClientSize()) {
            REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());
            ++recreations;
        }
        const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
        if (result.outcome != Monarc::RHI::AcquireOutcome::Acquired) {
            continue;
        }
        CHECK(result.presented);
    }
    MONARC_LOG(SwapchainTest, Info, "the move across monitors cost {} recreation(s)",
               recreations);

    WindowTestHooks::MoveTo(*harness.window, first.left + 100, first.top + 100);
    Settle(*harness.window);
    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("acquiring twice without presenting is refused rather than leaking a semaphore") {
    // **The one mistake in this interface that a validation layer catches late and a release
    // build not at all.** A successful acquire leaves a binary semaphore the presentation
    // engine will signal, and only a submission that waits on it consumes it; a second acquire
    // is `VUID-vkAcquireNextImageKHR-semaphore-01779`. The refusal is a returned Status in
    // every configuration, which is the whole point.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = harness.device->BeginFrame();
    REQUIRE(commands.has_value());

    const Monarc::Result<Monarc::RHI::AcquiredImage> first = harness.swapchain->Acquire();
    REQUIRE(first.has_value());
    REQUIRE(first->outcome == Monarc::RHI::AcquireOutcome::Acquired);

    const Monarc::Result<Monarc::RHI::AcquiredImage> second = harness.swapchain->Acquire();
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == Monarc::ErrorCode::InvalidArgument);

    // Presenting before the frame has been submitted is the other half of the same ordering,
    // and it is a *hang* rather than an error if it reaches the API: present waits on the
    // render-finished semaphore, and nothing would ever signal it.
    const Monarc::Status early = harness.swapchain->Present();
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error().code == Monarc::ErrorCode::InvalidArgument);

    // The frame still has to be finished, because the acquire above succeeded -- which is
    // exactly what the refusals are protecting. `Recreate` would do it too; this does it the
    // ordinary way.
    REQUIRE(RecordFrame(**commands, first->texture, harness.swapchain->Extent(),
                        Monarc::RHI::BufferHandle{})
                .has_value());
    REQUIRE(harness.swapchain->SubmitForPresent(harness.device->GraphicsQueue(), **commands)
                .has_value());
    REQUIRE(harness.swapchain->Present().has_value());
    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("a swapchain refuses a queue and a command list that are not its device's") {
    // **Two devices, which is what makes this testable at all** -- and this machine has two
    // adapters, so it is not a hypothetical. A frame submitted to the wrong device's queue is
    // not something a driver reports helpfully.
    if (Adapters().Size() < 2) {
        MONARC_LOG(SwapchainTest, Warning,
                   "this machine reports one adapter, so a foreign queue cannot be built; this "
                   "case did not run");
        return;
    }

    Harness first;
    REQUIRE(first.Open(Adapters()[0], false).has_value());

    Harness second;
    if (!second.Open(Adapters()[1], false)) {
        MONARC_LOG(SwapchainTest, Warning,
                   "the second adapter has no swapchain, so this case did not run");
        return;
    }

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = first.device->BeginFrame();
    REQUIRE(commands.has_value());
    const Monarc::Result<Monarc::RHI::AcquiredImage> acquired = first.swapchain->Acquire();
    REQUIRE(acquired.has_value());
    REQUIRE(acquired->outcome == Monarc::RHI::AcquireOutcome::Acquired);
    REQUIRE(RecordFrame(**commands, acquired->texture, first.swapchain->Extent(),
                        Monarc::RHI::BufferHandle{})
                .has_value());

    // The wrong queue: the right list, the right swapchain, another device's queue.
    const Monarc::Result<Monarc::u64> wrongQueue =
        first.swapchain->SubmitForPresent(second.device->GraphicsQueue(), **commands);
    REQUIRE_FALSE(wrongQueue.has_value());
    CHECK(wrongQueue.error().code == Monarc::ErrorCode::InvalidArgument);

    // The wrong swapchain: the right queue and list, another device's swapchain. Refused by
    // `ValidateForSubmit`, which is the shared guard `IQueue::Submit` uses -- so this is the
    // case that says the swapchain's submit did not skip it.
    const Monarc::Result<Monarc::u64> wrongSwapchain =
        second.swapchain->SubmitForPresent(second.device->GraphicsQueue(), **commands);
    REQUIRE_FALSE(wrongSwapchain.has_value());
    CHECK(wrongSwapchain.error().code == Monarc::ErrorCode::InvalidArgument);

    // And the right combination still works, so the refusals above are not passing because
    // submitting is broken.
    REQUIRE(first.swapchain->SubmitForPresent(first.device->GraphicsQueue(), **commands)
                .has_value());
    REQUIRE(first.swapchain->Present().has_value());
    REQUIRE(first.device->WaitIdle().has_value());
}

TEST_CASE("an image handle held across a recreation is stale rather than reused") {
    // ADR-0002's guarantee, applied to the one resource in the engine whose slots change hands
    // on a window resize. `ISwapchain::Recreate` releases every image's pool slot, which bumps
    // its generation -- so a frame loop that cached a handle gets a refusal and not the new
    // occupant of that slot.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = harness.device->BeginFrame();
    REQUIRE(commands.has_value());
    const Monarc::Result<Monarc::RHI::AcquiredImage> acquired = harness.swapchain->Acquire();
    REQUIRE(acquired.has_value());
    REQUIRE(acquired->outcome == Monarc::RHI::AcquireOutcome::Acquired);
    const Monarc::RHI::TextureHandle stale = acquired->texture;
    REQUIRE(stale.IsValid());

    REQUIRE(RecordFrame(**commands, stale, harness.swapchain->Extent(),
                        Monarc::RHI::BufferHandle{})
                .has_value());
    REQUIRE(harness.swapchain->SubmitForPresent(harness.device->GraphicsQueue(), **commands)
                .has_value());
    REQUIRE(harness.swapchain->Present().has_value());

    REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());

    // `BeginRendering` is the refusal that can be *observed*: it returns a Status. The three
    // `Barrier` overloads detect the same staleness and end the process instead, which
    // Monarc/RHI/Device.h argues at length about and which no in-process case can watch --
    // so this is the assertion, and that is the note.
    //
    // **Nothing is acquired for this frame, deliberately.** The first draft of this case
    // acquired an image, recorded only the refused pass, and presented anyway -- which
    // presented a swapchain image still in `VK_IMAGE_LAYOUT_UNDEFINED` and stopped the process
    // at `VUID-VkPresentInfoKHR-pImageIndices-01430`. The run is what found it. A frame that
    // records nothing must not be presented, so this one is submitted through
    // `IQueue::Submit` -- the non-presenting overload -- and the fresh image gets a frame of
    // its own below.
    const Monarc::Result<Monarc::RHI::ICommandList*> afterwards = harness.device->BeginFrame();
    REQUIRE(afterwards.has_value());
    REQUIRE((*afterwards)->Begin().has_value());

    const Monarc::RHI::ColorAttachment attachments[1] = {
        {stale, Monarc::RHI::LoadOp::Clear, Monarc::RHI::StoreOp::Store, kClearColor}};
    Monarc::RHI::RenderingDescription rendering{};
    rendering.extent           = harness.swapchain->Extent();
    rendering.colorAttachments = attachments;

    const Monarc::Status refused = (*afterwards)->BeginRendering(rendering);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);

    REQUIRE((*afterwards)->End().has_value());
    REQUIRE(harness.device->GraphicsQueue().Submit(**afterwards).has_value());
    REQUIRE(harness.device->WaitIdle().has_value());

    // The fresh handle for the same image works, which is what says the slot was reclaimed
    // rather than abandoned -- and it goes through a whole frame, so the assertion is that the
    // image is usable rather than merely that a different number came back.
    const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
    REQUIRE(result.outcome == Monarc::RHI::AcquireOutcome::Acquired);
    CHECK(result.presented);
    CHECK(result.texture.IsValid());
    // Different from the handle held across the recreation, which is the generation bump
    // `ReleaseTextureSlot` performs on release: a slot that only bumped on *claim* would hand
    // back the same handle here and the refusal above would have been the `live` flag's work
    // rather than the generation's. `TextureSlot::generation` records the measurement.
    CHECK(result.texture != stale);
    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("a swapchain that has been shut down answers every query rather than dereferencing") {
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    harness.swapchain->Shutdown();
    harness.swapchain->Shutdown();

    CHECK_FALSE(harness.swapchain->IsInitialized());
    CHECK(harness.swapchain->Extent().IsEmpty());
    CHECK(harness.swapchain->ImageFormat() == Monarc::RHI::Format::Unknown);
    CHECK(harness.swapchain->ImageCount() == 0);
    CHECK_FALSE(harness.swapchain->ReadbackAvailable());
    CHECK_FALSE(harness.swapchain->NeedsRecreation());

    CHECK_FALSE(harness.swapchain->Acquire().has_value());
    CHECK_FALSE(harness.swapchain->Present().has_value());
    // Recreate refuses too, and its message says why: the surface went with the shutdown, so
    // only a fresh `CreateSwapchain` can replace it.
    const Monarc::Status recreated =
        harness.swapchain->Recreate(harness.window->ClientSize());
    REQUIRE_FALSE(recreated.has_value());
    CHECK(recreated.error().code == Monarc::ErrorCode::InvalidArgument);
}

TEST_CASE("a swapchain is refused what it cannot honour") {
    REQUIRE_FALSE(Adapters().IsEmpty());

    Monarc::SystemAllocator allocator;
    Monarc::Result<Window>  window = Window::Create(kHarnessWindow);
    REQUIRE(window.has_value());

    Monarc::Result<Monarc::RHI::VulkanDevice> device =
        Backend().CreateDevice(allocator, Adapters()[0], Monarc::RHI::DeviceConfig{});
    REQUIRE(device.has_value());

    const auto attempt = [&](const Monarc::RHI::SwapchainDescription& description) {
        return Backend().CreateSwapchain(allocator, *device, description);
    };

    Monarc::RHI::SwapchainDescription base{};
    base.surface = window->Surface();
    base.extent  = window->ClientSize();
    base.format  = kSwapchainFormat;

    {
        // No window. `SurfaceDescription::IsEmpty()` is what refuses it, before any Vulkan call.
        Monarc::RHI::SwapchainDescription description = base;
        description.surface                          = {};
        const auto refused                           = attempt(description);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);
    }
    {
        // An empty extent, which is the minimised case arriving through creation rather than
        // through `Recreate`.
        Monarc::RHI::SwapchainDescription description = base;
        description.extent                            = {};
        const auto refused                            = attempt(description);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);
    }
    {
        Monarc::RHI::SwapchainDescription description = base;
        description.format                           = Monarc::RHI::Format::Unknown;
        const auto refused                           = attempt(description);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);
    }
    {
        // A format the surface does not offer. **Refused rather than substituted**, which is
        // what keeps every byte this suite asserts a statement about the format that was asked
        // for. `R8G8B8A8_UNORM` is a real format that Windows surfaces do not present in.
        Monarc::RHI::SwapchainDescription description = base;
        description.format                           = Monarc::RHI::Format::R8G8B8A8_UNORM;
        const auto refused                           = attempt(description);
        if (refused.has_value()) {
            // A finding rather than a failure: a surface that *does* offer it is allowed to,
            // and the assertion below would then be wrong about this machine rather than about
            // Monarc.
            MONARC_LOG(SwapchainTest, Warning,
                       "this surface offers R8G8B8A8_UNORM, so the format-refusal half of this "
                       "case did not run");
        } else {
            CHECK(refused.error().code == Monarc::ErrorCode::Unsupported);
        }
    }

    // And the base description still works, so the four refusals are not passing because
    // swapchain creation is broken.
    Monarc::Result<Monarc::RHI::VulkanSwapchain> ok = attempt(base);
    REQUIRE(ok.has_value());
    ok->Shutdown();
    device->Shutdown();
    window->Destroy();
}

TEST_CASE("a build that asked for validation actually has a messenger") {
    // Task 3's case, repeated here for Task 4's reason: "assert no validation output" is a claim
    // the fatal messenger enforces, and it means nothing if no messenger was ever installed.
    // This suite's whole no-validation-output guarantee rests on it.
    if (Monarc::RHI::ValidationDefault()) {
        CHECK(Backend().ValidationLayerEnabled());
        CHECK(Backend().DebugMessengerInstalled());
    } else {
        CHECK_FALSE(Backend().ValidationLayerEnabled());
        CHECK_FALSE(Backend().DebugMessengerInstalled());
    }
}

int main(int argc, char** argv) {
    const char* libraryName = nullptr;

    // doctest's own option parsing would object to an argument it does not recognise, so
    // --vulkan-library is stripped out here and everything else handed through unchanged. It is
    // the lever that makes the skip path *testable*: pass a name that cannot resolve and the
    // loader fails exactly as it would on a machine with no Vulkan.
    Monarc::SystemAllocator allocator;
    Monarc::Array<char*>    forwarded(allocator);
    forwarded.Reserve(static_cast<Monarc::usize>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.starts_with(kLibraryOption)) {
            libraryName = argv[i] + kLibraryOption.size();
            continue;
        }
        forwarded.Push(argv[i]);
    }

    if (!WindowPlatform::HasInteractiveSession()) {
        MONARC_LOG(SwapchainTest, Info,
                   "skipping the swapchain tests: this process has no interactive session to "
                   "put a window in (returning {} so CTest reports Skipped rather than Passed)",
                   kSkipReturnCode);
        return kSkipReturnCode;
    }

    Monarc::RHI::VulkanBackend::Config config{};
    config.applicationName = "Monarc.Host.Windowed.DeviceTests";
    config.libraryName     = libraryName;

    Monarc::Result<Monarc::RHI::VulkanBackend> created =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    if (!created) {
        MONARC_LOG(SwapchainTest, Info,
                   "skipping the swapchain tests: {} -- {} (returning {} so CTest reports "
                   "Skipped rather than Passed)",
                   Monarc::ToString(created.error().code), created.error().message,
                   kSkipReturnCode);
        return kSkipReturnCode;
    }
    Monarc::RHI::VulkanBackend& backend = *created;

    Monarc::Array<Monarc::RHI::AdapterInfo> adapters(allocator);
    if (const Monarc::Status enumerated = backend.EnumerateAdapters(adapters); !enumerated) {
        MONARC_LOG(SwapchainTest, Info,
                   "skipping the swapchain tests: enumeration failed -- {} -- {}",
                   Monarc::ToString(enumerated.error().code), enumerated.error().message);
        return kSkipReturnCode;
    }
    if (adapters.IsEmpty()) {
        MONARC_LOG(SwapchainTest, Info,
                   "skipping the swapchain tests: the Vulkan instance came up at {}.{}.{} and "
                   "reports no physical devices",
                   backend.InstanceApiVersion().major, backend.InstanceApiVersion().minor,
                   backend.InstanceApiVersion().patch);
        return kSkipReturnCode;
    }

    MONARC_LOG(SwapchainTest, Info, "instance {}.{}.{} | {} adapter(s) after dedupe",
               backend.InstanceApiVersion().major, backend.InstanceApiVersion().minor,
               backend.InstanceApiVersion().patch, adapters.Size());

    g_backend  = &backend;
    g_adapters = &adapters;

    doctest::Context context;
    context.applyCommandLine(static_cast<int>(forwarded.Size()), forwarded.Data());
    const int failures = context.run();

    // Cleared before the backend and the array go out of scope, so a doctest reporter or an
    // at-exit handler cannot reach a destroyed object.
    g_adapters = nullptr;
    g_backend  = nullptr;
    return failures;
}
