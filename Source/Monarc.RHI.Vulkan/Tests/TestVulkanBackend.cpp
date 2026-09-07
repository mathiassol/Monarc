// The one translation unit in Monarc.RHI.Vulkan.Tests that provides doctest's main().
//
// **Everything in Tests/ must run on a machine with no GPU, no Vulkan driver and no display.**
// That is not a style rule: GitHub's Windows runners are such machines, and a test here that
// quietly needed a device would either fail six CI legs or -- much worse -- be written to pass
// when it found nothing. Anything that needs a real Vulkan implementation lives in
// TestsDevice/, which is a separate binary registered with SKIP_RETURN_CODE 77.
//
// The one thing that makes this possible for the backend itself is Config::libraryName: a name
// that cannot exist drives the whole failure path -- Platform::Library, the loader and
// VulkanBackend::Create -- without any Vulkan runtime being involved.
//
// **What that leaves out, since VulkanBackend became a factory.** A backend now exists only if
// it came up, so there is no way to hold one on a machine with no Vulkan. The move
// constructor, move assignment, Shutdown and the accessors' behaviour on a moved-from backend
// therefore cannot be reached from here at all, and they moved to TestsDevice/ rather than
// being rewritten into cases that would pass by never running. What replaces them here is
// stronger about the path this suite *can* reach: a failed Create must leave the allocator
// exactly as it found it, which is the one thing the old two-phase constructor could not be
// asked, because it allocated whether or not the bring-up would work.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/RHI/Adapter.h>
#include <Monarc/RHI/Vulkan/VulkanBackend.h>

#include <string_view>

using Monarc::SystemAllocator;
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

    // Falsifiable only outside Debug, and worth saying so rather than leaving the impression
    // that this line stands on its own: the member initialiser *is* ValidationDefault(), so in
    // a Debug build both sides read true whatever the initialiser says. Measured by hardcoding
    // `bool validation = true;` and running the whole matrix -- msvc-debug and clang-debug
    // stayed green, and msvc-release, clang-release, clang-asan and clang-ubsan all failed on
    // this line. So the four non-Debug legs are the mechanism, and no rewriting of the
    // assertion changes that: in Debug the correct answer and the hardcoded one are the same
    // answer.
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

TEST_CASE("creating against a library that cannot exist reports NotFound, and the message "
          "outlives the call") {
    SystemAllocator allocator;

    Monarc::Error error{};
    {
        const Monarc::Result<VulkanBackend> created =
            VulkanBackend::Create(allocator, ConfigFor(kNoSuchLibrary));
        REQUIRE_FALSE(created.has_value());
        error = created.error();
    }

    // Read after the Result is gone, and after the State the failure was produced in has been
    // destroyed and deallocated inside Create. Error::message is a non-owning view, so this
    // only works because the message is a string literal -- the property the whole factory
    // shape rests on. The library name is deliberately not in it; Loader.cpp's log line has
    // it.
    //
    // **The exact text is the assertion, and it has to be.** Re-pointing this message at a
    // buffer inside the State -- what the two-phase shape did -- and rebuilding under
    // clang-asan produced *no* sanitizer report: the freed storage still held its bytes and
    // the read went unnoticed. What failed was this comparison. So a `find()` for a substring,
    // or a check that the message is merely non-empty, would let the dangling case through on
    // every one of the six presets.
    CHECK(error.code == Monarc::ErrorCode::NotFound);
    CHECK(error.message == "could not open the Vulkan runtime library");
}

TEST_CASE("a failed create leaves the allocator exactly as it found it") {
    SystemAllocator allocator;
    REQUIRE(allocator.BytesAllocated() == 0);

    REQUIRE_FALSE(
        VulkanBackend::Create(allocator, ConfigFor(kNoSuchLibrary)).has_value());

    // Create allocates the state before it tries anything, so this is a real question and not
    // a tautology: the failure path has to destroy and deallocate it before returning, and a
    // Create that simply returned the error would leak one State per attempt with nothing to
    // free it -- there is no object left holding the pointer.
    CHECK(allocator.BytesAllocated() == 0);
}
