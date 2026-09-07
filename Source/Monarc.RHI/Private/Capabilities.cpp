#include <Monarc/RHI/Capabilities.h>

namespace Monarc::RHI {

namespace {

/// Vulkan 1.3 core, and something to submit to. Both are load-bearing: a device that reports
/// 1.3 but no graphics family is not a device Monarc can render on, and A3's readback test
/// (Task 3) submits to a graphics queue.
[[nodiscard]] bool MeetsBaselineRequirements(const Capabilities& capabilities) {
    return capabilities.apiVersion >= ApiVersion{1, 3, 0} && capabilities.timelineSemaphores &&
           capabilities.dynamicRendering && capabilities.synchronization2 &&
           capabilities.graphicsQueueFamilyCount >= 1;
}

/// What is needed *in addition to* the baseline. Written as the increment rather than the
/// whole set, so DetermineTier's ladder is the only thing that composes them and a
/// requirement cannot be stated twice with two different thresholds.
[[nodiscard]] bool MeetsBindlessRequirements(const Capabilities& capabilities) {
    return capabilities.nonUniformIndexing && capabilities.runtimeDescriptorArray &&
           capabilities.partiallyBoundDescriptors &&
           capabilities.maxBindlessSampledImages >= kMinBindlessSampledImages;
}

[[nodiscard]] bool MeetsAdvancedRequirements(const Capabilities& capabilities) {
    return capabilities.meshShading && capabilities.rayTracing;
}

}  // namespace

// Both switches below are deliberately `default`-less, the same shape and for the same
// reason as Types.cpp's: adding an enumerator becomes a compile error here rather than a
// silent fall-through. C4062 is asked for by name in CMake/MonarcTargetOptions.cmake because
// /W4 does not enable it; Clang's -Wswitch is on at /W4 already. The trailing return still
// has to exist -- a fixed underlying type means the enum can hold a value outside its
// enumerator set, whatever the switch covers.

const char* ToString(DeviceType type) {
    switch (type) {
        case DeviceType::Other:         return "Other";
        case DeviceType::IntegratedGpu: return "IntegratedGpu";
        case DeviceType::DiscreteGpu:   return "DiscreteGpu";
        case DeviceType::VirtualGpu:    return "VirtualGpu";
        case DeviceType::Cpu:           return "Cpu";
    }
    return "<invalid DeviceType>";
}

const char* ToString(CapabilityTier tier) {
    switch (tier) {
        case CapabilityTier::Unsupported: return "Unsupported";
        case CapabilityTier::Baseline:    return "Baseline";
        case CapabilityTier::Bindless:    return "Bindless";
        case CapabilityTier::Advanced:    return "Advanced";
    }
    return "<invalid CapabilityTier>";
}

CapabilityTier DetermineTier(const Capabilities& capabilities) {
    // A ladder of early returns rather than one expression per tier. The shape is the
    // guarantee: reaching Advanced means having already passed the Bindless and Baseline
    // tests, so "meets a tier without meeting the one below it" is unrepresentable here
    // instead of merely being avoided by whoever wrote the requirements.
    if (!MeetsBaselineRequirements(capabilities)) {
        return CapabilityTier::Unsupported;
    }
    if (!MeetsBindlessRequirements(capabilities)) {
        return CapabilityTier::Baseline;
    }
    if (!MeetsAdvancedRequirements(capabilities)) {
        return CapabilityTier::Bindless;
    }
    return CapabilityTier::Advanced;
}

bool MeetsTier(const Capabilities& capabilities, CapabilityTier tier) {
    return DetermineTier(capabilities) >= tier;
}

}  // namespace Monarc::RHI
