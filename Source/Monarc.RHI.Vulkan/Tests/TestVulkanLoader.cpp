#include <doctest/doctest.h>

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Platform/Library.h>

#include <Loader.h>

#include <string_view>

using Monarc::RHI::Detail::Loader;

namespace {

/// A library name no machine can resolve.
constexpr const char* kNoSuchLibrary = "monarc_no_such_vulkan_runtime.dll";

/// True when every entry point in every table is null.
///
/// Written out rather than trusting Loader::IsOpen, because "not open" and "holding stale
/// pointers into an unmapped module" are exactly the two states a failed Open or a Close has
/// to keep apart -- and a table that kept its pointers would turn the next call into a jump
/// into freed address space rather than a null dereference.
[[nodiscard]] bool AllTablesEmpty(const Loader& loader) {
    bool empty = true;
#define MONARC_VK_CHECK_NULL(name) empty = empty && loader.Global().name == nullptr;
    MONARC_VK_GLOBAL_FUNCTIONS(MONARC_VK_CHECK_NULL)
#undef MONARC_VK_CHECK_NULL
#define MONARC_VK_CHECK_NULL(name) empty = empty && loader.Instance().name == nullptr;
    MONARC_VK_INSTANCE_FUNCTIONS(MONARC_VK_CHECK_NULL)
#undef MONARC_VK_CHECK_NULL
#define MONARC_VK_CHECK_NULL(name) empty = empty && loader.DebugUtils().name == nullptr;
    MONARC_VK_DEBUG_UTILS_FUNCTIONS(MONARC_VK_CHECK_NULL)
#undef MONARC_VK_CHECK_NULL
    return empty;
}

}  // namespace

TEST_CASE("opening a library that does not exist reports NotFound and names it") {
    // The same shape as Platform::Library's own first test, one rung up: Library answers with
    // the code, and the loader is what turns it into a sentence a player could quote.
    Loader loader;

    const Monarc::Status opened = loader.Open(kNoSuchLibrary);

    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Monarc::ErrorCode::NotFound);
    CHECK(opened.error().message.find(kNoSuchLibrary) != std::string_view::npos);
    CHECK_FALSE(loader.IsOpen());
    CHECK(AllTablesEmpty(loader));
}

TEST_CASE("a real library that is not the Vulkan loader reports the missing entry point") {
    // This is the case that needs no Vulkan and still exercises the second half of Open: the
    // library resolves, is mapped, and does not export vkGetInstanceProcAddr. Named by the
    // platform rather than hardcoded, exactly as Platform::Library's own tests do, so it
    // survives macOS -- and it is a library that exists on every machine this can run on,
    // which is the point.
    Loader loader;

    const Monarc::Status opened = loader.Open(Monarc::Platform::Library::SystemLibraryName());

    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == Monarc::ErrorCode::NotFound);
    CHECK(opened.error().message.find("vkGetInstanceProcAddr") != std::string_view::npos);
    CHECK(opened.error().message.find(Monarc::Platform::Library::SystemLibraryName()) !=
          std::string_view::npos);

    // Nothing is left mapped. A loader that had kept the library open would be holding a
    // reference to a DLL it has no use for, and IsOpen would be answering a different
    // question from "can I call through this".
    CHECK_FALSE(loader.IsOpen());
    CHECK(AllTablesEmpty(loader));
}

TEST_CASE("a default-constructed loader is closed, empty, and safe to query") {
    const Loader loader;
    CHECK_FALSE(loader.IsOpen());
    CHECK_FALSE(loader.HasDebugUtilsFunctions());
    CHECK(AllTablesEmpty(loader));
}

TEST_CASE("closing a loader is safe unconditionally and more than once") {
    Loader loader;
    loader.Close();
    loader.Close();
    CHECK_FALSE(loader.IsOpen());
    CHECK(AllTablesEmpty(loader));
}

TEST_CASE("resolving instance functions on a closed loader is rejected, not attempted") {
    Loader loader;

    // InvalidArgument, and it has to come before the null-instance check below is even
    // reachable: with no library open there is no vkGetInstanceProcAddr to call, so a version
    // of this that went ahead would dereference a null function pointer rather than return.
    const Monarc::Status loaded = loader.LoadInstanceFunctions(VK_NULL_HANDLE, false);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK_FALSE(loaded.error().message.empty());
    CHECK(AllTablesEmpty(loader));
}

TEST_CASE("reopening after a failure does not leave the previous attempt behind") {
    Loader loader;
    REQUIRE_FALSE(loader.Open(Monarc::Platform::Library::SystemLibraryName()).has_value());

    const Monarc::Status second = loader.Open(kNoSuchLibrary);
    REQUIRE_FALSE(second.has_value());

    // The second failure's message names the second library, not the first. A message buffer
    // that was written once and never again would still be describing kernel32 here, which is
    // the failure mode of every "last error" mechanism that forgets to overwrite.
    CHECK(second.error().message.find(kNoSuchLibrary) != std::string_view::npos);
    CHECK(second.error().message.find("vkGetInstanceProcAddr") == std::string_view::npos);
}

// Two things this file deliberately does not test, because it cannot without a Vulkan
// implementation, and a case written to pass in their absence would be worse than no case:
//
//   * A successful Open, and the global entry-point table being populated by it.
//   * Loader's move, which can only be told apart from a copy when there is a real library
//     handle to transfer -- with nothing open, a defaulted move and a memcpy are
//     indistinguishable.
//
// Both are in TestsDevice/TestVulkanDevice.cpp, which reports Skipped rather than Passed when
// there is no device to run them against.
