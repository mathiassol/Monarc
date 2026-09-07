// The Vulkan loader, the instance, the debug messenger, and adapter enumeration.
//
// **Validation errors stop the process, and that is the point.** A validation layer whose
// output scrolls past is worse than no validation, because it produces false confidence. The
// messenger below logs every severity it is given and, for an ERROR-severity message of
// VALIDATION type, follows the house pattern -- MONARC_CHECK with the message, then
// MONARC_DEBUG_BREAK() and std::abort() -- exactly as JobSystem::Wait's worker guard does and
// for the same reason: MONARC_CHECK reports and optionally breaks, and never alters control
// flow, so a check alone would print and carry on under any handler that declines to break.
//
// The VALIDATION-type qualifier is deliberate and is not a loophole. The validation layer
// marks its own findings VALIDATION; the *loader* reports its own problems as GENERAL, and one
// of those -- an implicit overlay layer built against an older API version, of which the
// development machine has several -- is a fact about the machine rather than a bug in Monarc.
// Those are logged at Error and stop nothing. A validation finding is a bug in Monarc's own
// use of Vulkan, every time.
//
// **The narrowing is accepted rather than free, and it has one way to bite.** A finding the
// layer chose to type GENERAL rather than VALIDATION would be logged at Error and would not
// stop the process -- the same outcome as the overlay-layer diagnostics, and indistinguishable
// from them here. Khronos types its findings VALIDATION, so this is a small risk against a
// certain cost: keying on severity alone would abort on every one of the several GENERAL
// errors this machine emits per process start, which is a fatal messenger that cannot be run
// at all. Recorded so the trade is stated rather than assumed; nothing here tests it, because
// producing a GENERAL-typed validation finding is the layer's choice and not ours to provoke.
//
// Release builds create no messenger and load no layers.
//
// Almost everything here is a member of VulkanBackend::State rather than of VulkanBackend
// itself, for one reason: these functions take VkPhysicalDevice, VkExtensionProperties and
// VkLayerProperties, and Monarc.RHI.Vulkan's public header may name no Vulkan type
// (Docs/Rendering/RHI.md; ADR-0014 rule 1). State is defined in this file, so its members can
// take whatever they need. The public class is the thin, Vulkan-free shell in front of them.

#include <Monarc/RHI/Vulkan/VulkanBackend.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Log.h>

#include <Loader.h>
#include <Translate.h>
#include <VulkanPlatform.h>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace Monarc::RHI {

namespace {

MONARC_LOG_CATEGORY(Vulkan, Info);

/// Validation and loader output. A separate category from Vulkan above so the engine's own
/// reporting and the layer's can be turned up or down independently.
///
/// Warning, not Info, and that threshold was chosen by measurement rather than taste. The
/// Vulkan *loader* reports its own progress through this same messenger, and on the
/// development machine a single vkCreateInstance produces roughly 180 INFO-severity messages
/// -- every layer manifest it located, every registry key it read, the full layer callstack --
/// which buries the handful of lines anyone actually ran the program to read. Warning keeps
/// the messages that mean something (three implicit overlay layers here are built against
/// Vulkan 1.2 and the loader says so) and drops the travelogue. Lowering this to Info or Trace
/// recovers all of it, which is the point of the threshold living here.
MONARC_LOG_CATEGORY(VulkanValidation, Warning);

/// Monarc's own version, reported to the driver and to tools. A label rather than a contract:
/// nothing reads it back.
constexpr u32 kEngineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);

/// The API version the instance is created at, and the floor for every device.
///
/// 1.3 and not 1.4, measured rather than assumed: the Intel UHD 730 on the development
/// machine reports 1.3.275, so requiring 1.4 would silently drop the lower-tier device out of
/// the test matrix -- which is the entire reason that device is valuable. 1.4 features are
/// opportunistic and always optional. See Docs/Status.md.
constexpr u32 kRequiredApiVersion = VK_API_VERSION_1_3;

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

// Monarc.RHI's public types are sized from the Vulkan spec's own limits. Asserted rather than
// trusted: Monarc/RHI/Adapter.h says so in a comment, and this is the only translation unit
// that can check it.
static_assert(AdapterUuid::kSize == VK_UUID_SIZE,
              "AdapterUuid must be exactly VkPhysicalDeviceIDProperties::deviceUUID's size");
static_assert(kMaxAdapterNameLength >= VK_MAX_PHYSICAL_DEVICE_NAME_SIZE,
              "AdapterInfo::name must hold a full VkPhysicalDeviceProperties::deviceName");

[[nodiscard]] LogLevel SeverityToLogLevel(VkDebugUtilsMessageSeverityFlagBitsEXT severity) {
    // Not a `default`-less switch: VkDebugUtilsMessageSeverityFlagBitsEXT is a *flag bits*
    // enum whose enumerators are individual bits plus a MAX_ENUM sentinel. Ordered tests read
    // better than four cases, and there is no exhaustiveness to guarantee -- a severity Vulkan
    // adds later is a new bit, which no switch would have covered either.
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        return LogLevel::Error;
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        return LogLevel::Warning;
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
        return LogLevel::Info;
    }
    return LogLevel::Trace;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             types,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/) {
    // pMessage is documented as null-terminated but pMessageIdName is optional, and a
    // messenger that crashed while reporting a validation error would be the least useful
    // failure mode available.
    const char* messageIdName = data != nullptr && data->pMessageIdName != nullptr
                                    ? data->pMessageIdName
                                    : "<no message id>";
    const char* message =
        data != nullptr && data->pMessage != nullptr ? data->pMessage : "<no message>";

    // MONARC_LOG takes its level as a token, so that arguments cost nothing when the level is
    // filtered out. The level therefore cannot be passed as a value, and one call per level is
    // the price. The switch is `default`-less so a level added to LogLevel is a compile error
    // here rather than a message that silently stops being logged.
    switch (SeverityToLogLevel(severity)) {
        case LogLevel::Error:
            MONARC_LOG(VulkanValidation, Error, "{} | {}", messageIdName, message);
            break;
        case LogLevel::Warning:
            MONARC_LOG(VulkanValidation, Warning, "{} | {}", messageIdName, message);
            break;
        case LogLevel::Info:
            MONARC_LOG(VulkanValidation, Info, "{} | {}", messageIdName, message);
            break;
        case LogLevel::Trace:
        case LogLevel::Debug:
        case LogLevel::Fatal:
            MONARC_LOG(VulkanValidation, Trace, "{} | {}", messageIdName, message);
            break;
    }

    const bool isValidationError =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 &&
        (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
    if (isValidationError) {
        MONARC_CHECK(false, message);
        // Unconditional, matching JobSystem::Wait's worker guard and Array<T>'s allocation
        // failure. MONARC_CHECK reports and optionally breaks; it never alters control flow,
        // so under a handler that declines to break -- a test harness, or Shipping --
        // execution would fall straight back into Vulkan and carry on with a command stream
        // already known to be invalid. That is the false confidence this messenger exists to
        // prevent.
        MONARC_DEBUG_BREAK();
        std::abort();
    }

    // VK_FALSE always, as the spec requires of an application's messenger: VK_TRUE is
    // reserved for the layers' own use.
    return VK_FALSE;
}

/// Fills `array` with `count` value-initialised elements, replacing whatever it held.
///
/// Array<T> has Reserve, Emplace and Pop but no Resize, and the count-then-fill idiom every
/// Vulkan enumeration uses needs one. Written here rather than added to Array because this is
/// its only caller in the tree; it moves into Array if a second one appears.
template <typename T>
void ResizeTo(Array<T>& array, usize count) {
    array.Clear();
    array.Reserve(count);
    for (usize i = 0; i < count; ++i) {
        array.Emplace();
    }
}

/// Shrinks `array` to `count` elements. The pair of it and ResizeTo is what stands in for
/// Array<T>::Resize on the "Vulkan wrote fewer than it said" path.
template <typename T>
void ShrinkTo(Array<T>& array, usize count) {
    while (array.Size() > count) {
        array.Pop();
    }
}

[[nodiscard]] bool ContainsExtension(const Array<VkExtensionProperties>& extensions,
                                     const char*                        name) {
    const std::string_view wanted(name);
    for (const VkExtensionProperties& extension : extensions) {
        if (std::string_view(extension.extensionName) == wanted) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ContainsLayer(const Array<VkLayerProperties>& layers, const char* name) {
    const std::string_view wanted(name);
    for (const VkLayerProperties& layer : layers) {
        if (std::string_view(layer.layerName) == wanted) {
            return true;
        }
    }
    return false;
}

void CopyName(char (&destination)[kMaxAdapterNameLength], const char* source) {
    const std::string_view text(source != nullptr ? source : "");
    const usize            length =
        text.size() < kMaxAdapterNameLength - 1 ? text.size() : kMaxAdapterNameLength - 1;
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

[[nodiscard]] VkDebugUtilsMessengerCreateInfoEXT MakeMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    // All four severities are requested, and the VulkanValidation category's own threshold
    // decides what is printed -- that is what LogCategory::minLevel is for, and it means
    // recovering verbose output is a threshold change rather than a rebuilt instance. Asking
    // for everything here also means an ERROR can never be filtered out at the source, which
    // is what the fatal path at the bottom of DebugMessengerCallback depends on.
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugMessengerCallback;
    return info;
}

}  // namespace

bool ValidationDefault() {
    // MONARC_VULKAN_VALIDATION is set PRIVATE by this module's CMakeLists.txt from the build
    // configuration, so this is the only place in the tree that knows the answer and no
    // consumer's own flags can disagree with it. Used as an ordinary expression rather than in
    // an #if, so a build that forgot to define it fails to compile instead of quietly
    // defaulting to off.
    //
    // Not a platform conditional: gate 10 in Tools/check_architecture.py matches platform
    // macros, and this is a build-configuration one -- the same category as
    // MONARC_ENABLE_ASSERTS, which every module already sees.
    return MONARC_VULKAN_VALIDATION != 0;
}

struct VulkanBackend::State {
    explicit State(IAllocator& allocatorToUse) : allocator(allocatorToUse) {}

    IAllocator&              allocator;
    Detail::Loader           loader;
    VkInstance               instance               = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger              = VK_NULL_HANDLE;
    ApiVersion               instanceApiVersion     = {};
    bool                     validationLayerEnabled = false;

    /// A Vulkan call that returned something other than VK_SUCCESS.
    ///
    /// The message is Detail::ToString(result) -- a pointer to a string literal, so it stays
    /// valid after this State has been destroyed, which is what lets VulkanBackend::Create be
    /// a factory. The composition -- which call, and the result's numeric value -- goes to
    /// the log line beside it. Keeping the integer is what keeps an unrecognised result
    /// actionable: ToString covers the results Monarc's own calls can return and says plainly
    /// when it does not recognise one, and the integer is then the only thing left to go on.
    ///
    /// Not a member of this struct out of necessity any more -- it needs nothing from it --
    /// but left here so every failure the bring-up produces is spelled in one place.
    ///
    /// **The code is `Detail::ToErrorCode(result)` and not a flat `BackendFailure`.** Three
    /// results mean the capability is absent rather than the call refused, and
    /// `VK_ERROR_INCOMPATIBLE_DRIVER` from `vkCreateInstance` -- a machine with `vulkan-1.dll`
    /// and no registered ICD -- is the one a caller most wants to branch on. Translate.h has
    /// the reasoning and Tests/TestVulkanTranslate.cpp pins the mapping.
    ///
    /// `phase` is a literal suffix distinguishing the two calls of a count-then-fill pair in
    /// the log line, and it can only be a literal: nothing on this path formats a string,
    /// because `Error::message` is a non-owning view.
    [[nodiscard]] std::unexpected<Error> FailVk(const char* operation, VkResult result,
                                                const char* phase = "") {
        MONARC_LOG(Vulkan, Warning, "{}{} failed: {} ({})", operation, phase,
                   Detail::ToString(result), static_cast<i32>(result));
        return Err(Detail::ToErrorCode(result), Detail::ToString(result));
    }

    [[nodiscard]] Status BringUp(const Config& config);
    void                 Shutdown();

    [[nodiscard]] Status QueryInstanceExtensions(Array<VkExtensionProperties>& out);
    [[nodiscard]] Status QueryInstanceLayers(Array<VkLayerProperties>& out);
    [[nodiscard]] Status QueryDeviceExtensions(VkPhysicalDevice              device,
                                               Array<VkExtensionProperties>& out);
    [[nodiscard]] Status EnumerateRaw(Array<AdapterInfo>& out);
    [[nodiscard]] Status DescribeAdapter(VkPhysicalDevice device, AdapterInfo& out);
};

Status VulkanBackend::State::BringUp(const Config& config) {
    const char* libraryName =
        config.libraryName != nullptr ? config.libraryName : Detail::VulkanLibraryName();
    Result<Detail::Loader> opened = Detail::Loader::Open(libraryName);
    if (!opened) {
        // Forwarded unchanged. Every message Loader::Open returns is a string literal, and
        // Loader.cpp's own log line has already named the library that would not open or the
        // entry point that was missing.
        return std::unexpected(opened.error());
    }
    loader = std::move(*opened);

    u32 packedInstanceVersion = 0;
    if (const VkResult result =
            loader.Global().vkEnumerateInstanceVersion(&packedInstanceVersion);
        result != VK_SUCCESS) {
        return FailVk("vkEnumerateInstanceVersion", result);
    }
    instanceApiVersion = Detail::ToApiVersion(packedInstanceVersion);

    const ApiVersion required = Detail::ToApiVersion(kRequiredApiVersion);
    if (instanceApiVersion < required) {
        MONARC_LOG(Vulkan, Warning,
                   "the Vulkan loader reports instance version {}.{}.{}; Monarc requires at "
                   "least {}.{}.{}",
                   instanceApiVersion.major, instanceApiVersion.minor, instanceApiVersion.patch,
                   required.major, required.minor, required.patch);
        // Both versions are in the log line above and neither is in the message, which names
        // no number on purpose: kRequiredApiVersion is the single source of truth for the
        // floor, and a literal spelling "1.3" would go stale the day it moves.
        return Err(ErrorCode::Unsupported,
                   "the Vulkan loader reports an instance version below the one Monarc "
                   "requires");
    }

    Array<VkExtensionProperties> availableExtensions(allocator);
    if (Status queried = QueryInstanceExtensions(availableExtensions); !queried) {
        return queried;
    }

    // VK_KHR_surface and the platform's own surface extension are required rather than
    // optional, even though Task 2 creates no surface. An instance extension cannot be added
    // after vkCreateInstance, so an instance that had not asked for these would have to be
    // torn down and rebuilt the moment Task 4 opened a window -- and a Vulkan implementation
    // that cannot present is not one Monarc can render with anyway.
    const char* const requiredExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                              Detail::PlatformSurfaceExtensionName()};

    // Sized from requiredExtensions, plus one slot for the VK_EXT_debug_utils name pushed
    // below when validation is available -- the only instance extension here that is asked
    // for conditionally.
    //
    // **Derived rather than the `[3]` this used to be, and the difference is memory
    // corruption.** Both `enabledExtensionCount++` sites below are unchecked, and a
    // hand-written size has nothing tying it to the list two lines up. Measured: with `[3]`
    // and a third entry in requiredExtensions, clang-asan reported
    // `stack-buffer-overflow ... [304, 328) 'enabledExtensions' <== Memory access at offset
    // 328 overflows this variable`, WRITE of size 8, at the debug-utils push -- and the whole
    // matrix had compiled at /W4 /WX without a word. Derived, the same edit grows the array.
    const char* enabledExtensions[std::size(requiredExtensions) + 1] = {};
    u32         enabledExtensionCount                                = 0;
    for (const char* name : requiredExtensions) {
        if (!ContainsExtension(availableExtensions, name)) {
            MONARC_LOG(Vulkan, Warning,
                       "this Vulkan implementation does not offer the required instance "
                       "extension {}",
                       name);
            // The message *is* the extension's name, and it can be, because both entries of
            // requiredExtensions are string literals with static storage:
            // VK_KHR_SURFACE_EXTENSION_NAME comes from the Vulkan headers, and
            // Detail::PlatformSurfaceExtensionName() returns
            // VK_KHR_WIN32_SURFACE_EXTENSION_NAME. So this view outlives the State it was
            // produced in, and the sentence around it -- which adds nothing a caller holding
            // ErrorCode::Unsupported does not already know -- stays in the log.
            return Err(ErrorCode::Unsupported, name);
        }
        enabledExtensions[enabledExtensionCount++] = name;
    }

    // From here on, a missing piece is a degradation rather than a failure. A machine with no
    // Vulkan SDK has no validation layer and no VK_EXT_debug_utils, and the plan is explicit
    // that such a machine still runs the game.
    if (config.validation) {
        Array<VkLayerProperties> availableLayers(allocator);
        if (Status queried = QueryInstanceLayers(availableLayers); !queried) {
            return queried;
        }

        const bool layerPresent = ContainsLayer(availableLayers, kValidationLayerName);
        const bool debugUtilsPresent =
            ContainsExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        if (layerPresent && debugUtilsPresent) {
            validationLayerEnabled                     = true;
            enabledExtensions[enabledExtensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        } else {
            // Warning, not Error: nothing is wrong with the machine, it simply has no SDK.
            // Both named, so the log says which of the two is missing.
            MONARC_LOG(Vulkan, Warning,
                       "validation was requested but is unavailable: {} is {}, {} is {}; "
                       "continuing with a degraded instance and no debug messenger",
                       kValidationLayerName, layerPresent ? "present" : "absent",
                       VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                       debugUtilsPresent ? "present" : "absent");
        }
    }

    // One layer, so there is no array to fill and no counter to increment past the end: the
    // array *is* its initialiser, and the count is 1 or 0 to select it or ignore it. A second
    // layer turns this back into something built at run time, at which point it gets
    // requiredExtensions' treatment above rather than a hand-written size of its own.
    const char* const enabledLayers[]   = {kValidationLayerName};
    const u32         enabledLayerCount = validationLayerEnabled ? 1U : 0U;

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName   = config.applicationName;
    applicationInfo.applicationVersion = kEngineVersion;
    applicationInfo.pEngineName        = "Monarc";
    applicationInfo.engineVersion      = kEngineVersion;
    applicationInfo.apiVersion         = kRequiredApiVersion;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo        = &applicationInfo;
    instanceInfo.enabledLayerCount       = enabledLayerCount;
    instanceInfo.ppEnabledLayerNames     = enabledLayerCount != 0 ? enabledLayers : nullptr;
    instanceInfo.enabledExtensionCount   = enabledExtensionCount;
    instanceInfo.ppEnabledExtensionNames = enabledExtensions;

    // The messenger create-info is chained into the instance's own pNext as well as being used
    // to create the standalone messenger below. Without the chained copy, anything the
    // validation layer has to say *about vkCreateInstance itself* has no messenger to reach --
    // the standalone one cannot exist until the instance does.
    //
    // Verified by removing it, with a deliberately invalid VkApplicationInfo::sType in place.
    // With the chain, the layer's VUID-VkApplicationInfo-sType-sType error arrives at
    // DebugMessengerCallback and the process stops at 0x80000003. Without it, the layer prints
    // the same error to stderr on its own and **vkCreateInstance succeeds, enumeration runs,
    // and the process exits zero** -- validation output that scrolls past, which is exactly
    // the false confidence the fatal messenger exists to prevent. So this chain is
    // load-bearing rather than good practice.
    const VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeMessengerCreateInfo();
    if (validationLayerEnabled) {
        instanceInfo.pNext = &messengerInfo;
    }

    if (const VkResult result =
            loader.Global().vkCreateInstance(&instanceInfo, nullptr, &instance);
        result != VK_SUCCESS) {
        instance = VK_NULL_HANDLE;
        return FailVk("vkCreateInstance", result);
    }

    if (Status loaded = loader.LoadInstanceFunctions(instance, validationLayerEnabled);
        !loaded) {
        return loaded;
    }

    if (validationLayerEnabled) {
        if (!loader.HasDebugUtilsFunctions()) {
            // The extension was enabled and its entry points did not resolve. That is a broken
            // implementation rather than a missing SDK, so it is worth an Error even though it
            // is not fatal: validation still runs, and its output still reaches the messenger
            // chained into vkCreateInstance for the duration of that call, but nothing after
            // it is watched. DebugMessengerInstalled() then reports false, which is what Task
            // 3's "assert the messenger was installed" case reads.
            MONARC_LOG(Vulkan, Error,
                       "{} was enabled but its entry points did not resolve; no debug "
                       "messenger is installed and validation output after instance creation "
                       "will be lost",
                       VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else if (const VkResult result = loader.DebugUtils().vkCreateDebugUtilsMessengerEXT(
                       instance, &messengerInfo, nullptr, &messenger);
                   result != VK_SUCCESS) {
            messenger = VK_NULL_HANDLE;
            return FailVk("vkCreateDebugUtilsMessengerEXT", result);
        }
    }

    MONARC_LOG(Vulkan, Info,
               "Vulkan instance created | loader {}.{}.{} | requested {}.{}.{} | validation "
               "{} | messenger {}",
               instanceApiVersion.major, instanceApiVersion.minor, instanceApiVersion.patch,
               required.major, required.minor, required.patch,
               validationLayerEnabled ? "on" : "off",
               messenger != VK_NULL_HANDLE ? "installed" : "absent");
    return {};
}

void VulkanBackend::State::Shutdown() {
    // Reverse creation order, and it matters. Destroying the instance while the messenger is
    // still alive is itself a validation error ("all child objects must have been destroyed"),
    // and unloading vulkan-1.dll before either would unmap the code that does the destroying.
    if (messenger != VK_NULL_HANDLE) {
        // No null check on this one, and it does not need the guard below: `messenger` is only
        // ever set after `loader.HasDebugUtilsFunctions()` came back true, which is exactly
        // "both of these resolved", and nothing between there and here can null them.
        loader.DebugUtils().vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        // **Guarded, because this pair can genuinely disagree.** BringUp creates the instance
        // and *then* resolves the instance table, so a LoadInstanceFunctions that fails --
        // vkDestroyInstance is the first name in MONARC_VK_INSTANCE_FUNCTIONS -- returns with
        // an instance to destroy and no entry point to destroy it with. Create's failure path
        // calls this. Measured: without the guard, and with vkDestroyInstance forced null on
        // a machine that has Vulkan, `FirstLight --adapters` exited 0xC0000005.
        //
        // Leaking the instance is all that is left to do about it, and it is the right thing:
        // the alternative is the crash above, and the process is on its way out through a
        // failed Create in the only way to reach here.
        if (loader.Instance().vkDestroyInstance != nullptr) {
            loader.Instance().vkDestroyInstance(instance, nullptr);
        } else {
            MONARC_LOG(Vulkan, Error,
                       "the Vulkan instance cannot be destroyed: vkDestroyInstance never "
                       "resolved, so it is leaked until the process exits");
        }
        instance = VK_NULL_HANDLE;
    }
    loader.Close();

    instanceApiVersion     = ApiVersion{};
    validationLayerEnabled = false;
}

Status VulkanBackend::State::QueryInstanceExtensions(Array<VkExtensionProperties>& out) {
    u32 count = 0;
    if (const VkResult result = loader.Global().vkEnumerateInstanceExtensionProperties(
            nullptr, &count, nullptr);
        result != VK_SUCCESS) {
        return FailVk("vkEnumerateInstanceExtensionProperties (count)", result);
    }

    ResizeTo(out, count);
    if (count == 0) {
        return {};
    }

    // VK_INCOMPLETE here means more extensions appeared between the two calls, which no amount
    // of retrying can rule out. `count` is then what was actually written, so the honest
    // response is to keep those and say so rather than loop.
    const VkResult result =
        loader.Global().vkEnumerateInstanceExtensionProperties(nullptr, &count, out.Data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return FailVk("vkEnumerateInstanceExtensionProperties", result);
    }
    if (result == VK_INCOMPLETE) {
        MONARC_LOG(Vulkan, Warning,
                   "vkEnumerateInstanceExtensionProperties reported more extensions than it "
                   "returned; continuing with the {} it wrote",
                   count);
    }
    ShrinkTo(out, count);
    return {};
}

Status VulkanBackend::State::QueryInstanceLayers(Array<VkLayerProperties>& out) {
    u32 count = 0;
    if (const VkResult result = loader.Global().vkEnumerateInstanceLayerProperties(&count,
                                                                                   nullptr);
        result != VK_SUCCESS) {
        return FailVk("vkEnumerateInstanceLayerProperties (count)", result);
    }

    ResizeTo(out, count);
    if (count == 0) {
        return {};
    }

    const VkResult result =
        loader.Global().vkEnumerateInstanceLayerProperties(&count, out.Data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return FailVk("vkEnumerateInstanceLayerProperties", result);
    }
    ShrinkTo(out, count);
    return {};
}

Status VulkanBackend::State::QueryDeviceExtensions(VkPhysicalDevice              device,
                                                   Array<VkExtensionProperties>& out) {
    const Detail::InstanceFunctions& fns = loader.Instance();

    u32 count = 0;
    if (const VkResult result =
            fns.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        result != VK_SUCCESS) {
        return FailVk("vkEnumerateDeviceExtensionProperties (count)", result);
    }

    ResizeTo(out, count);
    if (count == 0) {
        return {};
    }

    const VkResult result =
        fns.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, out.Data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return FailVk("vkEnumerateDeviceExtensionProperties", result);
    }
    ShrinkTo(out, count);
    return {};
}

Status VulkanBackend::State::EnumerateRaw(Array<AdapterInfo>& out) {
    const Detail::InstanceFunctions& fns = loader.Instance();

    u32 count = 0;
    if (const VkResult result = fns.vkEnumeratePhysicalDevices(instance, &count, nullptr);
        result != VK_SUCCESS) {
        return FailVk("vkEnumeratePhysicalDevices (count)", result);
    }
    if (count == 0) {
        // Not an error. A loader with no ICD registered behind it is exactly what a CI runner
        // looks like, and "there are no devices" is a true answer to the question asked.
        return {};
    }

    Array<VkPhysicalDevice> devices(allocator);
    ResizeTo(devices, count);
    const VkResult result = fns.vkEnumeratePhysicalDevices(instance, &count, devices.Data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return FailVk("vkEnumeratePhysicalDevices", result);
    }
    if (result == VK_INCOMPLETE) {
        MONARC_LOG(Vulkan, Warning,
                   "vkEnumeratePhysicalDevices reported more devices than it returned; "
                   "continuing with the {} it wrote",
                   count);
    }

    out.Reserve(count);
    for (u32 i = 0; i < count; ++i) {
        AdapterInfo info{};
        if (Status described = DescribeAdapter(devices[i], info); !described) {
            return described;
        }
        if (info.capabilities.apiVersion < ApiVersion{1, 1, 0}) {
            // A device below Vulkan 1.1 cannot report a deviceUUID at all --
            // VkPhysicalDeviceIDProperties is 1.1 core -- so it cannot be identified, and an
            // unidentifiable entry in a list whose entire purpose is identity would
            // deduplicate against every other one like it. It also cannot meet Monarc's 1.3
            // baseline, so naming it and moving on loses nothing.
            //
            // Not exercised by anything: no such device exists on the development machine or,
            // realistically, on any driver from the last several years. It is a guard, and
            // this comment is the extent of the claim made for it.
            MONARC_LOG(Vulkan, Warning,
                       "skipping physical device \"{}\": it reports Vulkan {}.{}.{}, below the "
                       "1.1 needed to query a device UUID",
                       info.name, info.capabilities.apiVersion.major,
                       info.capabilities.apiVersion.minor, info.capabilities.apiVersion.patch);
            continue;
        }
        out.Push(info);
    }
    return {};
}

Status VulkanBackend::State::DescribeAdapter(VkPhysicalDevice device, AdapterInfo& out) {
    const Detail::InstanceFunctions& fns = loader.Instance();

    // Separate property queries rather than one chained call, because which structures are
    // worth chaining depends on the version the device itself reports: the version is read
    // first, from the base structure alone, and each promoted-version structure is added only
    // once the device has said it understands it.
    //
    // **Not because validation objects.** That was the assumption, and it is wrong: chaining
    // VkPhysicalDeviceVulkan14Properties onto the 1.3.275 Intel device produced no message at
    // all from SDK 1.4.357's validation layer, and the process ran to completion. The gate is
    // here for the quieter reason. An implementation that does not recognise a chained
    // structure simply does not write it, so Monarc would read back whatever it initialised --
    // zeros -- as though a driver had reported them. Zeros happen to be safe for the bindless
    // *limit*, since a device would then fail a tier rather than pass one it should not; they
    // are not safe in general, and a feature struct left unwritten reads as all-false, which a
    // requirement phrased as "must not have X" would satisfy on a device that was never asked.
    VkPhysicalDeviceProperties2 baseProperties{};
    baseProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    fns.vkGetPhysicalDeviceProperties2(device, &baseProperties);

    CopyName(out.name, baseProperties.properties.deviceName);
    out.type                    = Detail::ToDeviceType(baseProperties.properties.deviceType);
    out.vendorId                = baseProperties.properties.vendorID;
    out.deviceId                = baseProperties.properties.deviceID;
    out.capabilities.apiVersion = Detail::ToApiVersion(baseProperties.properties.apiVersion);

    const ApiVersion deviceVersion = out.capabilities.apiVersion;
    if (deviceVersion < ApiVersion{1, 1, 0}) {
        // The caller drops this device. There is nothing more that can honestly be asked of it:
        // VkPhysicalDeviceIDProperties is 1.1 core, so a device below that has no UUID to
        // report and asking would read back the zeros above rather than an identity.
        return {};
    }

    VkPhysicalDeviceVulkan12Properties vulkan12Properties{};
    vulkan12Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;

    VkPhysicalDeviceIDProperties idProperties{};
    idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &idProperties;
    if (deviceVersion >= ApiVersion{1, 2, 0}) {
        idProperties.pNext = &vulkan12Properties;
    }
    fns.vkGetPhysicalDeviceProperties2(device, &properties);

    std::memcpy(out.uuid.bytes, idProperties.deviceUUID, AdapterUuid::kSize);
    out.capabilities.maxBindlessSampledImages =
        vulkan12Properties.maxDescriptorSetUpdateAfterBindSampledImages;

    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (deviceVersion >= ApiVersion{1, 2, 0}) {
        features.pNext = &vulkan12Features;
        if (deviceVersion >= ApiVersion{1, 3, 0}) {
            vulkan12Features.pNext = &vulkan13Features;
        }
    }
    fns.vkGetPhysicalDeviceFeatures2(device, &features);

    out.capabilities.timelineSemaphores = vulkan12Features.timelineSemaphore == VK_TRUE;
    out.capabilities.nonUniformIndexing =
        vulkan12Features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    out.capabilities.runtimeDescriptorArray =
        vulkan12Features.runtimeDescriptorArray == VK_TRUE;
    out.capabilities.partiallyBoundDescriptors =
        vulkan12Features.descriptorBindingPartiallyBound == VK_TRUE;
    out.capabilities.dynamicRendering = vulkan13Features.dynamicRendering == VK_TRUE;
    out.capabilities.synchronization2 = vulkan13Features.synchronization2 == VK_TRUE;

    u32 queueFamilyCount = 0;
    fns.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    Array<VkQueueFamilyProperties> queueFamilies(allocator);
    ResizeTo(queueFamilies, queueFamilyCount);
    if (queueFamilyCount != 0) {
        fns.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                                     queueFamilies.Data());
    }
    out.capabilities.queueFamilyCount = queueFamilyCount;
    for (u32 i = 0; i < queueFamilyCount; ++i) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            ++out.capabilities.graphicsQueueFamilyCount;
        }
    }

    Array<VkExtensionProperties> deviceExtensions(allocator);
    if (Status queried = QueryDeviceExtensions(device, deviceExtensions); !queried) {
        return queried;
    }
    out.capabilities.meshShading =
        ContainsExtension(deviceExtensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    // Both, not either: a ray-tracing pipeline with no acceleration structure to trace against
    // is not a capability, and the two ship together on every implementation that has them.
    out.capabilities.rayTracing =
        ContainsExtension(deviceExtensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
        ContainsExtension(deviceExtensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);

    out.tier = DetermineTier(out.capabilities);
    return {};
}

Result<VulkanBackend> VulkanBackend::Create(IAllocator& allocator, const Config& config) {
    // The allocation happens here, before anything is constructed, so a backend with no state
    // is never handed to a caller. Not fatal, unlike Array<T>'s allocation failure: this is a
    // fallible entry point already, so reporting costs nothing over aborting.
    void* storage = allocator.Allocate(sizeof(State), alignof(State));
    if (storage == nullptr) {
        MONARC_LOG(Vulkan, Warning,
                   "the allocator returned nothing for {} bytes of Vulkan backend state",
                   sizeof(State));
        return Err(ErrorCode::OutOfMemory,
                   "the allocator returned nothing for the Vulkan backend's state");
    }
    State* state = ::new (storage) State(allocator);

    if (Status brought = state->BringUp(config); !brought) {
        // Nothing half-built survives a failed Create. Copying the Error out before the State
        // is destroyed is the reason every message BringUp returns is a string literal: the
        // view has to still point at something once this storage is gone.
        const Error error = brought.error();
        state->Shutdown();
        state->~State();
        allocator.Deallocate(storage, sizeof(State), alignof(State));
        return std::unexpected(error);
    }

    return VulkanBackend(state);
}

VulkanBackend::VulkanBackend(State* state) noexcept : m_state(state) {}

VulkanBackend::~VulkanBackend() { Release(); }

VulkanBackend::VulkanBackend(VulkanBackend&& other) noexcept
    : m_state(std::exchange(other.m_state, nullptr)) {}

VulkanBackend& VulkanBackend::operator=(VulkanBackend&& other) noexcept {
    // Self-assignment would otherwise release this backend's state and then adopt the pointer
    // it had just freed.
    if (this != &other) {
        Release();
        m_state = std::exchange(other.m_state, nullptr);
    }
    return *this;
}

void VulkanBackend::Release() {
    if (m_state == nullptr) {
        return;
    }
    m_state->Shutdown();
    IAllocator& allocator = m_state->allocator;
    m_state->~State();
    allocator.Deallocate(m_state, sizeof(State), alignof(State));
    m_state = nullptr;
}

void VulkanBackend::Shutdown() {
    if (m_state != nullptr) {
        m_state->Shutdown();
    }
}

bool VulkanBackend::IsInitialized() const {
    return m_state != nullptr && m_state->instance != VK_NULL_HANDLE;
}

ApiVersion VulkanBackend::InstanceApiVersion() const {
    return m_state != nullptr ? m_state->instanceApiVersion : ApiVersion{};
}

bool VulkanBackend::ValidationLayerEnabled() const {
    return m_state != nullptr && m_state->validationLayerEnabled;
}

bool VulkanBackend::DebugMessengerInstalled() const {
    return m_state != nullptr && m_state->messenger != VK_NULL_HANDLE;
}

Status VulkanBackend::EnumerateAdaptersRaw(Array<AdapterInfo>& out) {
    out.Clear();
    if (!IsInitialized()) {
        return Err(ErrorCode::InvalidArgument,
                   "VulkanBackend::EnumerateAdaptersRaw called on a backend that has been "
                   "shut down or moved from");
    }
    Status enumerated = m_state->EnumerateRaw(out);
    if (!enumerated) {
        // EnumerateRaw appends as it describes each device, so a failure part-way through
        // leaves some in `out`. The Status is [[nodiscard]] and a caller that heeds it never
        // reads them, but "half a list" is not a state worth having available: clearing costs
        // nothing and removes the question.
        out.Clear();
    }
    return enumerated;
}

Status VulkanBackend::EnumerateAdapters(Array<AdapterInfo>& out) {
    if (Status raw = EnumerateAdaptersRaw(out); !raw) {
        return raw;
    }
    const usize kept = DeduplicateAdapters(std::span<AdapterInfo>(out.Data(), out.Size()));
    ShrinkTo(out, kept);
    return {};
}

}  // namespace Monarc::RHI
