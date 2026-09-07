#include <Monarc/RHI/Barrier.h>

#include <format>

namespace Monarc::RHI {

// All three switches below are deliberately `default`-less, the same shape
// Private/Types.cpp uses and for the same reason: adding an enumerator becomes a compile
// error here rather than a silent fall-through to the trailing return. MSVC's C4062 is off by
// default and /W4 does not enable it, so CMake/MonarcTargetOptions.cmake passes /w44062 to ask
// for it by name; Clang's -Wswitch is on at /W4; /WX makes both fatal.
//
// The trailing returns still have to exist. All three enums have a fixed underlying type, so
// each can hold a value no enumerator names, and for the two flag sets that is not even
// unusual -- `Copy | Blit` is a perfectly ordinary mask and is not an enumerator. The
// not-a-value name is the honest answer for both cases, and Barrier.h says so.

const char* ToString(PipelineStage stage) {
    switch (stage) {
        case PipelineStage::None:                  return "None";
        case PipelineStage::DrawIndirect:          return "DrawIndirect";
        case PipelineStage::VertexShader:          return "VertexShader";
        case PipelineStage::FragmentShader:        return "FragmentShader";
        case PipelineStage::EarlyFragmentTests:    return "EarlyFragmentTests";
        case PipelineStage::LateFragmentTests:     return "LateFragmentTests";
        case PipelineStage::ColorAttachmentOutput: return "ColorAttachmentOutput";
        case PipelineStage::ComputeShader:         return "ComputeShader";
        case PipelineStage::Copy:                  return "Copy";
        case PipelineStage::Blit:                  return "Blit";
        case PipelineStage::Resolve:               return "Resolve";
        case PipelineStage::Clear:                 return "Clear";
        case PipelineStage::Host:                  return "Host";
        case PipelineStage::AllGraphics:           return "AllGraphics";
        case PipelineStage::AllCommands:           return "AllCommands";
    }
    return "<not a single PipelineStage>";
}

const char* ToString(Access access) {
    switch (access) {
        case Access::None:                        return "None";
        case Access::IndirectCommandRead:         return "IndirectCommandRead";
        case Access::IndexRead:                   return "IndexRead";
        case Access::VertexAttributeRead:         return "VertexAttributeRead";
        case Access::UniformRead:                 return "UniformRead";
        case Access::ShaderSampledRead:           return "ShaderSampledRead";
        case Access::ShaderStorageRead:           return "ShaderStorageRead";
        case Access::ShaderStorageWrite:          return "ShaderStorageWrite";
        case Access::ColorAttachmentRead:         return "ColorAttachmentRead";
        case Access::ColorAttachmentWrite:        return "ColorAttachmentWrite";
        case Access::DepthStencilAttachmentRead:  return "DepthStencilAttachmentRead";
        case Access::DepthStencilAttachmentWrite: return "DepthStencilAttachmentWrite";
        case Access::TransferRead:                return "TransferRead";
        case Access::TransferWrite:               return "TransferWrite";
        case Access::HostRead:                    return "HostRead";
        case Access::HostWrite:                   return "HostWrite";
        case Access::MemoryRead:                  return "MemoryRead";
        case Access::MemoryWrite:                 return "MemoryWrite";
    }
    return "<not a single Access>";
}

const char* ToString(TextureLayout layout) {
    switch (layout) {
        case TextureLayout::Undefined:              return "Undefined";
        case TextureLayout::General:                return "General";
        case TextureLayout::ColorAttachment:        return "ColorAttachment";
        case TextureLayout::DepthStencilAttachment: return "DepthStencilAttachment";
        case TextureLayout::DepthStencilReadOnly:   return "DepthStencilReadOnly";
        case TextureLayout::ShaderReadOnly:         return "ShaderReadOnly";
        case TextureLayout::TransferSource:         return "TransferSource";
        case TextureLayout::TransferDestination:    return "TransferDestination";
    }
    return "<invalid TextureLayout>";
}

// `std::format_to_n` and not `std::format`, which is ADR-0003's condition on `<format>` in
// runtime code: format into a fixed buffer, never allocate a `std::string`. `Detail::Format`
// in Monarc/Core/Log.h and `JobSystem.cpp`'s worker naming are the precedents.
//
// The terminator is written explicitly even though `BarrierDescription::text` value-initialises
// its array and the byte is therefore already zero -- Adapter.cpp's `ToString(AdapterUuid)`
// says why, and it is the same reason here: this function's contract should not depend on a
// member initialiser in a different file staying as it is. `sizeof(text) - 1` is what leaves
// the byte to write it into, so `written.out` is in bounds even when the format fills the
// buffer.

BarrierDescription Describe(const BufferBarrier& barrier) {
    BarrierDescription description;
    const auto         written = std::format_to_n(
        description.text, sizeof(description.text) - 1,
        "buffer (slot {}, generation {}): sync {} (0x{:x}) -> {} (0x{:x}), access {} (0x{:x}) "
        "-> {} (0x{:x})",
        barrier.buffer.index, barrier.buffer.generation, ToString(barrier.syncBefore),
        static_cast<u32>(barrier.syncBefore), ToString(barrier.syncAfter),
        static_cast<u32>(barrier.syncAfter), ToString(barrier.accessBefore),
        static_cast<u32>(barrier.accessBefore), ToString(barrier.accessAfter),
        static_cast<u32>(barrier.accessAfter));
    *written.out = '\0';
    return description;
}

BarrierDescription Describe(const TextureBarrier& barrier) {
    BarrierDescription description;
    const auto         written = std::format_to_n(
        description.text, sizeof(description.text) - 1,
        "texture (slot {}, generation {}): layout {} -> {}, sync {} (0x{:x}) -> {} (0x{:x}), "
        "access {} (0x{:x}) -> {} (0x{:x})",
        barrier.Texture().index, barrier.Texture().generation,
        ToString(barrier.LayoutBefore()), ToString(barrier.LayoutAfter()),
        ToString(barrier.SyncBefore()), static_cast<u32>(barrier.SyncBefore()),
        ToString(barrier.SyncAfter()), static_cast<u32>(barrier.SyncAfter()),
        ToString(barrier.AccessBefore()), static_cast<u32>(barrier.AccessBefore()),
        ToString(barrier.AccessAfter()), static_cast<u32>(barrier.AccessAfter()));
    *written.out = '\0';
    return description;
}

}  // namespace Monarc::RHI
