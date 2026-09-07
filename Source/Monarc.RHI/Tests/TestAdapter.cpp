#include <doctest/doctest.h>

#include <Monarc/RHI/Adapter.h>

#include <cstring>
#include <span>
#include <string_view>

using Monarc::RHI::AdapterInfo;
using Monarc::RHI::AdapterUuid;
using Monarc::RHI::DeduplicateAdapters;
using Monarc::RHI::DeviceType;

namespace {

/// The RTX 3070 Ti's real deviceUUID on the development machine, recorded in Docs/Status.md
/// as `759c8156-7b91-7099-6511-5bd91de66f76`.
constexpr AdapterUuid kNvidiaUuid{
    {0x75, 0x9c, 0x81, 0x56, 0x7b, 0x91, 0x70, 0x99, 0x65, 0x11, 0x5b, 0xd9, 0x1d, 0xe6, 0x6f,
     0x76}};

/// The Intel UHD 730's real deviceUUID -- `86808b4c-0400-0000-0002-000000000000` -- which is
/// the UUID all *four* Intel entries in that machine's `vkEnumeratePhysicalDevices` result
/// report. The four-duplicate case below is built from this rather than from a made-up value
/// on purpose: it is a regression test for an observed defect, and the trailing run of zero
/// bytes is part of what makes it worth pinning, since a comparison that stopped at the first
/// zero byte would still pass on a prettier UUID.
constexpr AdapterUuid kIntelUuid{
    {0x86, 0x80, 0x8b, 0x4c, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00}};

/// Builds an adapter with a distinguishable name, so "keeps the first" is observable rather
/// than merely plausible: four entries sharing one UUID are otherwise indistinguishable, and
/// a test that could not tell which survived would pass whichever one did.
[[nodiscard]] AdapterInfo Adapter(const char* name, const AdapterUuid& uuid) {
    AdapterInfo info{};
    const std::string_view text(name);
    const Monarc::usize    length =
        text.size() < Monarc::RHI::kMaxAdapterNameLength - 1
            ? text.size()
            : Monarc::RHI::kMaxAdapterNameLength - 1;
    std::memcpy(info.name, text.data(), length);
    info.name[length] = '\0';
    info.uuid         = uuid;
    return info;
}

[[nodiscard]] std::string_view NameOf(const AdapterInfo& info) {
    return std::string_view(info.name);
}

}  // namespace

TEST_CASE("four entries sharing one UUID collapse to one, and the first is the survivor") {
    // Exactly the shape of the development machine's raw enumeration: the NVIDIA card, then
    // four Intel entries with one UUID between them.
    AdapterInfo adapters[] = {
        Adapter("NVIDIA GeForce RTX 3070 Ti", kNvidiaUuid),
        Adapter("Intel(R) UHD Graphics 730 #1", kIntelUuid),
        Adapter("Intel(R) UHD Graphics 730 #2", kIntelUuid),
        Adapter("Intel(R) UHD Graphics 730 #3", kIntelUuid),
        Adapter("Intel(R) UHD Graphics 730 #4", kIntelUuid),
    };

    const Monarc::usize kept = DeduplicateAdapters(std::span<AdapterInfo>(adapters));

    REQUIRE(kept == 2);
    CHECK(NameOf(adapters[0]) == "NVIDIA GeForce RTX 3070 Ti");
    CHECK(NameOf(adapters[1]) == "Intel(R) UHD Graphics 730 #1");
    CHECK(adapters[1].uuid == kIntelUuid);
}

TEST_CASE("order is preserved when nothing is a duplicate") {
    AdapterUuid third = kIntelUuid;
    third.bytes[0]    = 0x11;

    AdapterInfo adapters[] = {
        Adapter("first", kNvidiaUuid),
        Adapter("second", kIntelUuid),
        Adapter("third", third),
    };

    const Monarc::usize kept = DeduplicateAdapters(std::span<AdapterInfo>(adapters));

    REQUIRE(kept == 3);
    CHECK(NameOf(adapters[0]) == "first");
    CHECK(NameOf(adapters[1]) == "second");
    CHECK(NameOf(adapters[2]) == "third");
}

TEST_CASE("a duplicate that is not adjacent to its original still collapses, in place") {
    // A B A B A. Compacting has to move survivors *forward* here, which the adjacent-only
    // case above never exercises: an implementation that only compared each entry with the
    // one before it would return 5.
    AdapterInfo adapters[] = {
        Adapter("nvidia-1", kNvidiaUuid), Adapter("intel-1", kIntelUuid),
        Adapter("nvidia-2", kNvidiaUuid), Adapter("intel-2", kIntelUuid),
        Adapter("nvidia-3", kNvidiaUuid),
    };

    const Monarc::usize kept = DeduplicateAdapters(std::span<AdapterInfo>(adapters));

    REQUIRE(kept == 2);
    CHECK(NameOf(adapters[0]) == "nvidia-1");
    CHECK(NameOf(adapters[1]) == "intel-1");
}

TEST_CASE("two UUIDs differing by a single byte are two adapters, at every byte position") {
    // Every position, not just the first and last: the loop in operator== is the thing under
    // test, and a comparison written with the wrong bound or a `<=`/`<` slip would pass at
    // some positions and fail at others.
    for (Monarc::usize position = 0; position < AdapterUuid::kSize; ++position) {
        AdapterUuid altered = kIntelUuid;
        altered.bytes[position] ^= 0x01;
        REQUIRE(altered != kIntelUuid);

        AdapterInfo adapters[] = {
            Adapter("original", kIntelUuid),
            Adapter("altered", altered),
        };

        const Monarc::usize kept = DeduplicateAdapters(std::span<AdapterInfo>(adapters));

        CHECK(kept == 2);
        CHECK(NameOf(adapters[0]) == "original");
        CHECK(NameOf(adapters[1]) == "altered");
    }
}

TEST_CASE("an empty list deduplicates to nothing without touching anything") {
    CHECK(DeduplicateAdapters(std::span<AdapterInfo>{}) == 0);

    // A non-empty buffer viewed as an empty span: nothing may be written through it. A
    // length-zero std::span over a null pointer and one over a real pointer take different
    // paths through most loops, and only one of them is what enumeration actually produces
    // when a machine reports no devices.
    AdapterInfo adapters[] = {Adapter("untouched", kIntelUuid)};
    CHECK(DeduplicateAdapters(std::span<AdapterInfo>(adapters, 0)) == 0);
    CHECK(NameOf(adapters[0]) == "untouched");
}

TEST_CASE("a single adapter survives untouched") {
    AdapterInfo adapters[] = {Adapter("only", kNvidiaUuid)};
    REQUIRE(DeduplicateAdapters(std::span<AdapterInfo>(adapters)) == 1);
    CHECK(NameOf(adapters[0]) == "only");
    CHECK(adapters[0].uuid == kNvidiaUuid);
}

TEST_CASE("a formatted UUID matches what vulkaninfo prints for the same device") {
    // The point of the 8-4-4-4-12 grouping is that these two strings can be compared with a
    // vulkaninfo dump by eye. Both values are the ones recorded in Docs/Status.md.
    CHECK(std::string_view(Monarc::RHI::ToString(kNvidiaUuid).text) ==
          "759c8156-7b91-7099-6511-5bd91de66f76");
    CHECK(std::string_view(Monarc::RHI::ToString(kIntelUuid).text) ==
          "86808b4c-0400-0000-0002-000000000000");
}

TEST_CASE("a formatted UUID is always the documented length and null-terminated") {
    AdapterUuid uuid{};
    for (Monarc::usize i = 0; i < AdapterUuid::kSize; ++i) {
        uuid.bytes[i] = static_cast<Monarc::u8>(0xF0 | i);
    }
    const Monarc::RHI::AdapterUuidString text = Monarc::RHI::ToString(uuid);

    // 32 hex digits plus four dashes: the terminator sits at exactly the last index, so a
    // formatter that wrote one character too many would have nowhere to put it.
    CHECK(std::string_view(text.text).size() == Monarc::RHI::kAdapterUuidStringLength - 1);
    CHECK(text.text[Monarc::RHI::kAdapterUuidStringLength - 1] == '\0');
    CHECK(std::string_view(text.text) == "f0f1f2f3-f4f5-f6f7-f8f9-fafbfcfdfeff");
}

TEST_CASE("deduplication ignores everything except the UUID") {
    // Two entries with different names, types, vendor ids and tiers, and one UUID. Identity
    // is the UUID and nothing else -- the whole reason DeduplicateAdapters exists is that the
    // four Intel entries differ in nothing a human would notice either.
    AdapterInfo first  = Adapter("Intel(R) UHD Graphics 730", kIntelUuid);
    AdapterInfo second = Adapter("Something Else Entirely", kIntelUuid);
    first.type         = DeviceType::IntegratedGpu;
    second.type        = DeviceType::DiscreteGpu;
    first.vendorId     = 0x8086;
    second.vendorId    = 0x10de;
    second.tier        = Monarc::RHI::CapabilityTier::Advanced;

    AdapterInfo adapters[] = {first, second};
    REQUIRE(DeduplicateAdapters(std::span<AdapterInfo>(adapters)) == 1);
    CHECK(NameOf(adapters[0]) == "Intel(R) UHD Graphics 730");
    CHECK(adapters[0].type == DeviceType::IntegratedGpu);
}
