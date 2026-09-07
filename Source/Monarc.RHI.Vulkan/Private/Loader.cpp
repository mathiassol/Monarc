#include <Loader.h>

#include <Monarc/Core/Log.h>

#include <utility>

namespace Monarc::RHI::Detail {

namespace {

/// The loader's own reporting -- not the validation layer's, which has its own category in
/// Private/VulkanBackend.cpp.
///
/// This is where the detail a failure message no longer carries lives. `Open` returns string
/// literals, because a factory that hands back no object on failure can only return a message
/// with static storage; the library name and the code behind it go here instead.
///
/// Warning and not Error, deliberately. A missing Vulkan runtime is the expected outcome on a
/// machine without one -- `FirstLight --adapters` treats it as a finding and prints it, and the
/// device tests treat it as a skip -- so this file is not the place that decides it is a fault.
/// The caller does that.
MONARC_LOG_CATEGORY(VulkanLoader, Info);

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

Loader::Loader(Loader&& other) noexcept
    : m_library(std::move(other.m_library)),
      m_getInstanceProcAddr(other.m_getInstanceProcAddr),
      m_global(other.m_global),
      m_instance(other.m_instance),
      m_debugUtils(other.m_debugUtils) {
    // Platform::Library's move has already nulled other.m_library; this is the other half,
    // and it is the whole reason this constructor is written out rather than defaulted.
    other.ClearTables();
}

Loader& Loader::operator=(Loader&& other) noexcept {
    if (this != &other) {
        // Close rather than an inline release: the destination owns a module of its own, and
        // Close is where the "tables first, library second" order is stated. Guarding on
        // `this != &other` is what keeps `x = std::move(x)` from closing x and then adopting
        // the nulls Close just wrote -- the same guard, for the same reason, as
        // Platform::Library::operator=.
        Close();
        m_library             = std::move(other.m_library);
        m_getInstanceProcAddr = other.m_getInstanceProcAddr;
        m_global              = other.m_global;
        m_instance            = other.m_instance;
        m_debugUtils          = other.m_debugUtils;
        other.ClearTables();
    }
    return *this;
}

Result<Loader> Loader::Open(StringView libraryName) {
    Result<Platform::Library> library = Platform::Library::Open(libraryName);
    if (!library) {
        MONARC_LOG(VulkanLoader, Warning,
                   "could not open the Vulkan runtime library \"{}\": {}", libraryName,
                   ToString(library.error().code));
        // The code from Platform::Library is propagated rather than flattened to NotFound.
        // "the file is not there" and "the file is there and would not load" lead to
        // different advice, and only the first is what a machine without Vulkan looks like.
        return Err(library.error().code, "could not open the Vulkan runtime library");
    }

    // Built here and returned only once everything below it succeeded. Nothing partially
    // resolved leaves this function, so there is no half-open Loader for a caller to hold and
    // no state for a test to have to check for.
    Loader loader;
    loader.m_library = std::move(*library);

    loader.m_getInstanceProcAddr = CastSymbol<PFN_vkGetInstanceProcAddr>(
        loader.m_library.Symbol("vkGetInstanceProcAddr"));
    if (loader.m_getInstanceProcAddr == nullptr) {
        MONARC_LOG(VulkanLoader, Warning,
                   "\"{}\" opened but does not export vkGetInstanceProcAddr, so it is not a "
                   "Vulkan runtime library",
                   libraryName);
        return Err(ErrorCode::NotFound,
                   "the library opened but does not export vkGetInstanceProcAddr, so it is "
                   "not a Vulkan runtime library");
    }

    // Every global entry point is required, and there is no partial success: a table with a
    // null left in it turns a missing function into a crash at the first call site, which is
    // strictly worse than a Result naming it here.
#define MONARC_VK_RESOLVE_GLOBAL(name)                                                      \
    loader.m_global.name = reinterpret_cast<PFN_##name>(                                    \
        loader.m_getInstanceProcAddr(VK_NULL_HANDLE, #name));                               \
    if (loader.m_global.name == nullptr) {                                                   \
        MONARC_LOG(VulkanLoader, Warning,                                                   \
                   "\"{}\" opened but vkGetInstanceProcAddr returned null for " #name,      \
                   libraryName);                                                            \
        return Err(ErrorCode::NotFound,                                                     \
                   "vkGetInstanceProcAddr returned null for " #name                         \
                   ": this Vulkan loader does not provide an entry point Monarc requires");  \
    }

    MONARC_VK_GLOBAL_FUNCTIONS(MONARC_VK_RESOLVE_GLOBAL)
#undef MONARC_VK_RESOLVE_GLOBAL

    return loader;
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
        MONARC_LOG(VulkanLoader, Warning,                                                   \
                   "vkGetInstanceProcAddr returned null for " #name " on a created "        \
                   "instance");                                                             \
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

void Loader::ClearTables() {
    m_debugUtils          = DebugUtilsFunctions{};
    m_instance            = InstanceFunctions{};
    m_global              = GlobalFunctions{};
    m_getInstanceProcAddr = nullptr;
}

void Loader::Close() {
    // Tables first, so a caller holding a stale reference to one sees nulls rather than
    // pointers into an unmapped module. Nothing in Monarc holds such a reference across a
    // Close; clearing them costs nothing and removes the question.
    ClearTables();
    m_library.Close();
}

}  // namespace Monarc::RHI::Detail
