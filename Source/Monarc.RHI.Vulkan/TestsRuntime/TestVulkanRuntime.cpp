// Runtime-required, device-free tests for Monarc.RHI.Vulkan.
//
// **A missing Vulkan runtime is a SKIP, never a PASS**, exactly as a missing device is one for
// TestsDevice/. This binary is registered with CTest under SKIP_RETURN_CODE 77 and the decision
// is made in main() *before doctest runs a single case*: if `vulkan-1.dll` will not open, or
// will not yield every global entry point, main prints why and returns 77 without registering
// any result. The lever that makes that path testable is the same one the device suite uses --
// `--vulkan-library=<name>` with a name that cannot resolve.
//
// **What earns a third binary is a measurement, and it is CI's.** Phase A3's plan assumed
// GitHub's Windows runners had no Vulkan runtime at all, which would have made everything in
// this file unreachable there and left it in TestsDevice/ where it started. Run 34241306503 says
// otherwise: on `windows-2025-vs2026` the loader opens, all four global entry points resolve,
// and bring-up refuses afterwards -- `Unsupported -- VK_KHR_surface`, an instance extension a
// loader with no ICD registered behind it does not offer. So the runners are a machine with a
// runtime and no instance, and the four cases below could have been running in CI from the day
// they were written. Docs/Status.md records the probe output verbatim.
//
// **What is deliberately still in TestsDevice/, and why the split is where it is.** Two of the
// six cases the plan listed as candidates need a `VkInstance`, not just a runtime: "the instance
// reports a version at or above Monarc's 1.3 baseline" and "a messenger is only ever installed
// when the validation layer actually loaded" both read state off a live `VulkanBackend`, and
// `VulkanBackend::Create` is precisely what fails on a machine like the runner. Moving them here
// would turn a suite that skips honestly into one that cannot run on the machine it was added
// for. They stay device-gated.
//
// The gate here is the runtime and nothing above it: this file must not create an instance,
// enumerate an adapter, or bring a backend up. If a case needs any of those it belongs one
// directory over.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Log.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/Library.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Capabilities.h>

#include <Loader.h>
#include <LoaderTables.h>
#include <Translate.h>

#include <string_view>
#include <utility>

using Monarc::RHI::Detail::AllTablesEmpty;

namespace {

MONARC_LOG_CATEGORY(VulkanRuntimeTest, Info);

/// CTest's SKIP_RETURN_CODE for this binary, set in Source/Monarc.RHI.Vulkan/CMakeLists.txt
/// through monarc_runtime_test_module(). The same 77 the device suite uses, for the same
/// reason.
constexpr int kSkipReturnCode = 77;

constexpr std::string_view kLibraryOption = "--vulkan-library=";

/// The library name main() opened with, so the cases open the same one it gated on.
///
/// Null means "the platform's own", which is `Loader::Open`'s default. A raw pointer to a
/// pointer main already holds -- `argv` outlives everything here -- rather than a copy, for the
/// reason TestsDevice/ gives about its own globals: nothing Vulkan-shaped is constructed during
/// static initialisation.
const char* g_libraryName = nullptr;

[[nodiscard]] Monarc::Result<Monarc::RHI::Detail::Loader> OpenLoader() {
    return g_libraryName != nullptr ? Monarc::RHI::Detail::Loader::Open(g_libraryName)
                                    : Monarc::RHI::Detail::Loader::Open();
}

}  // namespace

TEST_CASE("every global entry point resolved, and the pointers actually work") {
    // main() already returned 77 if Open had failed, which is the point: this records the
    // assertion rather than leaving it implied by the binary having got this far. The same
    // shape as the device suite's "enumeration finds at least one adapter".
    //
    // **Calling one is the half a null check cannot reach.** `Loader::Open` refuses a null for
    // any Required entry, so a populated table only proves `vkGetInstanceProcAddr` returned
    // something; that the something is the function it was asked for is a separate claim, and
    // one call through the table is what tests it. `vkEnumerateInstanceVersion` is the one
    // global that can be called with no instance, no extension and no layer.
    const Monarc::Result<Monarc::RHI::Detail::Loader> loader = OpenLoader();
    REQUIRE(loader.has_value());
    REQUIRE(loader->IsOpen());
    CHECK_FALSE(AllTablesEmpty(*loader));

    Monarc::u32 packed = 0;
    REQUIRE(loader->Global().vkEnumerateInstanceVersion != nullptr);
    CHECK(loader->Global().vkEnumerateInstanceVersion(&packed) == VK_SUCCESS);

    // **Reported, and deliberately not asserted against Monarc's floor.** Monarc requires
    // instance 1.3 and `VulkanBackend::BringUp` is what refuses anything below it -- on a
    // machine with an older loader the *device* suite skips, and turning the same fact into a
    // failure here would make this suite red about a machine it has nothing to say about. What
    // it does instead is put the number in the log, which is what the probe is for and this is
    // a second place it happens.
    //
    // The one thing asserted about the value is that a major version was written at all. That
    // is weak on purpose and it is not nothing: `packed` is zero-initialised, so a resolver
    // that handed back a function which is not `vkEnumerateInstanceVersion` and returned
    // VK_SUCCESS without writing through the pointer leaves major at 0 and fails here.
    const Monarc::RHI::ApiVersion version = Monarc::RHI::Detail::ToApiVersion(packed);
    MONARC_LOG(VulkanRuntimeTest, Info, "instance version {}.{}.{} through the global table",
               version.major, version.minor, version.patch);
    CHECK(version.major >= 1);
}

TEST_CASE("a loader that is open transfers on move and leaves the source closed and empty") {
    // The case Tests/TestVulkanLoader.cpp cannot write, and the reason is the tables: telling a
    // move that clears the source from one that copies it needs a source whose tables were
    // populated, and only a real Vulkan runtime can populate them. With nothing open the two
    // are indistinguishable, so this is the earliest suite the assertion can live in -- it was
    // in TestsDevice/ until CI proved a device was never what it needed.
    Monarc::Result<Monarc::RHI::Detail::Loader> source = OpenLoader();
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
    Monarc::Result<Monarc::RHI::Detail::Loader> loader = OpenLoader();
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
    Monarc::Result<Monarc::RHI::Detail::Loader> destination = OpenLoader();
    Monarc::Result<Monarc::RHI::Detail::Loader> source      = OpenLoader();
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
    // Tests/TestVulkanLoader.cpp asserts the same thing and runs everywhere; this suite's gate
    // is what makes the repeat worth having. On a machine with a real Vulkan runtime
    // vkGetInstanceProcAddr *is* resolvable from somewhere, and a loader that had fallen back
    // to a process-wide symbol lookup rather than asking the library it opened would pass in
    // the device-free suite and fail here. Nothing does that today; this is the case that would
    // notice if it started -- and it is now a case CI runs, which it was not while it lived in
    // TestsDevice/.
    const Monarc::Result<Monarc::RHI::Detail::Loader> opened =
        Monarc::RHI::Detail::Loader::Open(Monarc::Platform::Library::SystemLibraryName());
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Monarc::ErrorCode::NotFound);
    CHECK(opened.error().message.find("vkGetInstanceProcAddr") != std::string_view::npos);
}

int main(int argc, char** argv) {
    const char* libraryName = nullptr;

    // doctest's own option parsing would object to an argument it does not recognise, so
    // --vulkan-library is stripped out here and everything else is handed through unchanged.
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
    g_libraryName = libraryName;

    // Opened and closed again before any case runs. The cases each open their own -- three of
    // them need two Loaders or a moved-from one -- so holding this open would only mean the
    // module's refcount never drops to zero, which is the one thing the move cases assert
    // about. What this call is for is the decision below it.
    {
        const Monarc::Result<Monarc::RHI::Detail::Loader> loader = OpenLoader();
        if (!loader) {
            // Info, not Error: on a machine with no Vulkan runtime this is the expected
            // outcome and not a fault, and CTest is about to print it as Skipped. Loader.cpp's
            // own Warning line above this one names the library and the code behind it.
            MONARC_LOG(VulkanRuntimeTest, Info,
                       "skipping the runtime tests: {} -- {} (returning {} so CTest reports "
                       "Skipped rather than Passed)",
                       Monarc::ToString(loader.error().code), loader.error().message,
                       kSkipReturnCode);
            return kSkipReturnCode;
        }
    }

    doctest::Context context;
    context.applyCommandLine(static_cast<int>(forwarded.Size()), forwarded.Data());
    const int failures = context.run();
    g_libraryName      = nullptr;
    return failures;
}
