#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/File.h>
#include <Monarc/Core/Platform/Path.h>

#include <cstring>

using Monarc::Array;
using Monarc::String;
using Monarc::SystemAllocator;
using Monarc::u8;
using Monarc::usize;
namespace File = Monarc::Platform::File;
namespace Path = Monarc::Platform::Path;

namespace {

/// A file in the platform's temporary directory, removed on scope exit. Named per-test so
/// a leftover from a crashed run cannot make a later run pass or fail spuriously.
class ScopedTempFile {
public:
    explicit ScopedTempFile(SystemAllocator& allocator, Monarc::StringView label)
        : m_path(Path::Join(allocator, File::TemporaryDirectory(allocator).View(), label)) {
        (void)File::Delete(m_path.View());
    }
    ~ScopedTempFile() { (void)File::Delete(m_path.View()); }
    [[nodiscard]] Monarc::StringView Path() const { return m_path.View(); }

private:
    String m_path;
};

}  // namespace

TEST_CASE("a written file exists, reports its size, and reads back byte-identical") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_write.bin");

    const u8 written[] = {0x00, 0x01, 0xFE, 0xFF, 'a', 'b', 0x00, 'c'};
    REQUIRE(File::WriteAll(temp.Path(), written, sizeof(written)).has_value());

    CHECK(File::Exists(temp.Path()));
    const auto size = File::Size(temp.Path());
    REQUIRE(size.has_value());
    CHECK(*size == sizeof(written));

    Array<u8> read(allocator);
    REQUIRE(File::ReadAll(temp.Path(), read).has_value());
    REQUIRE(read.Size() == sizeof(written));
    CHECK(std::memcmp(read.Data(), written, sizeof(written)) == 0);
}

TEST_CASE("an empty file round-trips") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_empty.bin");
    REQUIRE(File::WriteAll(temp.Path(), nullptr, 0).has_value());
    CHECK(File::Exists(temp.Path()));
    const auto size = File::Size(temp.Path());
    REQUIRE(size.has_value());
    CHECK(*size == 0);

    Array<u8> read(allocator);
    REQUIRE(File::ReadAll(temp.Path(), read).has_value());
    CHECK(read.Size() == 0);
}

TEST_CASE("reading a missing file fails with NotFound rather than crashing") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_absent.bin");
    CHECK_FALSE(File::Exists(temp.Path()));

    Array<u8> read(allocator);
    const auto result = File::ReadAll(temp.Path(), read);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Monarc::ErrorCode::NotFound);
}

TEST_CASE("Size of a missing file fails rather than reporting zero") {
    // Reporting 0 would be indistinguishable from an empty file, and a caller checking
    // "size == 0" would treat a missing asset as an empty one.
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_absent_size.bin");
    const auto size = File::Size(temp.Path());
    REQUIRE_FALSE(size.has_value());
    CHECK(size.error().code == Monarc::ErrorCode::NotFound);
}

TEST_CASE("writing replaces rather than appends") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_replace.bin");
    const u8 first[]  = {1, 2, 3, 4};
    const u8 second[] = {9, 9};
    REQUIRE(File::WriteAll(temp.Path(), first, sizeof(first)).has_value());
    REQUIRE(File::WriteAll(temp.Path(), second, sizeof(second)).has_value());

    Array<u8> read(allocator);
    REQUIRE(File::ReadAll(temp.Path(), read).has_value());
    REQUIRE(read.Size() == 2);
    CHECK(read[0] == 9);
}

TEST_CASE("Delete reports whether something was removed") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_delete.bin");
    const u8 byte = 7;
    REQUIRE(File::WriteAll(temp.Path(), &byte, 1).has_value());
    CHECK(File::Delete(temp.Path()).has_value());
    CHECK_FALSE(File::Exists(temp.Path()));
    CHECK_FALSE(File::Delete(temp.Path()).has_value());   // already gone
}

TEST_CASE("Exists is false for a directory path, not an error") {
    SystemAllocator allocator;
    const String dir = File::TemporaryDirectory(allocator);
    CHECK_FALSE(File::Exists(dir.View()));
}

TEST_CASE("the temporary directory is usable") {
    SystemAllocator allocator;
    const String dir = File::TemporaryDirectory(allocator);
    CHECK(dir.Size() > 0);
    CHECK(Path::IsAbsolute(dir.View()));
}
