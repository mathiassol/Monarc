#pragma once

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Platform/Library.h>
#include <Monarc/Core/Types.h>

#include <VulkanPlatform.h>
#include <vulkan/vulkan.h>

#include <format>
#include <string_view>
#include <utility>

namespace Monarc::RHI::Detail {

// The entry-point lists, as X-macros. Each list is expanded twice -- once to declare a table
// member and once to resolve it -- so a function appears in exactly one place and the
// declaration and the lookup cannot drift apart. Stringising the name in the same expansion
// is also what lets a failure message name the missing function without formatting anything
// at run time: `"... for " #name` is a string literal, which is precisely what Error::message
// (a non-owning view) needs.
//
// **Three tables, and no device-level list among them.** Global and instance are split by how
// they are resolved -- with a null instance before one exists, and against a created
// VkInstance afterwards. Debug-utils is split off from instance for a different reason: those
// two are the only entry points in the module whose absence is not an error, so keeping them
// apart is what lets "everything in this table was found" stay true of the other two.
//
// A device table would be the fourth, and its absence is deliberate rather than an oversight.
// Device entry points must be resolved with vkGetDeviceProcAddr against a VkDevice, and Task 2
// creates none: the logical device, its queues and command buffers are Task 3's. A fourth
// table declared now would be an empty struct nothing populates and nothing reads. Task 3 adds
// MONARC_VK_DEVICE_FUNCTIONS, a DeviceFunctions table, and vkGetDeviceProcAddr /
// vkCreateDevice / vkDestroyDevice to the instance list below, which is where those three
// belong -- they are instance-dispatched functions with no caller yet.

/// Resolved through vkGetInstanceProcAddr with a null instance, before any instance exists.
#define MONARC_VK_GLOBAL_FUNCTIONS(X)                                                      \
    X(vkEnumerateInstanceVersion)                                                          \
    X(vkEnumerateInstanceLayerProperties)                                                  \
    X(vkEnumerateInstanceExtensionProperties)                                              \
    X(vkCreateInstance)

/// Resolved against a created VkInstance. Every one of these has a caller in Task 2; a
/// function with no caller does not belong in a table whose only guarantee is that everything
/// in it was found.
#define MONARC_VK_INSTANCE_FUNCTIONS(X)                                                    \
    X(vkDestroyInstance)                                                                   \
    X(vkEnumeratePhysicalDevices)                                                          \
    X(vkGetPhysicalDeviceProperties2)                                                      \
    X(vkGetPhysicalDeviceFeatures2)                                                        \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                            \
    X(vkEnumerateDeviceExtensionProperties)

/// VK_EXT_debug_utils. Resolved best-effort and kept in its own table, because these two are
/// the only entry points in the module whose absence is not an error: a machine with no
/// Vulkan SDK has no validation layer and no debug-utils extension, and the plan requires
/// that machine to still run the game.
#define MONARC_VK_DEBUG_UTILS_FUNCTIONS(X)                                                 \
    X(vkCreateDebugUtilsMessengerEXT)                                                      \
    X(vkDestroyDebugUtilsMessengerEXT)

#define MONARC_VK_DECLARE_TABLE_MEMBER(name) PFN_##name name = nullptr;

struct GlobalFunctions {
    MONARC_VK_GLOBAL_FUNCTIONS(MONARC_VK_DECLARE_TABLE_MEMBER)
};

struct InstanceFunctions {
    MONARC_VK_INSTANCE_FUNCTIONS(MONARC_VK_DECLARE_TABLE_MEMBER)
};

struct DebugUtilsFunctions {
    MONARC_VK_DEBUG_UTILS_FUNCTIONS(MONARC_VK_DECLARE_TABLE_MEMBER)
};

#undef MONARC_VK_DECLARE_TABLE_MEMBER

/// The Vulkan runtime library, opened by us, and the entry-point tables resolved from it.
///
/// Monarc opens `vulkan-1.dll` through `Platform::Library` and resolves every entry point
/// through `vkGetInstanceProcAddr`. Nothing in this build links `vulkan-1.lib`, and
/// `VK_NO_PROTOTYPES` makes that a compiler-enforced property rather than a discipline: no
/// global `vkCreateInstance` is declared, so a direct call to one is
/// `error C3861: 'vkCreateInstance': identifier not found` -- verified by writing one.
/// The reason is not purity -- an import library makes a missing Vulkan runtime a Windows
/// loader failure before `main`, with a system dialog naming a DLL, which is useless to a
/// player and unrecoverable by us. Opening it ourselves makes the same situation an ordinary
/// `Result` with a message.
///
/// Two-phase: default-construct, then `Open`. That is not the house factory shape
/// (`Platform::Library::Open` returns a `Result<Library>`) and the reason is `Error::message`,
/// which is a non-owning view over storage that must outlive the error. A message naming the
/// library that could not be opened has to live *somewhere*, and the only somewhere that is
/// neither a hidden global nor a caller-supplied buffer is this object -- which means the
/// object must exist before the call that fails. A returned `Result<Loader>` would carry a
/// message pointing into a Loader that was never returned.
///
/// **A failure message viewing this object stays valid only as long as the object does**, and
/// only until the next failed call on it. That is exactly as long as a caller inspecting the
/// `Status` it just received needs it, and it is why `Open` is called on a Loader the caller
/// already holds.
///
/// Move-only, non-copyable: it owns an OS resource that must be released exactly once.
class Loader {
public:
    Loader()  = default;
    ~Loader() = default;

    Loader(const Loader&)            = delete;
    Loader& operator=(const Loader&) = delete;

    // Defaulted rather than written out: Platform::Library's own move transfers the module
    // handle and nulls the source, the three tables are trivially copyable, and the message
    // buffer is a plain array. There is nothing a hand-written move would do differently.
    // Note the consequence for the paragraph above -- a message obtained from a Loader that
    // has since been moved from views the *source's* buffer, not the destination's, so it
    // stays valid while the source lives and no longer.
    Loader(Loader&&)            = default;
    Loader& operator=(Loader&&) = default;

    /// Opens `libraryName` and resolves every global entry point from it. Fails with the code
    /// `Platform::Library::Open` reported -- `ErrorCode::NotFound` for a name that does not
    /// resolve -- and a message naming the library, or with `ErrorCode::NotFound` and a
    /// message naming the first entry point that was missing.
    ///
    /// The name is a parameter, defaulted to the platform's own, for one reason: it is how
    /// Tests/TestVulkanLoader.cpp exercises the not-found path, by passing a name that cannot
    /// exist. No environment variable and no build-time switch is involved, and shipped code
    /// behaves identically whether or not the test exists.
    ///
    /// A StringView and not a `const char*`, even though the default is one: neither thing
    /// done with the name needs a terminator -- `Platform::Library::Open` takes a StringView
    /// of its own, and the failure message formats one -- and requiring a terminator would
    /// force every caller holding a view to either copy it or assume `data()` happens to be
    /// null-terminated. `Platform::Library::SystemLibraryName()` returns exactly such a view,
    /// and it is what the second loader test passes.
    [[nodiscard]] Status Open(StringView libraryName = VulkanLibraryName());

    /// True once `Open` has succeeded. False after `Close`, and false if `Open` failed --
    /// a failed `Open` leaves nothing open.
    [[nodiscard]] bool IsOpen() const { return m_library.IsOpen(); }

    /// Resolves the instance-level entry points against `instance`. When `debugUtilsEnabled`,
    /// also resolves the debug-utils pair, best-effort: their absence leaves
    /// `HasDebugUtilsFunctions()` false and is not a failure.
    [[nodiscard]] Status LoadInstanceFunctions(VkInstance instance, bool debugUtilsEnabled);

    [[nodiscard]] const GlobalFunctions&     Global() const { return m_global; }
    [[nodiscard]] const InstanceFunctions&   Instance() const { return m_instance; }
    [[nodiscard]] const DebugUtilsFunctions& DebugUtils() const { return m_debugUtils; }

    /// True when both debug-utils entry points resolved. Both or neither: a messenger that
    /// could be created and not destroyed would leak on shutdown.
    [[nodiscard]] bool HasDebugUtilsFunctions() const {
        return m_debugUtils.vkCreateDebugUtilsMessengerEXT != nullptr &&
               m_debugUtils.vkDestroyDebugUtilsMessengerEXT != nullptr;
    }

    /// Clears every table and unloads the library. Safe to call unconditionally and more than
    /// once.
    ///
    /// **The caller must destroy anything created through these entry points first.**
    /// Unloading `vulkan-1.dll` while a `VkInstance` is still alive unmaps the code that
    /// would destroy it. `VulkanBackend::Shutdown` destroys the messenger, then the instance,
    /// then calls this.
    void Close();

private:
    /// Longest failure message this class formats. A stack-sized fixed buffer, so no error
    /// path here allocates -- the same rule Log.h's kMaxLogMessageLength follows, and for the
    /// same reason.
    static constexpr usize kMaxMessageLength = 256;

    /// Formats into `m_message` and returns a failure viewing it. Only for messages that must
    /// name something not known until run time; anything the X-macro can stringise gets a
    /// string literal instead and never comes through here.
    template <typename... Args>
    [[nodiscard]] std::unexpected<Error> Fail(ErrorCode code, std::format_string<Args...> fmt,
                                              Args&&... args) {
        const auto  result  = std::format_to_n(m_message, kMaxMessageLength - 1, fmt,
                                               std::forward<Args>(args)...);
        const usize written = static_cast<usize>(result.out - m_message);
        m_message[written]  = '\0';
        return Err(code, std::string_view(m_message, written));
    }

    Platform::Library         m_library;
    PFN_vkGetInstanceProcAddr m_getInstanceProcAddr = nullptr;
    GlobalFunctions           m_global;
    InstanceFunctions         m_instance;
    DebugUtilsFunctions       m_debugUtils;
    char                      m_message[kMaxMessageLength] = {};
};

}  // namespace Monarc::RHI::Detail
