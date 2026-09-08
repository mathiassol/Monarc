#include <Monarc/Render/Access.h>

namespace Monarc::Render {

// `default`-less, the shape Monarc.RHI/Private/Barrier.cpp uses and for its reason: adding an
// enumerator becomes a compile error here rather than a silent fall-through to the trailing
// return. MSVC's C4062 is off by default and /W4 does not enable it, so
// CMake/MonarcTargetOptions.cmake passes /w44062 by name; Clang's -Wswitch is on at /W4; /WX
// makes both fatal. `RequirementOf` in Access.h is the other switch a new enumerator has to
// pass through, and it is written the same way.
//
// The trailing return still has to exist: `ResourceAccess` has a fixed underlying type and can
// hold a value no enumerator names, which is exactly what Tests/TestAccess.cpp's completeness
// check hands it.

const char* ToString(ResourceAccess access) {
    switch (access) {
        case ResourceAccess::ColorAttachmentRead:         return "ColorAttachmentRead";
        case ResourceAccess::ColorAttachmentWrite:        return "ColorAttachmentWrite";
        case ResourceAccess::DepthStencilAttachmentRead:  return "DepthStencilAttachmentRead";
        case ResourceAccess::DepthStencilAttachmentWrite: return "DepthStencilAttachmentWrite";
        case ResourceAccess::SampledRead:                 return "SampledRead";
        case ResourceAccess::StorageRead:                 return "StorageRead";
        case ResourceAccess::StorageWrite:                return "StorageWrite";
        case ResourceAccess::IndirectRead:                return "IndirectRead";
    }
    return "<invalid ResourceAccess>";
}

}  // namespace Monarc::Render
