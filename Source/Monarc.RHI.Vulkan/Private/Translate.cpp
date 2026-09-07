#include <Translate.h>

#include <string_view>

namespace Monarc::RHI::Detail {

namespace {

/// Calls `translate` on each set bit of `mask`, one bit at a time, and returns the union of
/// the results. `mask` of zero calls nothing and returns zero.
///
/// This is what lets each flag set have one exhaustive `default`-less switch over its
/// *enumerators* while callers still pass masks -- see the note above the barrier section in
/// Translate.h. Exercised by every `ToVulkan(mask)` below, so it is not a helper that only a
/// test reaches.
template <typename Enum, typename Translate>
[[nodiscard]] auto ForEachBit(Enum mask, Translate translate) -> decltype(translate(mask)) {
    decltype(translate(mask)) result = 0;
    u32                       remaining = static_cast<u32>(mask);
    while (remaining != 0) {
        // The lowest set bit, and then clear it. Two operations rather than a shift loop over
        // all 32 positions, so the cost is the number of bits actually set.
        const u32 bit = remaining & ~(remaining - 1U);
        remaining &= remaining - 1U;
        result |= translate(static_cast<Enum>(bit));
    }
    return result;
}

}  // namespace

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

        // Added in Task 3, each because a call this module now makes can return it:
        // vkWaitSemaphores can time out; vkCreateDevice can report a feature absent, too many
        // objects, or a lost device; vkMapMemory can fail to map. Nothing speculative -- the
        // table's rule is that a result appears when a call that can produce it does.
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
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
        case VK_ERROR_EXTENSION_NOT_PRESENT:
        // The fourth, added in Task 3 with the call that can return it: vkCreateDevice
        // reports VK_ERROR_FEATURE_NOT_PRESENT when a requested feature is absent, and
        // Monarc requests three -- timeline semaphores, dynamic rendering, synchronization2.
        // "This device does not have it" is the same fact as the three above, arriving from a
        // different call.
        case VK_ERROR_FEATURE_NOT_PRESENT:   return ErrorCode::Unsupported;

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

// ---------------------------------------------------------------------------------------
// The barrier model. Every `ToVulkanBit` and `ToVulkan(TextureLayout)` below is
// `default`-less; every `FromVulkan*` is not, and Translate.h says why for each.
// ---------------------------------------------------------------------------------------

VkPipelineStageFlags2 ToVulkanBit(PipelineStage stage) {
    switch (stage) {
        case PipelineStage::None:                  return VK_PIPELINE_STAGE_2_NONE;
        case PipelineStage::DrawIndirect:          return VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        case PipelineStage::VertexShader:          return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        case PipelineStage::FragmentShader:        return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case PipelineStage::EarlyFragmentTests:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        case PipelineStage::LateFragmentTests:
            return VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case PipelineStage::ColorAttachmentOutput:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case PipelineStage::ComputeShader:         return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case PipelineStage::Copy:                  return VK_PIPELINE_STAGE_2_COPY_BIT;
        case PipelineStage::Blit:                  return VK_PIPELINE_STAGE_2_BLIT_BIT;
        case PipelineStage::Resolve:               return VK_PIPELINE_STAGE_2_RESOLVE_BIT;
        case PipelineStage::Clear:                 return VK_PIPELINE_STAGE_2_CLEAR_BIT;
        case PipelineStage::Host:                  return VK_PIPELINE_STAGE_2_HOST_BIT;
        case PipelineStage::AllGraphics:           return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        case PipelineStage::AllCommands:           return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    // Conservative, not invalid, and Translate.h gives the reason: a stage mask has no
    // obviously-wrong value the way a format does, so the only two answers available are
    // under- and over-synchronising.
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

VkPipelineStageFlags2 ToVulkan(PipelineStage stages) {
    return ForEachBit(stages, [](PipelineStage bit) { return ToVulkanBit(bit); });
}

PipelineStage FromVulkanStages(VkPipelineStageFlags2 stages) {
    // An if-chain and not a switch, because there is nothing to switch *on*: the argument is a
    // mask of several bits, and Vulkan's stage bits are `static const VkFlags64` constants
    // rather than an enum a compiler could check a switch against. So this direction gets no
    // exhaustiveness guarantee from the language -- exactly as `FromVulkan(VkFormat)` does not
    // -- and what pins it instead is the round-trip case in
    // Tests/TestVulkanBarrierTranslate.cpp, which fails for any row missing here.
    PipelineStage result = PipelineStage::None;
#define MONARC_VK_STAGE_BACK(vkBit, rhiStage)                                              \
    if ((stages & (vkBit)) != 0) {                                                         \
        result |= PipelineStage::rhiStage;                                                 \
    }
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, DrawIndirect)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VertexShader)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, FragmentShader)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, EarlyFragmentTests)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, LateFragmentTests)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, ColorAttachmentOutput)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ComputeShader)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_COPY_BIT, Copy)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_BLIT_BIT, Blit)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_RESOLVE_BIT, Resolve)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_CLEAR_BIT, Clear)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_HOST_BIT, Host)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, AllGraphics)
    MONARC_VK_STAGE_BACK(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, AllCommands)
#undef MONARC_VK_STAGE_BACK
    return result;
}

VkAccessFlags2 ToVulkanBit(Access access) {
    switch (access) {
        case Access::None:                return VK_ACCESS_2_NONE;
        case Access::IndirectCommandRead: return VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        case Access::IndexRead:           return VK_ACCESS_2_INDEX_READ_BIT;
        case Access::VertexAttributeRead: return VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        case Access::UniformRead:         return VK_ACCESS_2_UNIFORM_READ_BIT;
        case Access::ShaderSampledRead:   return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case Access::ShaderStorageRead:   return VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        case Access::ShaderStorageWrite:  return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        case Access::ColorAttachmentRead: return VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        case Access::ColorAttachmentWrite: return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case Access::DepthStencilAttachmentRead:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case Access::DepthStencilAttachmentWrite:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case Access::TransferRead:        return VK_ACCESS_2_TRANSFER_READ_BIT;
        case Access::TransferWrite:       return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        case Access::HostRead:            return VK_ACCESS_2_HOST_READ_BIT;
        case Access::HostWrite:           return VK_ACCESS_2_HOST_WRITE_BIT;
        case Access::MemoryRead:          return VK_ACCESS_2_MEMORY_READ_BIT;
        case Access::MemoryWrite:         return VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
}

VkAccessFlags2 ToVulkan(Access accesses) {
    return ForEachBit(accesses, [](Access bit) { return ToVulkanBit(bit); });
}

Access FromVulkanAccess(VkAccessFlags2 accesses) {
    // `FromVulkanStages`'s note applies unchanged.
    Access result = Access::None;
#define MONARC_VK_ACCESS_BACK(vkBit, rhiAccess)                                            \
    if ((accesses & (vkBit)) != 0) {                                                       \
        result |= Access::rhiAccess;                                                       \
    }
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, IndirectCommandRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_INDEX_READ_BIT, IndexRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VertexAttributeRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_UNIFORM_READ_BIT, UniformRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, ShaderSampledRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_SHADER_STORAGE_READ_BIT, ShaderStorageRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ShaderStorageWrite)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, ColorAttachmentRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, ColorAttachmentWrite)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                          DepthStencilAttachmentRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          DepthStencilAttachmentWrite)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_TRANSFER_READ_BIT, TransferRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_TRANSFER_WRITE_BIT, TransferWrite)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_HOST_READ_BIT, HostRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_HOST_WRITE_BIT, HostWrite)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_MEMORY_READ_BIT, MemoryRead)
    MONARC_VK_ACCESS_BACK(VK_ACCESS_2_MEMORY_WRITE_BIT, MemoryWrite)
#undef MONARC_VK_ACCESS_BACK
    return result;
}

VkImageLayout ToVulkan(TextureLayout layout) {
    switch (layout) {
        case TextureLayout::Undefined:       return VK_IMAGE_LAYOUT_UNDEFINED;
        case TextureLayout::General:         return VK_IMAGE_LAYOUT_GENERAL;
        case TextureLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case TextureLayout::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case TextureLayout::DepthStencilReadOnly:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case TextureLayout::ShaderReadOnly:  return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case TextureLayout::TransferSource:  return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case TextureLayout::TransferDestination: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    // Not VK_IMAGE_LAYOUT_UNDEFINED. See Translate.h: UNDEFINED is a legal `oldLayout` that
    // discards the texture's contents, so it would turn an invalid value into silent data
    // loss. MAX_ENUM is rejected wherever it appears.
    return VK_IMAGE_LAYOUT_MAX_ENUM;
}

TextureLayout FromVulkan(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return TextureLayout::Undefined;
        case VK_IMAGE_LAYOUT_GENERAL:   return TextureLayout::General;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return TextureLayout::ColorAttachment;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return TextureLayout::DepthStencilAttachment;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            return TextureLayout::DepthStencilReadOnly;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return TextureLayout::ShaderReadOnly;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return TextureLayout::TransferSource;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return TextureLayout::TransferDestination;
        default:                        return TextureLayout::Undefined;
    }
}

VkMemoryBarrier2 ToVulkan(const GlobalBarrier& barrier) {
    VkMemoryBarrier2 out{};
    out.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    out.srcStageMask  = ToVulkan(barrier.syncBefore);
    out.dstStageMask  = ToVulkan(barrier.syncAfter);
    out.srcAccessMask = ToVulkan(barrier.accessBefore);
    out.dstAccessMask = ToVulkan(barrier.accessAfter);
    return out;
}

VkBufferMemoryBarrier2 ToVulkan(const BufferBarrier& barrier, VkBuffer buffer) {
    VkBufferMemoryBarrier2 out{};
    out.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    out.srcStageMask        = ToVulkan(barrier.syncBefore);
    out.dstStageMask        = ToVulkan(barrier.syncAfter);
    out.srcAccessMask       = ToVulkan(barrier.accessBefore);
    out.dstAccessMask       = ToVulkan(barrier.accessAfter);
    out.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.buffer              = buffer;
    out.offset              = 0;
    out.size                = VK_WHOLE_SIZE;
    return out;
}

VkImageMemoryBarrier2 ToVulkan(const TextureBarrier& barrier, VkImage image) {
    VkImageMemoryBarrier2 out{};
    out.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    out.srcStageMask                = ToVulkan(barrier.SyncBefore());
    out.dstStageMask                = ToVulkan(barrier.SyncAfter());
    out.srcAccessMask               = ToVulkan(barrier.AccessBefore());
    out.dstAccessMask               = ToVulkan(barrier.AccessAfter());
    out.oldLayout                   = ToVulkan(barrier.LayoutBefore());
    out.newLayout                   = ToVulkan(barrier.LayoutAfter());
    out.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    out.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    out.image                       = image;
    out.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    out.subresourceRange.baseMipLevel   = 0;
    out.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
    out.subresourceRange.baseArrayLayer = 0;
    out.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
    return out;
}

GlobalBarrier FromVulkan(const VkMemoryBarrier2& barrier) {
    return GlobalBarrier{FromVulkanStages(barrier.srcStageMask),
                         FromVulkanStages(barrier.dstStageMask),
                         FromVulkanAccess(barrier.srcAccessMask),
                         FromVulkanAccess(barrier.dstAccessMask)};
}

BufferBarrier FromVulkan(const VkBufferMemoryBarrier2& barrier, BufferHandle buffer) {
    return BufferBarrier{buffer, FromVulkanStages(barrier.srcStageMask),
                         FromVulkanStages(barrier.dstStageMask),
                         FromVulkanAccess(barrier.srcAccessMask),
                         FromVulkanAccess(barrier.dstAccessMask)};
}

TextureBarrier FromVulkan(const VkImageMemoryBarrier2& barrier, TextureHandle texture) {
    return TextureBarrier(texture, FromVulkan(barrier.oldLayout), FromVulkan(barrier.newLayout),
                          FromVulkanStages(barrier.srcStageMask),
                          FromVulkanStages(barrier.dstStageMask),
                          FromVulkanAccess(barrier.srcAccessMask),
                          FromVulkanAccess(barrier.dstAccessMask));
}

// ---------------------------------------------------------------------------------------
// Resource creation and rendering.
// ---------------------------------------------------------------------------------------

VkImageUsageFlags ToVulkanBit(TextureUsage usage) {
    switch (usage) {
        case TextureUsage::None:                return 0;
        case TextureUsage::ColorAttachment:     return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        case TextureUsage::TransferSource:      return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        case TextureUsage::TransferDestination: return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return 0;
}

VkImageUsageFlags ToVulkan(TextureUsage usage) {
    return ForEachBit(usage, [](TextureUsage bit) { return ToVulkanBit(bit); });
}

VkBufferUsageFlags ToVulkanBit(BufferUsage usage) {
    switch (usage) {
        case BufferUsage::None:                return 0;
        case BufferUsage::TransferSource:      return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        case BufferUsage::TransferDestination: return VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    return 0;
}

VkBufferUsageFlags ToVulkan(BufferUsage usage) {
    return ForEachBit(usage, [](BufferUsage bit) { return ToVulkanBit(bit); });
}

VkAttachmentLoadOp ToVulkan(LoadOp loadOp) {
    switch (loadOp) {
        case LoadOp::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    // DONT_CARE and not LOAD. An unrecognised value is a bug, and of the three the one that
    // cannot silently produce a *plausible* frame is the one that leaves the attachment
    // undefined: LOAD would render over whatever was there and look almost right.
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp ToVulkan(StoreOp storeOp) {
    switch (storeOp) {
        case StoreOp::Store:    return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkMemoryPropertyFlags ToVulkan(MemoryLocation location) {
    switch (location) {
        case MemoryLocation::DeviceLocal: return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        case MemoryLocation::HostVisible:
            // Coherent as well as visible, so a mapped read needs no invalidation. Every
            // implementation must expose at least one type with both -- see Translate.h.
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    // No memory type can have every property bit set, so an unrecognised location finds no
    // type and the allocation reports Unsupported. Zero would instead match the *first* type
    // the device reports, which for a HostVisible request could be device-local memory the CPU
    // cannot address -- a mapping failure much further from its cause.
    return VK_MEMORY_PROPERTY_FLAG_BITS_MAX_ENUM;
}

u32 FindMemoryType(const VkPhysicalDeviceMemoryProperties& properties, u32 allowedTypeBits,
                   VkMemoryPropertyFlags required) {
    for (u32 index = 0; index < properties.memoryTypeCount; ++index) {
        const bool allowed = (allowedTypeBits & (1U << index)) != 0;
        const bool matches =
            (properties.memoryTypes[index].propertyFlags & required) == required;
        if (allowed && matches) {
            return index;
        }
    }
    return kNoMemoryType;
}

}  // namespace Monarc::RHI::Detail
