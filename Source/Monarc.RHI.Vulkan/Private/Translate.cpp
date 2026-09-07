#include <Translate.h>

#include <string_view>

namespace Monarc::RHI::Detail {

// ToVulkan(Format) and ToDeviceType below are deliberately `default`-less, the same shape
// Monarc.RHI/Private/Types.cpp uses and for the same reason: adding an enumerator becomes a
// compile error here rather than a silent fall-through. MSVC needs /w44062 asked for by name
// (CMake/MonarcTargetOptions.cmake does), Clang's -Wswitch is on at /W4, and /WX makes both
// fatal. The trailing returns still have to exist -- both enums have a fixed underlying type
// and can hold a value outside their enumerator set, whatever the switch covers.

VkFormat ToVulkan(Format format) {
    switch (format) {
        case Format::Unknown:        return VK_FORMAT_UNDEFINED;
        case Format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

Format FromVulkan(VkFormat format) {
    // The one switch in this file that has a `default`, and Translate.h says why: VkFormat has
    // several hundred enumerators, Monarc models two, and a `default`-less switch would mean
    // enumerating an API's whole format space to translate two of it.
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: return Format::R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return Format::B8G8R8A8_UNORM;
        default:                       return Format::Unknown;
    }
}

DeviceType ToDeviceType(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return DeviceType::Other;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return DeviceType::IntegratedGpu;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return DeviceType::DiscreteGpu;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return DeviceType::VirtualGpu;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return DeviceType::Cpu;

        // Vulkan's generated enums all carry a `_MAX_ENUM` sentinel at 0x7FFFFFFF, and it is
        // an enumerator like any other as far as a `default`-less switch is concerned: both
        // compilers demand a case for it. It is not a device kind and no driver returns it,
        // so it lands where an unrecognised value lands.
        case VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM:       return DeviceType::Other;
    }
    return DeviceType::Other;
}

ApiVersion ToApiVersion(u32 packed) {
    // VK_API_VERSION_MAJOR/MINOR/PATCH rather than hand-written shifts: the variant bits
    // moved into the top of this word when Vulkan SC was specified, and the macros were
    // updated with it where an open-coded `packed >> 22` was not.
    return ApiVersion{VK_API_VERSION_MAJOR(packed), VK_API_VERSION_MINOR(packed),
                      VK_API_VERSION_PATCH(packed)};
}

const char* ToString(VkResult result) {
    switch (result) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default:                                return "<VkResult not in Monarc's table>";
    }
}

ErrorCode ToErrorCode(VkResult result) {
    switch (result) {
        // "This implementation does not have it", every one of them -- see Translate.h. The
        // hand-written paths in VulkanBackend.cpp already reach for Unsupported when they
        // discover a missing layer or extension by *asking*; these are the same discoveries
        // arriving as a VkResult, and the funnel used to flatten them to BackendFailure.
        case VK_ERROR_INCOMPATIBLE_DRIVER:
        case VK_ERROR_LAYER_NOT_PRESENT:
        case VK_ERROR_EXTENSION_NOT_PRESENT: return ErrorCode::Unsupported;

        default:                             return ErrorCode::BackendFailure;
    }
}

bool ContainsExtension(const Array<VkExtensionProperties>& extensions, const char* name) {
    const std::string_view wanted(name);
    for (const VkExtensionProperties& extension : extensions) {
        // std::string_view's own == and not strncmp: whole-string equality, so a name that is
        // a prefix of an available one does not match. See Translate.h.
        if (std::string_view(extension.extensionName) == wanted) {
            return true;
        }
    }
    return false;
}

bool ContainsLayer(const Array<VkLayerProperties>& layers, const char* name) {
    const std::string_view wanted(name);
    for (const VkLayerProperties& layer : layers) {
        if (std::string_view(layer.layerName) == wanted) {
            return true;
        }
    }
    return false;
}

LogLevel SeverityToLogLevel(VkDebugUtilsMessageSeverityFlagBitsEXT severity) {
    // Descending, so a caller passing several bits at once gets the loudest of them. Vulkan
    // documents the callback's severity as a single bit; taking the maximum rather than the
    // first match is what keeps that an assumption this function does not depend on.
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        return LogLevel::Error;
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        return LogLevel::Warning;
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
        return LogLevel::Info;
    }
    return LogLevel::Trace;
}

}  // namespace Monarc::RHI::Detail
