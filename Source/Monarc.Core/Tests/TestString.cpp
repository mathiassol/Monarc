#include <doctest/doctest.h>

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <cstring>
#include <utility>

using Monarc::String;
using Monarc::StringView;
using Monarc::SystemAllocator;

TEST_CASE("a default String is empty, allocates nothing, and is still a valid C string") {
    SystemAllocator allocator;
    String s(allocator);
    CHECK(s.Size() == 0);
    CHECK(s.IsEmpty());
    CHECK(allocator.BytesAllocated() == 0);
    REQUIRE(s.CStr() != nullptr);
    CHECK(std::strlen(s.CStr()) == 0);
}

TEST_CASE("a String built from a view carries its contents") {
    SystemAllocator allocator;
    String s(allocator, "monarc");
    CHECK(s.Size() == 6);
    CHECK_FALSE(s.IsEmpty());
    CHECK(s.View() == "monarc");
    CHECK(std::strcmp(s.CStr(), "monarc") == 0);
}

TEST_CASE("Append grows and stays null-terminated") {
    SystemAllocator allocator;
    String s(allocator, "mon");
    s.Append("arc");
    CHECK(s.Size() == 6);
    CHECK(s.View() == "monarc");
    CHECK(std::strcmp(s.CStr(), "monarc") == 0);

    for (int i = 0; i < 100; ++i) {
        s.Append("x");
    }
    CHECK(s.Size() == 106);
    CHECK(std::strlen(s.CStr()) == 106);
}

TEST_CASE("PushBack appends a single character") {
    SystemAllocator allocator;
    String s(allocator);
    s.PushBack('a');
    s.PushBack('b');
    CHECK(s.View() == "ab");
    CHECK(std::strcmp(s.CStr(), "ab") == 0);
}

TEST_CASE("appending a String to itself is safe") {
    // The argument aliases the buffer the append may reallocate -- the same shape that was
    // a use-after-free in Array<T> before it was fixed.
    SystemAllocator allocator;
    String s(allocator, "abc");
    s.Append(s.View());
    CHECK(s.Size() == 6);
    CHECK(s.View() == "abcabc");
}

TEST_CASE("indexing reads and writes characters") {
    SystemAllocator allocator;
    String s(allocator, "cat");
    CHECK(s[0] == 'c');
    s[0] = 'b';
    CHECK(s.View() == "bat");
}

TEST_CASE("Clear empties without releasing capacity") {
    SystemAllocator allocator;
    String s(allocator, "something long enough to allocate");
    const Monarc::usize capacity = s.Capacity();
    REQUIRE(capacity > 0);

    s.Clear();
    CHECK(s.Size() == 0);
    CHECK(s.Capacity() == capacity);
    CHECK(std::strlen(s.CStr()) == 0);
}

TEST_CASE("Reserve allocates without changing size") {
    SystemAllocator allocator;
    String s(allocator, "ab");
    s.Reserve(256);
    CHECK(s.Size() == 2);
    CHECK(s.Capacity() >= 256);
    CHECK(s.View() == "ab");
}

TEST_CASE("moving transfers the buffer and leaves the source usable") {
    SystemAllocator allocator;
    String source(allocator, "moved");
    String moved(std::move(source));
    CHECK(moved.View() == "moved");
    CHECK(source.Size() == 0);
    CHECK(std::strlen(source.CStr()) == 0);   // still a valid empty C string
}

TEST_CASE("move assignment releases what the target held") {
    SystemAllocator allocator;
    String target(allocator, "target contents long enough to allocate");
    String source(allocator, "src");
    target = std::move(source);
    CHECK(target.View() == "src");
}

TEST_CASE("equality compares contents, not identity") {
    SystemAllocator allocator;
    String a(allocator, "same");
    String b(allocator, "same");
    String c(allocator, "different");
    CHECK(a == b);
    CHECK_FALSE(a == c);
    CHECK(a != c);
    CHECK(a == StringView("same"));
}

TEST_CASE("a String releases everything it allocated") {
    SystemAllocator allocator;
    {
        String s(allocator);
        for (int i = 0; i < 500; ++i) {
            s.Append("chunk");
        }
        CHECK(s.Size() == 2500);
    }
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("embedded null bytes are preserved in the view") {
    SystemAllocator allocator;
    String s(allocator, StringView("a\0b", 3));
    CHECK(s.Size() == 3);
    CHECK(s.View().size() == 3);
    CHECK(s.View()[1] == '\0');
}

TEST_CASE("a String is hashable and agrees with its view") {
    SystemAllocator allocator;
    String s(allocator, "key");
    CHECK(Monarc::Hash(s) == Monarc::Hash(StringView("key")));
}

TEST_CASE("appending a String to itself is safe when it forces a reallocation") {
    // The existing aliasing case fits inside its capacity, so it never reallocates --
    // and growth-plus-aliasing is the exact combination that was a use-after-free in
    // Array<T>. Fill to capacity first so the append must grow.
    SystemAllocator allocator;
    String s(allocator);
    s.Reserve(8);
    for (int i = 0; i < 8; ++i) {
        s.PushBack(static_cast<char>('a' + i));
    }
    REQUIRE(s.Size() == s.Capacity());   // the next write must reallocate

    s.Append(s.View());

    CHECK(s.Size() == 16);
    CHECK(s.View() == "abcdefghabcdefgh");
    CHECK(std::strcmp(s.CStr(), "abcdefghabcdefgh") == 0);
}
