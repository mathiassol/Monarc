#pragma once

#include <Monarc/Core/Types.h>

#include <expected>
#include <string_view>

namespace Monarc {

enum class ErrorCode : u32 {
    Unknown = 0,
    InvalidArgument,
    OutOfMemory,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    IoFailure,

    /// This device, platform or build cannot do what was asked -- the capability is absent,
    /// not broken.
    Unsupported,

    /// A backing API returned an error of its own. Distinct from Unsupported because the two
    /// lead to different handling: Unsupported means asking differently might work, where a
    /// backend failure means the call was legitimate and the API refused it. The message
    /// names the underlying result -- a VkResult's own spelling, for Monarc.RHI.Vulkan -- so
    /// that a report is actionable without a debugger attached.
    BackendFailure,
};

const char* ToString(ErrorCode code);

/// An error value. `message` is a non-owning view and must reference storage that
/// outlives the error — in practice a string literal. This is deliberate: error paths
/// do not allocate.
struct Error {
    ErrorCode        code    = ErrorCode::Unknown;
    std::string_view message = {};
};

template <typename T>
using Result = std::expected<T, Error>;

/// A fallible operation returning no value on success.
using Status = std::expected<void, Error>;

[[nodiscard]] inline std::unexpected<Error> Err(ErrorCode code,
                                                std::string_view message = {}) {
    return std::unexpected(Error{code, message});
}

}  // namespace Monarc
