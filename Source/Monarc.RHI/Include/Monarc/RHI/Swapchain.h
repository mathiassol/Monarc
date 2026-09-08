#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>
#include <Monarc/RHI/Types.h>

namespace Monarc::RHI {

/// The native handles a surface is created from: a platform's window, and whatever module or
/// display connection that window belongs to.
///
/// **Two `void*` and no platform type, which is the whole reason this struct exists.**
/// `Monarc.Host.Windowed` produces one of these and `Monarc.RHI.Vulkan`'s
/// `Private/Platform/Windows/VulkanSurface.cpp` consumes it, and neither the RHI's headers nor
/// the backend's may name a Win32 type (ADR-0016; ADR-0014 rule 1). It is also what keeps the
/// backend from including `Monarc/Host/` at all: gate 3 in Tools/check_architecture.py forbids
/// a tier-2 translation unit that include, so the window cannot be passed as a `Window&` --
/// the app takes the handles out of the window and puts them in here.
///
/// On Windows `nativeDisplay` is the `HINSTANCE` the window class was registered with and
/// `nativeWindow` is the `HWND`. On a platform whose surface extension needs one handle rather
/// than two, `nativeDisplay` is the one that goes unused; on X11 it is the `Display*`. Naming
/// them for their roles rather than for Win32 is what lets the second platform fill them in
/// without changing this header.
struct SurfaceDescription {
    void* nativeDisplay = nullptr;
    void* nativeWindow  = nullptr;

    /// Whether this names a window at all. `nativeWindow` alone, because that is the handle
    /// every platform's surface extension requires; a platform that also requires
    /// `nativeDisplay` refuses a null one itself, with a message naming which handle was
    /// missing.
    [[nodiscard]] constexpr bool IsEmpty() const { return nativeWindow == nullptr; }
};

/// What a swapchain should be created as.
struct SwapchainDescription {
    SurfaceDescription surface = {};

    /// The client-area size to create the swapchain at, in physical pixels.
    ///
    /// A request rather than a result: a platform may report a surface's size itself, and
    /// where it does that number wins -- on Windows `VkSurfaceCapabilitiesKHR::currentExtent`
    /// is always the window's client rect, so this is used only where a surface reports the
    /// "any size" sentinel. `ISwapchain::Extent()` is what the swapchain actually became.
    ///
    /// Empty is refused with `ErrorCode::InvalidArgument`, and that refusal is the reason
    /// `Extent2D::IsEmpty()` exists: a minimised window reports `0 x 0`, and a swapchain
    /// cannot be created from it. The frame loop parks instead -- see `ISwapchain::Recreate`.
    Extent2D extent = {};

    /// The format to ask the surface for. `B8G8R8A8_UNORM` is what Phase A3 commits to, with
    /// `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` alongside it, and a surface that does not offer
    /// the pair is `ErrorCode::Unsupported` rather than a silent substitution -- a swapchain
    /// whose format is not the one asked for would make every readback assertion in the suite
    /// a statement about a format nobody chose.
    ///
    /// UNORM and not sRGB is deliberate and Types.h's `Format` says why: whether a clear value
    /// on an sRGB image is encoded or written through unchanged is a real subtlety, and A3 has
    /// no business settling colour management before there is a shader and a reason to care.
    Format format = Format::B8G8R8A8_UNORM;

    /// Whether to ask for swapchain images a `ICommandList::CopyTextureToBuffer` can read.
    ///
    /// **This exists so that the clear can be proved rather than looked at.** With it, an
    /// acquired image can be barriered to `TextureLayout::TransferSource` and copied into a
    /// host-visible buffer before it is presented, which is the strongest evidence available
    /// that the clear reached *the image that gets presented* rather than an offscreen
    /// texture. Without it, the only evidence is a screenshot or a pair of eyes.
    ///
    /// **Asking is not getting, and the difference is reported rather than assumed.**
    /// `VkSurfaceCapabilitiesKHR::supportedUsageFlags` must offer
    /// `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, which is near-universal and not guaranteed. A
    /// surface that does not offer it produces a swapchain whose `ReadbackAvailable()` is
    /// false and a warning naming the surface, not a failure -- a game does not stop running
    /// because a driver will not let it screenshot itself.
    bool allowReadback = false;
};

/// What `ISwapchain::Acquire` found.
///
/// **An enumerator and not an `Error`, because out-of-date is an expected condition rather
/// than a fault.** A window that has just been resized has a swapchain that no longer matches
/// its surface, and every program that opens a resizable window meets that on an ordinary
/// frame. Reporting it as `std::unexpected` would make the frame loop branch on an error code
/// to find the normal case, and would put a failure in the log for something that is not one.
enum class AcquireOutcome : u32 {
    /// An image was acquired. `AcquiredImage::texture` names it and it must be presented or
    /// the swapchain recreated -- see `AcquiredImage`.
    Acquired = 0,

    /// The swapchain no longer matches its surface. **No image was acquired**, nothing may be
    /// presented, and the caller's next move is `ISwapchain::Recreate`.
    OutOfDate,
};

/// The enumerator's own spelling, for logs and test failures. Never nullptr; a value outside
/// the enumerator set gets a name of its own, for the reason `ToString(Format)` in Types.h
/// gives.
[[nodiscard]] const char* ToString(AcquireOutcome outcome);

/// One acquired swapchain image.
///
/// **The image is a `TextureHandle`, and that is the design decision this struct records.** A
/// swapchain image is registered in the device's own texture pool at swapchain creation, so
/// every command `ICommandList` already has -- `Barrier`, `BeginRendering`,
/// `CopyTextureToBuffer` -- works on it unchanged, with the same generation checking every
/// other resource gets (ADR-0002). The alternative was a parallel set of swapchain-only
/// commands, which would have been a second recording path with one caller.
///
/// It is also what makes the readback in `SwapchainDescription::allowReadback` possible with
/// no new entry point at all: the acquired image is copied to a buffer by the same call
/// Task 3's readback used.
///
/// **The handles are stable across acquires and invalidated by `Recreate`.** A swapchain owns
/// one handle per image for its whole life, so acquiring image 2 twice yields the same handle
/// twice. Recreation destroys the images and releases their pool slots, which bumps each
/// slot's generation -- so a handle held across a `Recreate` is stale and resolves to a
/// refusal rather than to whatever now occupies the slot.
struct AcquiredImage {
    /// Defaults to `OutOfDate` rather than `Acquired`: a caller who forgets to read this gets
    /// the conservative answer, where the other default would have them render into an invalid
    /// handle.
    AcquireOutcome outcome = AcquireOutcome::OutOfDate;

    /// Valid only when `outcome` is `Acquired`.
    TextureHandle texture = {};

    /// Which of the swapchain's images this is, in `[0, ImageCount())`. Not needed to render
    /// -- `texture` is what commands take -- and present takes it from the swapchain rather
    /// than from the caller. It is here because a test that asserts every image is eventually
    /// acquired needs to be able to say which one it got.
    u32 index = 0;

    /// The swapchain still works and no longer matches its surface exactly -- Vulkan's
    /// `VK_SUBOPTIMAL_KHR`. **The image is valid and must still be presented**; recreating
    /// before presenting it would abandon a signalled acquire semaphore, which is the one
    /// mistake in this interface that a validation layer catches late and a release build not
    /// at all.
    ///
    /// `ISwapchain::NeedsRecreation()` is set when this happens, so a loop that recreates
    /// between frames picks it up without reading this field at all.
    bool suboptimal = false;
};

/// A surface and the images presented to it.
///
/// **Created from a surface, not from a window**: `SwapchainDescription::surface` carries two
/// opaque handles and nothing in the RHI or in a backend knows what a window is. See
/// `SurfaceDescription`.
///
/// **The frame this interface expects, in order.** Each step is refused with
/// `ErrorCode::InvalidArgument` if taken out of order, rather than producing a hang or a
/// validation error:
///
/// 1. `IDevice::BeginFrame`, which waits on the frame slot's timeline value and resets its
///    command pool. Unchanged by presentation -- the wait is about the pool, not the image.
/// 2. `Acquire`. On `AcquireOutcome::OutOfDate` there is no image: `Recreate` and start again.
/// 3. Record: barrier into `TextureLayout::ColorAttachment`, `BeginRendering` with a clear,
///    `EndRendering`, barrier into `TextureLayout::PresentSource`, `End`.
/// 4. `SubmitForPresent`, which is `IQueue::Submit` plus the two binary semaphores
///    presentation needs.
/// 5. `Present`.
///
/// **Once `Acquire` has succeeded, the frame must reach `Present` or `Recreate`.** A successful
/// acquire leaves a binary semaphore that the presentation engine will signal, and the only
/// two things that consume it are the submission `SubmitForPresent` makes and the wholesale
/// teardown `Recreate` performs. A loop that acquires and then walks away leaves a signalled
/// semaphore behind, and the next acquire on it is
/// `VUID-vkAcquireNextImageKHR-semaphore-01779`. So `Acquire` refuses while an image is
/// already acquired, which turns that mistake into a returned `Status` in every
/// configuration rather than a validation error in one.
///
/// **Present uses binary semaphores and frame pacing uses the timeline, each where it
/// belongs.** `vkQueuePresentKHR` accepts only binary semaphores, which is exactly why Task 3
/// deferred them; the timeline remains what says whether frame N-2 has finished. Pretending
/// the timeline covered presentation would not survive contact with a swapchain.
///
/// **Swapchain images are not frames in flight.** The two counts are unrelated and are kept
/// so: the image count is what the surface asks for (`minImageCount + 1`, clamped to
/// `maxImageCount` when that is non-zero), and `kFramesInFlight` is Monarc's own CPU/GPU
/// overlap. Tying them together is a bug that only shows up on a driver that reports a
/// different minimum.
class ISwapchain {
public:
    virtual ~ISwapchain() = default;

    ISwapchain(const ISwapchain&)            = delete;
    ISwapchain& operator=(const ISwapchain&) = delete;

    /// The size the swapchain's images actually are, which is not necessarily the size that
    /// was asked for -- see `SwapchainDescription::extent`. `0 x 0` on a swapchain that has
    /// been shut down or moved from.
    [[nodiscard]] virtual Extent2D Extent() const = 0;

    /// The format the surface was created with. `Format::Unknown` on a swapchain that has
    /// been shut down or moved from.
    [[nodiscard]] virtual Format ImageFormat() const = 0;

    /// How many images the swapchain has. Zero on a swapchain that has been shut down or
    /// moved from.
    [[nodiscard]] virtual u32 ImageCount() const = 0;

    /// Whether an acquired image can be copied into a buffer -- `allowReadback` was asked for
    /// **and** the surface offered it. False on a swapchain that did not ask.
    ///
    /// **This has no caller in shipped code, which is stated rather than left to be noticed** --
    /// the exception `IQueue::CompletedValue` and `LastSubmittedValue` in Device.h already make,
    /// and for a related reason. Its only caller is the swapchain readback in
    /// `Monarc.Host.Windowed`'s device suite, and being *asked* is the whole point of it: a
    /// surface is not required to offer `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, and a suite that
    /// asserted about a copy the surface never permitted would be asserting about nothing. A
    /// shipped screenshot feature is its second caller and arrives with the feature.
    [[nodiscard]] virtual bool ReadbackAvailable() const = 0;

    /// Whether the swapchain has reported that it no longer matches its surface.
    ///
    /// Set by `VK_ERROR_OUT_OF_DATE_KHR` or `VK_SUBOPTIMAL_KHR` from *either* acquire or
    /// present, and cleared by `Recreate`. Sticky, so a loop that checks it once per frame
    /// cannot miss one that was reported by a present it already handled.
    ///
    /// **Both results from both calls, which is four cases and not two.** A resize on Windows
    /// typically surfaces as `VK_ERROR_OUT_OF_DATE_KHR` from acquire, and a monitor change as
    /// `VK_SUBOPTIMAL_KHR` from present; a loop that watched only acquire would run
    /// indefinitely on a suboptimal swapchain, which is a real bug with no symptom loud enough
    /// to find by looking.
    [[nodiscard]] virtual bool NeedsRecreation() const = 0;

    /// Acquires the next image to render into.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for a swapchain that has been shut down or
    /// moved from, and for one that already has an image acquired -- see the class comment for
    /// why the second is a refusal rather than a second acquire. `ErrorCode::BackendFailure`
    /// is the API's, including a wait that timed out: a timeout is not success, because a
    /// caller that rendered anyway would be rendering into an image the presentation engine
    /// still owns.
    ///
    /// An out-of-date swapchain is **not** a failure. See `AcquireOutcome`.
    [[nodiscard]] virtual Result<AcquiredImage> Acquire() = 0;

    /// Submits `commands` on `queue`, waiting on the image `Acquire` returned and signalling
    /// what `Present` will wait on. Returns the timeline value the submission will signal,
    /// exactly as `IQueue::Submit` does.
    ///
    /// **A member of the swapchain rather than a second `IQueue::Submit` overload, and the
    /// reason is that only the swapchain can check the pairing.** A queue handed an
    /// `ISwapchain&` cannot ask whether it belongs to the same device without a downcast --
    /// undefined for an unknown dynamic type, and `dynamic_cast` needs RTTI -- where a
    /// swapchain holding its own device can compare the `IQueue&` it was given against its
    /// device's own queue by address, which is well defined for any pointer. That is the same
    /// argument `VulkanDeviceState::FindOwnList` makes for command lists, and it matters for
    /// the same reason: this machine has two adapters and therefore two devices.
    ///
    /// Every refusal `IQueue::Submit` makes applies here unchanged -- a list still recording,
    /// a list that recorded nothing, a list already submitted, a list from another device --
    /// plus `ErrorCode::InvalidArgument` when nothing is acquired, and when `queue` is not
    /// this swapchain's device's queue.
    [[nodiscard]] virtual Result<u64> SubmitForPresent(IQueue& queue, ICommandList& commands) = 0;

    /// Presents the image `Acquire` returned, on the device's graphics queue.
    ///
    /// One queue, because A3 has one -- the queue is not a parameter here for the same reason
    /// `IDevice::GraphicsQueue()` is the only queue there is. A present queue distinct from
    /// the graphics queue is a real configuration on some hardware and is not this machine's:
    /// both local adapters report presentation support on their graphics family, which is
    /// measured rather than assumed (`VulkanBackend::AdapterCanPresent`).
    ///
    /// `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` are success with
    /// `NeedsRecreation()` set: the frame was presented as well as it could be and the next
    /// one needs a new swapchain.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for a swapchain that has been shut down or
    /// moved from, for one with nothing acquired, and for one whose acquired image has not
    /// been through `SubmitForPresent` -- the last is a hang rather than an error if it
    /// reaches the API, because the semaphore present waits on would never be signalled.
    [[nodiscard]] virtual Status Present() = 0;

    /// Destroys the swapchain and builds another for `extent`, keeping the surface.
    ///
    /// **This is where resize, maximise and restore all land, and it waits for the device to
    /// go idle before touching anything.** Destroying a swapchain whose images a submitted
    /// frame is still rendering into is undefined behaviour; the wait is not a convenience.
    ///
    /// Every `TextureHandle` this swapchain issued is stale afterwards, by the generation rule
    /// `IDevice::DestroyTexture` states: the slots change hands, so a handle held across this
    /// call resolves to a refusal.
    ///
    /// **A minimised window is refused rather than parked around.** `extent` must not be
    /// empty: `VkSurfaceCapabilitiesKHR::currentExtent` is `0 x 0` for a minimised window and
    /// `vkCreateSwapchainKHR` rejects that, so the caller's frame loop is what parks -- pump
    /// messages, present nothing, and come back when the window has area again. Refusing here
    /// with `ErrorCode::InvalidArgument` rather than succeeding at nothing is what makes a
    /// loop that forgot to park report instead of silently presenting no frames.
    [[nodiscard]] virtual Status Recreate(Extent2D extent) = 0;

protected:
    ISwapchain() = default;

    // `ICommandList`'s note on absent move operations does not apply here: a `VulkanSwapchain`
    // is returned by value from a factory, exactly as a `VulkanDevice` is, so this base
    // genuinely needs its moves to exist -- `VulkanSwapchain`'s own move constructor names
    // `ISwapchain(std::move(other))`, and deleting these two lines does not build.
    ISwapchain(ISwapchain&&)            = default;
    ISwapchain& operator=(ISwapchain&&) = default;
};

}  // namespace Monarc::RHI
