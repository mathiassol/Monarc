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

// Resolves one entry point into `table`, and for a Required one reports and returns if the
// lookup came back null.
//
// **One body, four call sites.** Task 2 shipped two near-identical copies of this and said
// plainly that a third -- the device table -- was the point at which parameterising paid.
// This is that. The copies differed in exactly the things that are parameters here: which
// table to write, how to look a name up, what to call the resolver in a message, where the
// lookup happened, what to put in the log line beside it, and what to clear on the way out.
//
// `requirement` is a `Requirement` enumerator's *name*, so `Requirement::requirement` names
// the value and `if constexpr` folds the whole guard away for an Optional entry. Not a plain
// `if`: MSVC's C4127 ("conditional expression is constant") is on at /W4 and /WX makes it
// fatal, so a runtime comparison of two constants would not build.
//
// The message stays a string literal, assembled entirely from `#name` and the two literal
// parameters, because `Error::message` is a non-owning view -- see Loader::Open. `context` is
// the one thing that may be a runtime value, and it goes only to the log.
#define MONARC_VK_RESOLVE_ENTRY(table, resolve, name, requirement, resolverName, where,     \
                                context, onFailure)                                         \
    (table).name = reinterpret_cast<PFN_##name>(resolve(#name));                            \
    if constexpr (Requirement::requirement == Requirement::Required) {                      \
        if ((table).name == nullptr) {                                                      \
            MONARC_LOG(VulkanLoader, Warning,                                               \
                       resolverName " returned null for " #name " " where " | {}", context);\
            onFailure;                                                                      \
            return Err(ErrorCode::NotFound,                                                 \
                       resolverName " returned null for " #name " " where                   \
                       ": this Vulkan implementation does not provide an entry point "      \
                       "Monarc requires");                                                  \
        }                                                                                   \
    }

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

    // Every global entry point is Required, and there is no partial success: a table with a
    // null left in it turns a missing function into a crash at the first call site, which is
    // strictly worse than a Result naming it here. Nothing to clear on the way out, because
    // the Loader being filled is a local that is only returned once every name resolved.
    const auto resolveGlobal = [&loader](const char* name) {
        return loader.m_getInstanceProcAddr(VK_NULL_HANDLE, name);
    };
#define MONARC_VK_RESOLVE_GLOBAL(name, requirement)                                        \
    MONARC_VK_RESOLVE_ENTRY(loader.m_global, resolveGlobal, name, requirement,             \
                            "vkGetInstanceProcAddr", "before any instance exists",         \
                            libraryName, (void)0)

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

    // `m_instance = InstanceFunctions{}` before the early return, so this function keeps the
    // promise Open's comment above makes: nothing partially resolved survives it. Without it a
    // failure part-way through the list leaves a table half full of live pointers on an object
    // whose IsOpen() is still true -- and vkDestroyInstance is *first* in the list, so the
    // half-populated table the caller is most likely to reach is the one missing the entry
    // point its own teardown needs. Measured: with vkDestroyInstance forced null on a machine
    // that has Vulkan, `FirstLight --adapters` exited 0xC0000005, a call through a null
    // function pointer from VulkanBackend::State::Shutdown. Fixed on both sides -- the call
    // site is guarded too -- because the two files are maintained independently.
    const auto resolveInstance = [this, instance](const char* name) {
        return m_getInstanceProcAddr(instance, name);
    };
#define MONARC_VK_RESOLVE_INSTANCE(name, requirement)                                      \
    MONARC_VK_RESOLVE_ENTRY(m_instance, resolveInstance, name, requirement,                \
                            "vkGetInstanceProcAddr", "on a created instance", "instance",  \
                            m_instance = InstanceFunctions{})

    MONARC_VK_INSTANCE_FUNCTIONS(MONARC_VK_RESOLVE_INSTANCE)
#undef MONARC_VK_RESOLVE_INSTANCE

    // The two Optional entries in the module. A machine with no Vulkan SDK has no
    // VK_EXT_debug_utils, and the plan is explicit that such a machine still runs the game --
    // so a null here is not a failure, and callers ask HasDebugUtilsFunctions() rather than
    // assuming. The same macro handles it: `Optional` folds the guard away entirely, so this
    // expansion is the assignment and nothing else.
#define MONARC_VK_RESOLVE_DEBUG_UTILS(name, requirement)                                   \
    MONARC_VK_RESOLVE_ENTRY(m_debugUtils, resolveInstance, name, requirement,              \
                            "vkGetInstanceProcAddr", "on a created instance", "instance",  \
                            (void)0)

    if (debugUtilsEnabled) {
        MONARC_VK_DEBUG_UTILS_FUNCTIONS(MONARC_VK_RESOLVE_DEBUG_UTILS)
    }
#undef MONARC_VK_RESOLVE_DEBUG_UTILS

    return {};
}

Status Loader::LoadDeviceFunctions(VkDevice device, DeviceFunctions& out) const {
    out = DeviceFunctions{};

    if (m_instance.vkGetDeviceProcAddr == nullptr) {
        // The instance table is resolved before any device is created, so reaching this means
        // a caller asked for a device table without one -- InvalidArgument rather than
        // NotFound, because the entry point is not missing from the implementation, it was
        // never looked up.
        return Err(ErrorCode::InvalidArgument,
                   "Loader::LoadDeviceFunctions called before the instance entry points were "
                   "resolved");
    }
    if (device == VK_NULL_HANDLE) {
        return Err(ErrorCode::InvalidArgument,
                   "Loader::LoadDeviceFunctions called with a null VkDevice");
    }

    const auto resolveDevice = [this, device](const char* name) {
        return m_instance.vkGetDeviceProcAddr(device, name);
    };
#define MONARC_VK_RESOLVE_DEVICE(name, requirement)                                        \
    MONARC_VK_RESOLVE_ENTRY(out, resolveDevice, name, requirement, "vkGetDeviceProcAddr",  \
                            "on a created device", "device", out = DeviceFunctions{})

    MONARC_VK_DEVICE_FUNCTIONS(MONARC_VK_RESOLVE_DEVICE)
#undef MONARC_VK_RESOLVE_DEVICE

    return {};
}

PFN_vkDestroyDevice Loader::ResolveDeviceDestroyer(VkDevice device) const {
    if (m_instance.vkGetDeviceProcAddr == nullptr || device == VK_NULL_HANDLE) {
        return nullptr;
    }
    // A function pointer to a function pointer, which is ordinary standard C++ -- the
    // `CastSymbol` note above is about the one place a `void*` is involved, and this is not it.
    return reinterpret_cast<PFN_vkDestroyDevice>(
        m_instance.vkGetDeviceProcAddr(device, "vkDestroyDevice"));
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
