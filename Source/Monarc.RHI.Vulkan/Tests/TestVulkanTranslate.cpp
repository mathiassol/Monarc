#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/RHI/Capabilities.h>
#include <Monarc/RHI/Types.h>

#include <Translate.h>

#include <cstring>
#include <iterator>
#include <string_view>

using Monarc::RHI::ApiVersion;
using Monarc::RHI::DeviceType;
using Monarc::RHI::Format;
using Monarc::RHI::Detail::ContainsExtension;
using Monarc::RHI::Detail::ContainsLayer;
using Monarc::RHI::Detail::FromVulkan;
using Monarc::RHI::Detail::SeverityToLogLevel;
using Monarc::RHI::Detail::ToApiVersion;
using Monarc::RHI::Detail::ToDeviceType;
using Monarc::RHI::Detail::ToErrorCode;
using Monarc::RHI::Detail::ToVulkan;

namespace {

/// A VkExtensionProperties naming `name`, as a driver would hand one back. The name is a
/// fixed-size char array in the struct, so this is what building one by hand takes.
[[nodiscard]] VkExtensionProperties Extension(const char* name) {
    VkExtensionProperties properties{};
    std::memcpy(properties.extensionName, name, std::strlen(name));
    return properties;
}

[[nodiscard]] VkLayerProperties Layer(const char* name) {
    VkLayerProperties properties{};
    std::memcpy(properties.layerName, name, std::strlen(name));
    return properties;
}

/// Every enumerator of Format, once. Same list and same reasoning as TestTypes.cpp's: the
/// `default`-less switch in Translate.cpp already refuses a new enumerator nobody gave a case
/// to, and this list covers the half the compiler cannot -- that the case it was given returns
/// the *right* Vulkan format rather than a neighbouring row's.
constexpr Format kAllFormats[] = {
    Format::Unknown,
    Format::R8G8B8A8_UNORM,
    Format::B8G8R8A8_UNORM,
};

/// A value Format can hold that no enumerator names. Well-defined: Format has a fixed
/// underlying type.
constexpr Format kNotAFormat = static_cast<Format>(4242);

/// Every VkPhysicalDeviceType, including the MAX_ENUM sentinel every generated Vulkan enum
/// carries. The sentinel is in the list because it is an enumerator, and because it is the one
/// the `default`-less switch in Translate.cpp had to be given a case for.
constexpr VkPhysicalDeviceType kAllPhysicalDeviceTypes[] = {
    VK_PHYSICAL_DEVICE_TYPE_OTHER,       VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
    VK_PHYSICAL_DEVICE_TYPE_CPU,          VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM,
};

}  // namespace

TEST_CASE("every format maps to the Vulkan format that spells the same thing") {
    CHECK(ToVulkan(Format::Unknown) == VK_FORMAT_UNDEFINED);
    CHECK(ToVulkan(Format::R8G8B8A8_UNORM) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(ToVulkan(Format::B8G8R8A8_UNORM) == VK_FORMAT_B8G8R8A8_UNORM);
}

TEST_CASE("kAllFormats lists every Format enumerator") {
    // The list is only worth iterating if it is complete, and C++ cannot ask an enum how many
    // enumerators it has. Same indirect check as TestTypes.cpp's: the index one past the end
    // of the list must not name a format, which ToString answers by returning its
    // not-a-format marker. Append an enumerator without extending this list and that index
    // becomes a named format, and this fails -- observed, by dropping B8G8R8A8_UNORM from the
    // list and watching it report "B8G8R8A8_UNORM == <invalid Format>".
    const Format onePastTheList = static_cast<Format>(std::size(kAllFormats));
    CHECK(std::string_view(Monarc::RHI::ToString(onePastTheList)) ==
          std::string_view(Monarc::RHI::ToString(kNotAFormat)));
}

TEST_CASE("no two formats map to one Vulkan format") {
    // The pair this matters for is B8G8R8A8_UNORM and R8G8B8A8_UNORM: they differ only in
    // channel order, so a copy-paste in Translate.cpp giving them one VkFormat is invisible to
    // every other test here and is exactly the confusion Task 3's exact-value readback exists
    // to catch. Unknown is excluded because VK_FORMAT_UNDEFINED is legitimately shared with
    // every value that is not a format at all.
    for (const Format outer : kAllFormats) {
        for (const Format inner : kAllFormats) {
            if (outer != inner && outer != Format::Unknown && inner != Format::Unknown) {
                CHECK(ToVulkan(outer) != ToVulkan(inner));
            }
        }
    }
}

TEST_CASE("every format round-trips through Vulkan and back") {
    // The property a swapchain format negotiated against a surface actually depends on: Task 4
    // asks the surface what it supports, gets VkFormats back, and has to turn them into
    // something Monarc can name.
    for (const Format format : kAllFormats) {
        CHECK(FromVulkan(ToVulkan(format)) == format);
    }
}

TEST_CASE("a Vulkan format Monarc does not model comes back as Unknown, not as a guess") {
    CHECK(FromVulkan(VK_FORMAT_R16G16B16A16_SFLOAT) == Format::Unknown);
    CHECK(FromVulkan(VK_FORMAT_D24_UNORM_S8_UINT) == Format::Unknown);
    // The near-misses matter most: these differ from a format Monarc does model by one letter,
    // and a `default` that fell through to the wrong row would be caught here and nowhere else.
    CHECK(FromVulkan(VK_FORMAT_R8G8B8A8_SRGB) == Format::Unknown);
    CHECK(FromVulkan(VK_FORMAT_B8G8R8A8_SRGB) == Format::Unknown);
    CHECK(FromVulkan(VK_FORMAT_R8G8B8A8_SNORM) == Format::Unknown);
    CHECK(FromVulkan(VK_FORMAT_UNDEFINED) == Format::Unknown);
}

TEST_CASE("a value that is not a Format translates to undefined rather than to a real format") {
    CHECK(ToVulkan(kNotAFormat) == VK_FORMAT_UNDEFINED);
}

TEST_CASE("every Vulkan device type maps to the RHI device kind of the same name") {
    CHECK(ToDeviceType(VK_PHYSICAL_DEVICE_TYPE_OTHER) == DeviceType::Other);
    CHECK(ToDeviceType(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) == DeviceType::IntegratedGpu);
    CHECK(ToDeviceType(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) == DeviceType::DiscreteGpu);
    CHECK(ToDeviceType(VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) == DeviceType::VirtualGpu);
    CHECK(ToDeviceType(VK_PHYSICAL_DEVICE_TYPE_CPU) == DeviceType::Cpu);
}

TEST_CASE("the four real device kinds map to four different RHI kinds") {
    // Discrete and integrated are the two that actually occur on the development machine, and
    // conflating them would make the tier report and any future device-selection heuristic
    // silently wrong on a laptop. Other and MAX_ENUM are excluded: both legitimately mean
    // "nothing better to say" and share DeviceType::Other.
    constexpr VkPhysicalDeviceType kReal[] = {VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                                              VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                              VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                                              VK_PHYSICAL_DEVICE_TYPE_CPU};
    for (const VkPhysicalDeviceType outer : kReal) {
        for (const VkPhysicalDeviceType inner : kReal) {
            if (outer != inner) {
                CHECK(ToDeviceType(outer) != ToDeviceType(inner));
            }
        }
    }
}

TEST_CASE("the MAX_ENUM sentinel and an unrecognised value both mean Other") {
    // MAX_ENUM is not a device kind and no driver returns it, but it is an enumerator, so the
    // `default`-less switch in Translate.cpp had to be given a case for it. This pins where
    // that case goes.
    CHECK(ToDeviceType(VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM) == DeviceType::Other);
    CHECK(ToDeviceType(static_cast<VkPhysicalDeviceType>(4242)) == DeviceType::Other);

    // And nothing in the list translates to a value outside DeviceType's own enumerators,
    // which is what "maps to exactly one Vulkan enumerator" means in this direction.
    for (const VkPhysicalDeviceType type : kAllPhysicalDeviceTypes) {
        CHECK_FALSE(std::string_view(Monarc::RHI::ToString(ToDeviceType(type))) ==
                    "<invalid DeviceType>");
    }
}

TEST_CASE("a packed Vulkan version decodes to its three parts") {
    CHECK(ToApiVersion(VK_MAKE_API_VERSION(0, 1, 3, 0)) == ApiVersion{1, 3, 0});
    CHECK(ToApiVersion(VK_API_VERSION_1_3) == ApiVersion{1, 3, 0});
    CHECK(ToApiVersion(VK_MAKE_API_VERSION(0, 1, 3, 275)) == ApiVersion{1, 3, 275});
    CHECK(ToApiVersion(VK_MAKE_API_VERSION(0, 1, 4, 351)) == ApiVersion{1, 4, 351});
}

TEST_CASE("the packed versions the development machine actually reports decode correctly") {
    // 4206867 and 4211039 are the integers `vulkaninfo` printed beside "1.3.275" and
    // "1.4.351" for the two devices on this machine. Written as literals rather than built
    // with VK_MAKE_API_VERSION on purpose: the case above already checks that the decoder
    // agrees with the encoder, which would still hold if both had the same bug. These are the
    // numbers a driver put in a struct.
    CHECK(ToApiVersion(4206867) == ApiVersion{1, 3, 275});
    CHECK(ToApiVersion(4211039) == ApiVersion{1, 4, 351});
}

TEST_CASE("the variant bits are dropped rather than folded into the version") {
    // Variant 1 is Vulkan SC, a different specification Monarc does not target. A decoder that
    // read the variant bits as part of the major version would report 1.3.0 here as something
    // else entirely, and the difference is only visible on a value no ordinary driver produces.
    CHECK(ToApiVersion(VK_MAKE_API_VERSION(1, 1, 3, 0)) == ApiVersion{1, 3, 0});
    CHECK(ToApiVersion(VK_MAKE_API_VERSION(1, 1, 3, 0)) ==
          ToApiVersion(VK_MAKE_API_VERSION(0, 1, 3, 0)));
}

TEST_CASE("a VkResult Monarc can receive is named by its own spelling") {
    using Monarc::RHI::Detail::ToString;
    CHECK(std::string_view(ToString(VK_SUCCESS)) == "VK_SUCCESS");
    CHECK(std::string_view(ToString(VK_INCOMPLETE)) == "VK_INCOMPLETE");
    CHECK(std::string_view(ToString(VK_ERROR_OUT_OF_HOST_MEMORY)) ==
          "VK_ERROR_OUT_OF_HOST_MEMORY");
    CHECK(std::string_view(ToString(VK_ERROR_OUT_OF_DEVICE_MEMORY)) ==
          "VK_ERROR_OUT_OF_DEVICE_MEMORY");
    CHECK(std::string_view(ToString(VK_ERROR_INITIALIZATION_FAILED)) ==
          "VK_ERROR_INITIALIZATION_FAILED");
    CHECK(std::string_view(ToString(VK_ERROR_LAYER_NOT_PRESENT)) ==
          "VK_ERROR_LAYER_NOT_PRESENT");
    CHECK(std::string_view(ToString(VK_ERROR_EXTENSION_NOT_PRESENT)) ==
          "VK_ERROR_EXTENSION_NOT_PRESENT");
    CHECK(std::string_view(ToString(VK_ERROR_INCOMPATIBLE_DRIVER)) ==
          "VK_ERROR_INCOMPATIBLE_DRIVER");
}

TEST_CASE("a VkResult outside Monarc's table says so rather than guessing") {
    using Monarc::RHI::Detail::ToString;
    // Translate.h is explicit that this table is not exhaustive and cannot usefully be. What
    // matters is that the fallback is honest: an unrecognised result must not be reported as
    // VK_SUCCESS, and it must not be reported as any other real result either. The numeric
    // value reaches the log through the call site, which is what keeps this actionable.
    const std::string_view unknown = ToString(static_cast<VkResult>(-987654));
    CHECK_FALSE(unknown.empty());
    CHECK(unknown != "VK_SUCCESS");
    CHECK(unknown.find("not in Monarc's table") != std::string_view::npos);

    // VK_ERROR_DEVICE_LOST is a real result that Task 3's submissions can return and Task 2's
    // calls cannot, so it is deliberately absent from the table today. Pinned here so that
    // adding it is a visible change to this case rather than a silent one.
    CHECK(std::string_view(ToString(VK_ERROR_DEVICE_LOST)) == unknown);
}

TEST_CASE("a result meaning the capability is absent is Unsupported, not BackendFailure") {
    // The distinction Monarc/Core/Error.h draws: Unsupported means asking differently might
    // work, BackendFailure means the call was legitimate and the API refused it. All three
    // below are the first, and VK_ERROR_INCOMPATIBLE_DRIVER is the one that matters most --
    // it is what vkCreateInstance returns on a machine with vulkan-1.dll and no registered
    // ICD, which is plausibly the CI runner's exact state. Branchability is the whole reason
    // BackendFailure exists, so a caller falling back on Unsupported must fire here.
    CHECK(ToErrorCode(VK_ERROR_INCOMPATIBLE_DRIVER) == Monarc::ErrorCode::Unsupported);
    CHECK(ToErrorCode(VK_ERROR_LAYER_NOT_PRESENT) == Monarc::ErrorCode::Unsupported);
    CHECK(ToErrorCode(VK_ERROR_EXTENSION_NOT_PRESENT) == Monarc::ErrorCode::Unsupported);
}

TEST_CASE("every other result the backend can receive stays BackendFailure") {
    // Including the two out-of-memory results, deliberately: Monarc's OutOfMemory means
    // *Monarc's* allocator returned nothing -- which is what a failed VulkanBackend::Create
    // reports for its own State -- and a driver heap running out is a different fact a caller
    // would handle differently. Conflating them would make one code mean two things.
    CHECK(ToErrorCode(VK_ERROR_OUT_OF_HOST_MEMORY) == Monarc::ErrorCode::BackendFailure);
    CHECK(ToErrorCode(VK_ERROR_OUT_OF_DEVICE_MEMORY) == Monarc::ErrorCode::BackendFailure);
    CHECK(ToErrorCode(VK_ERROR_INITIALIZATION_FAILED) == Monarc::ErrorCode::BackendFailure);

    // And a result the table has never heard of. The fallback must not quietly become
    // Unsupported, which would tell a caller a capability was missing on the strength of a
    // number nobody recognised.
    CHECK(ToErrorCode(static_cast<VkResult>(-987654)) == Monarc::ErrorCode::BackendFailure);
    CHECK(ToErrorCode(VK_ERROR_DEVICE_LOST) == Monarc::ErrorCode::BackendFailure);
}

TEST_CASE("an extension is found by its whole name and never by a prefix of one") {
    Monarc::SystemAllocator                       allocator;
    Monarc::Array<VkExtensionProperties>          extensions(allocator);
    extensions.Push(Extension("VK_KHR_surface_maintenance1"));
    extensions.Push(Extension("VK_KHR_win32_surface"));
    extensions.Push(Extension("VK_NV_mesh_shader"));

    CHECK(ContainsExtension(extensions, "VK_KHR_win32_surface"));
    CHECK(ContainsExtension(extensions, "VK_KHR_surface_maintenance1"));

    // **The near-misses are the point.** `VK_KHR_surface` is a prefix of an entry that *is*
    // present, and `VK_EXT_mesh_shader` differs from one by two letters. A prefix or substring
    // match would report both as available, which is a capability claim the implementation
    // would then fail to honour -- and Tasks 3 and 4 add many more of these queries.
    CHECK_FALSE(ContainsExtension(extensions, "VK_KHR_surface"));
    CHECK_FALSE(ContainsExtension(extensions, "VK_EXT_mesh_shader"));

    // And the other direction: a name longer than an available one must not match it either.
    CHECK_FALSE(ContainsExtension(extensions, "VK_KHR_win32_surface_2"));
    CHECK_FALSE(ContainsExtension(extensions, ""));
}

TEST_CASE("an empty extension list contains nothing, including the empty name") {
    // What a machine with no Vulkan SDK hands back, and the case a loop written with a
    // do-while or an off-by-one bound would read one element of anyway.
    Monarc::SystemAllocator              allocator;
    Monarc::Array<VkExtensionProperties> extensions(allocator);
    CHECK_FALSE(ContainsExtension(extensions, "VK_EXT_debug_utils"));
    CHECK_FALSE(ContainsExtension(extensions, ""));
}

TEST_CASE("a layer is found by its whole name, and the validation layer by exactly its own") {
    Monarc::SystemAllocator          allocator;
    Monarc::Array<VkLayerProperties> layers(allocator);
    layers.Push(Layer("VK_LAYER_KHRONOS_validation"));
    layers.Push(Layer("VK_LAYER_MEDAL_HOOK"));

    CHECK(ContainsLayer(layers, "VK_LAYER_KHRONOS_validation"));
    CHECK(ContainsLayer(layers, "VK_LAYER_MEDAL_HOOK"));

    // VK_LAYER_KHRONOS_validation is the one name whose presence decides whether Monarc's
    // instance gets validation at all, and the overlay layers this machine really does have
    // installed are what a loose match would collide with.
    CHECK_FALSE(ContainsLayer(layers, "VK_LAYER_KHRONOS"));
    CHECK_FALSE(ContainsLayer(layers, "VK_LAYER_LUNARG_standard_validation"));

    Monarc::Array<VkLayerProperties> none(allocator);
    CHECK_FALSE(ContainsLayer(none, "VK_LAYER_KHRONOS_validation"));
}

TEST_CASE("each debug-utils severity logs at the level that will actually be printed") {
    // The VulkanValidation category's minimum is Warning, so one level too low is not a
    // cosmetic difference: an ERROR mapped to Info is filtered out at the sink and the
    // validation finding disappears. Each of the four is pinned by name.
    CHECK(SeverityToLogLevel(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ==
          Monarc::LogLevel::Error);
    CHECK(SeverityToLogLevel(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ==
          Monarc::LogLevel::Warning);
    CHECK(SeverityToLogLevel(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) ==
          Monarc::LogLevel::Info);
    CHECK(SeverityToLogLevel(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) ==
          Monarc::LogLevel::Trace);
}

TEST_CASE("several severity bits at once report the loudest of them") {
    // Vulkan documents the callback's severity as a single bit, and the ordered tests take the
    // maximum rather than the first match so that this function does not depend on that.
    // Reversing the order of the tests -- the mistake the shape invites -- turns these red.
    CHECK(SeverityToLogLevel(static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(
              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
              VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)) == Monarc::LogLevel::Error);
    CHECK(SeverityToLogLevel(static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(
              VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
              VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)) == Monarc::LogLevel::Warning);
}

TEST_CASE("no severity maps to Debug or Fatal, which is what the callback's switch assumes") {
    // DebugMessengerCallback's switch is `default`-less over all six LogLevels, so it must
    // give Debug and Fatal a case -- and it currently folds them in with Trace. That is only
    // sound while this function cannot return either, so the range is pinned here rather than
    // assumed there. A severity that started returning Fatal would log the loudest level at
    // the quietest with no compile error; this is the case that would notice.
    constexpr VkDebugUtilsMessageSeverityFlagBitsEXT kAll[] = {
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT,
    };
    for (const VkDebugUtilsMessageSeverityFlagBitsEXT severity : kAll) {
        const Monarc::LogLevel level = SeverityToLogLevel(severity);
        CHECK(level != Monarc::LogLevel::Debug);
        CHECK(level != Monarc::LogLevel::Fatal);
    }

    // A bit Vulkan has not defined is a new severity as far as this is concerned, and it lands
    // at Trace -- the quietest level, which is the right place for something unrecognised.
    CHECK(SeverityToLogLevel(static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(0x40000)) ==
          Monarc::LogLevel::Trace);
}
