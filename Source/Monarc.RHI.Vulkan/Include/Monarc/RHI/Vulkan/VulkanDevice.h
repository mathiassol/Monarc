#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>

#include <span>

namespace Monarc::RHI {

namespace Detail {

/// Brings up a `VulkanDevice`. Declared here, defined in Private/VulkanDeviceFactory.h, and
/// befriended below so that the one place able to construct a `VulkanDevice` from its state is
/// a named thing rather than the world.
///
/// A forward-declared struct and not a free function, because a friend *function* declaration
/// would have to spell its parameters -- `VkPhysicalDevice` among them -- in this public
/// header, and no Vulkan type may appear here (Docs/Rendering/RHI.md; ADR-0014 rule 1).
struct VulkanDeviceFactory;

/// Brings up a `VulkanSwapchain`, and is befriended below for one reason: a swapchain needs
/// this device's `VulkanDeviceState` -- its entry-point table, its queue, and its texture pool,
/// which is where the swapchain's images are registered. `m_state` is private, and the house
/// answer to "who may reach into it" is a named friend rather than an accessor the world can
/// call. Declared in Monarc/RHI/Vulkan/VulkanSwapchain.h and defined in
/// Private/VulkanSwapchain.cpp.
struct VulkanSwapchainFactory;

/// Everything a `VulkanDevice` owns, behind one pointer, because it names Vulkan types and
/// this header may not.
///
/// At namespace scope rather than nested in `VulkanDevice` the way `VulkanBackend::State` is,
/// and the reason is reach: the command list and the queue are separate classes that both
/// need to name this type, and a *private* nested type is exactly what they could not.
struct VulkanDeviceState;

}  // namespace Detail

/// A Vulkan logical device: one adapter, one graphics queue, one timeline semaphore, the
/// resources created on it, and `kFramesInFlight` command pools.
///
/// **Polymorphic use without polymorphic ownership**, which is the decision `IDevice`'s own
/// header explains: this is a concrete, move-only class that `VulkanBackend::CreateDevice`
/// returns by value, and callers hold it as an `IDevice&`. There is no `IBackend` and no
/// owning device handle yet; both arrive with the second backend, and the `IDevice` interface
/// does not change when they do -- only the factory's return type.
///
/// **Factory construction**, the shape `VulkanBackend::Create` and `Platform::Library::Open`
/// use: a `CreateDevice` returning a `Result`, a `Shutdown` safe to call unconditionally and
/// repeatedly, and an `IsInitialized` query. A device a caller holds is a device that came up.
/// Three states exist and `IsInitialized()` separates them: live, shut down (the state
/// allocation is still there, every query answers as if empty), and moved-from (no state at
/// all, every query answers without dereferencing).
///
/// **A device must be destroyed before the backend that created it.** Not a style rule: the
/// backend owns `vulkan-1.dll` and the `VkInstance`, and `VulkanBackend::Shutdown` unloads the
/// module -- which unmaps the very `vkDestroyDevice` this class needs. The device keeps no
/// reference to the backend, so nothing here can enforce the order; a caller that gets it
/// wrong is calling through an unmapped page, which is the failure shape
/// `Detail::Loader`'s own class comment records ASan will not report.
///
/// Every failure message is a string literal, for `Error::message`'s reason -- it is a
/// non-owning view. Composed detail goes to `MONARC_LOG` at the failure site.
class VulkanDevice final : public IDevice {
public:
    /// Shuts down and releases the state. See `Shutdown`.
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&)            = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VulkanDevice(VulkanDevice&& other) noexcept;
    VulkanDevice& operator=(VulkanDevice&& other) noexcept;

    /// Waits for the device to go idle, then destroys every resource, command pool, semaphore
    /// and the `VkDevice` itself, in reverse creation order. Safe to call unconditionally,
    /// safe to call more than once, and safe on a device that has been moved from.
    ///
    /// The wait is not optional and not a convenience: destroying a command pool whose buffers
    /// are still executing, or an image a submitted copy still reads, is undefined behaviour
    /// and a validation error. A caller that wants a narrower wait has `IQueue::Wait`.
    void Shutdown();

    /// True while the `VkDevice` is alive.
    [[nodiscard]] bool IsInitialized() const;

    /// The graphics queue family index this device's queue came from. Not on `IDevice`: it is
    /// a Vulkan number with no meaning in the abstraction, and it is here because the device
    /// tests assert it is a family the adapter actually reported as graphics-capable.
    [[nodiscard]] u32 GraphicsQueueFamilyIndex() const;

    // IDevice. Every one of these answers safely on a shut-down or moved-from device: the
    // queries report empty and the fallible calls report `ErrorCode::InvalidArgument`.
    [[nodiscard]] const AdapterInfo& Adapter() const override;
    [[nodiscard]] IQueue&            GraphicsQueue() override;
    [[nodiscard]] Result<TextureHandle> CreateTexture(
        const TextureDescription& description) override;
    void                             DestroyTexture(TextureHandle texture) override;
    [[nodiscard]] Result<BufferHandle> CreateBuffer(
        const BufferDescription& description) override;
    void                             DestroyBuffer(BufferHandle buffer) override;
    [[nodiscard]] Result<std::span<const u8>> MapBufferForRead(BufferHandle buffer) override;
    void                                      UnmapBuffer(BufferHandle buffer) override;
    [[nodiscard]] Result<ICommandList*>       BeginFrame() override;
    [[nodiscard]] Status                      WaitIdle() override;

private:
    friend struct Detail::VulkanDeviceFactory;
    friend struct Detail::VulkanSwapchainFactory;

    /// Adopts a state the factory has already brought up. Private, so the only way to come by
    /// a device is a `CreateDevice` that succeeded.
    explicit VulkanDevice(Detail::VulkanDeviceState* state) noexcept;

    /// Shuts down and then destroys and deallocates the state. What the destructor and
    /// move-assignment share.
    void Release();

    /// Null only on a device that has been moved from.
    Detail::VulkanDeviceState* m_state = nullptr;
};

}  // namespace Monarc::RHI
