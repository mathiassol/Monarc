#include <VulkanPlatform.h>

// The only translation unit in Monarc.RHI.Vulkan that defines this, and it is defined here
// rather than on the target on purpose. Target-wide, every neutral source in the module would
// see vkCreateWin32SurfaceKHR and VkWin32SurfaceCreateInfoKHR declared, and the first
// accidental use of one would compile on this machine and fail on the next platform -- the
// exact rot ADR-0016 exists to prevent, relocated from an #ifdef into a compiler flag.
//
// Defined before the Vulkan headers, because it is what makes vulkan_win32.h's contents
// visible at all.
//
// Note that this is a #define and not a platform *conditional*: there is no #ifdef _WIN32
// anywhere in this file, and gate 10 in Tools/check_architecture.py -- which matches
// #if/#ifdef/#ifndef/#elif on platform macros -- would not fire here even if this directory
// were not exempt. The directory is what selects the file; monarc_module() hands the
// compiler only the current platform's Private/Platform/<Platform>/ tree.
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace Monarc::RHI::Detail {

const char* VulkanLibraryName() {
    // The Vulkan loader's own name, resolved through the platform's normal library search.
    // Not a path: Monarc does not go looking in a specific directory, because the loader is a
    // system component and the machine's own registration is what should decide which one
    // answers -- including, deliberately, when a graphics debugger has interposed its own.
    return "vulkan-1.dll";
}

const char* PlatformSurfaceExtensionName() {
    // VK_KHR_WIN32_SURFACE_EXTENSION_NAME, which is "VK_KHR_win32_surface". Taken from the
    // header rather than spelled out, so a typo is a compile error instead of an extension
    // that is silently never enabled.
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
}

}  // namespace Monarc::RHI::Detail
