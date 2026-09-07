#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Vulkan/VulkanDevice.h>

#include <Loader.h>
#include <vulkan/vulkan.h>

namespace Monarc::RHI::Detail {

/// The one thing permitted to construct a `VulkanDevice`.
///
/// **This exists because device creation and adapter lookup live in different translation
/// units, and each holds a private thing the other needs.** `VulkanBackend::CreateDevice` is
/// the entry point a caller sees, and it is defined in VulkanBackend.cpp because only that
/// file can see `VulkanBackend::State` -- the loader and the `VkInstance` to enumerate
/// physical devices from. Everything after the lookup is defined in VulkanDevice.cpp, because
/// only that file can see `VulkanDeviceState`. So the two meet here: a struct named in
/// `VulkanDevice`'s friend declaration, declared with Vulkan types in a private header, and
/// defined on the device's side of the line.
struct VulkanDeviceFactory {
    /// Creates a logical device on `physicalDevice` with one graphics queue, a timeline
    /// semaphore, `kFramesInFlight` command pools and the resource pools `config` sizes.
    ///
    /// `adapter` must be the description `physicalDevice` itself reported, freshly queried --
    /// not a copy a caller supplied. `VulkanBackend::CreateDevice` re-describes the device it
    /// found for exactly that reason: `IDevice::Adapter()` is then a fact about the driver
    /// rather than an echo of what the caller passed in.
    ///
    /// `loader` is read during this call only -- for `vkCreateDevice`, `vkGetDeviceProcAddr`
    /// and the two physical-device property queries -- and is not retained. The device stores
    /// its own `DeviceFunctions` table and needs nothing from the instance afterwards. It
    /// still must be destroyed before the backend, because the backend unloads the module the
    /// table points into; `VulkanDevice`'s class comment says so.
    ///
    /// `allocator` is used for the device's state and its resource pools and must outlive the
    /// device. Nothing survives a failure: the state is destroyed and deallocated before this
    /// returns.
    [[nodiscard]] static Result<VulkanDevice> Create(IAllocator& allocator, const Loader& loader,
                                                     VkPhysicalDevice   physicalDevice,
                                                     const AdapterInfo& adapter,
                                                     const DeviceConfig& config);
};

}  // namespace Monarc::RHI::Detail
