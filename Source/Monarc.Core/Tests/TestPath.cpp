#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/Path.h>

using Monarc::String;
using Monarc::StringView;
using Monarc::SystemAllocator;
namespace Path = Monarc::Platform::Path;

TEST_CASE("FileName returns the last component") {
    CHECK(Path::FileName("a/b/c.txt") == "c.txt");
    CHECK(Path::FileName("c.txt") == "c.txt");
    CHECK(Path::FileName("a/b/") == "");
    CHECK(Path::FileName("") == "");
}

TEST_CASE("Extension includes no dot and is empty when absent") {
    CHECK(Path::Extension("a/b/c.txt") == "txt");
    CHECK(Path::Extension("archive.tar.gz") == "gz");
    CHECK(Path::Extension("noext") == "");
    CHECK(Path::Extension("a.b/noext") == "");   // the dot is in a directory, not the name
    CHECK(Path::Extension(".hidden") == "");     // a leading dot is not an extension
}

TEST_CASE("Stem is the file name without its extension") {
    CHECK(Path::Stem("a/b/c.txt") == "c");
    CHECK(Path::Stem("archive.tar.gz") == "archive.tar");
    CHECK(Path::Stem("noext") == "noext");
    CHECK(Path::Stem(".hidden") == ".hidden");
}

TEST_CASE("Parent drops the last component and keeps no trailing separator") {
    CHECK(Path::Parent("a/b/c.txt") == "a/b");
    CHECK(Path::Parent("a/b") == "a");
    CHECK(Path::Parent("a") == "");
    CHECK(Path::Parent("") == "");
}

TEST_CASE("Join inserts exactly one separator") {
    SystemAllocator allocator;
    CHECK(Path::Join(allocator, "a", "b").View() == "a/b");
    CHECK(Path::Join(allocator, "a/", "b").View() == "a/b");
    CHECK(Path::Join(allocator, "a", "/b").View() == "a/b");
    CHECK(Path::Join(allocator, "a/", "/b").View() == "a/b");
    CHECK(Path::Join(allocator, "", "b").View() == "b");
    CHECK(Path::Join(allocator, "a", "").View() == "a");
}

TEST_CASE("Normalize resolves . and .. and collapses separators") {
    SystemAllocator allocator;
    CHECK(Path::Normalize(allocator, "a/./b").View() == "a/b");
    CHECK(Path::Normalize(allocator, "a//b").View() == "a/b");
    CHECK(Path::Normalize(allocator, "a/b/../c").View() == "a/c");
    CHECK(Path::Normalize(allocator, "a/b/../..").View() == "");
    CHECK(Path::Normalize(allocator, "./a").View() == "a");
}

TEST_CASE("Normalize keeps a leading .. that cannot be resolved") {
    // Dropping it would silently change which directory a relative path refers to.
    SystemAllocator allocator;
    CHECK(Path::Normalize(allocator, "../a").View() == "../a");
    CHECK(Path::Normalize(allocator, "a/../../b").View() == "../b");
}

TEST_CASE("IsAbsolute distinguishes rooted paths without asserting a platform's form") {
    // Only checks that a clearly-relative path is not absolute, and that the platform's own
    // notion of its root is. Asserting "C:" here would break on every other platform.
    CHECK_FALSE(Path::IsAbsolute("a/b"));
    CHECK_FALSE(Path::IsAbsolute(""));
    CHECK_FALSE(Path::IsAbsolute("./a"));
    CHECK(Path::IsAbsolute(Path::PreferredSeparator() == '\\' ? "C:/x" : "/x"));
}
