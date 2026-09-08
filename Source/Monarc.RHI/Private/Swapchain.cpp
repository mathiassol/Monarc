#include <Monarc/RHI/Swapchain.h>

namespace Monarc::RHI {

// `default`-less, for Types.cpp's reason and by the same mechanism: Clang's -Wswitch is on at
// /W4 and MSVC's C4062 is asked for by name in CMake/MonarcTargetOptions.cmake, so an
// enumerator added to AcquireOutcome is a compile error here rather than a silent fall-through.
// The trailing return still has to exist -- the enum has a fixed underlying type and can hold a
// value outside its enumerator set.

const char* ToString(AcquireOutcome outcome) {
    switch (outcome) {
        case AcquireOutcome::Acquired:  return "Acquired";
        case AcquireOutcome::OutOfDate: return "OutOfDate";
    }
    return "<invalid AcquireOutcome>";
}

}  // namespace Monarc::RHI
