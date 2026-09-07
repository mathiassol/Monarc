#include <Loader.h>

#include <utility>

namespace Monarc::RHI::Detail {

namespace {

/// Casts a library symbol address to a typed function pointer.
///
/// `reinterpret_cast` from an object pointer to a function pointer is conditionally supported
/// by the standard rather than guaranteed, and every platform Monarc targets supports it --
/// it is how `GetProcAddress` and `dlsym` are used everywhere, and
/// `Platform::Library::Symbol` already performs the other half of the same cast
/// (Windows/Library.cpp). Wrapped in a named function so that cast appears exactly once in
/// this module, at the one place a `void*` is involved. Everything resolved below it comes
/// from `vkGetInstanceProcAddr`, which returns a `PFN_vkVoidFunction`, and function-pointer
/// to function-pointer is ordinary standard C++ needing no such note.
template <typename Function>
[[nodiscard]] Function CastSymbol(void* address) {
    return reinterpret_cast<Function>(address);
}

}  // namespace

Status Loader::Open(StringView libraryName) {
    // Reopening a Loader is not an error, but leaving the previous library's entry points in
    // the tables while the new library failed to open would be: everything is cleared before
    // anything is attempted.
    Close();

    Result<Platform::Library> library = Platform::Library::Open(libraryName);
    if (!library) {
        // The code from Platform::Library is propagated rather than flattened to NotFound.
        // "the file is not there" and "the file is there and would not load" lead to
        // different advice, and only the first is what a machine without Vulkan looks like.
        return Fail(library.error().code,
                    "could not open the Vulkan runtime library \"{}\" ({})", libraryName,
                    ToString(library.error().code));
    }
    m_library = std::move(*library);

    m_getInstanceProcAddr =
        CastSymbol<PFN_vkGetInstanceProcAddr>(m_library.Symbol("vkGetInstanceProcAddr"));
    if (m_getInstanceProcAddr == nullptr) {
        Close();
        return Fail(ErrorCode::NotFound,
                    "\"{}\" opened but does not export vkGetInstanceProcAddr, so it is not a "
                    "Vulkan runtime library",
                    libraryName);
    }

    // Every global entry point is required, and there is no partial success: a table with a
    // null left in it turns a missing function into a crash at the first call site, which is
    // strictly worse than a Result naming it here.
#define MONARC_VK_RESOLVE_GLOBAL(name)                                                      \
    m_global.name = reinterpret_cast<PFN_##name>(                                           \
        m_getInstanceProcAddr(VK_NULL_HANDLE, #name));                                      \
    if (m_global.name == nullptr) {                                                         \
        Close();                                                                            \
        return Err(ErrorCode::NotFound,                                                     \
                   "vkGetInstanceProcAddr returned null for " #name                         \
                   ": this Vulkan loader does not provide an entry point Monarc requires");  \
    }

    MONARC_VK_GLOBAL_FUNCTIONS(MONARC_VK_RESOLVE_GLOBAL)
#undef MONARC_VK_RESOLVE_GLOBAL

    return {};
}

Status Loader::LoadInstanceFunctions(VkInstance instance, bool debugUtilsEnabled) {
    if (!IsOpen()) {
        return Err(ErrorCode::InvalidArgument,
                   "Loader::LoadInstanceFunctions called on a loader that is not open");
    }
    if (instance == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "Loader::LoadInstanceFunctions called with a null VkInstance");
    }

#define MONARC_VK_RESOLVE_INSTANCE(name)                                                    \
    m_instance.name = reinterpret_cast<PFN_##name>(m_getInstanceProcAddr(instance, #name)); \
    if (m_instance.name == nullptr) {                                                       \
        return Err(ErrorCode::NotFound,                                                     \
                   "vkGetInstanceProcAddr returned null for " #name                         \
                   " on a created instance: this Vulkan implementation does not provide an " \
                   "entry point Monarc requires");                                          \
    }

    MONARC_VK_INSTANCE_FUNCTIONS(MONARC_VK_RESOLVE_INSTANCE)
#undef MONARC_VK_RESOLVE_INSTANCE

    // Best-effort, and the only table in this file where a null is acceptable. A machine with
    // no Vulkan SDK has no VK_EXT_debug_utils, and the plan is explicit that such a machine
    // still runs the game -- so these are resolved without being required, and callers ask
    // HasDebugUtilsFunctions() rather than assuming.
#define MONARC_VK_RESOLVE_DEBUG_UTILS(name)                                                 \
    m_debugUtils.name = reinterpret_cast<PFN_##name>(m_getInstanceProcAddr(instance, #name));

    if (debugUtilsEnabled) {
        MONARC_VK_DEBUG_UTILS_FUNCTIONS(MONARC_VK_RESOLVE_DEBUG_UTILS)
    }
#undef MONARC_VK_RESOLVE_DEBUG_UTILS

    return {};
}

void Loader::Close() {
    // Tables first, so a caller holding a stale reference to one sees nulls rather than
    // pointers into an unmapped module. Nothing in Monarc holds such a reference across a
    // Close; clearing them costs nothing and removes the question.
    m_debugUtils          = DebugUtilsFunctions{};
    m_instance            = InstanceFunctions{};
    m_global              = GlobalFunctions{};
    m_getInstanceProcAddr = nullptr;
    m_library.Close();
}

}  // namespace Monarc::RHI::Detail
