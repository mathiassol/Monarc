#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/Library.h>

using Monarc::SystemAllocator;
namespace Platform = Monarc::Platform;

TEST_CASE("loading a nonexistent library fails rather than crashing") {
    auto library = Platform::Library::Open("monarc_no_such_library_exists");
    REQUIRE_FALSE(library.has_value());
    CHECK(library.error().code == Monarc::ErrorCode::NotFound);
}

TEST_CASE("a known system library loads and yields a symbol") {
    // Named by the platform rather than hardcoded, so this test survives macOS.
    auto library = Platform::Library::Open(Platform::Library::SystemLibraryName());
    REQUIRE(library.has_value());
    CHECK(library->IsOpen());

    void* symbol = library->Symbol(Platform::Library::SystemSymbolName());
    CHECK(symbol != nullptr);

    CHECK(library->Symbol("monarc_no_such_symbol") == nullptr);
}

TEST_CASE("a library closes on destruction and reports closed after an explicit close") {
    auto library = Platform::Library::Open(Platform::Library::SystemLibraryName());
    REQUIRE(library.has_value());
    library->Close();
    CHECK_FALSE(library->IsOpen());
    CHECK(library->Symbol(Platform::Library::SystemSymbolName()) == nullptr);
}

TEST_CASE("a default-constructed library is closed and safe to query") {
    Platform::Library library;
    CHECK_FALSE(library.IsOpen());
    CHECK(library.Symbol("anything") == nullptr);
}
