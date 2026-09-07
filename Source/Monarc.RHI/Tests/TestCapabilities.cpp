#include <doctest/doctest.h>

#include <Monarc/RHI/Capabilities.h>

#include <iterator>
#include <string_view>

using Monarc::RHI::ApiVersion;
using Monarc::RHI::Capabilities;
using Monarc::RHI::CapabilityTier;
using Monarc::RHI::DetermineTier;
using Monarc::RHI::DeviceType;
using Monarc::RHI::MeetsTier;

namespace {

/// Every tier, ascending. Written out rather than derived from the underlying values, so
/// that the ordering the tests below assert is one this file states and not one it reads back
/// out of the thing under test.
constexpr CapabilityTier kTiers[] = {
    CapabilityTier::Unsupported,
    CapabilityTier::Baseline,
    CapabilityTier::Bindless,
    CapabilityTier::Advanced,
};

/// Capabilities that satisfy every requirement, and are therefore Advanced. Each test below
/// takes this and removes exactly one thing.
[[nodiscard]] Capabilities Everything() {
    Capabilities capabilities{};
    capabilities.apiVersion               = ApiVersion{1, 3, 0};
    capabilities.timelineSemaphores       = true;
    capabilities.dynamicRendering         = true;
    capabilities.synchronization2         = true;
    capabilities.nonUniformIndexing       = true;
    capabilities.runtimeDescriptorArray   = true;
    capabilities.partiallyBoundDescriptors = true;
    capabilities.maxBindlessSampledImages = Monarc::RHI::kMinBindlessSampledImages;
    capabilities.queueFamilyCount         = 3;
    capabilities.graphicsQueueFamilyCount = 1;
    capabilities.meshShading              = true;
    capabilities.rayTracing               = true;
    return capabilities;
}

/// The Intel UHD 730 as measured on the development machine: Vulkan 1.3.275, one graphics
/// family, every descriptor-indexing feature, a million update-after-bind sampled images,
/// and no mesh shading or ray tracing.
[[nodiscard]] Capabilities IntelUhd730() {
    Capabilities capabilities        = Everything();
    capabilities.apiVersion         = ApiVersion{1, 3, 275};
    capabilities.maxBindlessSampledImages = 1048576;
    capabilities.queueFamilyCount   = 2;
    capabilities.meshShading        = false;
    capabilities.rayTracing         = false;
    return capabilities;
}

/// The RTX 3070 Ti as measured: Vulkan 1.4.351, sixteen graphics queues in the first of six
/// families, and mesh shading and ray tracing both present.
[[nodiscard]] Capabilities Rtx3070Ti() {
    Capabilities capabilities             = Everything();
    capabilities.apiVersion               = ApiVersion{1, 4, 351};
    capabilities.maxBindlessSampledImages = 1048576;
    capabilities.queueFamilyCount         = 6;
    return capabilities;
}

}  // namespace

TEST_CASE("the tiers are distinct and strictly ascending") {
    // MeetsTier is literally `DetermineTier(capabilities) >= tier`, so the enumerator values
    // *are* the comparison. Distinct but descending values -- Bindless = 5 and Advanced = 3
    // -- compile without a warning and make a device that meets Bindless also report as
    // meeting Advanced. Verified by doing exactly that: this case and two of the knock-out
    // cases below went red together.
    //
    // The other way to break the order, giving two enumerators the same value, never reaches
    // a test: ToString's `default`-less switch in Capabilities.cpp then has two cases with
    // one value and the module does not compile (MSVC C2196). Also verified.
    for (Monarc::usize i = 1; i < std::size(kTiers); ++i) {
        CHECK(kTiers[i - 1] < kTiers[i]);
        CHECK(kTiers[i - 1] != kTiers[i]);
    }

    // Antisymmetry and transitivity over every pair and triple. Cheap, and it is what makes
    // "total order" a checked claim rather than a word in a comment.
    for (const CapabilityTier a : kTiers) {
        for (const CapabilityTier b : kTiers) {
            CHECK(((a < b) + (a == b) + (a > b)) == 1);
            for (const CapabilityTier c : kTiers) {
                if (a < b && b < c) {
                    CHECK(a < c);
                }
            }
        }
    }
}

TEST_CASE("MeetsTier is monotone: meeting a tier means meeting every tier below it") {
    // Checked across a spread of capability sets rather than one, because monotonicity is a
    // property of the requirement ladder and not of any single device.
    const Capabilities sets[] = {Capabilities{}, Everything(), IntelUhd730(), Rtx3070Ti()};
    for (const Capabilities& capabilities : sets) {
        const CapabilityTier reached = DetermineTier(capabilities);
        for (const CapabilityTier tier : kTiers) {
            CHECK(MeetsTier(capabilities, tier) == (tier <= reached));
        }
    }
}

TEST_CASE("a default-constructed capability set is Unsupported, and meets only that") {
    const Capabilities nothing{};
    CHECK(DetermineTier(nothing) == CapabilityTier::Unsupported);
    CHECK(MeetsTier(nothing, CapabilityTier::Unsupported));
    CHECK_FALSE(MeetsTier(nothing, CapabilityTier::Baseline));
    CHECK_FALSE(MeetsTier(nothing, CapabilityTier::Bindless));
    CHECK_FALSE(MeetsTier(nothing, CapabilityTier::Advanced));
}

TEST_CASE("a device missing one baseline requirement never reaches Baseline") {
    // One subcase per requirement. Each starts from a set that *does* reach Advanced and
    // removes exactly one thing, so a requirement the ladder forgot to read shows up as this
    // subcase reporting Advanced instead of Unsupported.
    SUBCASE("an API version below 1.3") {
        Capabilities capabilities = Everything();
        capabilities.apiVersion   = ApiVersion{1, 2, 999};
        CHECK(DetermineTier(capabilities) == CapabilityTier::Unsupported);
        CHECK_FALSE(MeetsTier(capabilities, CapabilityTier::Baseline));
    }
    SUBCASE("no timeline semaphores") {
        Capabilities capabilities       = Everything();
        capabilities.timelineSemaphores = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Unsupported);
    }
    SUBCASE("no dynamic rendering") {
        Capabilities capabilities     = Everything();
        capabilities.dynamicRendering = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Unsupported);
    }
    SUBCASE("no synchronization2") {
        Capabilities capabilities     = Everything();
        capabilities.synchronization2 = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Unsupported);
    }
    SUBCASE("no graphics queue family") {
        Capabilities capabilities             = Everything();
        capabilities.graphicsQueueFamilyCount = 0;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Unsupported);
    }
}

TEST_CASE("a device missing one bindless requirement stops at Baseline") {
    SUBCASE("no non-uniform indexing") {
        Capabilities capabilities     = Everything();
        capabilities.nonUniformIndexing = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Baseline);
        CHECK_FALSE(MeetsTier(capabilities, CapabilityTier::Bindless));
    }
    SUBCASE("no runtime descriptor array") {
        Capabilities capabilities         = Everything();
        capabilities.runtimeDescriptorArray = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Baseline);
    }
    SUBCASE("no partially bound descriptors") {
        Capabilities capabilities            = Everything();
        capabilities.partiallyBoundDescriptors = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Baseline);
    }
    SUBCASE("a descriptor heap one below the floor") {
        // One below, not zero: the comparison is `>=`, and off-by-one in either direction is
        // the mistake a threshold invites.
        Capabilities capabilities = Everything();
        capabilities.maxBindlessSampledImages = Monarc::RHI::kMinBindlessSampledImages - 1;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Baseline);
    }
    SUBCASE("a descriptor heap exactly at the floor") {
        Capabilities capabilities = Everything();
        capabilities.maxBindlessSampledImages = Monarc::RHI::kMinBindlessSampledImages;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Advanced);
    }
}

TEST_CASE("a device missing mesh shading or ray tracing stops at Bindless") {
    SUBCASE("no mesh shading") {
        Capabilities capabilities = Everything();
        capabilities.meshShading  = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Bindless);
        CHECK_FALSE(MeetsTier(capabilities, CapabilityTier::Advanced));
        CHECK(MeetsTier(capabilities, CapabilityTier::Bindless));
    }
    SUBCASE("no ray tracing") {
        Capabilities capabilities = Everything();
        capabilities.rayTracing   = false;
        CHECK(DetermineTier(capabilities) == CapabilityTier::Bindless);
    }
}

TEST_CASE("the two development-machine devices land where their measurements say") {
    // Not a claim about hardware -- these two capability sets are transcribed from
    // Docs/Status.md and from vulkaninfo, and what is under test is that the ladder sorts
    // them the way the measurements imply. Monarc.RHI.Vulkan's device test is where the
    // *measurement* is checked against the real devices.
    //
    // Both reach Bindless, which is the finding that made a third tier necessary: descriptor
    // indexing does not separate an RTX 3070 Ti from an Intel UHD 730 on this machine, and
    // mesh shading and ray tracing do.
    CHECK(DetermineTier(IntelUhd730()) == CapabilityTier::Bindless);
    CHECK(DetermineTier(Rtx3070Ti()) == CapabilityTier::Advanced);
    CHECK(MeetsTier(IntelUhd730(), CapabilityTier::Bindless));
    CHECK_FALSE(MeetsTier(IntelUhd730(), CapabilityTier::Advanced));
}

TEST_CASE("a version compares by major, then minor, then patch") {
    CHECK(ApiVersion{1, 3, 275} >= ApiVersion{1, 3, 0});
    CHECK(ApiVersion{1, 4, 351} > ApiVersion{1, 3, 275});
    CHECK(ApiVersion{2, 0, 0} > ApiVersion{1, 9, 999});
    CHECK(ApiVersion{1, 3, 0} == ApiVersion{1, 3, 0});
    CHECK_FALSE(ApiVersion{1, 2, 999} >= ApiVersion{1, 3, 0});

    // A patch number larger than the minor number it sits beside would compare wrongly if
    // the members were declared, or compared, in the wrong order.
    CHECK(ApiVersion{1, 3, 0} < ApiVersion{1, 4, 0});
    CHECK_FALSE(ApiVersion{1, 3, 999} > ApiVersion{1, 4, 0});
}

TEST_CASE("ToString names every tier and every device type") {
    CHECK(std::string_view(Monarc::RHI::ToString(CapabilityTier::Unsupported)) == "Unsupported");
    CHECK(std::string_view(Monarc::RHI::ToString(CapabilityTier::Baseline)) == "Baseline");
    CHECK(std::string_view(Monarc::RHI::ToString(CapabilityTier::Bindless)) == "Bindless");
    CHECK(std::string_view(Monarc::RHI::ToString(CapabilityTier::Advanced)) == "Advanced");

    CHECK(std::string_view(Monarc::RHI::ToString(DeviceType::Other)) == "Other");
    CHECK(std::string_view(Monarc::RHI::ToString(DeviceType::IntegratedGpu)) == "IntegratedGpu");
    CHECK(std::string_view(Monarc::RHI::ToString(DeviceType::DiscreteGpu)) == "DiscreteGpu");
    CHECK(std::string_view(Monarc::RHI::ToString(DeviceType::VirtualGpu)) == "VirtualGpu");
    CHECK(std::string_view(Monarc::RHI::ToString(DeviceType::Cpu)) == "Cpu");
}

TEST_CASE("a value outside the enumerator set is named as invalid, not as a real tier") {
    // Both enums have a fixed underlying type, so they can hold this. Reporting it as
    // "Unsupported" or "Other" would send whoever read the log after a device problem that
    // does not exist -- see ToString(Format) in Types.h.
    CHECK(std::string_view(Monarc::RHI::ToString(static_cast<CapabilityTier>(99))) ==
          "<invalid CapabilityTier>");
    CHECK(std::string_view(Monarc::RHI::ToString(static_cast<DeviceType>(99))) ==
          "<invalid DeviceType>");
}
