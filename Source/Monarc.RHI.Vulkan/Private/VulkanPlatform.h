#pragma once

namespace Monarc::RHI::Detail {

/// The two things Task 2 of Phase A3 needs from the platform, and nothing else.
///
/// **This header names no platform type and no Vulkan type, and that is its whole job.**
/// `VK_USE_PLATFORM_WIN32_KHR` is defined in exactly two translation units --
/// Private/Platform/Windows/VulkanPlatform.cpp and Private/Platform/Windows/VulkanSurface.cpp
/// -- and never on the target, so no platform-neutral source in this module ever sees a Win32
/// declaration and ADR-0016 holds without needing gate 10's exemption to do any work.
///
/// **Task 4's surface creation is declared in Private/VulkanSurface.h, not here**, and the
/// reason is mechanical rather than tidy: it takes a `VkInstance` and fills a `VkSurfaceKHR`,
/// so its header must include `<vulkan/vulkan.h>` -- and `Loader.h` includes *this* header for
/// `VulkanLibraryName()`'s default argument, so pulling the Vulkan headers in here would fix
/// the include order of every file that reaches this one. The platform file that defines
/// `VK_USE_PLATFORM_WIN32_KHR` has to do so *before* `vulkan.h` is first seen, and that trap
/// is worth confining to the one header only that file includes. (Task 2's version of this
/// comment predicted surface creation would land in VulkanPlatform.cpp; that prediction is
/// what this paragraph corrects.)
///
/// `const char*` for both. The surface extension name needs it -- Vulkan's
/// `ppEnabledExtensionNames` is an array of null-terminated strings -- and the library name
/// does not, but making the two agree costs nothing and both are string literals with static
/// storage duration, so neither can dangle. `Loader::Open` widens the library name to a
/// StringView, which is the type that carries no terminator requirement.

/// Name of the platform's Vulkan runtime library, as `Platform::Library::Open` takes it --
/// "vulkan-1.dll" on Windows, "libvulkan.so.1" or "libvulkan.1.dylib" elsewhere. Monarc
/// applies no search logic of its own; the platform resolves the bare name.
[[nodiscard]] const char* VulkanLibraryName();

/// Name of the platform's window-system surface extension --
/// `VK_KHR_WIN32_SURFACE_EXTENSION_NAME` on Windows. Requested at instance creation
/// alongside `VK_KHR_surface`.
///
/// Task 2 requested the extension and did not use it. Task 4 does:
/// `Detail::CreatePlatformSurface` in Private/VulkanSurface.h is what needs it enabled, and
/// asking for it a phase early was deliberate rather than premature -- an instance extension
/// cannot be added after `vkCreateInstance`, so an instance that had not requested it would
/// have had to be torn down and rebuilt the moment a window appeared.
[[nodiscard]] const char* PlatformSurfaceExtensionName();

}  // namespace Monarc::RHI::Detail
