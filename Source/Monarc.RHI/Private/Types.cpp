#include <Monarc/RHI/Types.h>

namespace Monarc::RHI {

// Both switches below are deliberately `default`-less. Adding an enumerator to Format then
// becomes a compile error here rather than a silent fall-through to the trailing return,
// which is exactly the shape a format table needs. The two compilers get there differently:
// Clang's -Wswitch is on at /W4, while MSVC's C4062 is off by default and /W4 does not enable
// it, so CMake/MonarcTargetOptions.cmake passes /w44062 to ask for it by name. /WX makes both
// fatal. Without that flag this comment would be half true, which is worse than absent.
//
// The trailing return still has to exist: Format has a fixed underlying type,
// so it can hold a value outside its enumerator set, and no switch can be exhaustive at run
// time however complete it is at compile time.

const char* ToString(Format format) {
    switch (format) {
        case Format::Unknown:        return "Unknown";
        case Format::R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case Format::B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    }
    // Not "Unknown". Format::Unknown is a state Monarc chose to name -- "nothing has been
    // decided yet" -- and a value that is not a Format at all is a different problem with a
    // different cause, so a log that conflated them would misdirect whoever read it.
    return "<invalid Format>";
}

u32 BytesPerPixel(Format format) {
    switch (format) {
        case Format::Unknown:        return 0;
        case Format::R8G8B8A8_UNORM: return 4;
        case Format::B8G8R8A8_UNORM: return 4;
    }
    return 0;
}

}  // namespace Monarc::RHI
