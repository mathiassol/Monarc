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

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Capabilities.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>

#include <Loader.h>

#include <string_view>
#include <utility>

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

TEST_CASE("a backend can be shut down and brought back up") {
    // Shutdown destroys the messenger, the instance and the loader in that order, and getting
    // that order wrong is a validation error rather than a crash -- which, with the messenger
    // installed, stops the process. So this case both exercises the cycle and is the thing
    // that would catch the ordering being wrong.
    Monarc::SystemAllocator          allocator;
    Monarc::RHI::VulkanBackend       backend(allocator);
    Monarc::RHI::VulkanBackend::Config config{};

    REQUIRE(backend.Initialize(config).has_value());
    const Monarc::RHI::ApiVersion first = backend.InstanceApiVersion();

    backend.Shutdown();
    CHECK_FALSE(backend.IsInitialized());
    CHECK(backend.InstanceApiVersion() == Monarc::RHI::ApiVersion{0, 0, 0});

    REQUIRE(backend.Initialize(config).has_value());
    CHECK(backend.IsInitialized());
    CHECK(backend.InstanceApiVersion() == first);
}

TEST_CASE("a loader that is open transfers on move and leaves the source closed") {
    // The case TestVulkanLoader.cpp cannot write: with nothing open, a correct move and a
    // memcpy are indistinguishable, so the only place this can be told apart is a machine with
    // a real Vulkan runtime to hold a handle to.
    Monarc::RHI::Detail::Loader source;
    REQUIRE(source.Open().has_value());
    REQUIRE(source.IsOpen());
    REQUIRE(source.Global().vkCreateInstance != nullptr);

    Monarc::RHI::Detail::Loader destination(std::move(source));
    CHECK(destination.IsOpen());
    CHECK(destination.Global().vkCreateInstance != nullptr);

    // The source must not still hold the module. If it did, both would call FreeLibrary on it
    // and the second call would be releasing a reference nobody owns.
    CHECK_FALSE(source.IsOpen());
}

TEST_CASE("opening a library that is not the Vulkan loader still fails on a machine that has one") {
    // The device-free suite covers this too, and it is repeated here for one reason: on a
    // machine with a real Vulkan runtime, vkGetInstanceProcAddr *is* resolvable from
    // somewhere, and a loader that had fallen back to a process-wide symbol lookup rather than
    // asking the library it opened would pass in CI and fail here. Nothing does that today;
    // this is the case that would notice if it started.
    Monarc::RHI::Detail::Loader loader;
    const Monarc::Status        opened =
        loader.Open(Monarc::Platform::Library::SystemLibraryName());
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Monarc::ErrorCode::NotFound);
    CHECK(opened.error().message.find("vkGetInstanceProcAddr") != std::string_view::npos);
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

    Monarc::RHI::VulkanBackend         backend(allocator);
    Monarc::RHI::VulkanBackend::Config config{};
    config.libraryName = libraryName;

    if (const Monarc::Status initialized = backend.Initialize(config); !initialized) {
        // Info, not Error: on a machine with no Vulkan this is the expected outcome and not a
        // fault, and CTest is about to print it as Skipped. The message still names exactly
        // what was missing, which is the whole point of the probe entry that runs beside this.
        MONARC_LOG(VulkanDeviceTest, Info,
                   "skipping the device tests: {} -- {} (returning {} so CTest reports Skipped "
                   "rather than Passed)",
                   Monarc::ToString(initialized.error().code), initialized.error().message,
                   kSkipReturnCode);
        return kSkipReturnCode;
    }

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
