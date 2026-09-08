// The surface, the swapchain, and presentation: `ISwapchain` as Vulkan implements it.
//
// **The file the A3 plan named.** What it does *not* hold is the surface itself:
// `vkCreateWin32SurfaceKHR` is in Private/Platform/Windows/VulkanSurface.cpp, which is the only
// file in this module that includes `<windows.h>`, and this file reaches it through
// `Detail::CreatePlatformSurface` -- two opaque `void*` in and a `VkSurfaceKHR` out, with no
// Win32 type crossing the line (ADR-0016).
//
// **Two kinds of semaphore, each where it belongs.** `vkQueuePresentKHR` accepts only binary
// semaphores, which is exactly why Task 3 deferred them and shipped a timeline instead. Both
// are here now: an acquire semaphore per frame in flight and a render-finished semaphore per
// swapchain image, alongside the device's one timeline, which is still what says whether frame
// N-2 has finished. `VulkanDeviceState::SubmitList` is where all three meet.
//
// **Two counts that are not the same count.** `kFramesInFlight` is Monarc's CPU/GPU overlap and
// belongs to the device; the image count belongs to the surface and is
// `ChooseSwapchainImageCount(minImageCount, maxImageCount)`. On this machine they are 2 and 3.
// Nothing here derives either from the other, which is deliberate: a driver reporting a
// different minimum is where code that had tied them together goes wrong, and this machine
// would never show it.
//
// **`VulkanQueue::Submit`'s presenting counterpart is `VulkanSwapchain::SubmitForPresent`, and
// it is defined here rather than in VulkanDevice.cpp.** The reason is that it needs this file's
// `VulkanSwapchainState` -- the semaphores -- and VulkanDevice.cpp does not need to know a
// swapchain exists. What the two share is `VulkanDeviceState::ValidateForSubmit` and
// `VulkanDeviceState::SubmitList`, declared in Private/VulkanDeviceState.h precisely so that
// the `Submit` row of the command-list state table exists once.

#include <Monarc/RHI/Vulkan/VulkanSwapchain.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Log.h>
#include <Monarc/RHI/Vulkan/VulkanDevice.h>

#include <ArrayOps.h>
#include <Loader.h>
#include <Translate.h>
#include <VulkanDeviceState.h>
#include <VulkanSurface.h>
#include <VulkanSwapchainFactory.h>

#include <new>
#include <utility>

namespace Monarc::RHI::Detail {

namespace LogCategories {

/// Nested in a namespace of its own for VulkanDeviceState.h's exact reason:
/// MONARC_LOG_CATEGORY declares a *variable* named after the category, and a variable called
/// `VulkanSwapchain` in `Detail` would hide the class of that name -- so every
/// `Result<VulkanSwapchain>` in this file would stop compiling, naming a variable where a type
/// belongs. The nesting keeps the category's spelling, which is what appears in a log line.
MONARC_LOG_CATEGORY(VulkanSwapchain, Info);

}  // namespace LogCategories

namespace {

/// How long an acquire waits for an image before giving up.
///
/// Five seconds, and it is a deadlock detector rather than a frame budget --
/// `kTimelineWaitTimeoutNanoseconds` in VulkanDevice.cpp, for the same reason. With FIFO and
/// three images an acquire blocks for at most one vertical blank; anything past a second means
/// the presentation engine is never going to hand one back.
///
/// `UINT64_MAX` was the alternative and is worse in exactly the way it is worse there: a test
/// suite that hangs forever tells CI nothing, where one that fails names the call that never
/// returned. A timeout is safe to report rather than retry, because `vkAcquireNextImageKHR`
/// leaves the semaphore untouched on any result that is not `VK_SUCCESS` or `VK_SUBOPTIMAL_KHR`
/// -- so nothing is left signalled for the next acquire to trip over.
constexpr u64 kAcquireTimeoutNanoseconds = 5'000'000'000ULL;

/// The colour space Phase A3 commits to, paired with `SwapchainDescription::format`.
///
/// `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` is the only colour space Vulkan requires a surface to
/// support, and with a UNORM format it means "the bytes I write are the bytes the display
/// gets, interpreted as sRGB" -- which is what makes the readback and the screen capture
/// assertable to the byte. HDR and colour management are deferred by name
/// (Docs/Rendering/RHI.md).
constexpr VkColorSpaceKHR kColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

/// The present mode Phase A3 commits to.
///
/// FIFO is the only mode Vulkan *guarantees* a surface supports, and it is what a first-light
/// program wants: no tearing, no dependence on a mode the next machine may not have, and a
/// frame rate pinned to the display's so that a loop with no pacing work does not spin. Mailbox
/// and immediate arrive with frame pacing, which is deferred.
constexpr VkPresentModeKHR kPresentMode = VK_PRESENT_MODE_FIFO_KHR;

}  // namespace

/// One swapchain image: the image itself, the pool slot it was registered into, and the binary
/// semaphore whose signal says rendering to it has finished.
///
/// **The render-finished semaphore is per image and not per frame in flight, and that is not
/// arbitrary.** `vkQueuePresentKHR` waits on it, and the wait belongs to the presentation of
/// *that image*; a semaphore shared between images would be presented twice while one wait was
/// still outstanding. Reusing this one is safe because a later `vkAcquireNextImageKHR` returned
/// the same index, which means the presentation engine has released the image and therefore
/// finished with the present that waited on it.
///
/// **That last argument is the accepted pattern rather than a spec guarantee, and it is worth
/// saying so.** Vulkan has no direct signal for "the present's semaphore wait completed"; the
/// airtight version needs `VK_EXT_swapchain_maintenance1`'s present fences, which A3 does not
/// enable.
///
/// **The teardown reduces that exposure rather than removing it, and an earlier version of this
/// comment claimed removal.** `Recreate` destroys and rebuilds every semaphore, so no semaphore
/// is carried across a recreation and the reuse argument above never has to hold across one.
/// What it does *not* do is retire a present whose semaphore wait is still outstanding:
/// `TearDownSwapchain`'s `vkDeviceWaitIdle` waits on queue operations, and a presentation
/// engine's wait is not a queue operation. The best available ordering is what is done there --
/// `vkDestroySwapchainKHR` before the semaphores it presented with, so the one call known to
/// end the presentation engine's interest happens first -- and closing the gap properly still
/// needs maintenance1, which Docs/Status.md records under "A3 Task 4 delivered" as reduced
/// rather than closed.
struct SwapchainImage {
    VkImage       image          = VK_NULL_HANDLE;
    TextureHandle texture        = {};
    VkSemaphore   renderFinished = VK_NULL_HANDLE;
};

/// One acquire semaphore and the submission that consumed it.
struct AcquireSlot {
    VkSemaphore semaphore = VK_NULL_HANDLE;

    /// The timeline value of the submission that waited on `semaphore`, or zero when nothing
    /// has.
    ///
    /// **This is what makes reusing an acquire semaphore safe, and it is the trap the phase
    /// plan singles out.** An acquire semaphore must not be handed to
    /// `vkAcquireNextImageKHR` while a previous acquire that signalled it may still be pending
    /// or unwaited -- `VUID-vkAcquireNextImageKHR-semaphore-01779`. The exact condition is
    /// "the submission that waited on it has completed", because completing the wait is what
    /// leaves the semaphore unsignalled, and a timeline value is precisely how this codebase
    /// asks whether a submission has completed. So `Acquire` waits on this before reusing the
    /// slot.
    ///
    /// Indexing by frame in flight, as the plan asks, is what makes that wait free in the
    /// steady state: `IDevice::BeginFrame` has already waited on the same or a later value
    /// before the frame got here, so `vkWaitSemaphores` returns immediately.
    ///
    /// **What the wait is actually for was re-derived after a review, and it is not what an
    /// earlier version of this comment said.** That version justified it by "the drifted case
    /// -- an out-of-date acquire skips a submission and the two counters stop agreeing", and
    /// the drifted case is the one case that cannot strain the wait. `BeginFrame` advances
    /// `frameIndex` on every success; `acquireCursor` advances only on a *successful* acquire;
    /// so the frame index can run ahead of the cursor and never behind. Ahead means
    /// `BeginFrame` waits on a frame slot holding a submission at least as recent as the one
    /// this acquire slot recorded, and the timeline counter only increases -- so reaching the
    /// newer value has already reached the older one.
    ///
    /// The sequence that *does* reach this wait with nothing having covered it is a caller
    /// that acquires **without a `BeginFrame` in between**, which nothing in the API refuses:
    /// `phase` is `Idle` again after `Present`, and `Acquire` never consults the device's frame
    /// state. Two ordinary frames put a recorded value in each of the two slots and leave both
    /// frame slots un-waited; a third acquire taken before that frame's `BeginFrame` then finds
    /// slot 0's value with no `BeginFrame` having waited on it, and this wait is the only thing
    /// keeping the reuse legal. `acquiring without a BeginFrame in between is what the
    /// acquire-slot wait is for`, in Monarc.Host.Windowed's device suite, walks exactly that
    /// sequence and logs how far the timeline had actually got.
    ///
    /// So: unreachable as *work* under the pacing rule `ISwapchain`'s class comment documents,
    /// reachable the moment a caller departs from it. A recorded value is also what keeps the
    /// slot index honest -- deriving it from the device's frame counter would tie the two
    /// cursors together exactly where they are allowed to differ.
    u64 consumedByTimelineValue = 0;
};

/// Everything a `VulkanSwapchain` owns.
struct VulkanSwapchainState {
    VulkanSwapchainState(IAllocator& allocatorToUse, const Loader& loaderToUse,
                         VkInstance instanceToUse, VulkanDeviceState& deviceToUse)
        : allocator(allocatorToUse),
          loader(&loaderToUse),
          instance(instanceToUse),
          device(&deviceToUse),
          images(allocatorToUse) {}

    /// Where the frame is between acquire and present.
    ///
    /// **Three states, and they exist because two of the three transitions are hangs rather
    /// than errors if taken out of order.** Presenting without a submission means waiting
    /// forever on a semaphore nothing will signal; acquiring twice leaves the first acquire's
    /// semaphore signalled with nothing to consume it. Both are refused with a returned
    /// `Status` instead, in every configuration rather than only where a validation layer is
    /// loaded -- which is the same argument the command list's own state machine makes.
    enum class Phase : u8 {
        /// Nothing acquired. What creation, `Recreate` and a completed `Present` leave behind.
        Idle,

        /// An image is acquired and its acquire semaphore will be signalled. **The frame must
        /// now reach `SubmitForPresent` and `Present`, or `Recreate`.**
        Acquired,

        /// A submission has taken the frame and will signal the image's render-finished
        /// semaphore. `Present` is what waits on it.
        Rendered,
    };

    IAllocator& allocator;

    /// Read for the instance-level surface queries -- capabilities, formats and present modes,
    /// all of which `Recreate` needs again -- so unlike `VulkanDevice`, which reads the loader
    /// during creation and drops it, a swapchain keeps it. Which is another way of saying a
    /// swapchain must not outlive its backend, as its class comment does.
    const Loader* loader = nullptr;

    VkInstance instance = VK_NULL_HANDLE;

    /// The device the swapchain's images are registered in and whose queue presents them.
    VulkanDeviceState* device = nullptr;

    VkSurfaceKHR   surface   = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    Extent2D extent = {};
    Format   format = Format::Unknown;

    /// What the caller asked for, kept because `Recreate` re-negotiates: a surface that gains
    /// or loses `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` across a recreation is answered from the
    /// request rather than from the last answer.
    bool readbackRequested = false;
    bool readbackAvailable = false;

    bool needsRecreation = false;

    Phase phase         = Phase::Idle;
    u32   acquiredIndex = 0;

    /// Which acquire slot the current acquire used. Read by `SubmitForPresent`, which is what
    /// stamps that slot's timeline value.
    u32 acquiredSlot = 0;

    /// Which slot the next acquire will use. Advanced only by an acquire that succeeded: a
    /// failed one leaves its semaphore untouched, so reusing the same slot next time is
    /// correct and skipping it would retire a semaphore nothing had used.
    u32 acquireCursor = 0;

    Array<SwapchainImage> images;
    AcquireSlot           acquireSlots[kFramesInFlight];

    [[nodiscard]] std::unexpected<Error> FailVk(const char* operation, VkResult result) {
        // `VulkanDeviceState::FailVk`'s shape and reasoning: the message is the VkResult's own
        // spelling, which is a literal and outlives this state, and the composition goes to
        // the log beside it.
        MONARC_LOG(LogCategories::VulkanSwapchain, Warning, "{} failed: {} ({})", operation, ToString(result),
                   static_cast<i32>(result));
        return Err(ToErrorCode(result), ToString(result));
    }

    /// Vulkan's count-then-fill idiom, keeping whatever `VK_INCOMPLETE` left behind.
    ///
    /// A third copy of `VulkanBackend::State::EnumerateInto`'s shape, and its comment predicted
    /// this one: "Tasks 3 and 4 add queue-family, surface-format and present-mode". Not shared
    /// with it because that one is a private member of a struct defined in another translation
    /// unit; what is shared is the decision it records -- `VK_INCOMPLETE` on the second call
    /// means elements appeared between the two, `*count` is what was actually written, and the
    /// honest response is to keep those and say so rather than loop.
    template <typename T, typename Enumerate>
    [[nodiscard]] Status EnumerateInto(const char* operation, Array<T>& out,
                                       Enumerate enumerate) {
        u32 count = 0;
        if (const VkResult counted = enumerate(&count, nullptr); counted != VK_SUCCESS) {
            return FailVk(operation, counted);
        }
        ResizeTo(out, count);
        if (count == 0) {
            return {};
        }
        const VkResult filled = enumerate(&count, out.Data());
        if (filled != VK_SUCCESS && filled != VK_INCOMPLETE) {
            return FailVk(operation, filled);
        }
        if (filled == VK_INCOMPLETE) {
            MONARC_LOG(LogCategories::VulkanSwapchain, Warning,
                       "{} reported more elements than it returned; continuing with the {} it "
                       "wrote",
                       operation, count);
        }
        ShrinkTo(out, count);
        return {};
    }

    [[nodiscard]] Status BringUp(Extent2D requested);

    /// Destroys the swapchain, its images' pool slots and every semaphore, keeping the
    /// surface. What `Recreate` calls before building again, and what `Shutdown` calls before
    /// destroying the surface.
    void TearDownSwapchain();

    void Shutdown();
};

namespace {

/// Whether `formats` offers `wanted` paired with `kColorSpace`.
[[nodiscard]] bool OffersFormat(const Array<VkSurfaceFormatKHR>& formats, VkFormat wanted) {
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == wanted && candidate.colorSpace == kColorSpace) {
            return true;
        }
    }
    return false;
}

/// Whether `modes` offers `kPresentMode`.
///
/// Vulkan requires every surface to support FIFO, so this can only ever say yes -- which is
/// why it is checked rather than assumed: a claim that costs one query to verify and would
/// otherwise be a comment is worth verifying. A surface that said no would be reported as
/// `Unsupported` rather than producing `VUID-VkSwapchainCreateInfoKHR-presentMode-01281`.
[[nodiscard]] bool OffersPresentMode(const Array<VkPresentModeKHR>& modes) {
    for (const VkPresentModeKHR mode : modes) {
        if (mode == kPresentMode) {
            return true;
        }
    }
    return false;
}

}  // namespace

Status VulkanSwapchainState::BringUp(Extent2D requested) {
    const InstanceFunctions& instanceFunctions = loader->Instance();

    VkSurfaceCapabilitiesKHR capabilities{};
    if (const VkResult result = instanceFunctions.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            device->physicalDevice, surface, &capabilities);
        result != VK_SUCCESS) {
        return FailVk("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
    }

    const Extent2D chosen = ChooseSwapchainExtent(capabilities, requested);
    if (chosen.IsEmpty()) {
        // The surface has no area right now, which on Windows means the window is minimised
        // between the caller's own check and this call. `vkCreateSwapchainKHR` rejects it
        // (`VUID-VkSwapchainCreateInfoKHR-imageExtent-01689`), so refusing here is the
        // difference between a Status the frame loop can park on and a validation error that
        // stops the process.
        MONARC_LOG(LogCategories::VulkanSwapchain, Info,
                   "the surface reports a {}x{} extent, so no swapchain can be created from it "
                   "yet",
                   capabilities.currentExtent.width, capabilities.currentExtent.height);
        return Err(ErrorCode::InvalidArgument,
                   "this surface currently has no area, so a swapchain cannot be created from "
                   "it; the window is minimised and the frame loop must park");
    }

    Array<VkSurfaceFormatKHR> formats(allocator);
    if (Status enumerated = EnumerateInto(
            "vkGetPhysicalDeviceSurfaceFormatsKHR", formats,
            [&](u32* count, VkSurfaceFormatKHR* data) {
                return instanceFunctions.vkGetPhysicalDeviceSurfaceFormatsKHR(
                    device->physicalDevice, surface, count, data);
            });
        !enumerated) {
        return enumerated;
    }

    const VkFormat wantedFormat = ToVulkan(format);
    if (!OffersFormat(formats, wantedFormat)) {
        // **Refused rather than substituted, and that is the point.** A swapchain silently
        // created in some other format would make every byte this phase asserts a statement
        // about a format nobody chose -- and the readback would then fail for a reason two
        // steps away from its cause. Both local adapters offer B8G8R8A8_UNORM with
        // SRGB_NONLINEAR, measured.
        MONARC_LOG(LogCategories::VulkanSwapchain, Error,
                   "this surface offers {} format/colour-space pair(s) and none of them is {} "
                   "with VK_COLOR_SPACE_SRGB_NONLINEAR_KHR",
                   formats.Size(), RHI::ToString(format));
        return Err(ErrorCode::Unsupported,
                   "this surface does not offer the format and colour space Monarc's swapchain "
                   "asks for");
    }

    Array<VkPresentModeKHR> modes(allocator);
    if (Status enumerated = EnumerateInto(
            "vkGetPhysicalDeviceSurfacePresentModesKHR", modes,
            [&](u32* count, VkPresentModeKHR* data) {
                return instanceFunctions.vkGetPhysicalDeviceSurfacePresentModesKHR(
                    device->physicalDevice, surface, count, data);
            });
        !enumerated) {
        return enumerated;
    }
    if (!OffersPresentMode(modes)) {
        MONARC_LOG(LogCategories::VulkanSwapchain, Error,
                   "this surface offers {} present mode(s) and VK_PRESENT_MODE_FIFO_KHR, which "
                   "Vulkan requires of every surface, is not among them",
                   modes.Size());
        return Err(ErrorCode::Unsupported,
                   "this surface does not offer FIFO presentation");
    }

    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        // **Required rather than negotiated, because the alternative would quietly weaken an
        // assertion.** A non-opaque swapchain is composited against whatever is behind the
        // window, so the on-screen capture in Monarc.Host.Windowed's device tests would read a
        // blend of Monarc's clear and someone else's pixels -- and would then have to assert a
        // tolerance instead of a byte. Refusing keeps that assertion exact. Opaque is
        // universally supported on Windows and this branch is not exercised here.
        MONARC_LOG(LogCategories::VulkanSwapchain, Error,
                   "this surface does not support opaque composition (supportedCompositeAlpha "
                   "0x{:x})",
                   static_cast<u32>(capabilities.supportedCompositeAlpha));
        return Err(ErrorCode::Unsupported,
                   "this surface does not support opaque composition, which Monarc's swapchain "
                   "requires");
    }

    VkImageUsageFlags usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    TextureUsage      imageUsage  = TextureUsage::ColorAttachment;
    readbackAvailable             = false;
    if (readbackRequested) {
        // **Checked, not assumed.** `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` in
        // `supportedUsageFlags` is near-universal and is not guaranteed, and a swapchain
        // created with a usage the surface does not offer is
        // `VUID-VkSwapchainCreateInfoKHR-imageUsage-01276`. A surface that does not offer it
        // gets a swapchain that works and cannot be read back, and says so -- rather than a
        // failure, because a game does not stop running because a driver will not let it
        // screenshot itself.
        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageUsage        = imageUsage | TextureUsage::TransferSource;
            readbackAvailable = true;
        } else {
            MONARC_LOG(LogCategories::VulkanSwapchain, Warning,
                       "readback was asked for and this surface does not offer "
                       "VK_IMAGE_USAGE_TRANSFER_SRC_BIT (supportedUsageFlags 0x{:x}); the "
                       "swapchain will work and its images cannot be copied out of",
                       static_cast<u32>(capabilities.supportedUsageFlags));
        }
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface          = surface;
    swapchainInfo.minImageCount    = ChooseSwapchainImageCount(capabilities.minImageCount,
                                                               capabilities.maxImageCount);
    swapchainInfo.imageFormat      = wantedFormat;
    swapchainInfo.imageColorSpace  = kColorSpace;
    swapchainInfo.imageExtent      = VkExtent2D{chosen.width, chosen.height};
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage       = usage;
    // EXCLUSIVE, because one queue family does everything: A3 has one graphics queue and
    // presents on it, which `AdapterCanPresent` is what verifies per adapter and per surface.
    // A separate present family would need CONCURRENT or an ownership transfer, and that is
    // the configuration this machine does not have -- both local adapters present on their
    // graphics family, measured.
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // The surface's current transform rather than IDENTITY. On a desktop these are the same
    // thing; on a device whose display is rotated they are not, and asking for a transform the
    // surface does not report as current makes the presentation engine rotate every frame.
    swapchainInfo.preTransform   = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode    = kPresentMode;
    // Clipped: pixels the window system obscures need not be rendered readably. It is the
    // right answer for a swapchain nothing reads back *through presentation* -- and the
    // readback here does not, since it copies the image before presenting it rather than after.
    swapchainInfo.clipped = VK_TRUE;
    // **Null, because `TearDownSwapchain` has already destroyed the old one.** Handing the old
    // swapchain over is the alternative and it is an optimisation: it lets the driver reuse
    // resources across a resize. It also means two swapchains alive at once with one retired,
    // which is a second lifetime to get right for a saving no A3 measurement asks for.
    // Destroying first, after a device wait, is provably correct and is what this does.
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

    if (const VkResult result = device->functions.vkCreateSwapchainKHR(
            device->device, &swapchainInfo, nullptr, &swapchain);
        result != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return FailVk("vkCreateSwapchainKHR", result);
    }

    Array<VkImage> rawImages(allocator);
    if (Status enumerated =
            EnumerateInto("vkGetSwapchainImagesKHR", rawImages,
                          [&](u32* count, VkImage* data) {
                              return device->functions.vkGetSwapchainImagesKHR(
                                  device->device, swapchain, count, data);
                          });
        !enumerated) {
        return enumerated;
    }
    if (rawImages.IsEmpty()) {
        return Err(ErrorCode::BackendFailure,
                   "this swapchain was created and reports no images");
    }

    TextureDescription imageDescription{};
    imageDescription.extent = chosen;
    imageDescription.format = format;
    // The usage the swapchain was actually created with, so that `BeginRendering`'s attachment
    // check and `CopyTextureToBuffer`'s transfer-source check answer about this image rather
    // than about an assumption. That is what makes a copy from a swapchain whose surface
    // offered no transfer-source bit a returned `Status` instead of a VUID.
    imageDescription.usage = imageUsage;

    for (const VkImage image : rawImages) {
        SwapchainImage entry{};
        entry.image = image;

        Result<TextureHandle> adopted = device->AdoptImage(image, imageDescription);
        if (!adopted) {
            return std::unexpected(adopted.error());
        }
        entry.texture = *adopted;

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        // No VkSemaphoreTypeCreateInfo chained, so this is a *binary* semaphore -- the device's
        // timeline is created with one chained, a few hundred lines away in
        // BringUpDevice. Binary because `vkQueuePresentKHR` accepts nothing else.
        if (const VkResult result = device->functions.vkCreateSemaphore(
                device->device, &semaphoreInfo, nullptr, &entry.renderFinished);
            result != VK_SUCCESS) {
            entry.renderFinished = VK_NULL_HANDLE;
            // Pushed before returning, so the partially built entry is released by the
            // teardown the caller runs rather than leaked. The adopted slot is the thing that
            // would otherwise stay claimed for the device's life.
            images.Push(entry);
            return FailVk("vkCreateSemaphore (render finished)", result);
        }

        images.Push(entry);
    }

    for (AcquireSlot& slot : acquireSlots) {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (const VkResult result = device->functions.vkCreateSemaphore(
                device->device, &semaphoreInfo, nullptr, &slot.semaphore);
            result != VK_SUCCESS) {
            slot.semaphore = VK_NULL_HANDLE;
            return FailVk("vkCreateSemaphore (acquire)", result);
        }
        // Zero, because these semaphores are new: nothing has waited on one, so nothing has to
        // be waited for before the first acquire uses it. `Recreate` builds fresh semaphores
        // for exactly this reason -- see `TearDownSwapchain`.
        slot.consumedByTimelineValue = 0;
    }

    extent          = chosen;
    phase           = Phase::Idle;
    acquiredIndex   = 0;
    acquiredSlot    = 0;
    acquireCursor   = 0;
    needsRecreation = false;

    MONARC_LOG(LogCategories::VulkanSwapchain, Info,
               "swapchain created on \"{}\" | {}x{} | {} | {} image(s), min {} max {} | FIFO | "
               "readback {}",
               device->adapter.name, extent.width, extent.height, RHI::ToString(format),
               images.Size(), capabilities.minImageCount, capabilities.maxImageCount,
               readbackAvailable        ? "available"
               : readbackRequested      ? "unavailable"
                                        : "not asked for");
    return {};
}

void VulkanSwapchainState::TearDownSwapchain() {
    if (device == nullptr || device->device == VK_NULL_HANDLE) {
        // The device went first, which is a caller ordering mistake this cannot fix -- see
        // `VulkanSwapchain`'s class comment. Forgetting the handles is all that is available:
        // every entry point that would destroy them belongs to the device that is gone.
        images.Clear();
        for (AcquireSlot& slot : acquireSlots) {
            slot = AcquireSlot{};
        }
        swapchain = VK_NULL_HANDLE;
        extent    = Extent2D{};
        phase     = Phase::Idle;
        return;
    }

    // Idle first, and it is not a convenience. Destroying a swapchain whose images a submitted
    // frame is still rendering into is undefined behaviour, and destroying a semaphore a
    // submitted batch still refers to is `VUID-vkDestroySemaphore-semaphore-01137`.
    // `VulkanDeviceState::Shutdown`'s own wait, for the same reason.
    if (const VkResult result = device->functions.vkDeviceWaitIdle(device->device);
        result != VK_SUCCESS) {
        // Nothing can be done about it and pressing on is right: the alternative is leaking
        // the swapchain and its surface. A lost device is the realistic cause.
        MONARC_LOG(LogCategories::VulkanSwapchain, Error,
                   "vkDeviceWaitIdle failed while tearing down a swapchain: {} ({}); "
                   "destroying anyway",
                   ToString(result), static_cast<i32>(result));
    }

    for (SwapchainImage& entry : images) {
        // Releases the pool slot, which destroys the *view* Monarc made and leaves the image
        // alone -- `TextureSlot::ownsImage` is what says so. It also bumps the slot's
        // generation, which is what makes every handle this swapchain issued stale from here
        // on rather than from the next claim: exactly ADR-0002's guarantee, applied to
        // recreation.
        if (TextureSlot* slot = device->Resolve(entry.texture); slot != nullptr) {
            device->ReleaseTextureSlot(*slot);
        }
    }

    // After the views, because a view outliving its image is
    // `VUID-vkDestroyImage-image-01000`'s shape and the images belong to this swapchain.
    //
    // **Before both semaphore loops below, and that ordering is the correction a review
    // forced.** The wait above is `vkDeviceWaitIdle`, which retires *queue* operations -- and
    // a presentation engine's wait on a binary semaphore is not one. So a semaphore destroyed
    // between the device wait and this call could still be one the presentation engine has not
    // finished waiting on; `vkDestroySwapchainKHR` is what ends its interest in the images and
    // therefore in the semaphores those presents waited on. Doing it in this order does not
    // make the ordering *provable* -- Vulkan has no direct signal for "the present's semaphore
    // wait completed", which is the gap `VK_EXT_swapchain_maintenance1`'s present fences exist
    // to close and A3 does not enable them -- but it does put the one thing that is known to
    // end the presentation engine's use of them first instead of last.
    if (swapchain != VK_NULL_HANDLE) {
        device->functions.vkDestroySwapchainKHR(device->device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    for (SwapchainImage& entry : images) {
        if (entry.renderFinished != VK_NULL_HANDLE) {
            device->functions.vkDestroySemaphore(device->device, entry.renderFinished, nullptr);
        }
    }
    images.Clear();

    for (AcquireSlot& slot : acquireSlots) {
        if (slot.semaphore != VK_NULL_HANDLE) {
            // **Destroyed and rebuilt rather than carried across a recreation, which is what
            // keeps the reuse argument from having to hold across a recreation as well.** An
            // acquire that succeeded and was never submitted leaves its semaphore signalled,
            // and reusing that one is a validation error; a fresh semaphore has no history to
            // be in the wrong state. Legal to destroy in either state: a signalled binary
            // semaphore with no pending operation is not "in use", the device wait above is
            // what retired the operations there were, and the swapchain whose presentation
            // engine could still have signalled one is already gone.
            device->functions.vkDestroySemaphore(device->device, slot.semaphore, nullptr);
        }
        slot = AcquireSlot{};
    }

    extent        = Extent2D{};
    phase         = Phase::Idle;
    acquiredIndex = 0;
    acquiredSlot  = 0;
    acquireCursor = 0;
}

void VulkanSwapchainState::Shutdown() {
    TearDownSwapchain();

    if (surface != VK_NULL_HANDLE) {
        // The surface belongs to the *instance*, so this is the loader's entry point and not
        // the device's -- and it is why a swapchain must not outlive its backend. After the
        // swapchain, because destroying a surface a swapchain still names is
        // `VUID-vkDestroySurfaceKHR-surface-01266`.
        if (loader != nullptr && loader->Instance().vkDestroySurfaceKHR != nullptr) {
            loader->Instance().vkDestroySurfaceKHR(instance, surface, nullptr);
        } else {
            MONARC_LOG(LogCategories::VulkanSwapchain, Error,
                       "the Vulkan surface cannot be destroyed: vkDestroySurfaceKHR is not "
                       "available, so it is leaked until the process exits");
        }
        surface = VK_NULL_HANDLE;
    }

    format            = Format::Unknown;
    readbackAvailable = false;
    needsRecreation   = false;
}

// ---------------------------------------------------------------------------------------
// VulkanSwapchainFactory
// ---------------------------------------------------------------------------------------

Result<VulkanSwapchain> VulkanSwapchainFactory::Create(IAllocator& allocator,
                                                       const Loader& loader, VkInstance instance,
                                                       VulkanDevice&               device,
                                                       const SwapchainDescription& description) {
    if (description.surface.IsEmpty()) {
        return Err(ErrorCode::InvalidArgument,
                   "a swapchain cannot be created without a native window handle");
    }
    if (description.extent.IsEmpty()) {
        return Err(ErrorCode::InvalidArgument,
                   "a swapchain cannot be created at an empty extent; a minimised window is "
                   "what the frame loop parks on");
    }
    if (description.format == Format::Unknown) {
        return Err(ErrorCode::InvalidArgument,
                   "a swapchain cannot be created with Format::Unknown");
    }

    VulkanDeviceState* deviceState = device.m_state;
    if (deviceState == nullptr || deviceState->device == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "a swapchain cannot be created on a device that has been shut down or moved "
                   "from");
    }
    if (!deviceState->swapchainEnabled) {
        // **The one report `VulkanDeviceState::swapchainEnabled` exists to make.** The
        // extension is enabled per adapter, so this is the call that finds out it was not --
        // rather than the tier ladder declaring such a device unusable for the offscreen work
        // it is perfectly capable of. Not reachable on this machine: both local adapters offer
        // `VK_KHR_swapchain`.
        MONARC_LOG(LogCategories::VulkanSwapchain, Error,
                   "\"{}\" does not have {} enabled, so it cannot present",
                   deviceState->adapter.name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return Err(ErrorCode::Unsupported,
                   "this device does not support VK_KHR_swapchain, so it cannot present");
    }

    void* storage =
        allocator.Allocate(sizeof(VulkanSwapchainState), alignof(VulkanSwapchainState));
    if (storage == nullptr) {
        MONARC_LOG(LogCategories::VulkanSwapchain, Warning,
                   "the allocator returned nothing for {} bytes of swapchain state",
                   sizeof(VulkanSwapchainState));
        return Err(ErrorCode::OutOfMemory,
                   "the allocator returned nothing for the swapchain's state");
    }
    VulkanSwapchainState* state =
        ::new (storage) VulkanSwapchainState(allocator, loader, instance, *deviceState);
    state->format            = description.format;
    state->readbackRequested = description.allowReadback;

    // The state is destroyed and deallocated on every failure below, so nothing half-built is
    // handed back -- `VulkanDeviceFactory::Create`'s shape, and the reason every message on
    // this path is a string literal: the `Error` is copied out before the storage is gone.
    const auto fail = [&](const Error& error) -> Result<VulkanSwapchain> {
        state->Shutdown();
        state->~VulkanSwapchainState();
        allocator.Deallocate(storage, sizeof(VulkanSwapchainState),
                             alignof(VulkanSwapchainState));
        return std::unexpected(error);
    };

    if (Status created = CreatePlatformSurface(loader.GetInstanceProcAddr(), instance,
                                               description.surface, state->surface);
        !created) {
        return fail(created.error());
    }

    // **Presentation support, per adapter and per surface, checked before the swapchain and not
    // assumed.** `vkCreateSwapchainKHR` on a device whose queue family cannot present to this
    // surface is `VUID-VkSwapchainCreateInfoKHR-surface-01270`, and the reason to check rather
    // than let it fail is that "this GPU cannot drive this monitor" is a real configuration
    // with a real answer -- see `VulkanBackend::AdapterCanPresent`, which is the query a caller
    // makes *before* choosing a device. This is the same query, made again, because a caller is
    // not obliged to have asked.
    VkBool32 supported = VK_FALSE;
    if (const VkResult result = loader.Instance().vkGetPhysicalDeviceSurfaceSupportKHR(
            deviceState->physicalDevice, deviceState->graphicsQueueFamily, state->surface,
            &supported);
        result != VK_SUCCESS) {
        return fail(state->FailVk("vkGetPhysicalDeviceSurfaceSupportKHR", result).error());
    }
    if (supported == VK_FALSE) {
        MONARC_LOG(LogCategories::VulkanSwapchain, Error,
                   "\"{}\" cannot present to this surface on its graphics queue family {}",
                   deviceState->adapter.name, deviceState->graphicsQueueFamily);
        return fail(Error{ErrorCode::Unsupported,
                          "this adapter's graphics queue family cannot present to this surface"});
    }

    if (Status brought = state->BringUp(description.extent); !brought) {
        return fail(brought.error());
    }

    return VulkanSwapchain(state);
}

}  // namespace Monarc::RHI::Detail

namespace Monarc::RHI {

VulkanSwapchain::VulkanSwapchain(Detail::VulkanSwapchainState* state) noexcept
    : m_state(state) {}

VulkanSwapchain::~VulkanSwapchain() { Release(); }

VulkanSwapchain::VulkanSwapchain(VulkanSwapchain&& other) noexcept
    : ISwapchain(std::move(other)), m_state(std::exchange(other.m_state, nullptr)) {}

VulkanSwapchain& VulkanSwapchain::operator=(VulkanSwapchain&& other) noexcept {
    // Self-assignment would otherwise release this swapchain's state and then adopt the
    // pointer it had just freed -- `VulkanDevice::operator=`'s guard, for the same reason.
    if (this != &other) {
        Release();
        ISwapchain::operator=(std::move(other));
        m_state = std::exchange(other.m_state, nullptr);
    }
    return *this;
}

void VulkanSwapchain::Release() {
    if (m_state == nullptr) {
        return;
    }
    m_state->Shutdown();
    IAllocator& allocator = m_state->allocator;
    m_state->~VulkanSwapchainState();
    allocator.Deallocate(m_state, sizeof(Detail::VulkanSwapchainState),
                         alignof(Detail::VulkanSwapchainState));
    m_state = nullptr;
}

void VulkanSwapchain::Shutdown() {
    if (m_state != nullptr) {
        m_state->Shutdown();
    }
}

bool VulkanSwapchain::IsInitialized() const {
    return m_state != nullptr && m_state->swapchain != VK_NULL_HANDLE;
}

Extent2D VulkanSwapchain::Extent() const {
    return IsInitialized() ? m_state->extent : Extent2D{};
}

Format VulkanSwapchain::ImageFormat() const {
    return IsInitialized() ? m_state->format : Format::Unknown;
}

u32 VulkanSwapchain::ImageCount() const {
    return IsInitialized() ? static_cast<u32>(m_state->images.Size()) : 0;
}

bool VulkanSwapchain::ReadbackAvailable() const {
    return IsInitialized() && m_state->readbackAvailable;
}

bool VulkanSwapchain::NeedsRecreation() const {
    // Deliberately not gated on `IsInitialized()`, unlike the four above: a `Recreate` that
    // failed leaves the swapchain uninitialised *and* still needing recreation, and a frame
    // loop reading this is exactly the caller that has to see it.
    return m_state != nullptr && m_state->needsRecreation;
}

Result<AcquiredImage> VulkanSwapchain::Acquire() {
    using Phase = Detail::VulkanSwapchainState::Phase;

    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Acquire called on a swapchain that has been shut down, moved "
                   "from, or whose recreation failed");
    }
    if (m_state->phase != Phase::Idle) {
        // **Refused rather than acquiring again, and this is the guard the phase plan singles
        // out.** A successful acquire leaves a semaphore the presentation engine will signal,
        // and only a submission that waits on it -- or the wholesale teardown `Recreate`
        // performs -- consumes it. A second acquire on the same semaphore is
        // `VUID-vkAcquireNextImageKHR-semaphore-01779`; a second acquire on the *next*
        // semaphore would leave the first signalled forever. Either way the frame that
        // acquired has to finish, so this refuses instead.
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Acquire called while an image is already acquired; submit and "
                   "present it, or recreate the swapchain");
    }

    Detail::AcquireSlot& slot = m_state->acquireSlots[m_state->acquireCursor];
    if (slot.consumedByTimelineValue != 0) {
        // See `AcquireSlot::consumedByTimelineValue`: the submission that waited on this
        // semaphore must have completed before it is handed to another acquire. Returns
        // immediately in the steady state, because `IDevice::BeginFrame` has already waited on
        // the same or a later value.
        if (Status waited = m_state->device->queue.Wait(slot.consumedByTimelineValue,
                                                        Detail::kAcquireTimeoutNanoseconds);
            !waited) {
            return std::unexpected(waited.error());
        }
        slot.consumedByTimelineValue = 0;
    }

    u32            imageIndex = 0;
    const VkResult result     = m_state->device->functions.vkAcquireNextImageKHR(
        m_state->device->device, m_state->swapchain, Detail::kAcquireTimeoutNanoseconds,
        slot.semaphore, VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // **Not an error, and no image.** The spec leaves the semaphore untouched for any
        // result that is not `VK_SUCCESS` or `VK_SUBOPTIMAL_KHR`, so nothing is left signalled
        // and the cursor stays where it is: the same slot is used again by the next acquire.
        m_state->needsRecreation = true;
        MONARC_LOG(Detail::LogCategories::VulkanSwapchain, Info,
                   "acquire reports the swapchain is out of date; it needs recreating");
        return AcquiredImage{AcquireOutcome::OutOfDate, TextureHandle{}, 0, false};
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            // Reported rather than retried, and a failure rather than "no image": a caller
            // told "out of date" would recreate a swapchain that is perfectly current, and a
            // caller told nothing would spin. Five seconds without an image from a FIFO
            // swapchain with three of them means something is wrong that a retry will not fix.
            MONARC_LOG(Detail::LogCategories::VulkanSwapchain, Error,
                       "vkAcquireNextImageKHR returned {} after {} ns; no image became "
                       "available",
                       Detail::ToString(result), Detail::kAcquireTimeoutNanoseconds);
            return Err(ErrorCode::BackendFailure,
                       "no swapchain image became available before the acquire timed out");
        }
        return m_state->FailVk("vkAcquireNextImageKHR", result);
    }

    if (imageIndex >= m_state->images.Size()) {
        // The presentation engine returned an index outside the set it told us about. Nothing
        // can be done with it, and indexing the array would be the bug rather than the report.
        MONARC_LOG(Detail::LogCategories::VulkanSwapchain, Error,
                   "vkAcquireNextImageKHR returned image index {} for a swapchain with {} "
                   "image(s)",
                   imageIndex, m_state->images.Size());
        return Err(ErrorCode::BackendFailure,
                   "the presentation engine returned an image index this swapchain does not "
                   "have");
    }

    m_state->acquiredIndex = imageIndex;
    m_state->acquiredSlot  = m_state->acquireCursor;
    // Advanced only now, on an acquire that actually took the semaphore.
    m_state->acquireCursor = (m_state->acquireCursor + 1) % kFramesInFlight;
    m_state->phase         = Phase::Acquired;

    const bool suboptimal = result == VK_SUBOPTIMAL_KHR;
    if (suboptimal) {
        // **The image is valid and must still be presented.** Recreating now would abandon a
        // signalled acquire semaphore -- see `AcquiredImage::suboptimal`, which is the field
        // this sets, and `NeedsRecreation()`, which is what a loop reads between frames.
        m_state->needsRecreation = true;
    }

    return AcquiredImage{AcquireOutcome::Acquired, m_state->images[imageIndex].texture,
                         imageIndex, suboptimal};
}

Result<u64> VulkanSwapchain::SubmitForPresent(IQueue& queue, ICommandList& commands) {
    using Phase = Detail::VulkanSwapchainState::Phase;

    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::SubmitForPresent called on a swapchain that has been shut down, "
                   "moved from, or whose recreation failed");
    }
    if (&queue != static_cast<IQueue*>(&m_state->device->queue)) {
        // **Address comparison and not a downcast, which is what makes it safe** -- exactly
        // `VulkanDeviceState::FindOwnList`'s argument for command lists. Casting an `IQueue&`
        // of unknown dynamic type is undefined before there is anything left to check, and
        // `dynamic_cast` needs RTTI; comparing addresses through a common base is well defined
        // for any pointer, and it answers the question that can actually go wrong: any two
        // devices are two devices, and presenting a frame submitted to the wrong one is not
        // something a driver reports helpfully. **Two adapters are not what makes this
        // reachable** -- `CreateDevice` keeps nothing per adapter, so two `VkDevice`s on one
        // adapter are as foreign to each other as two on separate ones, which is the shape
        // Monarc.Host.Windowed's "a swapchain refuses a queue and a command list that are not
        // its device's" uses. Deleting this comparison makes that case's first refusal
        // *succeed*, because `SubmitList` below submits on `m_state->device`'s own queue and
        // never reads the one it was handed.
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::SubmitForPresent was given a queue that does not belong to this "
                   "swapchain's device");
    }
    if (m_state->phase != Phase::Acquired) {
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::SubmitForPresent needs an image this swapchain has acquired and "
                   "not yet submitted; call Acquire first");
    }

    Result<Detail::VulkanCommandList*> list =
        m_state->device->ValidateForSubmit("ISwapchain::SubmitForPresent", commands);
    if (!list) {
        return std::unexpected(list.error());
    }

    const VkSemaphore wait   = m_state->acquireSlots[m_state->acquiredSlot].semaphore;
    const VkSemaphore signal = m_state->images[m_state->acquiredIndex].renderFinished;

    Result<u64> value = m_state->device->SubmitList(**list, wait, signal);
    if (!value) {
        // The phase stays `Acquired`, because it is: a submission the driver rejected consumed
        // neither semaphore, so the frame may be recorded again or the swapchain recreated.
        return value;
    }

    // The value that retires this acquire semaphore. Stamped after the submission succeeded,
    // for the reason `SubmitList` marks the list submitted only then.
    m_state->acquireSlots[m_state->acquiredSlot].consumedByTimelineValue = *value;
    m_state->phase = Phase::Rendered;
    return value;
}

Status VulkanSwapchain::Present() {
    using Phase = Detail::VulkanSwapchainState::Phase;

    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Present called on a swapchain that has been shut down, moved "
                   "from, or whose recreation failed");
    }
    if (m_state->phase != Phase::Rendered) {
        // **A hang if it reached the API, which is why it is refused here.** Present waits on
        // the render-finished semaphore, and only `SubmitForPresent` signals it -- so a present
        // with nothing submitted would wait on a semaphore nothing is going to signal. There is
        // no validation error for that; there is a frozen program.
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Present needs a frame this swapchain has submitted; call "
                   "Acquire and SubmitForPresent first");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &m_state->images[m_state->acquiredIndex].renderFinished;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &m_state->swapchain;
    presentInfo.pImageIndices      = &m_state->acquiredIndex;

    const VkResult result =
        m_state->device->functions.vkQueuePresentKHR(m_state->device->graphicsQueue,
                                                     &presentInfo);
    // Idle whatever happened: the image has been handed over, or the attempt has failed and the
    // frame is over either way. Keeping the phase at `Rendered` after a failure would let a
    // caller present the same frame twice.
    m_state->phase = Phase::Idle;

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // **Success with `NeedsRecreation()` set, and both results are handled here for the
        // same reason `Acquire` handles both.** The frame was presented as well as it could be
        // and the next one needs a new swapchain. A loop that watched only acquire would run
        // indefinitely on a suboptimal swapchain -- which is what happens after a monitor
        // change on some drivers, and has no symptom loud enough to find by looking.
        m_state->needsRecreation = true;
        MONARC_LOG(Detail::LogCategories::VulkanSwapchain, Info,
                   "present reports the swapchain is {}; it needs recreating",
                   result == VK_ERROR_OUT_OF_DATE_KHR ? "out of date" : "suboptimal");
        return {};
    }
    if (result != VK_SUCCESS) {
        // The render-finished semaphore may be left signalled by a present that failed outright
        // -- there is no way to tell. `Recreate` destroys every semaphore, which is what makes
        // that recoverable rather than a state to reason about; a caller that neither recreates
        // nor shuts down after this is presenting on a surface that has stopped working.
        return m_state->FailVk("vkQueuePresentKHR", result);
    }
    return {};
}

Status VulkanSwapchain::Recreate(Extent2D extent) {
    if (m_state == nullptr) {
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Recreate called on a swapchain that has been moved from");
    }
    if (m_state->device == nullptr || m_state->device->device == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Recreate called on a swapchain whose device has been shut down");
    }
    if (m_state->surface == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Recreate called on a swapchain that has been shut down; its "
                   "surface is gone and only a fresh CreateSwapchain can replace it");
    }
    if (extent.IsEmpty()) {
        // The frame loop's park condition, refused rather than worked around. See
        // `ISwapchain::Recreate` for why the parking belongs to the caller: only the caller
        // knows how to wait for the window to come back.
        return Err(ErrorCode::InvalidArgument,
                   "ISwapchain::Recreate was given an empty extent; a minimised window is what "
                   "the frame loop parks on rather than recreating at 0 x 0");
    }

    m_state->TearDownSwapchain();
    // A failure here leaves the surface owned and no swapchain, which `IsInitialized()`
    // reports and a further `Recreate` retries -- see that function's comment for why that is
    // the state rather than an attempt to put the old swapchain back.
    return m_state->BringUp(extent);
}

}  // namespace Monarc::RHI
