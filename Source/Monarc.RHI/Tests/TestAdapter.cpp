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
///
/// Through `CopyAdapterName` rather than reimplementing the truncation, which is what this
/// helper used to do: two copies of one bound is one more than can be kept in step, and
/// neither copy tested the other.
[[nodiscard]] AdapterInfo Adapter(const char* name, const AdapterUuid& uuid) {
    AdapterInfo info{};
    Monarc::RHI::CopyAdapterName(info.name, name);
    info.uuid = uuid;
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

TEST_CASE("a duplicate that is not adjacent to its original still collapses") {
    // A B A B A. What this exercises is the *span* of the duplicate check: every entry is
    // measured against the whole kept prefix rather than against the entry before it, and an
    // implementation that compared only with the previous survivor would return 5 here.
    //
    // It does not exercise the compaction. Both survivors are already at indices 0 and 1, so
    // `kept != i` is false on every iteration and nothing is ever moved -- which is true of
    // every case in this file except the one below it.
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

TEST_CASE("a duplicate ahead of a distinct entry does not displace it") {
    // A A B, and it is the only case here whose survivors do *not* start at their final
    // indices: the NVIDIA card has to be moved from index 2 to index 1 by the compaction.
    //
    // That makes this the case that fails when `adapters[kept] = adapters[i]` is deleted.
    // Without it the array is left as [intel-1, intel-2, nvidia] and the returned count is
    // still 2, so `VulkanBackend::EnumerateAdapters` -- which calls ShrinkTo(out, kept) --
    // would hand a caller two Intel entries and drop the NVIDIA card. A driver reporting its
    // devices in that order is all it takes; this machine's happens to report the distinct
    // one first, and CI has no devices at all, so the defect is invisible on both.
    AdapterInfo adapters[] = {
        Adapter("intel-1", kIntelUuid),
        Adapter("intel-2", kIntelUuid),
        Adapter("nvidia", kNvidiaUuid),
    };

    const Monarc::usize kept = DeduplicateAdapters(std::span<AdapterInfo>(adapters));

    // The count is not the assertion. The count is 2 either way -- it is the identities that
    // say *which* two, and the second survivor is the card, not the duplicate.
    REQUIRE(kept == 2);
    CHECK(NameOf(adapters[0]) == "intel-1");
    CHECK(NameOf(adapters[1]) == "nvidia");
    CHECK(adapters[0].uuid == kIntelUuid);
    CHECK(adapters[1].uuid == kNvidiaUuid);
}

TEST_CASE("two UUIDs differing only in a trailing zero byte are two adapters") {
    // **This used to run over all sixteen positions, and it does not need to.** That loop
    // existed to police a hand-written `operator==`: "a comparison written with the wrong
    // bound or a `<=`/`<` slip would pass at some positions and fail at others" is a risk of
    // a loop, and AdapterUuid's `==` is now defaulted, which compares all sixteen elements by
    // construction. Forty-eight assertions of a language guarantee.
    //
    // One position is kept, and it is chosen rather than arbitrary: byte 15 of the real Intel
    // UUID is zero, and the whole tail from byte 10 on is zeros. The observed defect this
    // file is a regression test for was a comparison that stopped at the first zero byte, and
    // that is a mistake `DeduplicateAdapters` -- Monarc's own code, not the language's --
    // could still make by comparing something other than the UUID. A prettier UUID would not
    // catch it.
    AdapterUuid altered = kIntelUuid;
    REQUIRE(altered.bytes[AdapterUuid::kSize - 1] == 0x00);
    altered.bytes[AdapterUuid::kSize - 1] = 0x01;
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

TEST_CASE("a name that fits is copied whole, and a shorter one leaves the rest alone") {
    char name[Monarc::RHI::kMaxAdapterNameLength];
    std::memset(name, 'Z', sizeof name);

    Monarc::RHI::CopyAdapterName(name, "Intel(R) UHD Graphics 730");
    CHECK(std::string_view(name) == "Intel(R) UHD Graphics 730");

    // A second, shorter copy over the first. The terminator has to move back with it, or the
    // tail of the longer name reads as part of the shorter one.
    Monarc::RHI::CopyAdapterName(name, "ab");
    CHECK(std::string_view(name) == "ab");

    // And nullptr is an empty name rather than a crash: pMessageIdName-style optional strings
    // are a fact of the Vulkan structures this is fed from.
    Monarc::RHI::CopyAdapterName(name, nullptr);
    CHECK(std::string_view(name).empty());
}

TEST_CASE("a name longer than the field is truncated to fit, terminator included") {
    // **The bound is the assertion.** `kMaxAdapterNameLength - 1` is what leaves room for the
    // terminator, and changing it to `kMaxAdapterNameLength` -- a one-byte overrun of a fixed
    // array -- used to pass both suites, because the only caller was in Monarc.RHI.Vulkan's
    // anonymous namespace where no test could reach it.
    //
    // The destination sits in a struct with a run of guard bytes behind it, rather than being
    // a bare local. That is not decoration: with the bound wrong, a bare local turns this into
    // stack corruption, which under MSVC Debug stops at a runtime-check dialog *before doctest
    // reports anything* -- a hang, not a red assertion. Behind a guard the stray byte lands in
    // memory this test owns and both assertions below simply fail, which is the outcome a
    // mutation experiment needs. Measured: with the bound at `kMaxAdapterNameLength`, a bare
    // local hung and this shape reports `256 == 255` and a clobbered guard.
    struct Guarded {
        char name[Monarc::RHI::kMaxAdapterNameLength];
        char guard[8];
    };

    constexpr Monarc::usize kOversized = Monarc::RHI::kMaxAdapterNameLength + 64;
    char                    source[kOversized];
    std::memset(source, 'a', sizeof source - 1);
    source[sizeof source - 1] = '\0';

    Guarded buffer{};
    std::memset(buffer.name, 'Z', sizeof buffer.name);
    std::memset(buffer.guard, '#', sizeof buffer.guard);
    Monarc::RHI::CopyAdapterName(buffer.name, source);

    // 255 characters and a terminator at index 255, which is the last writable byte. One more
    // and there is nowhere to put it.
    CHECK(std::string_view(buffer.name).size() == Monarc::RHI::kMaxAdapterNameLength - 1);
    CHECK(buffer.name[Monarc::RHI::kMaxAdapterNameLength - 1] == '\0');

    // Nothing was written past the field, said directly rather than inferred from the length.
    CHECK(std::string_view(buffer.guard, sizeof buffer.guard) == "########");

    // Exactly at the boundary: a source of 255 characters must survive whole, so the
    // truncation cannot be paid for by dropping a byte from a name that already fits.
    char exact[Monarc::RHI::kMaxAdapterNameLength];
    std::memset(exact, 'b', sizeof exact - 1);
    exact[sizeof exact - 1] = '\0';
    Monarc::RHI::CopyAdapterName(buffer.name, exact);
    CHECK(std::string_view(buffer.name) == std::string_view(exact));
    CHECK(std::string_view(buffer.name).size() == Monarc::RHI::kMaxAdapterNameLength - 1);
    CHECK(std::string_view(buffer.guard, sizeof buffer.guard) == "########");
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
