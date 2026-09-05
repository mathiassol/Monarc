#include <Monarc/Core/Error.h>

namespace Monarc {

const char* ToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Unknown:          return "Unknown";
        case ErrorCode::InvalidArgument:  return "InvalidArgument";
        case ErrorCode::OutOfMemory:      return "OutOfMemory";
        case ErrorCode::NotFound:         return "NotFound";
        case ErrorCode::AlreadyExists:    return "AlreadyExists";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::IoFailure:        return "IoFailure";
        case ErrorCode::Unsupported:      return "Unsupported";
    }
    return "Unknown";
}

}  // namespace Monarc
