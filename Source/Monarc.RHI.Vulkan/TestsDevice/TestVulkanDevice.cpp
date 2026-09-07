// Device-required tests for Monarc.RHI.Vulkan.
//
// **A missing device is a SKIP, never a PASS.** This binary is registered with CTest under
// SKIP_RETURN_CODE 77, and the decision is made in main() *before doctest runs a single case*:
// if there is no Vulkan runtime, or the instance will not come up, or the machine reports no
// adapters, main prints why and returns 77 without registering any result at all. That
// ordering is the whole design. A doctest filter inside a binary that had already started
// would report "0 tests, all passed", which is the shape of green this phase exists to avoid --
// GitHub's Windows runners have no GPU, so this is not a hypothetical.
//
// The lever that makes the skip path *testable* is --vulkan-library=<name>: pass a name that
// cannot resolve and the loader fails exactly as it would on a machine with no Vulkan, so the
// skip can be provoked on a machine that has one. It is the same defaulted parameter shipped
// code uses, exposed on a command line; no environment variable is involved.
//
// **VulkanBackend's move semantics and allocator accounting live here rather than in Tests/,
// and that is a consequence of the factory shape.** A backend exists only if it came up, so
// holding one to move from, move-assign over, or shut down requires a real Vulkan
// implementation. The device-free suite keeps the failure path, where the allocator can still
// be measured; the four cases below are what the two-phase constructor used to make reachable
// with no Vulkan at all, at the price of a backend that could exist without a state.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Capabilities.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Types.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>
#include <Monarc/RHI/Vulkan/VulkanDevice.h>

#include <Loader.h>
#include <LoaderTables.h>

#include <string_view>
#include <utility>

using Monarc::RHI::Detail::AllTablesEmpty;

namespace {

MONARC_LOG_CATEGORY(VulkanDeviceTest, Info);

/// CTest's SKIP_RETURN_CODE for this binary, set in Source/Monarc.RHI.Vulkan/CMakeLists.txt
/// through monarc_device_test_module(). 77 is the value the Phase A3 plan names, and it is
/// autotools' long-standing convention for the same thing.
constexpr int kSkipReturnCode = 77;

constexpr std::string_view kLibraryOption = "--vulkan-library=";

/// The backend main() brought up, for the cases below to use. A raw pointer to a local in
/// main rather than a static object, so that nothing Vulkan-shaped is constructed during
/// static initialisation and the teardown order at exit is main's rather than the linker's.
Monarc::RHI::VulkanBackend* g_backend = nullptr;

/// Adapters, deduplicated, enumerated once in main. Enumerating per case would be honest too;
/// enumerating once is what makes "the raw list and the deduplicated list disagree by exactly
/// the duplicates" a statement about one observation rather than two.
Monarc::Array<Monarc::RHI::AdapterInfo>* g_adapters = nullptr;
Monarc::Array<Monarc::RHI::AdapterInfo>* g_rawAdapters = nullptr;

[[nodiscard]] Monarc::RHI::VulkanBackend& Backend() {
    MONARC_CHECK(g_backend != nullptr, "the device test's backend was never brought up");
    return *g_backend;
}

[[nodiscard]] const Monarc::Array<Monarc::RHI::AdapterInfo>& Adapters() { return *g_adapters; }
[[nodiscard]] const Monarc::Array<Monarc::RHI::AdapterInfo>& RawAdapters() {
    return *g_rawAdapters;
}

/// The colour the readback clears to and asserts, as bytes and as the floats that produce them.
///
/// **Exact, with no tolerance, and the values are chosen so that exactness is available.**
/// Each of 64, 128 and 192 over 255 survives the round trip through `f32` and back through
/// UNORM quantisation, so the driver has no rounding decision to make -- which is the whole
/// point: a tolerance here would hide exactly the two bugs this test exists to catch, a
/// channel order swapped (64 and 192 are far apart and asymmetric, so R and B swapping is
/// visible) and a colour space applied (an sRGB encode of 0.251 lands near 137, not 64).
constexpr Monarc::u8 kExpectedBytes[4] = {64, 128, 192, 255};

constexpr Monarc::RHI::ClearColor kClearColor{
    static_cast<Monarc::f32>(kExpectedBytes[0]) / 255.0F,
    static_cast<Monarc::f32>(kExpectedBytes[1]) / 255.0F,
    static_cast<Monarc::f32>(kExpectedBytes[2]) / 255.0F,
    static_cast<Monarc::f32>(kExpectedBytes[3]) / 255.0F};

/// The readback target's size.
///
/// Four by four rather than one by one, deliberately: a single pixel would pass with a copy
/// whose row pitch was wrong, and sixteen is enough for a row-stride mistake to land the
/// second row's bytes somewhere this test looks. Small enough that every pixel is asserted
/// individually rather than sampled.
constexpr Monarc::RHI::Extent2D kReadbackExtent{4, 4};

constexpr Monarc::RHI::Format kReadbackFormat = Monarc::RHI::Format::R8G8B8A8_UNORM;

[[nodiscard]] Monarc::u64 ReadbackByteCount() {
    return static_cast<Monarc::u64>(kReadbackExtent.width) * kReadbackExtent.height *
           Monarc::RHI::BytesPerPixel(kReadbackFormat);
}

void ReportAdapter(const char* label, Monarc::usize index,
                   const Monarc::RHI::AdapterInfo& info) {
    MONARC_LOG(VulkanDeviceTest, Info, "{} [{}] {} | {} | {} | Vulkan {}.{}.{} | tier {}", label,
               index, info.name, Monarc::RHI::ToString(info.uuid).text,
               Monarc::RHI::ToString(info.type), info.capabilities.apiVersion.major,
               info.capabilities.apiVersion.minor, info.capabilities.apiVersion.patch,
               Monarc::RHI::ToString(info.tier));
}

}  // namespace

TEST_CASE("the instance reports a version at or above Monarc's 1.3 baseline") {
    const Monarc::RHI::ApiVersion version = Backend().InstanceApiVersion();
    CHECK(version >= Monarc::RHI::ApiVersion{1, 3, 0});
    CHECK(Backend().IsInitialized());
}

TEST_CASE("enumeration finds at least one adapter") {
    // main() already returned 77 if this were not true, which is the point: this case exists
    // so that the assertion is *recorded* on a machine that has a device, rather than being
    // implied by the binary having got this far.
    CHECK_FALSE(Adapters().IsEmpty());
    CHECK_FALSE(RawAdapters().IsEmpty());
}

TEST_CASE("enumeration clears what it was handed rather than appending to it") {
    // VulkanBackend.h states this -- "`out` is cleared first: enumeration is a snapshot, not
    // something to accumulate" -- and nothing tested it: deleting `out.Clear()` from
    // EnumerateAdaptersRaw passed both suites, because every other case hands it an array that
    // was already empty. A sentinel is the whole difference.
    Monarc::SystemAllocator                 allocator;
    Monarc::Array<Monarc::RHI::AdapterInfo> out(allocator);

    Monarc::RHI::AdapterInfo sentinel{};
    Monarc::RHI::CopyAdapterName(sentinel.name, "not an adapter");
    sentinel.uuid.bytes[0] = 0xAB;
    out.Push(sentinel);
    out.Push(sentinel);
    REQUIRE(out.Size() == 2);

    REQUIRE(Backend().EnumerateAdaptersRaw(out).has_value());

    // The count is the assertion that catches an append, and the name is the one that catches
    // a clear that ran too late. Both, because a machine with two adapters and two sentinels
    // would give the same size either way.
    CHECK(out.Size() == RawAdapters().Size());
    for (Monarc::usize i = 0; i < out.Size(); ++i) {
        CHECK(std::string_view(out[i].name) != "not an adapter");
    }

    // The deduplicating overload goes through the same clear, so it gets the same question.
    out.Push(sentinel);
    REQUIRE(Backend().EnumerateAdapters(out).has_value());
    CHECK(out.Size() == Adapters().Size());
    for (Monarc::usize i = 0; i < out.Size(); ++i) {
        CHECK(std::string_view(out[i].name) != "not an adapter");
    }
}

TEST_CASE("no two adapters in the returned list share a UUID") {
    // The headline of Task 2. On the development machine the raw list has five entries for two
    // devices, four of them a single Intel GPU re-registered by virtual display adapters, and
    // this is the assertion that the list a caller sees does not.
    const auto& adapters = Adapters();
    for (Monarc::usize i = 0; i < adapters.Size(); ++i) {
        for (Monarc::usize j = i + 1; j < adapters.Size(); ++j) {
            CHECK(adapters[i].uuid != adapters[j].uuid);
        }
    }
}

TEST_CASE("the deduplicated list is the raw list with later duplicates dropped") {
    const auto& raw     = RawAdapters();
    const auto& deduped = Adapters();

    REQUIRE(deduped.Size() <= raw.Size());

    // Every raw adapter is represented. A deduplication that dropped a *distinct* device --
    // a comparison that was too loose, or a compaction that overwrote a survivor -- would
    // pass the no-shared-UUID case above and fail here.
    for (Monarc::usize i = 0; i < raw.Size(); ++i) {
        bool represented = false;
        for (Monarc::usize j = 0; j < deduped.Size(); ++j) {
            represented = represented || raw[i].uuid == deduped[j].uuid;
        }
        CHECK(represented);
    }

    // And the survivors appear in the order Vulkan first reported them: the deduplicated list
    // is a subsequence of the raw one, matched by UUID.
    Monarc::usize rawCursor = 0;
    for (Monarc::usize j = 0; j < deduped.Size(); ++j) {
        while (rawCursor < raw.Size() && raw[rawCursor].uuid != deduped[j].uuid) {
            ++rawCursor;
        }
        CHECK(rawCursor < raw.Size());
        ++rawCursor;
    }
}

TEST_CASE("every adapter carries a name, an identity and a tier that matches its capabilities") {
    for (Monarc::usize i = 0; i < Adapters().Size(); ++i) {
        const Monarc::RHI::AdapterInfo& info = Adapters()[i];

        CHECK_FALSE(std::string_view(info.name).empty());

        // A device that reported an all-zero UUID would deduplicate against every other one
        // that did, which is silent and wrong rather than loud and wrong.
        CHECK(info.uuid != Monarc::RHI::AdapterUuid{});

        // The cached tier and the pure function must agree. This is what Monarc.RHI's own
        // TestAdapter.cpp deliberately does not check, because there it would only be testing
        // whatever the test itself had assigned: here the capabilities came from a driver.
        CHECK(info.tier == Monarc::RHI::DetermineTier(info.capabilities));

        // A device the instance handed back must be at least Vulkan 1.1, because enumeration
        // drops anything below that -- see EnumerateRaw. Anything else means the filter is
        // not doing what it says.
        CHECK(info.capabilities.apiVersion >= Monarc::RHI::ApiVersion{1, 1, 0});
    }
}

TEST_CASE("a messenger is only ever installed when the validation layer actually loaded") {
    // Not "a messenger is installed": that depends on whether this machine has a Vulkan SDK,
    // and asserting it would make the suite fail on a machine that merely lacks one. What is
    // always true is the implication, and it is the half that can be wired up wrongly -- a
    // messenger created against an instance that never loaded the layer would report nothing
    // and look installed. Task 3 adds the stronger assertion, where a validation-clean run is
    // the actual claim being made.
    if (Backend().DebugMessengerInstalled()) {
        CHECK(Backend().ValidationLayerEnabled());
    }
    MONARC_LOG(VulkanDeviceTest, Info, "validation layer {} | debug messenger {}",
               Backend().ValidationLayerEnabled() ? "enabled" : "not enabled",
               Backend().DebugMessengerInstalled() ? "installed" : "absent");
}

TEST_CASE("a backend can be shut down, twice, and another brought up in its place") {
    // Shutdown destroys the messenger, the instance and the loader in that order, and getting
    // that order wrong is a validation error rather than a crash -- which, with the messenger
    // installed, stops the process. So this case both exercises the cycle and is the thing
    // that would catch the ordering being wrong.
    Monarc::SystemAllocator                  allocator;
    const Monarc::RHI::VulkanBackend::Config config{};

    Monarc::Result<Monarc::RHI::VulkanBackend> backend =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    REQUIRE(backend.has_value());
    const Monarc::RHI::ApiVersion first = backend->InstanceApiVersion();

    backend->Shutdown();
    backend->Shutdown();
    CHECK_FALSE(backend->IsInitialized());
    CHECK(backend->InstanceApiVersion() == Monarc::RHI::ApiVersion{0, 0, 0});

    // Bringing one back up is a second Create rather than a second Initialize, which is what
    // the factory shape costs and what `Platform::Library` already asks of its callers.
    const Monarc::Result<Monarc::RHI::VulkanBackend> again =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    REQUIRE(again.has_value());
    CHECK(again->IsInitialized());
    CHECK(again->InstanceApiVersion() == first);
}

TEST_CASE("a moved-from backend is unusable and says so rather than crashing") {
    Monarc::SystemAllocator allocator;

    Monarc::Result<Monarc::RHI::VulkanBackend> source =
        Monarc::RHI::VulkanBackend::Create(allocator,
                                           Monarc::RHI::VulkanBackend::Config{});
    REQUIRE(source.has_value());

    const Monarc::RHI::VulkanBackend destination(std::move(*source));
    CHECK(destination.IsInitialized());

    // The moved-from object has no state at all, so every query has to answer without
    // dereferencing it. This is the case that would be a null dereference if any accessor
    // forgot its null check -- and it is reachable in ordinary code, because a backend handed
    // to something else by value leaves one of these behind.
    CHECK_FALSE(source->IsInitialized());
    CHECK(source->InstanceApiVersion() == Monarc::RHI::ApiVersion{0, 0, 0});
    CHECK_FALSE(source->ValidationLayerEnabled());
    CHECK_FALSE(source->DebugMessengerInstalled());
    source->Shutdown();

    // InvalidArgument rather than an empty list: "there are no adapters" and "this backend is
    // not up" are different answers, and a caller told the first would go looking for a driver
    // problem.
    //
    // Pre-filled, so `adapters.IsEmpty()` is a question about the clear rather than about the
    // array having started empty -- the clear runs before the IsInitialized check, so the
    // failure path owes the same contract the success path does.
    Monarc::Array<Monarc::RHI::AdapterInfo> adapters(allocator);
    adapters.Push(Monarc::RHI::AdapterInfo{});
    const Monarc::Status raw = source->EnumerateAdaptersRaw(adapters);
    REQUIRE_FALSE(raw.has_value());
    CHECK(raw.error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(adapters.IsEmpty());
}

TEST_CASE("move assignment releases what the destination held") {
    Monarc::SystemAllocator                  allocator;
    const Monarc::RHI::VulkanBackend::Config config{};

    Monarc::Result<Monarc::RHI::VulkanBackend> first =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    Monarc::Result<Monarc::RHI::VulkanBackend> second =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    const Monarc::usize beforeMove = allocator.BytesAllocated();
    REQUIRE(beforeMove > 0);

    *first = std::move(*second);

    // One state's worth of memory has gone: the destination's own, released before it adopted
    // the source's. A defaulted move-assignment would have leaked it, and nothing about the
    // object's observable behaviour would have changed -- which is why this measures the
    // allocator rather than asking the backend a question.
    CHECK(allocator.BytesAllocated() < beforeMove);
    CHECK(first->IsInitialized());
    CHECK_FALSE(second->IsInitialized());
}

TEST_CASE("a backend releases everything it allocated") {
    Monarc::SystemAllocator allocator;
    REQUIRE(allocator.BytesAllocated() == 0);
    {
        const Monarc::Result<Monarc::RHI::VulkanBackend> backend =
            Monarc::RHI::VulkanBackend::Create(allocator,
                                               Monarc::RHI::VulkanBackend::Config{});
        REQUIRE(backend.has_value());
        CHECK(allocator.BytesAllocated() > 0);
    }
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("a loader that is open transfers on move and leaves the source closed and empty") {
    // The case TestVulkanLoader.cpp cannot write, and the reason is the tables: telling a
    // move that clears the source from one that copies it needs a source whose tables were
    // populated, and only a real Vulkan runtime can populate them. With nothing open the two
    // are indistinguishable, so this is the only suite the assertion can live in.
    Monarc::Result<Monarc::RHI::Detail::Loader> source = Monarc::RHI::Detail::Loader::Open();
    REQUIRE(source.has_value());
    REQUIRE(source->IsOpen());
    REQUIRE(source->Global().vkCreateInstance != nullptr);

    const Monarc::RHI::Detail::Loader destination(std::move(*source));
    CHECK(destination.IsOpen());
    CHECK(destination.Global().vkCreateInstance != nullptr);

    // The source must not still hold the module. If it did, both would call FreeLibrary on it
    // and the second call would be releasing a reference nobody owns.
    CHECK_FALSE(source->IsOpen());

    // And its tables must be null, which is the half a defaulted move gets wrong: the three
    // tables are trivially copyable, so a defaulted move copies them and leaves the source
    // holding live-looking pointers into a module the destination now owns. This is the line
    // that fails if Loader's moves are ever defaulted again -- verified by defaulting them.
    CHECK(AllTablesEmpty(*source));
}

TEST_CASE("move-assigning a loader to itself leaves it open") {
    Monarc::Result<Monarc::RHI::Detail::Loader> loader = Monarc::RHI::Detail::Loader::Open();
    REQUIRE(loader.has_value());
    REQUIRE(loader->IsOpen());

    // Not a contrivance to reach a line. Loader::operator= releases the destination through
    // Close() before adopting the source, so an unguarded version applied to one object
    // unloads the library and then copies back the nulls Close() had just written -- both
    // checks below go red, and the module is gone. The `this != &other` guard is the whole
    // of what prevents it, and it is Platform::Library::operator='s own guard.
    Monarc::RHI::Detail::Loader& alias = *loader;
    *loader                            = std::move(alias);

    CHECK(loader->IsOpen());
    CHECK(loader->Global().vkCreateInstance != nullptr);
}

TEST_CASE("move assignment clears the source's tables, and not only move construction does") {
    // Both operators are written out, so both need the assertion: a hand-written constructor
    // beside a defaulted assignment would pass the case above and fail this one. This is also
    // the operator VulkanBackend::State::BringUp uses on every bring-up -- `loader =
    // std::move(*opened)` -- so it is the moved-from Loader that actually exists in shipped
    // code, rather than only in a test.
    //
    // What is *not* asserted here is that the destination's own module was released: two
    // Loaders opened from the same name hold the same refcounted HMODULE, and nothing in the
    // public interface can see the count. Loader::operator= releases through Close() for that
    // reason -- one release path, stated once -- and the claim stops where the observation
    // does.
    Monarc::Result<Monarc::RHI::Detail::Loader> destination =
        Monarc::RHI::Detail::Loader::Open();
    Monarc::Result<Monarc::RHI::Detail::Loader> source = Monarc::RHI::Detail::Loader::Open();
    REQUIRE(destination.has_value());
    REQUIRE(source.has_value());
    REQUIRE(source->Global().vkCreateInstance != nullptr);

    *destination = std::move(*source);

    CHECK(destination->IsOpen());
    CHECK(destination->Global().vkCreateInstance != nullptr);
    CHECK_FALSE(source->IsOpen());
    CHECK(AllTablesEmpty(*source));
}

TEST_CASE("opening a library that is not the Vulkan loader still fails on a machine that has one") {
    // The device-free suite covers this too, and it is repeated here for one reason: on a
    // machine with a real Vulkan runtime, vkGetInstanceProcAddr *is* resolvable from
    // somewhere, and a loader that had fallen back to a process-wide symbol lookup rather than
    // asking the library it opened would pass in CI and fail here. Nothing does that today;
    // this is the case that would notice if it started.
    const Monarc::Result<Monarc::RHI::Detail::Loader> opened =
        Monarc::RHI::Detail::Loader::Open(Monarc::Platform::Library::SystemLibraryName());
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Monarc::ErrorCode::NotFound);
    CHECK(opened.error().message.find("vkGetInstanceProcAddr") != std::string_view::npos);
}

// ---------------------------------------------------------------------------------------
// Task 3: the device, its queue and timeline, its resource pools, and the readback.
// ---------------------------------------------------------------------------------------

namespace {

/// A device brought up on `adapter`, with an allocator of its own.
///
/// A struct and not a factory function, for the ordering: the allocator must outlive the
/// device, and declaring it first is what guarantees that -- members are destroyed in reverse
/// declaration order, so the device releases its state before the allocator it came from goes
/// away. A pair of locals in each case would work too and would put the ordering rule in
/// twelve places instead of one.
struct DeviceUnderTest {
    explicit DeviceUnderTest(const Monarc::RHI::AdapterInfo&  adapter,
                             const Monarc::RHI::DeviceConfig& config = {})
        : created(Backend().CreateDevice(allocator, adapter, config)) {}

    Monarc::SystemAllocator                   allocator;
    Monarc::Result<Monarc::RHI::VulkanDevice> created;
};

}  // namespace

TEST_CASE("a device comes up on every deduplicated adapter in turn and shuts down cleanly") {
    // **Both local GPUs, not just the default**, which is the checkbox's own wording and the
    // reason the Intel part is worth having: a device-creation path that only ever ran on the
    // NVIDIA card would be validated against one driver.
    REQUIRE_FALSE(Adapters().IsEmpty());

    for (Monarc::usize i = 0; i < Adapters().Size(); ++i) {
        const Monarc::RHI::AdapterInfo& adapter = Adapters()[i];
        DeviceUnderTest                 device(adapter);

        REQUIRE(device.created.has_value());
        CHECK(device.created->IsInitialized());

        // The device describes itself from the driver rather than echoing the argument --
        // VulkanBackend::CreateDevice re-queries, and only the UUID is taken from what was
        // passed in. The UUID must match (that is the lookup) and the name must too (that is
        // the re-query having found the same device).
        CHECK(device.created->Adapter().uuid == adapter.uuid);
        CHECK(std::string_view(device.created->Adapter().name) ==
              std::string_view(adapter.name));

        // The queue came from a family the adapter reported as graphics-capable. Six families
        // on the RTX 3070 Ti and two on the Intel UHD 730, one graphics family each, so an
        // off-by-one in the family search would produce a different index on the two.
        CHECK(device.created->GraphicsQueueFamilyIndex() < adapter.queueFamilyCount);

        // Nothing submitted, so the timeline is untouched. Zero means "nothing submitted"
        // because no submission ever signals zero -- FrameSlot::timelineValue depends on that.
        CHECK(device.created->GraphicsQueue().LastSubmittedValue() == 0);
        const Monarc::Result<Monarc::u64> completed =
            device.created->GraphicsQueue().CompletedValue();
        REQUIRE(completed.has_value());
        CHECK(*completed == 0);

        REQUIRE(device.created->WaitIdle().has_value());

        // Shut down twice, then confirm the queries answer as an empty device rather than
        // dereferencing. Getting the teardown order wrong inside Shutdown is a validation
        // error and not a crash -- which, with the fatal messenger installed, stops this
        // process -- so this case is also what catches that.
        device.created->Shutdown();
        device.created->Shutdown();
        CHECK_FALSE(device.created->IsInitialized());

        MONARC_LOG(VulkanDeviceTest, Info, "device created and destroyed on \"{}\" | tier {}",
                   adapter.name, Monarc::RHI::ToString(adapter.tier));
    }
}

TEST_CASE("a device releases everything it allocated") {
    Monarc::SystemAllocator allocator;
    REQUIRE(allocator.BytesAllocated() == 0);
    {
        Monarc::Result<Monarc::RHI::VulkanDevice> device =
            Backend().CreateDevice(allocator, Adapters()[0], Monarc::RHI::DeviceConfig{});
        REQUIRE(device.has_value());

        // Non-zero before the scope ends, so the assertion after it is about a release rather
        // than about nothing ever having been allocated.
        CHECK(allocator.BytesAllocated() > 0);
    }
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("a device destroyed with live resources still in its pools destroys them too") {
    // **What `VulkanDeviceState::Shutdown`'s two live-slot loops are for, and until this case
    // nothing reached them.** Every other case in this file destroys what it created, so
    // deleting both loops left the whole device suite green -- 296 of 296. The sweep is not
    // dead in fact, only under test, and this is the case that makes the difference visible.
    //
    // **The validation layer is what enforces this one, not an assertion, and that is the
    // honest shape for it.** A leaked `VkImage` is invisible through `IDevice`:
    // `SystemAllocator::BytesAllocated()` counts Monarc's own bytes and reaches zero either
    // way, because the pool arrays are freed whether or not their contents were destroyed. The
    // layer does see it -- with the loops removed this case ends at
    // `VUID-vkDestroyDevice-device-05137`, "VkDevice ... has 5 leaked objects that have not
    // been destroyed. VkDeviceMemory, VkDeviceMemory, VkBuffer, VkImage, VkImageView" -- and
    // with the fatal messenger installed that stops the process rather than printing. So the
    // `CHECK`s below say the resources really were created, and the absence of a validation
    // error is the assertion the case exists to make.
    Monarc::SystemAllocator allocator;
    {
        Monarc::Result<Monarc::RHI::VulkanDevice> device =
            Backend().CreateDevice(allocator, Adapters()[0], Monarc::RHI::DeviceConfig{});
        REQUIRE(device.has_value());

        Monarc::RHI::TextureDescription textureDescription{};
        textureDescription.extent = kReadbackExtent;
        textureDescription.format = kReadbackFormat;
        // ColorAttachment, so the texture carries an image view as well as an image and its
        // memory -- three of the five objects the layer counts above, and the only usage that
        // makes `ReleaseTextureSlot`'s view branch matter.
        textureDescription.usage = Monarc::RHI::TextureUsage::ColorAttachment;
        const Monarc::Result<Monarc::RHI::TextureHandle> texture =
            device->CreateTexture(textureDescription);
        REQUIRE(texture.has_value());

        Monarc::RHI::BufferDescription bufferDescription{};
        bufferDescription.size     = ReadbackByteCount();
        bufferDescription.usage    = Monarc::RHI::BufferUsage::TransferDestination;
        bufferDescription.location = Monarc::RHI::MemoryLocation::HostVisible;
        const Monarc::Result<Monarc::RHI::BufferHandle> buffer =
            device->CreateBuffer(bufferDescription);
        REQUIRE(buffer.has_value());

        // **Left mapped on purpose, and this is the half of `ReleaseBufferSlot`'s unmap branch
        // that reaches it from `Shutdown` -- but not the half that pins it.** Measured:
        // deleting that branch leaves this case green at 6 of 6 with no validation error,
        // because freeing memory that is still mapped is legal Vulkan. What pins the branch is
        // the slot it leaves behind, which the next case asserts.
        const Monarc::Result<std::span<const Monarc::u8>> mapped =
            device->MapBufferForRead(*buffer);
        REQUIRE(mapped.has_value());
        CHECK(mapped->size() == ReadbackByteCount());

        // Nothing is destroyed and nothing is unmapped: the device goes out of scope holding
        // all of it, which is the whole point.
    }
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("destroying a mapped buffer unmaps it, so its slot can be mapped again") {
    // `ReleaseBufferSlot`'s unmap-on-release branch, from `DestroyBuffer` rather than from
    // `Shutdown`, and this is the half of it a `CHECK` can see. Freeing memory that is still
    // mapped is legal Vulkan and draws no validation error, so the case above cannot pin this
    // branch -- what pins it is the *slot*: the branch clears `mapped` as well as calling
    // vkUnmapMemory, and a slot reclaimed with a stale non-null `mapped` refuses the next
    // `MapBufferForRead` with "already mapped".
    //
    // A pool of one buffer, so the second create is guaranteed to land in the same slot rather
    // than probably landing there.
    Monarc::RHI::DeviceConfig config{};
    config.maxBuffers = 1;
    DeviceUnderTest held(Adapters()[0], config);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::BufferDescription description{};
    description.size     = ReadbackByteCount();
    description.usage    = Monarc::RHI::BufferUsage::TransferDestination;
    description.location = Monarc::RHI::MemoryLocation::HostVisible;

    const Monarc::Result<Monarc::RHI::BufferHandle> first = device.CreateBuffer(description);
    REQUIRE(first.has_value());
    REQUIRE(device.MapBufferForRead(*first).has_value());

    // No UnmapBuffer, which is the caller mistake the branch answers.
    device.DestroyBuffer(*first);

    const Monarc::Result<Monarc::RHI::BufferHandle> second = device.CreateBuffer(description);
    REQUIRE(second.has_value());

    // The assertion. With the unmap branch deleted this reports InvalidArgument -- "was given
    // a buffer that is already mapped" -- about a mapping the previous occupant made.
    const Monarc::Result<std::span<const Monarc::u8>> remapped =
        device.MapBufferForRead(*second);
    REQUIRE(remapped.has_value());
    CHECK(remapped->size() == ReadbackByteCount());

    device.UnmapBuffer(*second);
    device.DestroyBuffer(*second);
}

TEST_CASE("THE READBACK: a clear through BeginRendering reads back as the exact bytes") {
    // **The headline of the phase, and it runs on every adapter in turn.** A result that
    // differs between vendors is a finding and not a flake, which is why every pixel is
    // asserted and the bytes are logged per adapter rather than only on failure.
    //
    // The clear goes through `BeginRendering` with `LoadOp::Clear` and *not* through a
    // clear-image command, and that is the point of the whole case. A readback that proved a
    // clear-image path would be coverage for a code path nothing else in the engine uses:
    // Task 4's swapchain clear and A4's render graph both clear through a dynamic-rendering
    // load-op, so this is the path that has to be the one under test.
    REQUIRE_FALSE(Adapters().IsEmpty());

    for (Monarc::usize adapterIndex = 0; adapterIndex < Adapters().Size(); ++adapterIndex) {
        const Monarc::RHI::AdapterInfo& adapter = Adapters()[adapterIndex];
        DeviceUnderTest                 held(adapter);
        REQUIRE(held.created.has_value());
        Monarc::RHI::IDevice& device = *held.created;

        Monarc::RHI::TextureDescription textureDescription{};
        textureDescription.extent = kReadbackExtent;
        textureDescription.format = kReadbackFormat;
        textureDescription.usage  = Monarc::RHI::TextureUsage::ColorAttachment |
                                   Monarc::RHI::TextureUsage::TransferSource;

        const Monarc::Result<Monarc::RHI::TextureHandle> texture =
            device.CreateTexture(textureDescription);
        REQUIRE(texture.has_value());

        Monarc::RHI::BufferDescription bufferDescription{};
        bufferDescription.size     = ReadbackByteCount();
        bufferDescription.usage    = Monarc::RHI::BufferUsage::TransferDestination;
        bufferDescription.location = Monarc::RHI::MemoryLocation::HostVisible;

        const Monarc::Result<Monarc::RHI::BufferHandle> buffer =
            device.CreateBuffer(bufferDescription);
        REQUIRE(buffer.has_value());

        const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
        REQUIRE(commands.has_value());
        REQUIRE(*commands != nullptr);
        Monarc::RHI::ICommandList& list = **commands;
        REQUIRE(list.Begin().has_value());

        // Barrier one. Undefined is the layout every texture starts in, and transitioning out
        // of it discards whatever the memory held -- correct here, because the very next thing
        // is a clear.
        list.Barrier(Monarc::RHI::TextureBarrier(
            *texture, Monarc::RHI::TextureLayout::Undefined,
            Monarc::RHI::TextureLayout::ColorAttachment, Monarc::RHI::PipelineStage::None,
            Monarc::RHI::PipelineStage::ColorAttachmentOutput, Monarc::RHI::Access::None,
            Monarc::RHI::Access::ColorAttachmentWrite));

        const Monarc::RHI::ColorAttachment attachments[1] = {
            {*texture, Monarc::RHI::LoadOp::Clear, Monarc::RHI::StoreOp::Store, kClearColor}};

        Monarc::RHI::RenderingDescription rendering{};
        rendering.extent           = kReadbackExtent;
        rendering.colorAttachments = attachments;

        // No draw call, and none is needed: a load-op clear happens when rendering begins.
        REQUIRE(list.BeginRendering(rendering).has_value());
        list.EndRendering();

        // Barrier two.
        list.Barrier(Monarc::RHI::TextureBarrier(
            *texture, Monarc::RHI::TextureLayout::ColorAttachment,
            Monarc::RHI::TextureLayout::TransferSource,
            Monarc::RHI::PipelineStage::ColorAttachmentOutput, Monarc::RHI::PipelineStage::Copy,
            Monarc::RHI::Access::ColorAttachmentWrite, Monarc::RHI::Access::TransferRead));

        REQUIRE(list.CopyTextureToBuffer(*texture, *buffer).has_value());

        // **Barrier three, which the phase plan did not list and correctness needs.** Waiting
        // on the timeline makes the copy's writes *available*; it does not make them visible
        // to the host. A memory dependency into the host stage is what does, and without it a
        // mapped read is reading memory whose visibility nothing established -- a bug that
        // would show up as intermittently stale bytes on some driver rather than as a failure
        // here. It also gives `BufferBarrier` a real caller instead of only a pure-function
        // test, which is worth having on its own.
        list.Barrier(Monarc::RHI::BufferBarrier{
            *buffer, Monarc::RHI::PipelineStage::Copy, Monarc::RHI::PipelineStage::Host,
            Monarc::RHI::Access::TransferWrite, Monarc::RHI::Access::HostRead});

        REQUIRE(list.End().has_value());

        const Monarc::Result<Monarc::u64> submitted = device.GraphicsQueue().Submit(list);
        REQUIRE(submitted.has_value());

        // The first submission on a fresh device signals one, from a timeline that started at
        // zero. A queue that signalled zero would make FrameSlot::timelineValue's "zero means
        // never submitted" wrong, and BeginFrame would stop waiting.
        CHECK(*submitted == 1);
        CHECK(device.GraphicsQueue().LastSubmittedValue() == *submitted);

        REQUIRE(device.GraphicsQueue().Wait(*submitted, 5'000'000'000ULL).has_value());

        const Monarc::Result<Monarc::u64> completed = device.GraphicsQueue().CompletedValue();
        REQUIRE(completed.has_value());
        CHECK(*completed >= *submitted);

        const Monarc::Result<std::span<const Monarc::u8>> mapped =
            device.MapBufferForRead(*buffer);
        REQUIRE(mapped.has_value());
        REQUIRE(mapped->size() == ReadbackByteCount());

        // Every pixel, individually. The first-pixel assertion is the headline and the loop is
        // what catches a copy whose row pitch was wrong -- on a 4-wide target a stride bug
        // moves the second row's bytes somewhere inside this span rather than past the end.
        MONARC_LOG(VulkanDeviceTest, Info,
                   "readback on \"{}\": first pixel = ({}, {}, {}, {}), expected ({}, {}, {}, "
                   "{})",
                   adapter.name, (*mapped)[0], (*mapped)[1], (*mapped)[2], (*mapped)[3],
                   kExpectedBytes[0], kExpectedBytes[1], kExpectedBytes[2], kExpectedBytes[3]);

        CHECK((*mapped)[0] == kExpectedBytes[0]);
        CHECK((*mapped)[1] == kExpectedBytes[1]);
        CHECK((*mapped)[2] == kExpectedBytes[2]);
        CHECK((*mapped)[3] == kExpectedBytes[3]);

        Monarc::usize matchingPixels = 0;
        for (Monarc::usize pixel = 0; pixel < mapped->size() / 4; ++pixel) {
            const Monarc::u8* bytes = mapped->data() + pixel * 4;
            if (bytes[0] == kExpectedBytes[0] && bytes[1] == kExpectedBytes[1] &&
                bytes[2] == kExpectedBytes[2] && bytes[3] == kExpectedBytes[3]) {
                ++matchingPixels;
            }
        }
        CHECK(matchingPixels == mapped->size() / 4);
        MONARC_LOG(VulkanDeviceTest, Info, "readback on \"{}\": {} of {} pixel(s) exact",
                   adapter.name, matchingPixels, mapped->size() / 4);

        device.UnmapBuffer(*buffer);

        // Destroyed explicitly rather than left to Shutdown, so the release path a frame loop
        // uses is the one under test. Shutdown's own sweep over live slots is covered by "a
        // device destroyed with live resources still in its pools destroys them too" above,
        // which leaves a texture and a mapped buffer in place and lets the device go -- and is
        // enforced by the validation layer rather than by an assertion, for the reason that
        // case gives.
        device.DestroyBuffer(*buffer);
        device.DestroyTexture(*texture);
    }
}

TEST_CASE("a second frame reuses the other slot's pool after waiting for it") {
    // kFramesInFlight is two, so this is what exercises the wrap: the third BeginFrame returns
    // slot 0 again and must wait on the value the *first* submission signalled before resetting
    // that pool. Resetting a command pool whose buffers are still executing is undefined
    // behaviour and a validation error, so a missing wait stops this process rather than
    // producing a wrong number.
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::ICommandList* first = nullptr;
    Monarc::u64                previous = 0;

    for (Monarc::u32 frame = 0; frame < Monarc::RHI::kFramesInFlight + 1; ++frame) {
        const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
        REQUIRE(commands.has_value());
        REQUIRE(*commands != nullptr);

        if (frame == 0) {
            first = *commands;
        } else if (frame == 1) {
            // Distinct lists, which is what says the frames really do have their own pools
            // rather than one pool handed out twice.
            CHECK(*commands != first);
        } else {
            // And the wrap comes back to the first one.
            CHECK(*commands == first);
        }

        REQUIRE((*commands)->Begin().has_value());
        // A barrier that changes nothing, recorded on purpose: an empty command buffer is
        // legal, and this makes the submission carry the barrier the interface says is never
        // dropped rather than nothing at all.
        (*commands)->Barrier(Monarc::RHI::GlobalBarrier{});
        REQUIRE((*commands)->End().has_value());

        const Monarc::Result<Monarc::u64> submitted =
            device.GraphicsQueue().Submit(**commands);
        REQUIRE(submitted.has_value());

        // Strictly increasing, one per submission, which is what a caller holding an earlier
        // value depends on: a timeline that reused a value would let a wait return for work
        // that had not run.
        CHECK(*submitted == previous + 1);
        previous = *submitted;
    }

    REQUIRE(device.WaitIdle().has_value());
}

TEST_CASE("a command list from one device is refused by another device's queue") {
    // Two adapters means two devices, so this is a mistake that can actually be made here
    // rather than a hypothetical. It is refused by identity -- the queue looks for the list
    // among its own device's -- and not by a downcast, which is what keeps it safe rather than
    // undefined; see VulkanDeviceState::FindOwnList.
    if (Adapters().Size() < 2) {
        MONARC_LOG(VulkanDeviceTest, Info,
                   "this machine has one adapter, so there is no second device to cross; "
                   "skipping the cross-device submission case");
        return;
    }

    DeviceUnderTest first(Adapters()[0]);
    DeviceUnderTest second(Adapters()[1]);
    REQUIRE(first.created.has_value());
    REQUIRE(second.created.has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = first.created->BeginFrame();
    REQUIRE(commands.has_value());
    REQUIRE((*commands)->Begin().has_value());
    REQUIRE((*commands)->End().has_value());

    const Monarc::Result<Monarc::u64> crossed =
        second.created->GraphicsQueue().Submit(**commands);
    REQUIRE_FALSE(crossed.has_value());
    CHECK(crossed.error().code == Monarc::ErrorCode::InvalidArgument);

    // And the same list is accepted by the queue it does belong to, so the refusal above is
    // about ownership rather than about the list being unusable.
    const Monarc::Result<Monarc::u64> accepted =
        first.created->GraphicsQueue().Submit(**commands);
    CHECK(accepted.has_value());
    REQUIRE(first.created->WaitIdle().has_value());
}

TEST_CASE("a list that is still recording is refused rather than submitted") {
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = held.created->BeginFrame();
    REQUIRE(commands.has_value());
    REQUIRE((*commands)->Begin().has_value());

    // Submitting a command buffer that vkEndCommandBuffer has not closed is a validation
    // error, which with the fatal messenger installed would stop this process -- so the
    // interface refusing it is what keeps the mistake reportable.
    const Monarc::Result<Monarc::u64> submitted =
        held.created->GraphicsQueue().Submit(**commands);
    REQUIRE_FALSE(submitted.has_value());
    CHECK(submitted.error().code == Monarc::ErrorCode::InvalidArgument);

    // Beginning twice is refused too, and for the same reason: vkBeginCommandBuffer on a
    // buffer already in the recording state is a validation error.
    const Monarc::Status again = (*commands)->Begin();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == Monarc::ErrorCode::InvalidArgument);

    REQUIRE((*commands)->End().has_value());
}

TEST_CASE("a list that has recorded nothing since BeginFrame is refused rather than submitted") {
    // **A different condition from the case above, and it was the one `Submit` did not check.**
    // `BeginFrame` resets the whole pool, which returns the command buffer to Vulkan's
    // *initial* state -- so a list nobody called `Begin` on is not recording either, and
    // `IsRecording()` cannot tell the two apart. Submitting one is
    // `VUID-vkQueueSubmit2-commandBuffer-03874`, "is unrecorded and contains no commands",
    // which with the fatal messenger installed stops the process: measured, before the guard,
    // at exit 3221226505 with doctest reporting the case CRASHED.
    //
    // Reachable from a frame loop with an early-out between `BeginFrame` and recording, which
    // is why it is a returned Status and not a comment.
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    const Monarc::Result<Monarc::RHI::ICommandList*> first = device.BeginFrame();
    REQUIRE(first.has_value());

    const Monarc::Result<Monarc::u64> unrecorded = device.GraphicsQueue().Submit(**first);
    REQUIRE_FALSE(unrecorded.has_value());
    CHECK(unrecorded.error().code == Monarc::ErrorCode::InvalidArgument);

    // An *empty* recording is accepted, so the refusal is about nothing having been recorded
    // rather than about no commands: a command buffer with no commands in it is legal Vulkan,
    // and a guard that refused this too would break the frame loop it exists to protect.
    REQUIRE((*first)->Begin().has_value());
    REQUIRE((*first)->End().has_value());
    const Monarc::Result<Monarc::u64> recorded = device.GraphicsQueue().Submit(**first);
    REQUIRE(recorded.has_value());
    CHECK(*recorded == 1);

    // The second slot, begun and never submitted, so the wrap below has a slot to come back to
    // and this list is left in exactly the state `Reset` has to clear.
    const Monarc::Result<Monarc::RHI::ICommandList*> second = device.BeginFrame();
    REQUIRE(second.has_value());
    CHECK(*second != *first);

    // **The wrap is what says the bit is cleared and not merely set.** kFramesInFlight is two,
    // so this third BeginFrame hands back the list that was `End`ed above -- and if `Reset` did
    // not clear the flag, `Submit` would accept a command buffer whose pool has just been
    // reset, which is the same VUID again.
    const Monarc::Result<Monarc::RHI::ICommandList*> wrapped = device.BeginFrame();
    REQUIRE(wrapped.has_value());
    CHECK(*wrapped == *first);

    const Monarc::Result<Monarc::u64> afterReset = device.GraphicsQueue().Submit(**wrapped);
    REQUIRE_FALSE(afterReset.has_value());
    CHECK(afterReset.error().code == Monarc::ErrorCode::InvalidArgument);

    REQUIRE(device.WaitIdle().has_value());
}

TEST_CASE("a command list outlives its device's shutdown and refuses to record") {
    // **The one surface the device hands out that used to answer a shut-down device by
    // dereferencing it.** `Shutdown` destroys the command pools, and a `VulkanCommandList` a
    // caller still holds from `BeginFrame` kept its recording flag and its `VkCommandBuffer`
    // across that -- so the next recording call dispatched through a freed buffer on a
    // destroyed `VkDevice`. Measured, by deleting the detach loop from
    // `VulkanDeviceState::Shutdown`: this case then exits 3221226505 (0xC0000409) and prints
    // nothing at all, the process being gone before doctest's output is flushed, while the
    // rest of the suite -- this case excluded by name -- stays green at 35 cases and 338
    // assertions. Nothing else in the suite reached it.
    //
    // `Shutdown` is documented as safe to call unconditionally and more than once, which is
    // what makes the ordering a teardown path rather than only a dangling pointer, and
    // Monarc/RHI/Vulkan/VulkanDevice.h promises every one of these answers safely on a
    // shut-down device.
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = held.created->BeginFrame();
    REQUIRE(commands.has_value());
    Monarc::RHI::ICommandList& list = **commands;
    REQUIRE(list.Begin().has_value());

    held.created->Shutdown();

    // The four calls that have a Status to refuse through.
    const Monarc::Status begun = list.Begin();
    REQUIRE_FALSE(begun.has_value());
    CHECK(begun.error().code == Monarc::ErrorCode::InvalidArgument);

    const Monarc::Status ended = list.End();
    REQUIRE_FALSE(ended.has_value());
    CHECK(ended.error().code == Monarc::ErrorCode::InvalidArgument);

    Monarc::RHI::RenderingDescription rendering{};
    rendering.extent               = kReadbackExtent;
    const Monarc::Status rendered  = list.BeginRendering(rendering);
    REQUIRE_FALSE(rendered.has_value());
    CHECK(rendered.error().code == Monarc::ErrorCode::InvalidArgument);

    const Monarc::Status copied =
        list.CopyTextureToBuffer(Monarc::RHI::TextureHandle{}, Monarc::RHI::BufferHandle{});
    REQUIRE_FALSE(copied.has_value());
    CHECK(copied.error().code == Monarc::ErrorCode::InvalidArgument);

    // **And the three that have no Status, which is where the crash was.** `Barrier` returns
    // void, so its only channel is the assertion handler -- see `ICommandList::Barrier`. A
    // handler that declines to break is what lets this be observed in process at all, which is
    // TestSystemAllocator.cpp's own shape; without the detach in `Shutdown` nothing fires here
    // and the process ends inside vkCmdPipelineBarrier2 instead.
    struct Captured {
        int fired = 0;
    } captured;
    static Captured* s_captured = nullptr;
    s_captured                  = &captured;

    Monarc::AssertHandler previous =
        Monarc::SetAssertHandler([](const char*, const char*, int, const char*) {
            ++s_captured->fired;
            return false;   // do not break into the debugger
        });

    list.Barrier(Monarc::RHI::GlobalBarrier{});
    list.EndRendering();

    Monarc::SetAssertHandler(previous);
    CHECK(captured.fired == 2);
}

TEST_CASE("a copy is refused unless both resources carry the transfer usage") {
    // `CopyTextureToBuffer` documented the destination's `BufferUsage::TransferDestination` and
    // did not check it, and never mentioned the source's `TextureUsage::TransferSource` at all.
    // Both are `VkImageUsageFlags`/`VkBufferUsageFlags` bits the copy requires --
    // `VUID-VkCopyImageToBufferInfo2-srcImage-00186` and `VUID-...-dstBuffer-00191` -- and both
    // stopped the process at exit 3221226505 before the checks went in. Same class of caller
    // mistake as an attachment created without `TextureUsage::ColorAttachment`, which was
    // already a returned Status with a case of its own.
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::TextureDescription attachmentOnly{};
    attachmentOnly.extent = kReadbackExtent;
    attachmentOnly.format = kReadbackFormat;
    attachmentOnly.usage  = Monarc::RHI::TextureUsage::ColorAttachment;
    const Monarc::Result<Monarc::RHI::TextureHandle> unreadable =
        device.CreateTexture(attachmentOnly);
    REQUIRE(unreadable.has_value());

    Monarc::RHI::TextureDescription copyable = attachmentOnly;
    copyable.usage = Monarc::RHI::TextureUsage::ColorAttachment |
                     Monarc::RHI::TextureUsage::TransferSource;
    const Monarc::Result<Monarc::RHI::TextureHandle> source = device.CreateTexture(copyable);
    REQUIRE(source.has_value());

    Monarc::RHI::BufferDescription wrongWay{};
    wrongWay.size     = ReadbackByteCount();
    wrongWay.usage    = Monarc::RHI::BufferUsage::TransferSource;
    wrongWay.location = Monarc::RHI::MemoryLocation::HostVisible;
    const Monarc::Result<Monarc::RHI::BufferHandle> unwritable = device.CreateBuffer(wrongWay);
    REQUIRE(unwritable.has_value());

    Monarc::RHI::BufferDescription rightWay = wrongWay;
    rightWay.usage = Monarc::RHI::BufferUsage::TransferDestination;
    const Monarc::Result<Monarc::RHI::BufferHandle> destination =
        device.CreateBuffer(rightWay);
    REQUIRE(destination.has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
    REQUIRE(commands.has_value());
    Monarc::RHI::ICommandList& list = **commands;
    REQUIRE(list.Begin().has_value());

    // Both sizes are exactly right and both handles resolve, so these two refusals can only be
    // about the usage.
    const Monarc::Status noSource = list.CopyTextureToBuffer(*unreadable, *destination);
    REQUIRE_FALSE(noSource.has_value());
    CHECK(noSource.error().code == Monarc::ErrorCode::InvalidArgument);

    const Monarc::Status noDestination = list.CopyTextureToBuffer(*source, *unwritable);
    REQUIRE_FALSE(noDestination.has_value());
    CHECK(noDestination.error().code == Monarc::ErrorCode::InvalidArgument);

    // The pair that carries both is accepted and submitted, which is what makes the refusals
    // above about the two missing bits rather than about the call. The barrier is what puts the
    // source in the layout the copy needs; the usage is what `CreateTexture` put in the image.
    list.Barrier(Monarc::RHI::TextureBarrier(
        *source, Monarc::RHI::TextureLayout::Undefined,
        Monarc::RHI::TextureLayout::TransferSource, Monarc::RHI::PipelineStage::None,
        Monarc::RHI::PipelineStage::Copy, Monarc::RHI::Access::None,
        Monarc::RHI::Access::TransferRead));
    CHECK(list.CopyTextureToBuffer(*source, *destination).has_value());

    REQUIRE(list.End().has_value());
    REQUIRE(device.GraphicsQueue().Submit(list).has_value());
    REQUIRE(device.WaitIdle().has_value());

    device.DestroyBuffer(*unwritable);
    device.DestroyBuffer(*destination);
    device.DestroyTexture(*unreadable);
    device.DestroyTexture(*source);
}

TEST_CASE("a rendering pass must be ended before the list is") {
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::TextureDescription description{};
    description.extent = kReadbackExtent;
    description.format = kReadbackFormat;
    description.usage  = Monarc::RHI::TextureUsage::ColorAttachment;

    const Monarc::Result<Monarc::RHI::TextureHandle> texture = device.CreateTexture(description);
    REQUIRE(texture.has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
    REQUIRE(commands.has_value());
    Monarc::RHI::ICommandList& list = **commands;
    REQUIRE(list.Begin().has_value());

    list.Barrier(Monarc::RHI::TextureBarrier(
        *texture, Monarc::RHI::TextureLayout::Undefined,
        Monarc::RHI::TextureLayout::ColorAttachment, Monarc::RHI::PipelineStage::None,
        Monarc::RHI::PipelineStage::ColorAttachmentOutput, Monarc::RHI::Access::None,
        Monarc::RHI::Access::ColorAttachmentWrite));

    const Monarc::RHI::ColorAttachment attachments[1] = {
        {*texture, Monarc::RHI::LoadOp::Clear, Monarc::RHI::StoreOp::Store, kClearColor}};
    Monarc::RHI::RenderingDescription rendering{};
    rendering.extent           = kReadbackExtent;
    rendering.colorAttachments = attachments;

    REQUIRE(list.BeginRendering(rendering).has_value());

    // Nested passes and copies inside a pass are both validation errors, and both are refused
    // here instead. Refusing is what keeps a caller mistake a returned Status rather than a
    // process that stops at 0x80000003.
    const Monarc::Status nested = list.BeginRendering(rendering);
    REQUIRE_FALSE(nested.has_value());
    CHECK(nested.error().code == Monarc::ErrorCode::InvalidArgument);

    const Monarc::Status ended = list.End();
    REQUIRE_FALSE(ended.has_value());
    CHECK(ended.error().code == Monarc::ErrorCode::InvalidArgument);

    // And after ending the pass properly, the list closes.
    list.EndRendering();
    CHECK(list.End().has_value());

    device.DestroyTexture(*texture);
}

TEST_CASE("a stale resource handle is reported rather than resolved to its slot's new owner") {
    // ADR-0002's whole point, on a real device. A destroyed handle must not resolve, and a
    // handle to a *recycled* slot must not resolve to whatever now occupies it -- which is
    // what the generation counter is for and what an index-only handle could not do.
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::TextureDescription textureDescription{};
    textureDescription.extent = kReadbackExtent;
    textureDescription.format = kReadbackFormat;
    textureDescription.usage  = Monarc::RHI::TextureUsage::ColorAttachment |
                               Monarc::RHI::TextureUsage::TransferSource;

    Monarc::RHI::BufferDescription bufferDescription{};
    bufferDescription.size     = ReadbackByteCount();
    bufferDescription.usage    = Monarc::RHI::BufferUsage::TransferDestination;
    bufferDescription.location = Monarc::RHI::MemoryLocation::HostVisible;

    const Monarc::Result<Monarc::RHI::TextureHandle> first =
        device.CreateTexture(textureDescription);
    REQUIRE(first.has_value());

    // One, on the first texture of a fresh device: the slot starts at zero and the claim bumps
    // it. That is what makes generation zero mean "never claimed" and never name a live
    // resource, which `Resolve` relies on to refuse a forged `ForTesting(index, 0)` handle --
    // and, with the release bump below, what makes a live slot's generation odd and a free
    // one's even.
    CHECK(first->generation == 1);

    device.DestroyTexture(*first);

    // **Destroyed and not yet replaced, which is the case the generation has to cover on its
    // own.** Every other stale-handle assertion below has an intervening creation, so the
    // claim-side bump alone would satisfy them; this one has none. It is here because the
    // destroyed-but-unreclaimed window was where a claim-only bump left the generation still
    // matching, and a backend flag was the only refusal -- measured, by dropping the `live`
    // check from `Resolve`: with the claim-only bump that handed a VK_NULL_HANDLE image to
    // vkCmdCopyImageToBuffer2 and stopped the process with 204 assertions green and none red.
    // `DestroyTexture` now bumps too, so this assertion is answered by the generation.
    const Monarc::Result<Monarc::RHI::BufferHandle> scratch =
        device.CreateBuffer(bufferDescription);
    REQUIRE(scratch.has_value());
    {
        const Monarc::Result<Monarc::RHI::ICommandList*> probe = device.BeginFrame();
        REQUIRE(probe.has_value());
        REQUIRE((*probe)->Begin().has_value());
        const Monarc::Status justDestroyed = (*probe)->CopyTextureToBuffer(*first, *scratch);
        REQUIRE_FALSE(justDestroyed.has_value());
        CHECK(justDestroyed.error().code == Monarc::ErrorCode::InvalidArgument);

        // And the buffer half of the same fact, through the one call that takes a buffer
        // handle and reports.
        device.DestroyBuffer(*scratch);
        const Monarc::Result<std::span<const Monarc::u8>> mappedDead =
            device.MapBufferForRead(*scratch);
        REQUIRE_FALSE(mappedDead.has_value());
        CHECK(mappedDead.error().code == Monarc::ErrorCode::InvalidArgument);

        REQUIRE((*probe)->End().has_value());
    }

    // The same slot, claimed again. The index is expected to repeat -- it is the first free
    // slot -- and the generation is what must not.
    const Monarc::Result<Monarc::RHI::TextureHandle> second =
        device.CreateTexture(textureDescription);
    REQUIRE(second.has_value());
    CHECK(second->index == first->index);
    CHECK(second->generation != first->generation);
    CHECK(*second != *first);

    // **Two, and the exact number is the assertion.** One bump for the destroy and one for
    // the claim, so a version that bumped only on claim -- which is what this branch used to
    // do -- lands on `first->generation + 1` and turns this red. The inequality above cannot
    // tell the two designs apart; this can, and it is the only place the destroy-side bump is
    // observable through the public interface at all.
    CHECK(second->generation == first->generation + 2);

    const Monarc::Result<Monarc::RHI::BufferHandle> buffer =
        device.CreateBuffer(bufferDescription);
    REQUIRE(buffer.has_value());

    // The buffer pool's half of the same fact, and it needs its own assertion: the two pools
    // have separate claim and release helpers, so a release-side bump present in one and
    // missing in the other is a live shape. `scratch` held this slot and was destroyed above.
    CHECK(buffer->index == scratch->index);
    CHECK(buffer->generation == scratch->generation + 2);

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
    REQUIRE(commands.has_value());
    Monarc::RHI::ICommandList& list = **commands;
    REQUIRE(list.Begin().has_value());

    // Now the stale handle names a live slot with a live image in it, so a resolve that
    // checked only the index -- or only whether the slot was live -- would succeed and render
    // into the wrong texture. These two are the fallible calls that take a handle, so they
    // are where the generation check is observable.
    const Monarc::RHI::ColorAttachment stale[1] = {
        {*first, Monarc::RHI::LoadOp::Clear, Monarc::RHI::StoreOp::Store, kClearColor}};
    Monarc::RHI::RenderingDescription rendering{};
    rendering.extent           = kReadbackExtent;
    rendering.colorAttachments = stale;

    const Monarc::Status begun = list.BeginRendering(rendering);
    REQUIRE_FALSE(begun.has_value());
    CHECK(begun.error().code == Monarc::ErrorCode::InvalidArgument);

    const Monarc::Status copied = list.CopyTextureToBuffer(*first, *buffer);
    REQUIRE_FALSE(copied.has_value());
    CHECK(copied.error().code == Monarc::ErrorCode::InvalidArgument);

    // A handle no device ever issued, and a default-constructed one, are refused the same way.
    const Monarc::Status forged = list.CopyTextureToBuffer(
        Monarc::RHI::TextureHandle::ForTesting(4242, 1), *buffer);
    REQUIRE_FALSE(forged.has_value());
    CHECK(forged.error().code == Monarc::ErrorCode::InvalidArgument);

    const Monarc::Status invalid =
        list.CopyTextureToBuffer(Monarc::RHI::TextureHandle{}, *buffer);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == Monarc::ErrorCode::InvalidArgument);

    // **The one case the generation cannot answer, and the reason the free/occupied flag stays
    // in `Resolve` alongside it.** A forged handle at generation zero naming a slot no device
    // has ever claimed *matches* that slot's generation, because a fresh slot's generation is
    // zero too. Only the slot not being live refuses it. This device has used buffer slot 0
    // twice and slot 1 never, so slot 1 is the untouched one.
    //
    // Asked through `MapBufferForRead` rather than through a command, because that is where the
    // two answers separate: `InvalidArgument` means the resolve refused it, and `Unsupported`
    // -- a never-claimed slot's `BufferDescription` is default-constructed, so its location
    // reads `DeviceLocal` -- means the resolve let it through and something downstream caught
    // it. Measured: dropping the live check turns this from the first into the second.
    const Monarc::Result<std::span<const Monarc::u8>> neverClaimed =
        device.MapBufferForRead(Monarc::RHI::BufferHandle::ForTesting(1, 0));
    REQUIRE_FALSE(neverClaimed.has_value());
    CHECK(neverClaimed.error().code == Monarc::ErrorCode::InvalidArgument);

    // And the live handle works, so the four refusals above are about staleness rather than
    // about the call being broken.
    CHECK(list.CopyTextureToBuffer(*second, *buffer).has_value());
    REQUIRE(list.End().has_value());

    // Destroying a stale handle a second time is a no-op and not a double free, which is what
    // a teardown path actually needs. Under clang-asan a double free here would be reported.
    device.DestroyTexture(*first);
    device.DestroyTexture(*second);
    device.DestroyBuffer(*buffer);
    device.DestroyBuffer(*buffer);
}

// **A stale handle in `ICommandList::Barrier` has no case here, and cannot have one.** There
// was one, and it worked by installing an assertion handler that declined to break so that the
// refusal could be watched and the case could carry on. `Barrier`'s refusal now ends with
// `MONARC_DEBUG_BREAK()` and an unconditional `std::abort()` -- Monarc/RHI/Device.h argues it
// at length, and the short version is that `MONARC_CHECK` alters no control flow, so the plain
// `return` that case relied on meant a *skipped barrier* under any handler that declined to
// break, and a dropped barrier is a synchronisation hole rather than a refused operation.
// There is no handler under which a test can survive the call, so there is nothing left for an
// in-process case to assert. `Array<T>::OnAllocationFailed` and `JobSystem::PopQueueLocked` are
// in the same position -- a fatal guard with no case in either suite -- and neither is a gap
// this file can close.
//
// **What was lost, exactly**, so that no comment here claims coverage that is gone:
//
//   - That the refusal fires *on a device*, against a handle a real `DestroyTexture` made
//     stale, rather than against a forged one. Nothing replaces this. The generation-and-live
//     resolve it goes through is the same one `BeginRendering` and `CopyTextureToBuffer` use,
//     and the case above -- "a stale resource handle is reported rather than resolved to its
//     slot's new owner" -- covers those two on a real device with a real destroyed texture,
//     so what is unobserved is `Barrier`'s dispatch to `Resolve` and not `Resolve` itself.
//   - That the `MONARC_CHECK` message names a stale handle. Nothing replaces this; the
//     message is a string literal inside the same `if` as the `Resolve` that failed.
//
// **What moved rather than being lost**, and moved into CI where there is no GPU: the whole
// content of the log line beside that check. It is composed by `Describe` in Monarc.RHI now,
// and Monarc.RHI/Tests/TestBarrier.cpp pins it in four cases with no Vulkan linked -- the
// texture form's layout pair, both forms' synchronisation scopes with every mask's hex, a mask
// that has no enumerator name and so is readable only by its number, and the fixed buffer's
// capacity against the widest description either form can produce. That also keeps
// `ToString(PipelineStage)`, `ToString(Access)` and `ToString(TextureLayout)` honestly
// shipped-called: `Describe` calls all three, and `VulkanDevice.cpp`'s two handle-resolving
// `Barrier` overloads call `Describe`.
//
// The abort itself was measured out of process rather than asserted here -- a declining
// handler installed, a stale-handle barrier issued against a real device, and the process
// gone. Docs/Status.md records the run and its exit code.

TEST_CASE("a full resource pool reports rather than growing") {
    // The pools are fixed at device creation and never grown -- JobSystem's discipline, so
    // that creation cannot reallocate and invalidate a handle's slot. Two slots, so the third
    // creation is the one that must fail.
    Monarc::RHI::DeviceConfig config{};
    config.maxTextures = 2;
    config.maxBuffers  = 2;

    DeviceUnderTest held(Adapters()[0], config);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::TextureDescription description{};
    description.extent = kReadbackExtent;
    description.format = kReadbackFormat;
    description.usage  = Monarc::RHI::TextureUsage::ColorAttachment;

    const Monarc::Result<Monarc::RHI::TextureHandle> a = device.CreateTexture(description);
    const Monarc::Result<Monarc::RHI::TextureHandle> b = device.CreateTexture(description);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->index != b->index);

    const Monarc::Result<Monarc::RHI::TextureHandle> overflow =
        device.CreateTexture(description);
    REQUIRE_FALSE(overflow.has_value());
    // OutOfMemory and not Unsupported: the device can make this texture, there is simply no
    // slot left to name it by.
    CHECK(overflow.error().code == Monarc::ErrorCode::OutOfMemory);

    // Freeing one makes room again, which is what says a full pool is a full pool and not a
    // pool that has stopped working.
    device.DestroyTexture(*a);
    const Monarc::Result<Monarc::RHI::TextureHandle> reused = device.CreateTexture(description);
    REQUIRE(reused.has_value());
    CHECK(reused->index == a->index);

    device.DestroyTexture(*b);
    device.DestroyTexture(*reused);
}

TEST_CASE("resource creation refuses a description it cannot honour") {
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::TextureDescription valid{};
    valid.extent = kReadbackExtent;
    valid.format = kReadbackFormat;
    valid.usage  = Monarc::RHI::TextureUsage::ColorAttachment;

    // Each of the three fields cleared in turn, from a description that is otherwise good --
    // so a failure names which check is missing rather than only that something refused.
    Monarc::RHI::TextureDescription noExtent = valid;
    noExtent.extent                          = Monarc::RHI::Extent2D{};
    CHECK(device.CreateTexture(noExtent).error().code == Monarc::ErrorCode::InvalidArgument);

    Monarc::RHI::TextureDescription noFormat = valid;
    noFormat.format                          = Monarc::RHI::Format::Unknown;
    CHECK(device.CreateTexture(noFormat).error().code == Monarc::ErrorCode::InvalidArgument);

    Monarc::RHI::TextureDescription noUsage = valid;
    noUsage.usage                           = Monarc::RHI::TextureUsage::None;
    CHECK(device.CreateTexture(noUsage).error().code == Monarc::ErrorCode::InvalidArgument);

    Monarc::RHI::BufferDescription noSize{};
    noSize.usage = Monarc::RHI::BufferUsage::TransferDestination;
    CHECK(device.CreateBuffer(noSize).error().code == Monarc::ErrorCode::InvalidArgument);

    Monarc::RHI::BufferDescription noBufferUsage{};
    noBufferUsage.size = 64;
    CHECK(device.CreateBuffer(noBufferUsage).error().code ==
          Monarc::ErrorCode::InvalidArgument);

    // And the valid one still works, so the five refusals are about the fields cleared rather
    // than about creation being broken on this device.
    const Monarc::Result<Monarc::RHI::TextureHandle> created = device.CreateTexture(valid);
    REQUIRE(created.has_value());
    device.DestroyTexture(*created);
}

TEST_CASE("a copy into a buffer too small to hold the texture is refused") {
    // `CopyTextureToBuffer` documents this bound, and it is worth checking rather than
    // trusting: the copy would otherwise be recorded, and a destination smaller than the
    // region is a validation error that stops the process rather than a returned Status.
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::TextureDescription textureDescription{};
    textureDescription.extent = kReadbackExtent;
    textureDescription.format = kReadbackFormat;
    textureDescription.usage  = Monarc::RHI::TextureUsage::TransferSource;
    const Monarc::Result<Monarc::RHI::TextureHandle> texture =
        device.CreateTexture(textureDescription);
    REQUIRE(texture.has_value());

    Monarc::RHI::BufferDescription tooSmall{};
    // One byte short of the whole texture, which is the boundary the check has to get right:
    // a `<=` where a `<` belongs would accept this.
    tooSmall.size     = ReadbackByteCount() - 1;
    tooSmall.usage    = Monarc::RHI::BufferUsage::TransferDestination;
    tooSmall.location = Monarc::RHI::MemoryLocation::HostVisible;
    const Monarc::Result<Monarc::RHI::BufferHandle> small = device.CreateBuffer(tooSmall);
    REQUIRE(small.has_value());

    Monarc::RHI::BufferDescription exact = tooSmall;
    exact.size                           = ReadbackByteCount();
    const Monarc::Result<Monarc::RHI::BufferHandle> fits = device.CreateBuffer(exact);
    REQUIRE(fits.has_value());

    const Monarc::Result<Monarc::RHI::ICommandList*> commands = device.BeginFrame();
    REQUIRE(commands.has_value());
    Monarc::RHI::ICommandList& list = **commands;
    REQUIRE(list.Begin().has_value());

    // The texture is moved into TransferSource before either copy, because the accepted one
    // below is submitted: a copy recorded against an image in the wrong layout is a validation
    // error at submit time, and the point of this case is the size bound, not that.
    list.Barrier(Monarc::RHI::TextureBarrier(
        *texture, Monarc::RHI::TextureLayout::Undefined,
        Monarc::RHI::TextureLayout::TransferSource, Monarc::RHI::PipelineStage::None,
        Monarc::RHI::PipelineStage::Copy, Monarc::RHI::Access::None,
        Monarc::RHI::Access::TransferRead));

    const Monarc::Status refused = list.CopyTextureToBuffer(*texture, *small);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Monarc::ErrorCode::InvalidArgument);

    // Exactly the right size is accepted, so the refusal above is about the bound rather than
    // about the call. This is the assertion an off-by-one in the other direction fails.
    CHECK(list.CopyTextureToBuffer(*texture, *fits).has_value());

    // And a transfer-only texture cannot be an attachment, which is the other half of the same
    // fact: it has no image view, because Vulkan will not create one for an image whose usage
    // has no view-compatible bit. **Found by the fatal messenger during this task**, on the
    // first texture created with `TransferSource` alone -- `CreateTexture` was making a view
    // unconditionally and the layer stopped the process at
    // `VUID-VkImageViewCreateInfo-image-04441`. Now it is a returned Status naming the missing
    // usage, and this is the assertion that keeps it one.
    const Monarc::RHI::ColorAttachment noView[1] = {
        {*texture, Monarc::RHI::LoadOp::Clear, Monarc::RHI::StoreOp::Store, kClearColor}};
    Monarc::RHI::RenderingDescription intoNoView{};
    intoNoView.extent           = kReadbackExtent;
    intoNoView.colorAttachments = noView;

    const Monarc::Status unrenderable = list.BeginRendering(intoNoView);
    REQUIRE_FALSE(unrenderable.has_value());
    CHECK(unrenderable.error().code == Monarc::ErrorCode::InvalidArgument);

    REQUIRE(list.End().has_value());
    REQUIRE(device.GraphicsQueue().Submit(list).has_value());
    REQUIRE(device.WaitIdle().has_value());

    device.DestroyBuffer(*small);
    device.DestroyBuffer(*fits);
    device.DestroyTexture(*texture);
}

TEST_CASE("only a host-visible buffer can be mapped, and only once at a time") {
    DeviceUnderTest held(Adapters()[0]);
    REQUIRE(held.created.has_value());
    Monarc::RHI::IDevice& device = *held.created;

    Monarc::RHI::BufferDescription deviceLocal{};
    // 250 and not 256, deliberately. Vulkan rounds a buffer's *allocation* up to the memory
    // type's alignment, so a mapped span whose length came from `VkMemoryRequirements::size`
    // rather than from the description would be 256 here and indistinguishable at 256.
    deviceLocal.size     = 250;
    deviceLocal.usage    = Monarc::RHI::BufferUsage::TransferDestination;
    deviceLocal.location = Monarc::RHI::MemoryLocation::DeviceLocal;

    const Monarc::Result<Monarc::RHI::BufferHandle> fast = device.CreateBuffer(deviceLocal);
    REQUIRE(fast.has_value());

    // Unsupported and not InvalidArgument: the handle is fine and the request is not something
    // this buffer's memory can do. Mapping it anyway would fail at vkMapMemory with a
    // VkResult, much further from the cause.
    const Monarc::Result<std::span<const Monarc::u8>> refused = device.MapBufferForRead(*fast);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Monarc::ErrorCode::Unsupported);

    Monarc::RHI::BufferDescription hostVisible = deviceLocal;
    hostVisible.location = Monarc::RHI::MemoryLocation::HostVisible;

    const Monarc::Result<Monarc::RHI::BufferHandle> readable =
        device.CreateBuffer(hostVisible);
    REQUIRE(readable.has_value());

    const Monarc::Result<std::span<const Monarc::u8>> mapped =
        device.MapBufferForRead(*readable);
    REQUIRE(mapped.has_value());
    // The buffer's own size and not the allocation's, which the driver rounds up -- see the
    // note on 250 above, which is what makes this assertion able to tell the two apart.
    CHECK(mapped->size() == hostVisible.size);
    CHECK(mapped->data() != nullptr);

    // Vulkan forbids mapping already-mapped memory. Refusing beats returning the same pointer,
    // which would leave two callers each expecting to unmap.
    const Monarc::Result<std::span<const Monarc::u8>> twice =
        device.MapBufferForRead(*readable);
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == Monarc::ErrorCode::InvalidArgument);

    // Unmapping twice is a no-op, and mapping again after an unmap works -- so the guard above
    // tracks the mapping rather than latching on the first one.
    device.UnmapBuffer(*readable);
    device.UnmapBuffer(*readable);
    CHECK(device.MapBufferForRead(*readable).has_value());
    device.UnmapBuffer(*readable);

    device.DestroyBuffer(*fast);
    device.DestroyBuffer(*readable);
}

TEST_CASE("a device on an adapter no physical device reports is NotFound") {
    // Only `adapter.uuid` is taken from the argument, so this is the one field that can be
    // wrong in a way the backend has to notice. A UUID no device reports must be NotFound and
    // not a device created on whichever adapter happened to be first.
    Monarc::RHI::AdapterInfo fabricated = Adapters()[0];
    fabricated.uuid.bytes[0] = static_cast<Monarc::u8>(fabricated.uuid.bytes[0] ^ 0xFF);

    Monarc::SystemAllocator allocator;
    const Monarc::Result<Monarc::RHI::VulkanDevice> device =
        Backend().CreateDevice(allocator, fabricated, Monarc::RHI::DeviceConfig{});
    REQUIRE_FALSE(device.has_value());
    CHECK(device.error().code == Monarc::ErrorCode::NotFound);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("a device configured with an empty pool is refused") {
    Monarc::RHI::DeviceConfig config{};
    config.maxTextures = 0;

    Monarc::SystemAllocator allocator;
    const Monarc::Result<Monarc::RHI::VulkanDevice> device =
        Backend().CreateDevice(allocator, Adapters()[0], config);
    REQUIRE_FALSE(device.has_value());
    CHECK(device.error().code == Monarc::ErrorCode::InvalidArgument);

    // Nothing allocated, which is what says the refusal happened before the state was created
    // rather than after -- a check placed later would leak on this path or have to unwind.
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("a moved-from device answers every query rather than dereferencing") {
    Monarc::SystemAllocator allocator;
    Monarc::Result<Monarc::RHI::VulkanDevice> source =
        Backend().CreateDevice(allocator, Adapters()[0], Monarc::RHI::DeviceConfig{});
    REQUIRE(source.has_value());

    Monarc::RHI::VulkanDevice destination(std::move(*source));
    CHECK(destination.IsInitialized());

    // The moved-from object has no state at all, so every accessor has to answer without
    // dereferencing. This is the case that would be a null dereference if one forgot, and it
    // is reachable in ordinary code: a device handed to something else by value leaves one of
    // these behind.
    CHECK_FALSE(source->IsInitialized());
    CHECK(source->Adapter().uuid == Monarc::RHI::AdapterUuid{});
    CHECK(source->GraphicsQueueFamilyIndex() == 0);
    source->Shutdown();

    CHECK(source->CreateTexture(Monarc::RHI::TextureDescription{}).error().code ==
          Monarc::ErrorCode::InvalidArgument);
    CHECK(source->BeginFrame().error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(source->WaitIdle().error().code == Monarc::ErrorCode::InvalidArgument);
    source->DestroyTexture(Monarc::RHI::TextureHandle{});
    source->UnmapBuffer(Monarc::RHI::BufferHandle{});

    // GraphicsQueue() has to return a reference to *something*, and the detached queue is what
    // it returns -- a null object whose every call refuses. An abort here would have been the
    // alternative, and it would have made this line untestable.
    Monarc::RHI::IQueue& detached = source->GraphicsQueue();
    CHECK(detached.LastSubmittedValue() == 0);
    CHECK(detached.CompletedValue().error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(detached.Wait(1, 0).error().code == Monarc::ErrorCode::InvalidArgument);

    // Move assignment releases what the destination held before adopting. One state's worth of
    // memory has to go, and nothing about the object's behaviour would show it -- which is why
    // this measures the allocator.
    Monarc::Result<Monarc::RHI::VulkanDevice> other =
        Backend().CreateDevice(allocator, Adapters()[0], Monarc::RHI::DeviceConfig{});
    REQUIRE(other.has_value());
    const Monarc::usize beforeMove = allocator.BytesAllocated();
    REQUIRE(beforeMove > 0);
    destination = std::move(*other);
    CHECK(allocator.BytesAllocated() < beforeMove);
    CHECK(destination.IsInitialized());
    CHECK_FALSE(other->IsInitialized());
}

TEST_CASE("a build that asked for validation actually has a messenger") {
    // **The stronger half of Task 2's implication, and the plan's own checkbox**: "assert the
    // messenger was installed, so a build that quietly failed to load the layer cannot pass as
    // clean." Zero validation errors across this suite is a claim the fatal messenger enforces,
    // and it means nothing if no messenger was ever installed.
    //
    // Conditioned on ValidationDefault() and on nothing else, which means **the device suite
    // requires a Vulkan SDK in a Debug build**. That is the trade the checkbox asks for: the
    // only way this can fail is the case it exists for -- a build that asked for validation and
    // did not get it -- and weakening it to "if the layer happens to be present" would make it
    // unable to fail at all.
    if (Monarc::RHI::ValidationDefault()) {
        CHECK(Backend().ValidationLayerEnabled());
        CHECK(Backend().DebugMessengerInstalled());
    } else {
        // Release asks for neither, and having one anyway would mean the generator expression
        // in this module's CMakeLists.txt had inverted -- a mutation Task 2 measured, which
        // turns exactly the non-Debug legs red.
        CHECK_FALSE(Backend().ValidationLayerEnabled());
        CHECK_FALSE(Backend().DebugMessengerInstalled());
    }
}

int main(int argc, char** argv) {
    const char* libraryName = nullptr;

    // doctest's own option parsing would object to an argument it does not recognise, so
    // --vulkan-library is stripped out here and everything else is handed through unchanged.
    // Filtering rather than rejecting, so `ctest` and a developer running the binary by hand
    // can both still pass doctest's flags.
    Monarc::SystemAllocator allocator;
    Monarc::Array<char*>    forwarded(allocator);
    forwarded.Reserve(static_cast<Monarc::usize>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.starts_with(kLibraryOption)) {
            libraryName = argv[i] + kLibraryOption.size();
            continue;
        }
        forwarded.Push(argv[i]);
    }

    Monarc::RHI::VulkanBackend::Config config{};
    config.libraryName = libraryName;

    Monarc::Result<Monarc::RHI::VulkanBackend> created =
        Monarc::RHI::VulkanBackend::Create(allocator, config);
    if (!created) {
        // Info, not Error: on a machine with no Vulkan this is the expected outcome and not a
        // fault, and CTest is about to print it as Skipped. The message is a string literal
        // saying which step failed; the backend's own Warning lines above this one name the
        // library, the VkResult or the version it actually saw, which is the whole point of
        // the probe entry that runs beside this.
        MONARC_LOG(VulkanDeviceTest, Info,
                   "skipping the device tests: {} -- {} (returning {} so CTest reports Skipped "
                   "rather than Passed)",
                   Monarc::ToString(created.error().code), created.error().message,
                   kSkipReturnCode);
        return kSkipReturnCode;
    }
    Monarc::RHI::VulkanBackend& backend = *created;

    Monarc::Array<Monarc::RHI::AdapterInfo> rawAdapters(allocator);
    if (const Monarc::Status enumerated = backend.EnumerateAdaptersRaw(rawAdapters);
        !enumerated) {
        MONARC_LOG(VulkanDeviceTest, Info,
                   "skipping the device tests: raw enumeration failed -- {} -- {}",
                   Monarc::ToString(enumerated.error().code), enumerated.error().message);
        return kSkipReturnCode;
    }

    Monarc::Array<Monarc::RHI::AdapterInfo> adapters(allocator);
    if (const Monarc::Status enumerated = backend.EnumerateAdapters(adapters); !enumerated) {
        MONARC_LOG(VulkanDeviceTest, Info,
                   "skipping the device tests: enumeration failed -- {} -- {}",
                   Monarc::ToString(enumerated.error().code), enumerated.error().message);
        return kSkipReturnCode;
    }

    if (adapters.IsEmpty()) {
        // A Vulkan loader with no ICD registered behind it: the instance comes up and there is
        // nothing to render on. Distinguished from a missing loader in the message, because
        // the two need different things done about them.
        MONARC_LOG(VulkanDeviceTest, Info,
                   "skipping the device tests: the Vulkan instance came up at {}.{}.{} and "
                   "reports no physical devices",
                   backend.InstanceApiVersion().major, backend.InstanceApiVersion().minor,
                   backend.InstanceApiVersion().patch);
        return kSkipReturnCode;
    }

    MONARC_LOG(VulkanDeviceTest, Info, "instance {}.{}.{} | {} raw adapter(s) | {} after dedupe",
               backend.InstanceApiVersion().major, backend.InstanceApiVersion().minor,
               backend.InstanceApiVersion().patch, rawAdapters.Size(), adapters.Size());
    for (Monarc::usize i = 0; i < rawAdapters.Size(); ++i) {
        ReportAdapter("raw", i, rawAdapters[i]);
    }
    for (Monarc::usize i = 0; i < adapters.Size(); ++i) {
        ReportAdapter("deduplicated", i, adapters[i]);
    }

    g_backend     = &backend;
    g_rawAdapters = &rawAdapters;
    g_adapters    = &adapters;

    doctest::Context context;
    context.applyCommandLine(static_cast<int>(forwarded.Size()), forwarded.Data());
    const int failures = context.run();

    // The pointers are cleared before backend and the arrays go out of scope, so a doctest
    // reporter or an at-exit handler cannot reach a destroyed object.
    g_adapters    = nullptr;
    g_rawAdapters = nullptr;
    g_backend     = nullptr;
    return failures;
}
