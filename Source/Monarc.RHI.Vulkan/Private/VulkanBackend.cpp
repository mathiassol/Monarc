#include <Monarc/RHI/Vulkan/VulkanBackend.h>

namespace Monarc::RHI {

Status CreateVulkanBackend() {
    // Unsupported, not Unknown or IoFailure: nothing went wrong, this backend simply does
    // not exist yet. Error::message is a non-owning view over storage that must outlive the
    // error, which a string literal does (Monarc/Core/Error.h).
    return Err(ErrorCode::Unsupported,
               "Monarc.RHI.Vulkan has no backend yet: the loader, instance and adapter "
               "enumeration arrive in Task 2 of Phase A3");
}

}  // namespace Monarc::RHI
