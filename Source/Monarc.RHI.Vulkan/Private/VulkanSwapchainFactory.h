#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/RHI/Swapchain.h>
#include <Monarc/RHI/Vulkan/VulkanDevice.h>
#include <Monarc/RHI/Vulkan/VulkanSwapchain.h>

#include <Loader.h>
#include <vulkan/vulkan.h>

namespace Monarc::RHI::Detail {

/// The one thing permitted to construct a `VulkanSwapchain`.
///
/// **`VulkanDeviceFactory.h`'s shape and its exact reason: the two halves of the operation are
/// in different translation units and each holds a private thing the other needs.**
/// `VulkanBackend::CreateSwapchain` is the entry point a caller sees and is defined in
/// VulkanBackend.cpp, because only that file can see `VulkanBackend::State` -- the loader and
/// the `VkInstance` a surface is created on. Everything after that is defined in
/// VulkanSwapchain.cpp, because only that file can see `VulkanSwapchainState`. So the two meet
/// here: a struct named in `VulkanSwapchain`'s friend declaration and in `VulkanDevice`'s,
/// declared with Vulkan types in a private header, and defined on the swapchain's side of the
/// line.
///
/// It is befriended by *both* classes, which `VulkanDeviceFactory` is not, and that asymmetry
/// is the point: a swapchain is the one object in this module that needs the internals of two
/// others -- its own state, and the device's pool and queue.
struct VulkanSwapchainFactory {
    /// Creates a surface for `description.surface` on `instance`, checks that `device`'s
    /// graphics queue family can present to it, and builds a swapchain.
    ///
    /// `loader` is **retained** by the returned swapchain, unlike `VulkanDeviceFactory::Create`
    /// which reads it and drops it: `ISwapchain::Recreate` has to re-query the surface's
    /// capabilities, formats and present modes, and those are instance-level calls. So `loader`
    /// and `instance` must both outlive the swapchain, which is another way of saying a
    /// swapchain must be destroyed before its backend -- as `VulkanSwapchain`'s class comment
    /// says.
    ///
    /// `device` must outlive the swapchain too, and for a second reason on top of the entry
    /// points: the swapchain's images are registered in that device's texture pool.
    ///
    /// `allocator` is used for the swapchain's state and its image array and must outlive it.
    /// Nothing survives a failure: the state is shut down, destroyed and deallocated before
    /// this returns, so the surface is not leaked by a swapchain that failed to build.
    ///
    /// Failures are `ErrorCode::InvalidArgument` (no native window, an empty extent,
    /// `Format::Unknown`, or a device that has been shut down), `ErrorCode::Unsupported` (the
    /// device has no `VK_KHR_swapchain`, its graphics family cannot present to this surface, or
    /// the surface offers neither the format/colour-space pair nor FIFO nor opaque
    /// composition), `ErrorCode::NotFound` (the platform's surface entry point did not
    /// resolve), `ErrorCode::OutOfMemory` (Monarc's allocator, or the device's texture pool)
    /// or `ErrorCode::BackendFailure`.
    [[nodiscard]] static Result<VulkanSwapchain> Create(IAllocator& allocator,
                                                        const Loader& loader,
                                                        VkInstance    instance,
                                                        VulkanDevice& device,
                                                        const SwapchainDescription& description);
};

}  // namespace Monarc::RHI::Detail
