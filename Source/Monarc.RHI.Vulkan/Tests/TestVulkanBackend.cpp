// The one translation unit in Monarc.RHI.Vulkan.Tests that provides doctest's main().
//
// **Everything in Tests/ must run on a machine with no GPU, no Vulkan driver and no display.**
// That is not a style rule: GitHub's Windows runners are such machines, and a test here that
// quietly needed a device would either fail six CI legs or -- much worse -- be written to pass
// when it found nothing. Anything that needs a real Vulkan implementation lives in
// TestsDevice/, which is a separate binary registered with SKIP_RETURN_CODE 77.
//
// The one thing that makes this possible for the backend itself is Config::libraryName: a name
// that cannot exist drives the whole failure path -- Platform::Library, the loader, Initialize,
// and the state a failed Initialize leaves behind -- without any Vulkan runtime being involved.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>

#include <string_view>
#include <utility>

using Monarc::SystemAllocator;
using Monarc::RHI::AdapterInfo;
using Monarc::RHI::ApiVersion;
using Monarc::RHI::VulkanBackend;

namespace {

/// A library name no machine can resolve. The `.dll` is there so the name reads as what it is
/// at a glance in a log; nothing about the test depends on the extension.
constexpr const char* kNoSuchLibrary = "monarc_no_such_vulkan_runtime.dll";

[[nodiscard]] VulkanBackend::Config ConfigFor(const char* libraryName) {
    VulkanBackend::Config config{};
    config.libraryName = libraryName;
    // Validation off regardless of the build configuration: nothing here gets far enough to
    // create an instance, and leaving it at the default would make this test's behaviour
    // differ between Debug and Release for no reason.
    config.validation = false;
    return config;
}

}  // namespace

TEST_CASE("a config defaults to the platform's own library and the build's own validation") {
    const VulkanBackend::Config config{};

    // nullptr, not "vulkan-1.dll". The platform's library name is chosen inside the module, so
    // that no platform name appears in a public header (ADR-0016) -- and a caller who has not
    // asked for a specific library should not be able to read one out of the config either.
    CHECK(config.libraryName == nullptr);
    CHECK(std::string_view(config.applicationName) == "Monarc");
    CHECK(config.validation == Monarc::RHI::ValidationDefault());
}

TEST_CASE("validation is on by default in Debug and off otherwise") {
    // ValidationDefault() reads a definition this module's CMakeLists.txt sets from the build
    // configuration. Checked against the same configuration signal every module already sees,
    // so the two cannot drift apart -- and so that a build which lost the definition, or got
    // the generator expression's polarity backwards, fails here rather than by silently
    // shipping a Release binary that loads the validation layer.
#if MONARC_ENABLE_ASSERTS
    CHECK(Monarc::RHI::ValidationDefault());
#else
    CHECK_FALSE(Monarc::RHI::ValidationDefault());
#endif
}

TEST_CASE("a backend that has not been initialised reports nothing and enumerates nothing") {
    SystemAllocator allocator;
    VulkanBackend   backend(allocator);

    CHECK_FALSE(backend.IsInitialized());
    CHECK(backend.InstanceApiVersion() == ApiVersion{0, 0, 0});
    CHECK_FALSE(backend.ValidationLayerEnabled());
    CHECK_FALSE(backend.DebugMessengerInstalled());

    // InvalidArgument rather than an empty list: "there are no adapters" and "you have not
    // brought the backend up" are different answers, and a caller told the first would go
    // looking for a driver problem.
    Monarc::Array<AdapterInfo> adapters(allocator);
    const Monarc::Status       raw = backend.EnumerateAdaptersRaw(adapters);
    REQUIRE_FALSE(raw.has_value());
    CHECK(raw.error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(adapters.IsEmpty());
}

TEST_CASE("initialising against a library that cannot exist reports NotFound and names it") {
    SystemAllocator allocator;
    VulkanBackend   backend(allocator);

    const Monarc::Status initialized = backend.Initialize(ConfigFor(kNoSuchLibrary));

    REQUIRE_FALSE(initialized.has_value());
    CHECK(initialized.error().code == Monarc::ErrorCode::NotFound);

    // The message has to name the library, or a player's report of it says only that
    // something was not found. This is the same shape as Platform::Library's own tests, one
    // rung up: Library reports the code, and the loader is what turns it into a sentence.
    CHECK(initialized.error().message.find(kNoSuchLibrary) != std::string_view::npos);
}

TEST_CASE("a failed initialise leaves nothing half-built") {
    SystemAllocator allocator;
    VulkanBackend   backend(allocator);

    REQUIRE_FALSE(backend.Initialize(ConfigFor(kNoSuchLibrary)).has_value());

    CHECK_FALSE(backend.IsInitialized());
    CHECK(backend.InstanceApiVersion() == ApiVersion{0, 0, 0});
    CHECK_FALSE(backend.DebugMessengerInstalled());

    // Retrying is safe, and fails the same way: a backend that had kept a half-open loader
    // from the first attempt would report something different the second time.
    const Monarc::Status second = backend.Initialize(ConfigFor(kNoSuchLibrary));
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == Monarc::ErrorCode::NotFound);
}

TEST_CASE("shutting down a backend that was never up is safe, and repeatable") {
    SystemAllocator allocator;
    VulkanBackend   backend(allocator);
    backend.Shutdown();
    backend.Shutdown();
    CHECK_FALSE(backend.IsInitialized());
}

TEST_CASE("a moved-from backend is unusable and says so rather than crashing") {
    SystemAllocator allocator;
    VulkanBackend   source(allocator);
    VulkanBackend   destination(std::move(source));

    CHECK_FALSE(destination.IsInitialized());

    // The moved-from object has no state at all, so every query has to answer without
    // dereferencing it. This is the case that would be a null dereference if any accessor
    // forgot its null check -- and it is reachable in ordinary code, because a backend handed
    // to something else by value leaves one of these behind.
    CHECK_FALSE(source.IsInitialized());
    CHECK(source.InstanceApiVersion() == ApiVersion{0, 0, 0});
    CHECK_FALSE(source.ValidationLayerEnabled());
    CHECK_FALSE(source.DebugMessengerInstalled());
    source.Shutdown();

    const Monarc::Status initialized = source.Initialize(ConfigFor(kNoSuchLibrary));
    REQUIRE_FALSE(initialized.has_value());
    CHECK(initialized.error().code == Monarc::ErrorCode::OutOfMemory);
    CHECK_FALSE(initialized.error().message.empty());
}

TEST_CASE("move assignment releases what the destination held") {
    SystemAllocator allocator;
    VulkanBackend   first(allocator);
    VulkanBackend   second(allocator);

    const Monarc::usize beforeMove = allocator.BytesAllocated();
    REQUIRE(beforeMove > 0);

    first = std::move(second);

    // One state's worth of memory has gone: the destination's own, released before it adopted
    // the source's. A defaulted move-assignment would have leaked it, and nothing about the
    // object's observable behaviour would have changed -- which is why this measures the
    // allocator rather than asking the backend a question.
    CHECK(allocator.BytesAllocated() < beforeMove);
    CHECK_FALSE(first.IsInitialized());
    CHECK_FALSE(second.IsInitialized());
}

TEST_CASE("a backend releases everything it allocated") {
    SystemAllocator allocator;
    REQUIRE(allocator.BytesAllocated() == 0);
    {
        VulkanBackend backend(allocator);
        CHECK(allocator.BytesAllocated() > 0);
        REQUIRE_FALSE(backend.Initialize(ConfigFor(kNoSuchLibrary)).has_value());
    }
    CHECK(allocator.BytesAllocated() == 0);
}
