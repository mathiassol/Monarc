#pragma once

#include <Monarc/Core/Types.h>

#include <compare>

namespace Monarc::RHI {

/// A graphics API version, spelled the way the APIs spell one.
///
/// Ordered by major, then minor, then patch, through a defaulted `<=>` rather than the free
/// `operator==`/`operator!=` pair Types.h's Extent2D carries. The difference is not style:
/// this type genuinely needs an *order*, because "at least Vulkan 1.3" is the only question
/// anything asks of it, and member-wise lexicographic comparison in declaration order is
/// exactly that order. Declaring a defaulted `<=>` also implicitly declares a defaulted
/// `==`, so equality comes with it.
struct ApiVersion {
    u32 major = 0;
    u32 minor = 0;
    u32 patch = 0;

    constexpr auto operator<=>(const ApiVersion&) const = default;
};

/// What kind of device an adapter is.
///
/// The enumerators are Vulkan's `VkPhysicalDeviceType` set, because that is the distinction
/// every API draws and there is nothing to gain from a different vocabulary. No backend
/// value is encoded here: Monarc.RHI.Vulkan translates, in a `default`-less switch, so a
/// device kind Vulkan adds later cannot silently become `Other`.
///
/// **This is deliberately not part of Capabilities below, and no tier requirement reads
/// it.** The A3 plan listed device type among the tier query's inputs, and measuring the two
/// local devices is what settled otherwise: the Intel UHD 730 -- integrated, one graphics
/// queue, Vulkan 1.3.275 -- reports every descriptor-indexing feature and limit the RTX 3070
/// Ti does (both `maxDescriptorSetUpdateAfterBindSampledImages = 1048576`; see
/// Docs/Status.md). Whether a device is on the far side of a PCIe bus predicts nothing about
/// the tiers Monarc actually defines, so making it a requirement would be a claim the
/// hardware contradicts. It stays on AdapterInfo, where it is a fact about the device that a
/// log line and a device-selection heuristic both want.
enum class DeviceType : u32 {
    Other = 0,
    IntegratedGpu,
    DiscreteGpu,
    VirtualGpu,
    Cpu,
};

/// The enumerator's own spelling, for logs and test failures. Never nullptr. A value outside
/// the enumerator set gets a name of its own rather than "Other" -- see ToString(Format) in
/// Types.h for why conflating a named state with an invalid one misdirects whoever reads the
/// log.
[[nodiscard]] const char* ToString(DeviceType type);

/// What a device can do, as a set of feature bits and limits. Every field is an input to the
/// tier ladder below and nothing else: a field here that no requirement reads would be a
/// capability Monarc claims to consider and does not.
///
/// Populated by a backend from its own queries -- for Vulkan, from
/// `vkGetPhysicalDeviceFeatures2`, `vkGetPhysicalDeviceProperties2`,
/// `vkGetPhysicalDeviceQueueFamilyProperties` and `vkEnumerateDeviceExtensionProperties`,
/// all of which take a physical device and need no logical device.
struct Capabilities {
    /// The version the *device* reports, which is not the version the loader reports for the
    /// instance. On the development machine the instance is 1.4.357 while one of the two
    /// devices is 1.3.275, and it is the device's number that decides what may be called on
    /// it.
    ApiVersion apiVersion = {};

    // Vulkan 1.3 core. Named individually rather than inferred from apiVersion >= 1.3
    // because a device may report a version and still have a driver that does not implement
    // the feature bit -- the bit is what a call actually depends on, and asking for it costs
    // one query.
    bool timelineSemaphores = false;
    bool dynamicRendering   = false;
    bool synchronization2   = false;

    // Descriptor indexing, core since Vulkan 1.2. These four are what a bindless descriptor
    // heap indexed by handle actually needs: an array sized at run time
    // (runtimeDescriptorArray), holes in it (partiallyBoundDescriptors), indices that vary
    // per invocation (nonUniformIndexing), and enough of them to be worth doing.
    bool nonUniformIndexing        = false;
    bool runtimeDescriptorArray    = false;
    bool partiallyBoundDescriptors = false;
    u32  maxBindlessSampledImages  = 0;

    /// Total queue families the device exposes, and how many of them can do graphics. A3
    /// needs one graphics family; the total is here because "how many families are there"
    /// is the question a later phase's async-compute or transfer queue asks first.
    u32 queueFamilyCount         = 0;
    u32 graphicsQueueFamilyCount = 0;

    // Mesh shading and ray tracing, as extension presence on the physical device. These are
    // what separate the two local adapters: the RTX 3070 Ti reports VK_EXT_mesh_shader,
    // VK_KHR_ray_tracing_pipeline and VK_KHR_acceleration_structure, and the Intel UHD 730
    // reports none of them.
    bool meshShading = false;
    bool rayTracing  = false;
};

/// Smallest `maxBindlessSampledImages` Monarc treats as a usable descriptor heap.
///
/// A floor, not a target, and chosen low on purpose. The number that will matter is the size
/// the descriptor heap is actually created with, which is Phase B's decision once there is a
/// shader to index it from; this one only has to separate "descriptor indexing exists in a
/// form worth building on" from "the driver reports the feature and a limit of 96". Both
/// local devices report 1048576, so this floor is not what distinguishes them, and saying so
/// is more useful than a number tuned until the local hardware passed.
inline constexpr u32 kMinBindlessSampledImages = 16384;

/// How capable a device is, as one ordered value features can ask about instead of probing
/// a dozen booleans. See Docs/Rendering/RHI.md.
///
/// **The numeric order is the comparison order, and MeetsTier depends on it** -- it is
/// literally `DetermineTier(capabilities) >= tier`. Tests/TestCapabilities.cpp pins that the
/// enumerators are distinct and ascending for exactly that reason.
///
/// The two ways to break that are not equally dangerous, and only one of them needs a test.
/// Giving two enumerators the *same* value is a compile error, because ToString's
/// `default`-less switch below then has two cases with one value -- verified by doing it:
/// MSVC says "error C2196: case value 'Monarc::RHI::CapabilityTier::Bindless' already used".
/// Giving them *distinct but descending* values compiles perfectly well and silently makes
/// one tier satisfy another, which is what the ordering case in the tests catches -- also
/// verified, by setting `Bindless = 5, Advanced = 3` and watching three cases go red.
///
/// A tier above Advanced arrives with the first feature that needs one. Adding one now would
/// mean shipping a tier no code asks about and no local device can be told apart by, which
/// is the kind of guess a later phase has to undo -- the same reason Types.h's Format list
/// is two entries long.
enum class CapabilityTier : u32 {
    /// Cannot run Monarc's renderer. Every device meets this tier, which is what makes it
    /// the floor rather than a failure: DetermineTier returns it for a device that meets
    /// nothing else, and MeetsTier(anything, Unsupported) is true.
    Unsupported = 0,

    /// Vulkan 1.3 core as Monarc's renderer is written against it: dynamic rendering,
    /// synchronization2, timeline semaphores, and a graphics queue to submit to.
    Baseline,

    /// Baseline, plus descriptor indexing in the shape a bindless heap needs.
    Bindless,

    /// Bindless, plus mesh shading and ray tracing.
    Advanced,
};

/// The enumerator's own spelling. Never nullptr; a value outside the enumerator set gets a
/// name of its own.
[[nodiscard]] const char* ToString(CapabilityTier tier);

/// The highest tier `capabilities` satisfies.
///
/// This is the only place the requirements live. MeetsTier is defined in terms of it rather
/// than the other way round, which is what makes the ladder monotone by construction: there
/// is no way to write a requirement set in which a device meets Advanced but not Bindless,
/// because reaching Advanced means falling through the Bindless test first.
[[nodiscard]] CapabilityTier DetermineTier(const Capabilities& capabilities);

/// Whether `capabilities` is at least `tier`.
[[nodiscard]] bool MeetsTier(const Capabilities& capabilities, CapabilityTier tier);

}  // namespace Monarc::RHI
