#include <Monarc/RHI/Device.h>

// The spellings of Device.h's two plain enums.
//
// Both switches are `default`-less, the shape Private/Barrier.cpp and Private/Types.cpp use and
// for their reason: adding an enumerator becomes a compile error here rather than a silent
// fall-through to the trailing return. MSVC's C4062 is off by default and /W4 does not enable
// it, so CMake/MonarcTargetOptions.cmake passes /w44062 by name; Clang's -Wswitch is on at /W4;
// /WX makes both fatal.
//
// The trailing returns still have to exist: each enum has a fixed underlying type and can hold a
// value no enumerator names, which is what Tests/TestDevice.cpp's completeness checks hand them.
//
// **Nothing else in this header has a `ToString`, and that is the membership rule rather than an
// omission.** `TextureUsage` and `BufferUsage` are flag sets, whose spelling is a composition
// rather than a lookup -- `GraphInspection.cpp` in Monarc.Render renders a usage as hex for
// exactly that reason -- and `MemoryLocation` has no caller that prints one. Each arrives with
// the first thing that needs to read it back.

namespace Monarc::RHI {

const char* ToString(LoadOp op) {
    switch (op) {
        case LoadOp::Load:     return "Load";
        case LoadOp::Clear:    return "Clear";
        case LoadOp::DontCare: return "DontCare";
    }
    return "<invalid LoadOp>";
}

const char* ToString(StoreOp op) {
    switch (op) {
        case StoreOp::Store:    return "Store";
        case StoreOp::DontCare: return "DontCare";
    }
    return "<invalid StoreOp>";
}

}  // namespace Monarc::RHI
