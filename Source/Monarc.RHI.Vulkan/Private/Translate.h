#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Capabilities.h>
#include <Monarc/RHI/Types.h>

#include <vulkan/vulkan.h>

namespace Monarc::RHI::Detail {

/// RHI enum to Vulkan enum and back, as pure functions.
///
/// Every function here is total, allocates nothing, and touches no Vulkan state, which is
/// what lets CI test the whole file with no device and no loader. The `default`-less switches
/// are the point: adding an RHI enumerator becomes a compile error in Translate.cpp rather
/// than a silent fall-through to whatever the trailing return says.

/// The Vulkan format `format` names. `VK_FORMAT_UNDEFINED` for `Format::Unknown`, and for any
/// value outside the enumerator set.
///
/// Task 2 has no caller for this: there is no swapchain and no texture until Tasks 3 and 4.
/// It is here because the exhaustiveness guarantee is the thing being built -- the test that
/// pins it needs an RHI enumerator set to be exhaustive over, and `Format` is the one that
/// exists.
[[nodiscard]] VkFormat ToVulkan(Format format);

/// The RHI format `format` names, or `Format::Unknown` if Monarc does not model it.
///
/// **This direction is not exhaustive and cannot be.** `VkFormat` has several hundred
/// enumerators and Monarc models two, so the switch here has a `default` -- deliberately,
/// and it is not the same kind of switch as the one above. What the round-trip test pins is
/// that `FromVulkan(ToVulkan(f)) == f` for every `f` Monarc does model, which is the property
/// a swapchain format negotiated against a surface actually depends on.
[[nodiscard]] Format FromVulkan(VkFormat format);

/// The RHI device kind `type` names.
[[nodiscard]] DeviceType ToDeviceType(VkPhysicalDeviceType type);

/// Decodes a packed Vulkan version -- `VK_MAKE_API_VERSION`'s encoding, as
/// `VkPhysicalDeviceProperties::apiVersion` and `vkEnumerateInstanceVersion` report one.
///
/// The variant bits (31..29) are deliberately dropped rather than surfaced. They are non-zero
/// only for Vulkan SC, a different specification that Monarc does not target, and a variant
/// Monarc cannot run on would not be made runnable by reporting its number.
[[nodiscard]] ApiVersion ToApiVersion(u32 packed);

/// The `VkResult` enumerator's own spelling, for a log line and for an
/// `ErrorCode::BackendFailure` message. Never nullptr.
///
/// A table of the results Monarc's own calls can actually return, and not an exhaustive
/// switch over `VkResult` -- that enum has well over a hundred enumerators, most of them
/// belonging to extensions this module does not enable, and three hundred cases would be a
/// claim of completeness nothing verifies. So this one has a `default`, and the fallback says
/// it does not know rather than guessing.
///
/// The numeric value is not lost when the fallback is hit: every call site in VulkanBackend
/// formats the result's integer value into the message alongside this string, which is what
/// makes an unrecognised result still actionable. The list grows when a call that can return
/// something new is added -- Tasks 3 and 4 bring `VK_ERROR_DEVICE_LOST`,
/// `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` with them.
[[nodiscard]] const char* ToString(VkResult result);

/// The `ErrorCode` a failed Vulkan call deserves.
///
/// **Three results mean "the capability is absent", not "the API refused a legitimate
/// call".** `Monarc/Core/Error.h` draws that line itself: `Unsupported` means asking
/// differently might work, where `BackendFailure` means the call was fine and the
/// implementation said no. `VK_ERROR_INCOMPATIBLE_DRIVER`, `VK_ERROR_LAYER_NOT_PRESENT` and
/// `VK_ERROR_EXTENSION_NOT_PRESENT` are all the first meaning, and the first of the three is
/// the standard outcome of `vkCreateInstance` on a machine that has `vulkan-1.dll` and no
/// registered ICD -- which is what a CI runner most plausibly is. Branchability is the whole
/// stated reason `BackendFailure` exists, so a caller written to fall back on `Unsupported`
/// must fire on the result that most deserves it.
///
/// Everything else is `BackendFailure`, `VK_ERROR_OUT_OF_HOST_MEMORY` included: Monarc's
/// `OutOfMemory` means *Monarc's* allocator returned nothing, and a driver's heap running out
/// is a different fact that a caller would handle differently. The message stays the
/// `VkResult`'s own spelling either way -- `ToString` above -- so nothing about what a report
/// says changes with the code.
///
/// A `default` rather than an exhaustive switch, for `ToString`'s reason: `VkResult` has well
/// over a hundred enumerators and three hundred cases would be a claim of completeness
/// nothing verifies.
[[nodiscard]] ErrorCode ToErrorCode(VkResult result);

}  // namespace Monarc::RHI::Detail
