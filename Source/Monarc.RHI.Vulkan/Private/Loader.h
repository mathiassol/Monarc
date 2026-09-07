#pragma once

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Platform/Library.h>
#include <Monarc/Core/Types.h>

#include <VulkanPlatform.h>
#include <vulkan/vulkan.h>

// No <string_view> and no <utility>. Nothing here is a std::string_view -- `StringView` comes
// from Monarc/Core/Containers/String.h above -- and nothing here moves, exchanges or forwards;
// the only mention of std::move in this file is inside a comment, and Loader.cpp includes
// <utility> for its own use.

namespace Monarc::RHI::Detail {

/// Whether a null from a proc-address lookup is a failure.
///
/// **Per entry, not per table, and Task 3 is where that stopped being a distinction without a
/// difference.** Until now "optional" meant "its own table": `VK_EXT_debug_utils` had one, and
/// with a single optional extension that worked. It does not survive what comes next. Mesh
/// shading, ray tracing and the swapchain all bring device entry points whose availability
/// varies *per adapter* -- so on a machine like this one, the same `DeviceFunctions` table is
/// fully populated for the RTX 3070 Ti and partly populated for the Intel UHD 730, and a table
/// whose whole identity was "everything in me was found" cannot describe that. Optionality
/// belongs to the entry point.
///
/// So it is declared beside the name, in the lists below, and the resolver reads it. Task 3's
/// device table is entirely Required and the only Optional entries are the two debug-utils
/// ones that were already optional -- no machinery is added for extensions that do not exist
/// yet. What is added is the *shape* that will hold when they do: a new Optional entry is one
/// word in one list.
enum class Requirement : u8 { Required, Optional };

// The entry-point lists, as X-macros. Each list is expanded twice -- once to declare a table
// member and once to resolve it -- so a function appears in exactly one place and the
// declaration and the lookup cannot drift apart. Stringising the name in the same expansion
// is also what lets a failure message name the missing function without formatting anything
// at run time: `"... for " #name` is a string literal, which is precisely what Error::message
// (a non-owning view) needs.
//
// Each entry is `X(name, requirement)`, with `requirement` naming a `Requirement` enumerator
// above.
//
// **Four tables, split by how and when they are resolved -- no longer by whether a null is
// fatal.** Global is resolved with a null instance before one exists; instance against a
// created VkInstance; debug-utils only when the extension was actually enabled; and device
// against a VkDevice through vkGetDeviceProcAddr, which returns the driver's own function
// rather than the loader's dispatch trampoline. Those are four genuinely different moments,
// and that is now the entire reason there are four lists.
//
// vkGetDeviceProcAddr and vkCreateDevice are in the *instance* list, because that is what they
// are: instance-dispatched commands taking a VkPhysicalDevice. vkDestroyDevice is in the
// device list, though it can be resolved either way -- resolving it against the device it
// destroys is what keeps every call on a live VkDevice off the trampoline.

/// Resolved through vkGetInstanceProcAddr with a null instance, before any instance exists.
#define MONARC_VK_GLOBAL_FUNCTIONS(X)                                                      \
    X(vkEnumerateInstanceVersion, Required)                                                \
    X(vkEnumerateInstanceLayerProperties, Required)                                        \
    X(vkEnumerateInstanceExtensionProperties, Required)                                    \
    X(vkCreateInstance, Required)

/// Resolved against a created VkInstance. Every one of these has a caller; a function with no
/// caller does not belong in a table at all.
#define MONARC_VK_INSTANCE_FUNCTIONS(X)                                                    \
    X(vkDestroyInstance, Required)                                                         \
    X(vkEnumeratePhysicalDevices, Required)                                                \
    X(vkGetPhysicalDeviceProperties2, Required)                                            \
    X(vkGetPhysicalDeviceFeatures2, Required)                                              \
    X(vkGetPhysicalDeviceQueueFamilyProperties, Required)                                  \
    X(vkGetPhysicalDeviceMemoryProperties, Required)                                       \
    X(vkEnumerateDeviceExtensionProperties, Required)                                      \
    X(vkCreateDevice, Required)                                                            \
    X(vkGetDeviceProcAddr, Required)

/// VK_EXT_debug_utils. Resolved only when the extension was enabled, and Optional even then:
/// a machine with no Vulkan SDK has no validation layer and no debug-utils extension, and the
/// plan requires that machine to still run the game.
#define MONARC_VK_DEBUG_UTILS_FUNCTIONS(X)                                                 \
    X(vkCreateDebugUtilsMessengerEXT, Optional)                                            \
    X(vkDestroyDebugUtilsMessengerEXT, Optional)

/// Resolved against a created VkDevice through vkGetDeviceProcAddr. All Vulkan 1.3 core --
/// dynamic rendering, synchronization2, timeline semaphores and copy_commands2 are all
/// promoted, so Monarc enables no device extension at all and every name here is Required.
#define MONARC_VK_DEVICE_FUNCTIONS(X)                                                      \
    X(vkDestroyDevice, Required)                                                           \
    X(vkGetDeviceQueue, Required)                                                          \
    X(vkDeviceWaitIdle, Required)                                                          \
    X(vkCreateSemaphore, Required)                                                         \
    X(vkDestroySemaphore, Required)                                                        \
    X(vkGetSemaphoreCounterValue, Required)                                                \
    X(vkWaitSemaphores, Required)                                                          \
    X(vkQueueSubmit2, Required)                                                            \
    X(vkCreateCommandPool, Required)                                                       \
    X(vkDestroyCommandPool, Required)                                                      \
    X(vkResetCommandPool, Required)                                                        \
    X(vkAllocateCommandBuffers, Required)                                                  \
    X(vkBeginCommandBuffer, Required)                                                      \
    X(vkEndCommandBuffer, Required)                                                        \
    X(vkCmdPipelineBarrier2, Required)                                                     \
    X(vkCmdBeginRendering, Required)                                                       \
    X(vkCmdEndRendering, Required)                                                         \
    X(vkCmdCopyImageToBuffer2, Required)                                                   \
    X(vkCreateImage, Required)                                                             \
    X(vkDestroyImage, Required)                                                            \
    X(vkCreateImageView, Required)                                                         \
    X(vkDestroyImageView, Required)                                                        \
    X(vkGetImageMemoryRequirements, Required)                                              \
    X(vkBindImageMemory, Required)                                                         \
    X(vkCreateBuffer, Required)                                                            \
    X(vkDestroyBuffer, Required)                                                           \
    X(vkGetBufferMemoryRequirements, Required)                                             \
    X(vkBindBufferMemory, Required)                                                        \
    X(vkAllocateMemory, Required)                                                          \
    X(vkFreeMemory, Required)                                                              \
    X(vkMapMemory, Required)                                                               \
    X(vkUnmapMemory, Required)

#define MONARC_VK_DECLARE_TABLE_MEMBER(name, requirement) PFN_##name name = nullptr;

struct GlobalFunctions {
    MONARC_VK_GLOBAL_FUNCTIONS(MONARC_VK_DECLARE_TABLE_MEMBER)
};

struct InstanceFunctions {
    MONARC_VK_INSTANCE_FUNCTIONS(MONARC_VK_DECLARE_TABLE_MEMBER)
};

struct DebugUtilsFunctions {
    MONARC_VK_DEBUG_UTILS_FUNCTIONS(MONARC_VK_DECLARE_TABLE_MEMBER)
};

/// One logical device's entry points.
///
/// **Not a member of `Loader`, unlike the other three, and the reason is arithmetic.** A
/// process has one Vulkan runtime and one instance, so one table each is right. It can have as
/// many logical devices as it has adapters -- two on this machine, and Task 3's device tests
/// create one on each in turn -- and a device-dispatched function pointer is only valid for the
/// device it was resolved against. A table on the Loader would hold whichever device was
/// created last. So each device owns its own, and `Loader::LoadDeviceFunctions` fills one the
/// caller supplies. (`Private/Loader.h`'s Task 2 comment anticipated a fourth table *on the
/// Loader*; that part of it was wrong and this is the correction.)
struct DeviceFunctions {
    MONARC_VK_DEVICE_FUNCTIONS(MONARC_VK_DECLARE_TABLE_MEMBER)
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
/// Default-constructible and closed, with a static `Open` returning a `Result<Loader>`, a
/// `Close` safe to call unconditionally, and an `IsOpen` query: the shape
/// `Platform::Library` uses, one rung up, and for the same reason -- it owns an OS resource
/// that must be released exactly once. Move-only, non-copyable.
///
/// **A moved-from Loader is a closed one -- every table null, `IsOpen()` false -- and the
/// hand-written move below is what makes that true.** `Platform::Library`'s move nulls the
/// source's module handle on its own, but the three tables are trivially copyable, so a
/// *defaulted* move would copy them: the source would be left holding the same entry-point
/// pointers the destination now uses, into a module it no longer owns. Once the destination
/// is destroyed, a call through one of those is a use-after-unload, and nothing mechanical
/// in this codebase catches that shape of bug: an equivalent dangling read in this module --
/// an `Error::message` re-pointed into freed `VulkanBackend::State` -- was run under
/// `clang-asan` and ASan reported nothing at all, because the freed storage still held its
/// bytes; only a test's exact-text comparison caught it. Docs/Status.md records that run. So
/// the moves clear the source rather than leaving the hazard documented, which is the
/// difference between a state that must not be used and one that cannot be. `Close()` on a
/// moved-from Loader is a genuine no-op as a result: there is nothing left to clear and
/// nothing left to unload.
class Loader {
public:
    Loader()  = default;
    ~Loader() = default;

    Loader(const Loader&)            = delete;
    Loader& operator=(const Loader&) = delete;

    // Written out rather than defaulted, for the one reason the class comment gives: a
    // defaulted move copies the tables, and the source's copy outlives its claim on the
    // module. Shaped exactly like Platform::Library's own pair -- noexcept, and move
    // assignment guarded on `this != &other`, without which `x = std::move(x)` would Close()
    // x and then adopt the nulls Close() had just written.
    Loader(Loader&& other) noexcept;
    Loader& operator=(Loader&& other) noexcept;

    /// Opens `libraryName` and resolves every global entry point from it, returning a Loader
    /// only if both succeeded. Fails with the code `Platform::Library::Open` reported --
    /// `ErrorCode::NotFound` for a name that does not resolve -- or with `ErrorCode::NotFound`
    /// and a message naming the entry point that was missing.
    ///
    /// Every message is a string literal, which is what `Error::message` -- a non-owning view
    /// -- requires of a factory that returns no object on failure. The library name is not in
    /// any of them: it is caller-supplied, so the caller already has it, and
    /// `Platform::Library::Open` takes the same position with its own bare
    /// `"could not load library"`. What names the library is the log line at the failure site.
    ///
    /// The name is a parameter, defaulted to the platform's own, for one reason: it is how
    /// Tests/TestVulkanLoader.cpp exercises the not-found path, by passing a name that cannot
    /// exist. No environment variable and no build-time switch is involved, and shipped code
    /// behaves identically whether or not the test exists.
    ///
    /// A StringView and not a `const char*`, even though the default is one: nothing done with
    /// the name needs a terminator -- `Platform::Library::Open` takes a StringView of its own
    /// -- and requiring one would force every caller holding a view to either copy it or
    /// assume `data()` happens to be null-terminated.
    /// `Platform::Library::SystemLibraryName()` returns exactly such a view, and it is what
    /// the second loader test passes.
    [[nodiscard]] static Result<Loader> Open(StringView libraryName = VulkanLibraryName());

    /// True while a library is open. False on a default-constructed Loader, false after
    /// `Close`, and false on one that has been moved from.
    [[nodiscard]] bool IsOpen() const { return m_library.IsOpen(); }

    /// Resolves the instance-level entry points against `instance`. When `debugUtilsEnabled`,
    /// also resolves the debug-utils pair, whose entries are Optional: their absence leaves
    /// `HasDebugUtilsFunctions()` false and is not a failure.
    [[nodiscard]] Status LoadInstanceFunctions(VkInstance instance, bool debugUtilsEnabled);

    /// Resolves `device`'s entry points into `out`, through `vkGetDeviceProcAddr`.
    ///
    /// `out` is filled rather than stored, for the reason `DeviceFunctions` gives: a table is
    /// valid only for the device it was resolved against, and a process can have several
    /// devices at once. `out` is cleared first, and cleared again if any Required entry is
    /// missing -- so a partially resolved table never leaves this function, exactly as
    /// `LoadInstanceFunctions` promises of its own.
    ///
    /// `const`, because it writes nothing on the Loader: the only thing it reads is
    /// `vkGetDeviceProcAddr` out of the instance table.
    [[nodiscard]] Status LoadDeviceFunctions(VkDevice device, DeviceFunctions& out) const;

    /// Resolves `device`'s `vkDestroyDevice`, and nothing else. Null if the instance entry
    /// points have not been resolved, if `device` is null, or if the lookup itself came back
    /// empty.
    ///
    /// **This exists for one caller and one situation: a `LoadDeviceFunctions` that failed.**
    /// That function clears the whole table when a Required entry is missing, which is the
    /// promise it makes -- a partially resolved table never leaves it -- and the cost is that
    /// the freshly created `VkDevice` would have nothing left able to destroy it. Resolving
    /// that one entry separately is what keeps the factory's failure path from leaking a
    /// Vulkan object, and it does not weaken the promise: the caller asks for the destroyer by
    /// name rather than being handed a table that is partly filled in.
    ///
    /// Through `vkGetDeviceProcAddr`, the same resolver the device table uses, which is
    /// available because it is an *instance* entry point and was resolved before any device
    /// existed. See the note above the entry-point lists on `vkDestroyDevice` being resolvable
    /// either way.
    [[nodiscard]] PFN_vkDestroyDevice ResolveDeviceDestroyer(VkDevice device) const;

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
    /// Nulls `m_getInstanceProcAddr` and every table entry, without touching the library.
    /// The half of `Close` the moves need, so that "what a closed Loader's tables look like"
    /// is written once and the moved-from state cannot drift away from the closed one.
    void ClearTables();

    Platform::Library         m_library;
    PFN_vkGetInstanceProcAddr m_getInstanceProcAddr = nullptr;
    GlobalFunctions           m_global;
    InstanceFunctions         m_instance;
    DebugUtilsFunctions       m_debugUtils;
};

}  // namespace Monarc::RHI::Detail
