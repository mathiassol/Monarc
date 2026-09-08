#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Swapchain.h>
#include <Monarc/RHI/Types.h>

namespace Monarc::RHI {

namespace Detail {

/// Brings up a `VulkanSwapchain`. Declared here, defined in Private/VulkanSwapchain.cpp, and
/// befriended below so that the one place able to construct a `VulkanSwapchain` from its state
/// is a named thing rather than the world. `VulkanDeviceFactory`'s shape and its reasoning: a
/// friend *function* declaration would have to spell `VkInstance` in this public header, and no
/// Vulkan type may appear here (Docs/Rendering/RHI.md; ADR-0014 rule 1).
struct VulkanSwapchainFactory;

/// Everything a `VulkanSwapchain` owns, behind one pointer, because it names Vulkan types and
/// this header may not.
struct VulkanSwapchainState;

}  // namespace Detail

/// A Vulkan surface, its swapchain, and the presentation semaphores that go with them.
///
/// **Concrete and move-only, returned by value from `VulkanBackend::CreateSwapchain`** --
/// `VulkanDevice`'s shape, for the reason `IDevice`'s header gives: polymorphic use without
/// polymorphic ownership, until `Monarc.Core` has an owning-pointer type and there is a second
/// backend to need one. Callers hold it as an `ISwapchain&`, and that interface does not change
/// when the ownership does.
///
/// **The destruction order is surface-inside-device-inside-backend, and nothing here can
/// enforce it.** A swapchain must be destroyed before the `VulkanDevice` it was made on -- it
/// holds that device's `vkDestroySwapchainKHR` and image views -- and before the
/// `VulkanBackend`, which owns the `VkInstance` its `VkSurfaceKHR` belongs to and unloads
/// `vulkan-1.dll`. That is the same rule `VulkanDevice`'s class comment states one rung up,
/// with the same consequence for getting it wrong: a call through an unmapped page, which
/// `Detail::Loader`'s comment records ASan will not report.
///
/// **It holds `TextureHandle`s from the device's own pool, and `Recreate` invalidates them.**
/// Each swapchain image is registered through `VulkanDeviceState::AdoptImage`, so an acquired
/// image is an ordinary texture handle with ADR-0002's generation checking intact -- which is
/// what lets `ICommandList`'s existing barrier, rendering and copy calls work on one with no
/// new entry point. Recreation releases every slot, so a handle held across it goes stale
/// rather than naming whatever now occupies the slot.
///
/// Every failure message is a string literal, for `Error::message`'s reason -- it is a
/// non-owning view. Composed detail goes to `MONARC_LOG` at the failure site.
class VulkanSwapchain final : public ISwapchain {
public:
    /// Shuts down and releases the state. See `Shutdown`.
    ~VulkanSwapchain() override;

    VulkanSwapchain(const VulkanSwapchain&)            = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    VulkanSwapchain(VulkanSwapchain&& other) noexcept;
    VulkanSwapchain& operator=(VulkanSwapchain&& other) noexcept;

    /// Waits for the device to go idle, releases the images' pool slots, destroys the
    /// semaphores, the swapchain and the surface, in that order. Safe to call unconditionally,
    /// safe to call more than once, and safe on a swapchain that has been moved from.
    ///
    /// The wait is not a convenience: destroying a swapchain whose images a submitted frame is
    /// still rendering into is undefined behaviour, and destroying a semaphore a submitted
    /// batch still refers to is `VUID-vkDestroySemaphore-semaphore-01137`.
    void Shutdown();

    /// True while the `VkSwapchainKHR` is alive.
    ///
    /// **False in one state that is neither shut down nor moved from: a `Recreate` that
    /// failed.** Recreation destroys before it builds, so a surface that has stopped being
    /// usable -- a window closed under the process, a device lost -- leaves a swapchain whose
    /// surface is still owned and whose swapchain is gone. Every accessor answers as empty and
    /// a further `Recreate` is what retries; the alternative was keeping a swapchain nobody
    /// could present to and reporting it as live.
    [[nodiscard]] bool IsInitialized() const;

    // ISwapchain. Every one of these answers safely on a shut-down or moved-from swapchain:
    // the queries report empty and the fallible calls report `ErrorCode::InvalidArgument`.
    [[nodiscard]] Extent2D              Extent() const override;
    [[nodiscard]] Format                ImageFormat() const override;
    [[nodiscard]] u32                   ImageCount() const override;
    [[nodiscard]] bool                  ReadbackAvailable() const override;
    [[nodiscard]] bool                  NeedsRecreation() const override;
    [[nodiscard]] Result<AcquiredImage> Acquire() override;
    [[nodiscard]] Result<u64> SubmitForPresent(IQueue& queue, ICommandList& commands) override;
    [[nodiscard]] Status      Present() override;
    [[nodiscard]] Status      Recreate(Extent2D extent) override;

private:
    friend struct Detail::VulkanSwapchainFactory;

    /// Adopts a state the factory has already brought up. Private, so the only way to come by
    /// a swapchain is a `CreateSwapchain` that succeeded.
    explicit VulkanSwapchain(Detail::VulkanSwapchainState* state) noexcept;

    /// Shuts down and then destroys and deallocates the state. What the destructor and
    /// move-assignment share.
    void Release();

    /// Null only on a swapchain that has been moved from.
    Detail::VulkanSwapchainState* m_state = nullptr;
};

}  // namespace Monarc::RHI
