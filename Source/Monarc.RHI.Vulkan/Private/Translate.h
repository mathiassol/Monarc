#pragma once

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Capabilities.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>
#include <Monarc/RHI/Types.h>

#include <vulkan/vulkan.h>

namespace Monarc::RHI::Detail {

/// Pure functions over Vulkan values: RHI enum to Vulkan enum and back, and the few
/// predicates the backend asks of what Vulkan handed it.
///
/// **The membership rule is the property, not the subject matter.** Every function here is
/// total, allocates nothing, and touches no Vulkan *state* -- no instance, no device, no
/// entry-point table -- which is what lets CI test the whole file with no Vulkan present at
/// all. That is why `ContainsExtension` and `SeverityToLogLevel` live here rather than in
/// VulkanBackend.cpp's anonymous namespace, where they were unreachable by any test and
/// mutations to them passed both suites.
///
/// The barrier translators at the foot of this file are in it under that same rule, and one of
/// them is worth being precise about: they take a `VkBuffer` or a `VkImage` and copy it into
/// the structure they build. That is not touching state -- the handle is never dereferenced,
/// never dispatched through, and never compared against anything -- so `VK_NULL_HANDLE` is a
/// legitimate argument, which is exactly how the device-free tests round-trip a barrier with no
/// device to make a real image on. Resolving an RHI handle *to* a `VkImage` is device state and
/// stays in VulkanDevice.cpp, on the other side of these calls.
///
/// The `default`-less switches are the other point: adding an RHI enumerator becomes a
/// compile error in Translate.cpp rather than a silent fall-through to whatever the trailing
/// return says.

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
/// The numeric value is not lost when the fallback is hit: every failure path that formats
/// this string -- `VulkanBackend`'s call sites, `VulkanDeviceState::FailVk`,
/// `VulkanSwapchainState::FailVk`, `CreatePlatformSurface`, and the teardown's
/// `vkDeviceWaitIdle` -- puts the result's integer value beside it, which is what makes an
/// unrecognised result still actionable. The one exception is `ISwapchain::Acquire`'s
/// timed-out branch, which names `VK_TIMEOUT` and `VK_NOT_READY` and nothing else, and both
/// are in the table.
///
/// The list grows when a call that can return something new is added. Task 3 brought
/// `VK_ERROR_DEVICE_LOST` and four others; **Task 4 brought `VK_ERROR_SURFACE_LOST_KHR`,
/// `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR` and `VK_NOT_READY`, and this sentence used to predict
/// `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` instead.** Neither of those is in the
/// table and neither should be: both are handled by name at every call site that can produce
/// them -- they are outcomes, not failures -- so no path stringifies either. Translate.cpp
/// says so where the rows are.
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

/// Whether `extensions` contains one named exactly `name`.
///
/// **Exactly, and that is the whole of it.** Vulkan's extension names nest -- `VK_KHR_surface`
/// is a prefix of `VK_KHR_surface_maintenance1`, and `VK_EXT_mesh_shader` differs from
/// `VK_NV_mesh_shader` by two letters -- so a prefix or substring match would report a
/// capability the implementation does not have. Tasks 3 and 4 bring many more of these
/// queries, which is why this is a tested function rather than a loop at each call site.
[[nodiscard]] bool ContainsExtension(const Array<VkExtensionProperties>& extensions,
                                     const char*                        name);

/// Whether `layers` contains one named exactly `name`. `ContainsExtension`'s reasoning,
/// applied to `VkLayerProperties::layerName`.
[[nodiscard]] bool ContainsLayer(const Array<VkLayerProperties>& layers, const char* name);

/// The `LogLevel` a debug-utils message of `severity` is logged at.
///
/// **This threshold decides whether a validation error is printed at all.** The
/// `VulkanValidation` category's own minimum is Warning, so a severity that mapped one level
/// too low would be filtered out at the sink -- an ERROR reported as Info disappears, which is
/// the silent version of the failure the fatal messenger exists to prevent. The tests pin the
/// mapping and pin its *range*: the switch in `DebugMessengerCallback` is `default`-less so
/// that a level added to `LogLevel` is a compile error there, and the range is what says which
/// of its cases are reachable.
///
/// Ordered bit tests rather than a switch: `VkDebugUtilsMessageSeverityFlagBitsEXT` is a
/// flag-bits enum whose enumerators are individual bits, a caller may pass more than one, and
/// a severity Vulkan adds later is a new bit that no switch would have covered either.
[[nodiscard]] LogLevel SeverityToLogLevel(VkDebugUtilsMessageSeverityFlagBitsEXT severity);

// ---------------------------------------------------------------------------------------
// The barrier model -- ADR-0005, as Monarc/RHI/Barrier.h declares it.
//
// **Every enumerator is translated, though A3 records only three barriers.** The plan asks
// for exactly that: translation for all of the model, two barriers used, and the rest carried
// by the pure-function tests in Tests/TestVulkanBarrierTranslate.cpp. So the coverage claim
// here is a test's, not a caller's.
//
// The forward direction is split in two for each flag set, because a mask and one bit are the
// same C++ type and cannot be overloads. `ToVulkanBit` is the exhaustive `default`-less switch
// over the enumerators; `ToVulkan` walks a mask's set bits through it and ORs the results, so
// `ToVulkan(Copy | Blit)` is the union and `ToVulkan(None)` is zero. Callers want `ToVulkan`.
//
// The reverse direction is `FromVulkanStages` / `FromVulkanAccess` rather than two overloads
// of one name, because `VkPipelineStageFlags2` and `VkAccessFlags2` are both `VkFlags64` and
// are therefore the same type: an overload pair would not compile, and one that did would be
// picked by the argument's width rather than by its meaning.
// ---------------------------------------------------------------------------------------

/// The Vulkan stage bit `stage` names, for one enumerator.
///
/// **An unrecognised value translates to `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`, and that is
/// the opposite of what `ToVulkan(Format)` above does with one.** A format has an
/// obviously-invalid answer -- `VK_FORMAT_UNDEFINED` makes Vulkan reject the call, loudly. A
/// stage mask has none: every bit pattern is a legal mask, so the choice is between
/// under-synchronising and over-synchronising, and only one of those is safe. Over-
/// synchronising is indistinguishable from `PipelineStage::AllCommands` in the result, which
/// is accepted: both mean "wait for everything".
///
/// Reachable only through a cast -- `PipelineStage` has a fixed underlying type -- or through a
/// mask carrying a bit no enumerator names, which is the same thing. Tests/ pins it.
[[nodiscard]] VkPipelineStageFlags2 ToVulkanBit(PipelineStage stage);

/// The union of `ToVulkanBit` over every bit set in `stages`. `PipelineStage::None` is zero,
/// which is `VK_PIPELINE_STAGE_2_NONE`.
[[nodiscard]] VkPipelineStageFlags2 ToVulkan(PipelineStage stages);

/// Every stage in `stages` that Monarc models, as a `PipelineStage` mask.
///
/// **Not exhaustive, and cannot be**, for the reason `FromVulkan(VkFormat)` gives: Vulkan
/// names far more stages than Monarc does. A bit Monarc does not model is dropped rather than
/// approximated, because there is no honest stage to approximate it with -- and the property
/// the tests pin is the one that matters, that `FromVulkanStages(ToVulkan(s)) == s` for every
/// `s` Monarc does model.
[[nodiscard]] PipelineStage FromVulkanStages(VkPipelineStageFlags2 stages);

/// `ToVulkanBit(PipelineStage)`'s counterpart for access. An unrecognised value becomes
/// `VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT` -- the conservative answer,
/// for the reason given there.
[[nodiscard]] VkAccessFlags2 ToVulkanBit(Access access);

/// The union of `ToVulkanBit` over every bit set in `accesses`.
[[nodiscard]] VkAccessFlags2 ToVulkan(Access accesses);

/// Every access in `accesses` that Monarc models. `FromVulkanStages`'s note applies.
[[nodiscard]] Access FromVulkanAccess(VkAccessFlags2 accesses);

/// The Vulkan layout `layout` names. `VK_IMAGE_LAYOUT_MAX_ENUM` for a value outside the
/// enumerator set -- an answer Vulkan rejects wherever it is used, which is what a layout's
/// equivalent of `VK_FORMAT_UNDEFINED` has to be: `VK_IMAGE_LAYOUT_UNDEFINED` would be
/// accepted as a `layoutBefore` and would silently discard the texture's contents.
[[nodiscard]] VkImageLayout ToVulkan(TextureLayout layout);

/// The RHI layout `layout` names, or `TextureLayout::Undefined` if Monarc does not model it.
/// Not exhaustive; `FromVulkan(VkFormat)`'s reasoning.
[[nodiscard]] TextureLayout FromVulkan(VkImageLayout layout);

/// A global memory barrier. `sType` is set: a structure handed to `vkCmdPipelineBarrier2` with
/// a zero `sType` is rejected, and nothing else in the pipeline would notice the omission.
///
/// A barrier that changes nothing translates to a real `VkMemoryBarrier2` with `NONE` masks
/// rather than to nothing at all. Dropping it is not this function's decision to make -- see
/// `ICommandList::Barrier`.
[[nodiscard]] VkMemoryBarrier2 ToVulkan(const GlobalBarrier& barrier);

/// A buffer memory barrier over the whole of `buffer` -- offset zero, `VK_WHOLE_SIZE`.
///
/// A sub-range arrives with a caller that has one; A3's readback barriers the buffer it just
/// filled, entire. Queue family indices are `VK_QUEUE_FAMILY_IGNORED` on both sides: Monarc
/// has one queue, so there is no ownership to transfer.
[[nodiscard]] VkBufferMemoryBarrier2 ToVulkan(const BufferBarrier& barrier, VkBuffer buffer);

/// An image memory barrier over the whole of `image` -- every mip level, every array layer.
///
/// The aspect is `VK_IMAGE_ASPECT_COLOR_BIT`, and that is a consequence of `Format` in
/// Types.h having no depth or stencil format rather than a simplification: there is no
/// texture in Monarc today whose aspect could be anything else. A depth aspect arrives with
/// the first depth format, alongside the aspect-aware size query Types.h already says that
/// format needs.
///
/// **It is hard-coded and not asserted, so a depth `Format` makes a depth barrier silently
/// wrong rather than loudly refused, and that is the cost worth naming.** `TextureLayout`'s
/// note in Monarc/RHI/Barrier.h counts this as the second of the two things a depth pass has
/// to fix -- the first being that `TextureUsage` cannot express
/// `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, which is why the two depth layouts cannot be
/// reached validly today at all.
[[nodiscard]] VkImageMemoryBarrier2 ToVulkan(const TextureBarrier& barrier, VkImage image);

/// The RHI barrier `barrier` came from. The reverse of `ToVulkan(const GlobalBarrier&)`, and
/// what makes "the struct round-trips through translation unchanged" a statement a test can
/// make rather than an inspection of two switches.
[[nodiscard]] GlobalBarrier FromVulkan(const VkMemoryBarrier2& barrier);

/// The RHI barrier `barrier` came from, over `buffer`.
///
/// The handle is a parameter because it is not in the Vulkan structure: that carries a
/// `VkBuffer`, and turning one back into a pool handle would need the device's pools. So the
/// round-trip this supports is over the sync and access fields, and that the `VkBuffer` was
/// carried across at all is asserted directly on `ToVulkan`'s result instead.
[[nodiscard]] BufferBarrier FromVulkan(const VkBufferMemoryBarrier2& barrier,
                                       BufferHandle                  buffer);

/// The RHI barrier `barrier` came from, over `texture`. See `FromVulkan(const
/// VkBufferMemoryBarrier2&, BufferHandle)` for why the handle is a parameter.
[[nodiscard]] TextureBarrier FromVulkan(const VkImageMemoryBarrier2& barrier,
                                        TextureHandle                texture);

// ---------------------------------------------------------------------------------------
// Resource creation and rendering. Not part of ADR-0005's model, and governed by the
// narrower membership rule Monarc/RHI/Device.h states for its own enums: an enumerator
// arrives with the resource that needs one.
// ---------------------------------------------------------------------------------------

/// The Vulkan image usage bit `usage` names, for one enumerator. Zero for an unrecognised
/// value, which is `ToVulkanBit(PipelineStage)`'s opposite and deliberately so: a dropped
/// usage is caught by validation at the first use of the resource, where a dropped stage would
/// be a synchronisation hole nothing reports.
[[nodiscard]] VkImageUsageFlags ToVulkanBit(TextureUsage usage);

/// The union of `ToVulkanBit` over every bit set in `usage`.
[[nodiscard]] VkImageUsageFlags ToVulkan(TextureUsage usage);

/// `ToVulkanBit(TextureUsage)` for buffers.
[[nodiscard]] VkBufferUsageFlags ToVulkanBit(BufferUsage usage);

/// The union of `ToVulkanBit` over every bit set in `usage`.
[[nodiscard]] VkBufferUsageFlags ToVulkan(BufferUsage usage);

/// The Vulkan load operation `loadOp` names.
///
/// No reverse. The round-trip requirement is ADR-0005's, about barriers; for these two the
/// property worth pinning is that each enumerator maps to the *right* Vulkan value and that no
/// two share one, which a test asserts on the forward direction alone. A reverse function
/// existing only to be called by a test would be the machinery, not the coverage.
[[nodiscard]] VkAttachmentLoadOp ToVulkan(LoadOp loadOp);

/// The Vulkan store operation `storeOp` names. `ToVulkan(LoadOp)`'s note about the missing
/// reverse applies.
[[nodiscard]] VkAttachmentStoreOp ToVulkan(StoreOp storeOp);

/// The memory property bits a buffer in `location` must be allocated from.
///
/// `HostVisible` asks for `HOST_COHERENT` as well as `HOST_VISIBLE`, which is what lets
/// `IDevice::MapBufferForRead` return a span a caller may simply read: without coherence a
/// mapped read needs `vkInvalidateMappedMemoryRanges` around it, and every Vulkan
/// implementation is required to expose at least one memory type with both bits.
[[nodiscard]] VkMemoryPropertyFlags ToVulkan(MemoryLocation location);

/// No memory type has this index. Returned by `FindMemoryType` when none matches.
inline constexpr u32 kNoMemoryType = static_cast<u32>(-1);

/// Index of the first memory type that is both allowed by `allowedTypeBits` and has every bit
/// of `required` set, or `kNoMemoryType`.
///
/// `allowedTypeBits` is `VkMemoryRequirements::memoryTypeBits` -- a mask over
/// `properties.memoryTypes`, bit *i* meaning type *i* is usable for that resource. Both halves
/// have to hold: a type the resource cannot use is no good however well its properties match,
/// and a matching-but-unusable type is exactly the mistake that produces a mapped pointer into
/// device-local memory.
///
/// **First match, not best.** Vulkan requires implementations to list memory types in an order
/// where the earlier of two equally-suitable types is no worse -- the spec's own recommended
/// search is this one -- so "first" is the ordering the driver chose rather than an arbitrary
/// pick. `required` is a *subset* test and not equality: a host-visible, host-coherent type
/// that is also device-local satisfies a HostVisible request, and on an integrated part like
/// the Intel UHD 730 that is the only kind there is.
///
/// Pure, and in this file rather than in VulkanDevice.cpp, under the membership rule at the
/// top: it reads a properties struct the caller hands it and touches no device. That is what
/// makes it testable in CI against memory layouts this machine does not have -- an
/// all-device-local device, a type whose bit is masked out -- which is the half a real GPU
/// cannot exercise.
[[nodiscard]] u32 FindMemoryType(const VkPhysicalDeviceMemoryProperties& properties,
                                 u32 allowedTypeBits, VkMemoryPropertyFlags required);

// ---------------------------------------------------------------------------------------
// Swapchain negotiation. Two decisions that are wholly determined by what a surface reports,
// so they are functions of it rather than lines inside `vkCreateSwapchainKHR`'s caller.
//
// **They are here for the reason `FindMemoryType` is: this is the half of swapchain creation
// that CI can run.** A swapchain needs a window, a surface, a device and a presenting queue
// family, none of which a GitHub runner has -- but the arithmetic that turns
// `VkSurfaceCapabilitiesKHR` into an image count and an extent needs none of them, and it is
// the part with edge cases: an unbounded maximum, a maximum equal to the minimum, the
// "surface has no preference" sentinel, and clamping a requested size into a range. Every one
// of those is a driver behaviour this machine does not exhibit, so a device test could not
// reach them even with a GPU present.
// ---------------------------------------------------------------------------------------

/// How many images to ask a swapchain for, given what the surface reports.
///
/// `minImageCount + 1`, so that the application can be working on one image while the
/// presentation engine displays another -- the minimum alone leaves the CPU blocked in
/// `vkAcquireNextImageKHR` for most of every frame. Clamped to `maxImageCount` when that is
/// non-zero; zero is Vulkan's spelling of "no limit" and must not be clamped to.
///
/// **This is not `kFramesInFlight` and must never be derived from it.** Swapchain images
/// belong to the surface and frames in flight belong to Monarc's own pacing; on this machine
/// both surfaces report a minimum of 2 with a maximum well above 3 -- 8 on the NVIDIA surface
/// and 64 on the Intel one -- so the clamp never bites and the two numbers happen to be 3 and
/// 2. A driver reporting a minimum of 3 would make them 4 and 2, and code that had tied them
/// together would be wrong only on that machine.
///
/// (This said "a minimum of 2 and no maximum" until a review checked it against
/// `VulkanSwapchain.cpp`'s own creation log, which prints both. Docs/Status.md had the maxima
/// right; the header was the stale copy. The derived 3 was correct either way -- the premise
/// was not, and "no maximum" is a *different* branch of this function from the one this
/// machine takes.)
[[nodiscard]] u32 ChooseSwapchainImageCount(u32 minImageCount, u32 maxImageCount);

/// The extent to create a swapchain at.
///
/// `capabilities.currentExtent` when the surface reports one, and `requested` clamped into
/// `[minImageExtent, maxImageExtent]` when it reports the "no preference" sentinel
/// (`0xFFFFFFFF` in both dimensions).
///
/// **The surface's own answer wins where it gives one, and on Windows it always does**:
/// `VkSurfaceCapabilitiesKHR::currentExtent` is the window's client rect, so a swapchain
/// created at any other size is `VUID-VkSwapchainCreateInfoKHR-imageExtent-01274`. The
/// sentinel branch is therefore unreachable on this platform and is written because it is
/// reachable on others -- Wayland reports it -- and because a pure function that handled only
/// the local case would be a trap for whoever ports this.
///
/// Returns an empty extent when the surface's own is empty, which is what a minimised window
/// reports. The caller refuses that rather than this function inventing a size: see
/// `ISwapchain::Recreate`.
[[nodiscard]] Extent2D ChooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                             Extent2D                        requested);

}  // namespace Monarc::RHI::Detail
