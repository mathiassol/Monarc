#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/RHI/Swapchain.h>

#include <vulkan/vulkan.h>

namespace Monarc::RHI::Detail {

/// Surface creation, as the one thing the platform provides that needs Vulkan types.
///
/// **A header of its own rather than a third declaration in VulkanPlatform.h**, and the reason
/// is include order and not taste. This header includes `<vulkan/vulkan.h>`;
/// `Private/Platform/Windows/VulkanSurface.cpp` must define `VK_USE_PLATFORM_WIN32_KHR`
/// *before* that header is first seen, or `vulkan_win32.h` is never pulled in and
/// `vkCreateWin32SurfaceKHR` does not exist. `VulkanPlatform.h` is included by `Loader.h`,
/// which is included by nearly every file in the module -- so putting these declarations
/// there would make that ordering constraint everyone's problem. Here it is one file's.
///
/// **This header names no platform type either.** `HWND` and `VkWin32SurfaceCreateInfoKHR`
/// appear only in the platform .cpp; what crosses this line is `SurfaceDescription`'s two
/// opaque `void*` (ADR-0016).
///
/// A `Private/` header naming Vulkan types is the same thing `Loader.h` and
/// `VulkanDeviceState.h` are: the rule that no Vulkan type may appear in this module's headers
/// is about its *public* ones (Docs/Rendering/RHI.md; ADR-0014 rule 1).

/// Creates a `VkSurfaceKHR` on `instance` for the native window `description` names.
///
/// `getInstanceProcAddr` is `Loader::GetInstanceProcAddr()`, passed as the one function
/// pointer the platform needs rather than as a `const Loader&`: this header would otherwise
/// have to include `Loader.h`, which includes `VulkanPlatform.h`, and a header cycle for the
/// sake of one pointer is not worth having.
///
/// The platform's surface constructor is resolved on every call rather than cached in a table,
/// which is the point of resolving it here at all: `PFN_vkCreateWin32SurfaceKHR` cannot be a
/// member of `InstanceFunctions` without putting a Win32 type in `Loader.h`. Surface creation
/// happens once per window, so the lookup is on no path that matters.
///
/// Fails with `ErrorCode::InvalidArgument` for a `description` missing a handle the platform
/// requires or a null `instance`, `ErrorCode::NotFound` when the platform's surface entry point
/// does not resolve -- which means the instance was created without
/// `PlatformSurfaceExtensionName()`, and `BringUp` requires it -- and
/// `ErrorCode::BackendFailure` or `ErrorCode::Unsupported` for what the API said. `out` is left
/// `VK_NULL_HANDLE` on every failure.
[[nodiscard]] Status CreatePlatformSurface(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                           VkInstance                instance,
                                           const SurfaceDescription& description,
                                           VkSurfaceKHR&             out);

}  // namespace Monarc::RHI::Detail
