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

/// Reports a pixel, naming what its fourth byte actually is.
///
/// **`fourthByte` is a parameter because the two callers disagree about it, and one shared
/// format string calling it "alpha" was wrong for one of them.** The swapchain readback's
/// fourth byte is the image's alpha channel and reads 255; the screen capture's is the unused
/// byte of a 32-bit `BI_RGB` DIB, which has no defined value and is GDI's padding rather than
/// anything Monarc wrote. The capture case deliberately does not assert it -- see it -- so a
/// log line calling it alpha was the only place that claimed otherwise.
void ReportBytes(const char* what, const char* adapterName, std::span<const Monarc::u8> bytes,
                 const char* fourthByte) {
    MONARC_LOG(SwapchainTest, Info,
               "{} on \"{}\": first pixel = ({}, {}, {}, {}) as blue, green, red, {}; "
               "expected ({}, {}, {}, {})",
               what, adapterName, bytes[0], bytes[1], bytes[2], bytes[3], fourthByte,
               kExpectedSwapchainBytes[0], kExpectedSwapchainBytes[1],
               kExpectedSwapchainBytes[2], kExpectedSwapchainBytes[3]);
}

/// Asserts that a swapchain which is not initialised answers every query as empty and refuses
/// every fallible member.
///
/// **Four fallible members and not three.** `SubmitForPresent` carries the same
/// `IsInitialized()` guard as `Acquire`, `Present` and `Recreate`, and was the one of the four
/// no case had ever called on a swapchain that was not initialised.
///
/// `NeedsRecreation()` and `Recreate` are deliberately absent: neither answers the same way in
/// the two states that reach here. `NeedsRecreation()` is not gated on `IsInitialized()` on
/// purpose -- a recreation that failed leaves it set, which is exactly the caller that has to
/// see it -- and `Recreate` refuses a shut-down swapchain, whose surface is gone, while
/// retrying a failed one. Each case asserts those itself.
void CheckAnsweredAsEmpty(Harness& harness) {
    REQUIRE(harness.swapchain.has_value());
    Monarc::RHI::VulkanSwapchain& swapchain = *harness.swapchain;

    CHECK_FALSE(swapchain.IsInitialized());
    // **The three accessors agree with `IsInitialized()`, which is the pair of readings that
    // contradicted each other before a review.** A recreation that failed after
    // `vkCreateSwapchainKHR` had already succeeded reported `IsInitialized() == true` beside
    // an `ImageCount()` of 0 and an `Extent()` of `0 x 0`.
    CHECK(swapchain.Extent().IsEmpty());
    CHECK(swapchain.ImageCount() == 0);
    CHECK(swapchain.ImageFormat() == Monarc::RHI::Format::Unknown);
    CHECK_FALSE(swapchain.ReadbackAvailable());

    // The one that reached Vulkan with a null semaphore and a null fence --
    // `VUID-vkAcquireNextImageKHR-semaphore-01780`, fatal under the Debug messenger and
    // silent in Release.
    CHECK_FALSE(swapchain.Acquire().has_value());
    CHECK_FALSE(swapchain.Present().has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = harness.device->BeginFrame();
    REQUIRE(commands.has_value());
    CHECK_FALSE(
        swapchain.SubmitForPresent(harness.device->GraphicsQueue(), **commands).has_value());
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

        ReportBytes("swapchain readback", adapter.name, *mapped, "alpha");

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
    // Three traps are worth naming because each cost time. `PrintWindow`, and a `BitBlt` from
    // the *window's* device context, return black for a Vulkan window: the swapchain's contents
    // never enter the window's GDI surface, because the compositor puts them on screen
    // directly, so the capture is from the screen DC -- see
    // `WindowTestHooks::CaptureScreenPixel`. A fixed number of frames before reading makes the
    // answer depend on how long the previous case took. And the desktop has other tenants: for
    // an afternoon this read a stable `(106, 71, 35)` -- the clear at 55% -- because a
    // `Shell_SystemDim` overlay was over the window while a Windows Security dialog was open.
    // Each of those is handled below, and none of them by loosening what is asserted.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    const Monarc::RHI::Extent2D size = harness.window->ClientSize();
    REQUIRE_FALSE(size.IsEmpty());
    const Monarc::i32 centreX = static_cast<Monarc::i32>(size.width / 2);
    const Monarc::i32 centreY = static_cast<Monarc::i32>(size.height / 2);

    // **Put somewhere the window is actually the top one, trying more than one monitor.**
    // Windows opens a `CW_USEDEFAULT` window in a cascade, so where it lands depends on how
    // many windows the process has already opened -- which made this case's answer depend on
    // which cases ran before it. And a pinned position is not enough on its own: Windows'
    // `Shell_SystemDim` overlay covers the monitor a system security dialog is on, so a window
    // pinned to *that* monitor reads the dim's blend and nothing else, for as long as the
    // dialog is up. Measured; `WindowTestHooks::WindowAtClientPoint` records the diagnosis.
    //
    // Two candidates, and neither names a monitor: where the window already is, then the
    // top-left of the virtual screen -- which on a one-monitor machine is the same place and on
    // this one is the other display. The first where the window is genuinely on top wins.
    const WindowTestHooks::MonitorInfo monitor = WindowTestHooks::Monitors(*harness.window);
    struct Candidate {
        Monarc::i32 x;
        Monarc::i32 y;
    };
    const Candidate candidates[] = {{monitor.left + 64, monitor.top + 64},
                                    {monitor.virtualLeft + 64, monitor.virtualTop + 64}};

    bool onTop = false;
    for (const Candidate& candidate : candidates) {
        WindowTestHooks::MoveTo(*harness.window, candidate.x, candidate.y);
        Settle(*harness.window);
        const bool foregrounded = WindowTestHooks::BringToForeground(*harness.window);
        Settle(*harness.window);

        const WindowTestHooks::PointOwner owner =
            WindowTestHooks::WindowAtClientPoint(*harness.window, centreX, centreY);
        MONARC_LOG(SwapchainTest, Info,
                   "at ({}, {}): fronted {}, and the top window at the capture point is \"{}\" "
                   "(Monarc's: {})",
                   candidate.x, candidate.y, foregrounded, owner.className, owner.isOurs);
        if (owner.isOurs) {
            onTop = true;
            break;
        }
    }

    // **Presented in batches until two consecutive captures agree, rather than after a fixed
    // number of frames -- and the reason is a measurement, not caution.** Forty frames was the
    // first attempt, about two thirds of a second under FIFO on a 60 Hz display, and it read
    // correctly when the case ran with the rest of the suite in front of it and wrongly when it
    // ran alone. Waiting for the reading to stop changing removes that dependence on how long
    // the previous case took.
    //
    // It does **not** widen what is asserted: the settled value is compared against the exact
    // bytes below, so a window that settled on the wrong colour fails and reports what it read.
    Monarc::u8            previous[4]     = {};
    Monarc::u8            pixel[4]        = {};
    bool                  settled         = false;
    Monarc::u32           batches         = 0;
    Monarc::u32           presented       = 0;
    constexpr Monarc::u32 kFramesPerBatch = 20;
    constexpr Monarc::u32 kMaxBatches     = 25;

    for (; batches < kMaxBatches && !settled; ++batches) {
        for (Monarc::u32 frame = 0; frame < kFramesPerBatch; ++frame) {
            harness.window->PumpEvents();
            const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
            if (result.outcome != Monarc::RHI::AcquireOutcome::Acquired) {
                REQUIRE(harness.swapchain->Recreate(harness.window->ClientSize()).has_value());
                continue;
            }
            ++presented;
        }
        REQUIRE(harness.device->WaitIdle().has_value());
        Settle(*harness.window);

        REQUIRE(WindowTestHooks::CaptureScreenPixel(*harness.window, centreX, centreY, pixel)
                    .has_value());
        if (batches > 0 && pixel[0] == previous[0] && pixel[1] == previous[1] &&
            pixel[2] == previous[2]) {
            settled = true;
        }
        for (int i = 0; i < 4; ++i) {
            previous[i] = pixel[i];
        }
    }

    MONARC_LOG(SwapchainTest, Info,
               "the screen reading settled after {} batch(es) of {} frame(s) -- {} presented in "
               "all; settled: {}",
               batches, kFramesPerBatch, presented, settled);
    CHECK(settled);

    // "GDI padding" and not "alpha": a 32-bit `BI_RGB` DIB's fourth byte has no defined value.
    // It has read 255 on every run of this that was Monarc's to read, and it is not asserted.
    ReportBytes("screen capture", Adapters()[0].name, pixel, "GDI padding");

    // **Whose pixels those were, asked before they are asserted about.** The desktop has other
    // tenants, and a screen capture that did not check would sooner or later assert about one
    // of them -- which is exactly what happened: a stable `(106, 71, 35)`, the expected bytes
    // at 55%, from a layered topmost `Shell_SystemDim`.
    //
    // **Asserted and not skipped, and that is a correction to how this case first shipped.**
    // It used to `return` here, which meant a desktop with an overlay over the window reported
    // this case green having asserted nothing at all about its own subject. Measured rather
    // than argued: forcing `PointOwner::isOurs` to false in `WindowAtClientPoint` left
    // `ctest -R Monarc.Host.Windowed.DeviceTests --output-on-failure` printing `Passed` and
    // nothing else, 15 of 15 cases green, and the only trace was the assertion total dropping
    // from 884 to 881 -- the three exact-byte checks below, which are the entire reason this
    // case exists. A three-in-884 drop is invisible without diffing two runs.
    //
    // So the two facts the capture depends on are `REQUIRE`s. A desktop that got in the way is
    // now a red case whose log names the tenant, which is a finding; the bytes below are
    // unchanged, so this is a refusal to decline quietly rather than a widened tolerance. The
    // swapchain readback above remains the assertion that needs nothing of the desktop.
    //
    // **Two assertions and not one, because they are two different failures** -- and the single
    // line that reported both said something self-contradictory whenever only the second was
    // false: "the topmost window at the capture point is `Monarc.Host.Windowed.Window` and not
    // Monarc's on any monitor tried". `onTop` is about the candidate loop above; `owner.isOurs`
    // is about this instant, several hundred presented frames later, during which something
    // could have come over a window that had been on top when it was positioned.
    const WindowTestHooks::PointOwner owner =
        WindowTestHooks::WindowAtClientPoint(*harness.window, centreX, centreY);
    if (!onTop) {
        MONARC_LOG(SwapchainTest, Error,
                   "the window was not the top one at the capture point on any monitor tried, "
                   "so the screen reading is not Monarc's; \"{}\" was there at the last look, "
                   "and the reading was ({}, {}, {}, {})",
                   owner.className, pixel[0], pixel[1], pixel[2], pixel[3]);
    } else if (!owner.isOurs) {
        MONARC_LOG(SwapchainTest, Error,
                   "the window was the top one where it was positioned, but \"{}\" is over the "
                   "capture point now, so the screen reading is not Monarc's; it read "
                   "({}, {}, {}, {})",
                   owner.className, pixel[0], pixel[1], pixel[2], pixel[3]);
    }
    REQUIRE(onTop);
    REQUIRE(owner.isOurs);

    // Blue, green and red exactly. **The alpha byte is deliberately not asserted**: a 32-bit
    // `BI_RGB` device-independent bitmap has an unused fourth byte with no defined value, so
    // asserting it would be asserting about GDI's padding rather than about Monarc's clear. It
    // is logged above, so what it actually was is in the record either way -- and it has read
    // 255 on every run of this that was Monarc's to read.
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

    // **A second monitor is a requirement this case declares, not a condition it declines
    // on.** It used to `return` here on `monitorCount < 2`, which is the same silent-green
    // shape the screen-capture case had: everything below is the case's whole subject, so a
    // one-monitor machine reported it green having asserted nothing about a monitor change.
    // Forcing `MonitorInfo::monitorCount` to 1 in `WindowTestHooks::Monitors` was enough to
    // make the case pass with two assertions instead of nine.
    //
    // A `CHECK` rather than a `REQUIRE`, because the presenting half below does not need a
    // second monitor and is worth running either way -- so a one-monitor machine gets one
    // named failure here plus the whole present loop, rather than a green case and a warning
    // nobody diffs. That is a deliberate trade: this suite already requires a GPU and an
    // interactive session, and declaring the second monitor alongside them is cheaper than a
    // coverage hole that only shows up as a smaller assertion count.
    CHECK(first.monitorCount >= 2);
    const bool crossMonitor = first.monitorCount >= 2;

    // Off to the left, which on this machine is `\\.\DISPLAY5` at `(-1920, 0)`; within the one
    // monitor there is where the count says there is only one, so the present loop below
    // exercises a move on any machine.
    WindowTestHooks::MoveTo(*harness.window, crossMonitor ? first.left - 1000 : first.left + 200,
                            first.top + 100);
    Settle(*harness.window);

    const WindowTestHooks::MonitorInfo second = WindowTestHooks::Monitors(*harness.window);
    MONARC_LOG(SwapchainTest, Info,
               "after the move it is on the one at ({}, {})-({}, {}) at {} DPI", second.left,
               second.top, second.right, second.bottom, second.dpi);

    if (crossMonitor) {
        REQUIRE(second.left != first.left);

        // **Both monitors report the same DPI here, so this does not exercise WM_DPICHANGED**,
        // and that is stated rather than implied: the window keeps its client size, so the
        // swapchain does not even need recreating. What the case does cover is a present to a
        // surface whose window has moved to a different display -- which some drivers answer
        // with `VK_SUBOPTIMAL_KHR`, and the loop below handles that the way a frame loop must.
        CHECK(second.dpi == first.dpi);
    } else {
        MONARC_LOG(SwapchainTest, Warning,
                   "this machine reports one monitor, so the move above was within it and the "
                   "cross-monitor half of this case is unexercised here -- the failed CHECK "
                   "above is what says so, rather than a return nobody sees");
        // The move stayed on the one monitor, which is the only thing there is to assert about
        // it -- and it is an assertion rather than nothing.
        CHECK(second.left == first.left);
    }

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

TEST_CASE("acquiring without a BeginFrame in between is what the acquire-slot wait is for") {
    // **The sequence that reaches `AcquireSlot::consumedByTimelineValue`'s wait with nothing
    // else having covered it**, walked here because the comment beside that wait used to
    // justify it with a case that cannot happen. `BeginFrame` advances the device's frame index
    // on every success and `acquireCursor` only on a successful acquire, so the frame index runs
    // ahead of the cursor and never behind -- and ahead means `BeginFrame`'s wait is on a
    // timeline value at least as new as the one the reused acquire slot recorded. The "drifted"
    // case is therefore the one that *cannot* strain the wait.
    //
    // What can is this: `Acquire` never consults the device's frame state, and `phase` is
    // `Idle` again after `Present`, so acquiring before this frame's `BeginFrame` -- or without
    // one at all -- is not refused. After two ordinary frames each acquire slot holds a
    // recorded value and *neither* frame slot has been waited on, so the third acquire finds
    // slot 0's value uncovered.
    //
    // **What this case asserts is that the sequence exists and completes** -- not that the wait
    // blocked, which depends on how far the GPU has got and is not something this machine
    // produces on demand. The queue's completed value is logged immediately before the third
    // acquire so the record says how close it came. Twelve runs in isolation: the recorded
    // value is always 1 and the queue has always submitted up to 2, and the completed value
    // read 2 ten times and 1 twice -- so the *second* frame's submission was still in flight
    // twice, one step from the first frame's being in flight too. A reading of 0 is what would
    // make the wait block; it did not occur.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    Monarc::u64 firstValue = 0;
    for (int frame = 0; frame < 2; ++frame) {
        harness.window->PumpEvents();
        const FrameResult result = PresentOneFrame(harness, Monarc::RHI::BufferHandle{});
        REQUIRE(result.presented);
        if (frame == 0) {
            firstValue = result.timelineValue;
        }
    }
    REQUIRE(firstValue != 0);

    // The value the guard is about to wait on is the first frame's, because the acquire cursor
    // has two slots and has just wrapped back to the one that frame used.
    const Monarc::Result<Monarc::u64> completed =
        harness.device->GraphicsQueue().CompletedValue();
    REQUIRE(completed.has_value());
    MONARC_LOG(SwapchainTest, Info,
               "before an acquire with no BeginFrame in front of it: the acquire slot recorded "
               "{}, the queue has submitted up to {} and completed {}",
               firstValue, harness.device->GraphicsQueue().LastSubmittedValue(), *completed);

    const Monarc::Result<Monarc::RHI::AcquiredImage> acquired = harness.swapchain->Acquire();
    REQUIRE(acquired.has_value());
    CHECK(acquired->outcome == Monarc::RHI::AcquireOutcome::Acquired);

    // And the frame finishes, which is what makes this a usable sequence rather than a call
    // that merely was not refused -- the list comes from a `BeginFrame` taken *after* the
    // acquire.
    const Monarc::Result<Monarc::RHI::ICommandList*> commands = harness.device->BeginFrame();
    REQUIRE(commands.has_value());
    REQUIRE(RecordFrame(**commands, acquired->texture, harness.swapchain->Extent(),
                        Monarc::RHI::BufferHandle{})
                .has_value());
    REQUIRE(harness.swapchain->SubmitForPresent(harness.device->GraphicsQueue(), **commands)
                .has_value());
    REQUIRE(harness.swapchain->Present().has_value());
    REQUIRE(harness.device->WaitIdle().has_value());
}

TEST_CASE("a swapchain refuses a queue and a command list that are not its device's") {
    // **Two devices on *one* adapter, which is what lets this case run everywhere.** The
    // subject is device identity and not adapter identity: both guards below are address
    // comparisons -- `&queue != &m_state->device->queue` in `VulkanSwapchain::SubmitForPresent`
    // and `VulkanDeviceState::FindOwnList` under `ValidateForSubmit` -- so a second `VkDevice`
    // on `Adapters()[0]` is as foreign as one on another adapter by every test they apply.
    //
    // It used to build the second device on `Adapters()[1]`, which cost it two `return`s it
    // did not need: one on `Adapters().Size() < 2`, one on the second adapter declining to
    // present. Either left the case reporting green having asserted nothing about its own
    // subject, and neither condition is one this suite may assume away -- "a case that
    // required every adapter to present would fail on hardware Monarc is meant to support" is
    // argued in "a swapchain comes up on every adapter that can present".
    // `VulkanBackend::CreateDevice` takes an `AdapterInfo` and keeps nothing per adapter, so
    // calling it twice with the same one is two `VkDevice`s, and the precondition drops to the
    // one every case here that does not loop over the adapters already has: `Adapters()[0]`
    // opens.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness first;
    REQUIRE(first.Open(Adapters()[0], false).has_value());

    Harness second;
    REQUIRE(second.Open(Adapters()[0], false).has_value());

    // One adapter, asserted rather than assumed. `CreateDevice` re-describes the adapter from
    // the driver instead of copying the argument through, so this is the driver's own answer
    // twice over and it says the two devices sit on the same physical device.
    CHECK(first.device->Adapter().uuid == second.device->Adapter().uuid);
    MONARC_LOG(SwapchainTest, Info,
               "two devices and two swapchains on adapter [0] \"{}\" ({})",
               first.device->Adapter().name,
               Monarc::RHI::ToString(first.device->Adapter().uuid).text);

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = first.device->BeginFrame();
    REQUIRE(commands.has_value());
    const Monarc::Result<Monarc::RHI::AcquiredImage> acquired = first.swapchain->Acquire();
    REQUIRE(acquired.has_value());
    REQUIRE(acquired->outcome == Monarc::RHI::AcquireOutcome::Acquired);
    REQUIRE(RecordFrame(**commands, acquired->texture, first.swapchain->Extent(),
                        Monarc::RHI::BufferHandle{})
                .has_value());

    // **The second device records a frame of its own before the refusals, and the order is
    // load-bearing.** `SubmitForPresent` checks the queue, then the acquire phase, then the
    // command list, so a second swapchain that had not acquired refuses a foreign list at the
    // *phase* guard and `ValidateForSubmit` is never reached. That is what the two-adapter
    // version of this case did while its comment named the guard it did not reach: the message
    // it actually returned was "needs an image this swapchain has acquired and not yet
    // submitted; call Acquire first", measured by logging it. Both refusals are
    // `InvalidArgument`, so nothing but the message distinguishes them -- which is why the
    // messages below are asserted and not only the codes.
    const Monarc::Result<Monarc::RHI::ICommandList*> otherCommands =
        second.device->BeginFrame();
    REQUIRE(otherCommands.has_value());
    const Monarc::Result<Monarc::RHI::AcquiredImage> otherAcquired = second.swapchain->Acquire();
    REQUIRE(otherAcquired.has_value());
    REQUIRE(otherAcquired->outcome == Monarc::RHI::AcquireOutcome::Acquired);
    REQUIRE(RecordFrame(**otherCommands, otherAcquired->texture, second.swapchain->Extent(),
                        Monarc::RHI::BufferHandle{})
                .has_value());

    // The wrong queue: the right list, the right swapchain, the other device's queue. Refused
    // by `SubmitForPresent`'s own comparison, and that comparison is the only thing standing
    // in the way -- the submission underneath goes through `m_state->device->SubmitList` and
    // never reads the queue it was handed, so deleting the guard makes this call *succeed*.
    const Monarc::Result<Monarc::u64> wrongQueue =
        first.swapchain->SubmitForPresent(second.device->GraphicsQueue(), **commands);
    REQUIRE_FALSE(wrongQueue.has_value());
    CHECK(wrongQueue.error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(wrongQueue.error().message.find("queue that does not belong") !=
          std::string_view::npos);

    // The wrong list: the other swapchain, its own queue, and a list belonging to neither of
    // them. Refused by `ValidateForSubmit`, the shared guard `IQueue::Submit` uses -- so this
    // is the case that says the swapchain's submit did not skip it, and the message is what
    // says the refusal came from there rather than from either guard in front of it.
    const Monarc::Result<Monarc::u64> wrongList =
        second.swapchain->SubmitForPresent(second.device->GraphicsQueue(), **commands);
    REQUIRE_FALSE(wrongList.has_value());
    CHECK(wrongList.error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(wrongList.error().message.find("command list does not belong") !=
          std::string_view::npos);

    // Logged as well as asserted, because "which guard refused" is the whole content of this
    // case and a reader of the run should not have to re-derive it from two `find` calls.
    MONARC_LOG(SwapchainTest, Info, "the other device's queue: {} -- {}",
               Monarc::ToString(wrongQueue.error().code), wrongQueue.error().message);
    MONARC_LOG(SwapchainTest, Info, "the other device's list: {} -- {}",
               Monarc::ToString(wrongList.error().code), wrongList.error().message);

    // And both right combinations work, so the refusals above are not passing because
    // submitting is broken -- and this is what says two devices on one adapter are both
    // *usable* rather than merely both constructed.
    REQUIRE(first.swapchain->SubmitForPresent(first.device->GraphicsQueue(), **commands)
                .has_value());
    REQUIRE(first.swapchain->Present().has_value());
    REQUIRE(second.swapchain->SubmitForPresent(second.device->GraphicsQueue(), **otherCommands)
                .has_value());
    REQUIRE(second.swapchain->Present().has_value());
    REQUIRE(first.device->WaitIdle().has_value());
    REQUIRE(second.device->WaitIdle().has_value());
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

    CHECK_FALSE(harness.swapchain->NeedsRecreation());
    // Recreate refuses too, and its message says why: the surface went with the shutdown, so
    // only a fresh `CreateSwapchain` can replace it.
    const Monarc::Status recreated =
        harness.swapchain->Recreate(harness.window->ClientSize());
    REQUIRE_FALSE(recreated.has_value());
    CHECK(recreated.error().code == Monarc::ErrorCode::InvalidArgument);

    // The five queries and the other three fallible members, shared with the failed-recreation
    // case below -- which is what added `SubmitForPresent` to the set. Last, because it takes
    // a frame from the device to have a command list to refuse.
    CheckAnsweredAsEmpty(harness);
}

TEST_CASE("a recreation that fails leaves a swapchain that refuses rather than one that lies") {
    // **The state `VulkanSwapchain::IsInitialized()` documents and a review measured it not
    // being in.** `Recreate` tears the swapchain down before it builds, so a `BringUp` that
    // fails leaves the surface owned and no swapchain -- and every accessor has to say so,
    // because a caller told `IsInitialized() == true` goes on to `Acquire`, whose own guard is
    // that same query. What it reached with it passing was
    // `vkAcquireNextImageKHR` with a null semaphore *and* a null fence:
    // `VUID-vkAcquireNextImageKHR-semaphore-01780`, fatal under this suite's messenger and
    // undefined behaviour in a Release build with no layer loaded.
    //
    // **A surface whose window is gone is how the failure is reached here, and it is the one
    // `IsInitialized()`'s comment already names** -- "a window closed under the process". The
    // window is destroyed with the surface still alive, which `Window::Destroy` says is the
    // caller's ordering mistake; it is made deliberately, and nothing is presented afterwards.
    // What the surface queries answer for a dead `HWND` is the driver's to decide, so this
    // case asserts about whichever answer comes back rather than requiring the failure --
    // see the two branches below, both of which assert.
    REQUIRE_FALSE(Adapters().IsEmpty());

    Harness harness;
    REQUIRE(harness.Open(Adapters()[0], false).has_value());

    harness.window->PumpEvents();
    REQUIRE(PresentOneFrame(harness, Monarc::RHI::BufferHandle{}).presented);
    REQUIRE(harness.device->WaitIdle().has_value());

    const Monarc::RHI::Extent2D size = harness.swapchain->Extent();
    REQUIRE_FALSE(size.IsEmpty());

    harness.window->Destroy();
    REQUIRE_FALSE(harness.window->IsOpen());

    const Monarc::Status recreated = harness.swapchain->Recreate(size);
    if (recreated) {
        // The driver answered the surface queries for a destroyed window and built a swapchain
        // anyway. Asserted rather than shrugged at: whatever came back has to be internally
        // consistent, which is the same property the other branch checks from the other side.
        MONARC_LOG(SwapchainTest, Warning,
                   "\"{}\" recreated a swapchain on a surface whose window is gone, so the "
                   "failed-recreation half of this case is unexercised here; what is asserted "
                   "instead is that the swapchain it returned is self-consistent",
                   Adapters()[0].name);
        CHECK(harness.swapchain->IsInitialized());
        CHECK(harness.swapchain->ImageCount() > 0);
        CHECK_FALSE(harness.swapchain->Extent().IsEmpty());
        CHECK(harness.swapchain->ImageFormat() == kSwapchainFormat);
    } else {
        MONARC_LOG(SwapchainTest, Info,
                   "recreating on a surface whose window is gone was refused: {} -- {}",
                   Monarc::ToString(recreated.error().code), recreated.error().message);
        CheckAnsweredAsEmpty(harness);
    }

    // The swapchain is shut down before `~Harness` gets to it either way, so the surface goes
    // before the device does. It already is in the refused branch; `Shutdown` is idempotent.
    harness.swapchain->Shutdown();
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

    // **Counted, because three of these four refusals are machine-independent and the fourth
    // is not.** The fourth declines on this machine -- see below -- and a case that quietly
    // dropped one of the other three would look exactly like this one does today. The count is
    // asserted before the base attempt, so a block that stopped running is a failure rather
    // than a smaller assertion total nobody diffs.
    Monarc::u32 refusals = 0;

    {
        // No window. `SurfaceDescription::IsEmpty()` is what refuses it, before any Vulkan call.
        Monarc::RHI::SwapchainDescription description = base;
        description.surface                          = {};
        const auto refused                           = attempt(description);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);
        ++refusals;
    }
    {
        // An empty extent, which is the minimised case arriving through creation rather than
        // through `Recreate`.
        Monarc::RHI::SwapchainDescription description = base;
        description.extent                            = {};
        const auto refused                            = attempt(description);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);
        ++refusals;
    }
    {
        Monarc::RHI::SwapchainDescription description = base;
        description.format                           = Monarc::RHI::Format::Unknown;
        const auto refused                           = attempt(description);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);
        ++refusals;
    }

    // Three, and they are the three that do not depend on what this surface offers.
    CHECK(refusals == 3);

    {
        // A format the surface does not offer. **Refused rather than substituted**, which is
        // what keeps every byte this suite asserts a statement about the format that was asked
        // for.
        //
        // **This half does not run on this machine, and the code says so rather than leaving
        // it to Task 4's report.** `R8G8B8A8_UNORM` was picked as a format Windows surfaces do
        // not present in; both adapters here offer it, so `attempt` succeeds and the refusal
        // is unexercised. The branch is therefore not a skip: a surface that offers the format
        // has to *honour* it exactly, which is the same "no silent substitution" guarantee
        // seen from the other side, so one of the two branches asserts about Monarc either
        // way and neither returns.
        Monarc::RHI::SwapchainDescription description = base;
        description.format                           = Monarc::RHI::Format::R8G8B8A8_UNORM;
        auto       attempted                         = attempt(description);
        if (attempted.has_value()) {
            MONARC_LOG(SwapchainTest, Warning,
                       "this surface offers R8G8B8A8_UNORM, so the format-refusal half of this "
                       "case is unexercised on this machine; what is asserted instead is that "
                       "the format asked for is the format that came back");
            CHECK(attempted->ImageFormat() == Monarc::RHI::Format::R8G8B8A8_UNORM);
            // Before the base attempt below, so two swapchains never name this window at once.
            // `~VulkanSwapchain` would do it, and the explicit call is what makes the ordering
            // a statement rather than a side effect of scope.
            attempted->Shutdown();
        } else {
            CHECK(attempted.error().code == Monarc::ErrorCode::Unsupported);
        }
    }

    // And the base description still works, so the refusals above are not passing because
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
