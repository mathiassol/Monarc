#include <doctest/doctest.h>

#include <Monarc/Core/Containers/HashMap.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/Guid.h>

using Monarc::Guid;
using Monarc::HashMap;
using Monarc::SystemAllocator;

TEST_CASE("a default Guid is nil and distinguishable from a generated one") {
    const Guid nil;
    CHECK(nil.IsNil());
    CHECK(nil == Guid{});
    CHECK_FALSE(Guid::Generate().IsNil());
}

TEST_CASE("generated Guids do not collide") {
    // Not a statistical test of the entropy source -- just that Generate is not returning a
    // constant or a low-entropy counter, which is the realistic failure mode.
    SystemAllocator allocator;
    HashMap<Guid, int> seen(allocator);
    constexpr int kCount = 4096;
    for (int i = 0; i < kCount; ++i) { seen.Insert(Guid::Generate(), i); }
    CHECK(seen.Size() == kCount);
}

TEST_CASE("a Guid round-trips through its string form") {
    const Guid original = Guid::Generate();
    char text[Guid::kStringLength + 1] = {};
    original.Format(text);
    const auto parsed = Guid::Parse(text);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == original);
}

TEST_CASE("the string form is the canonical 36-character hyphenated shape") {
    const Guid nil;
    char text[Guid::kStringLength + 1] = {};
    nil.Format(text);
    CHECK(Monarc::StringView(text) == "00000000-0000-0000-0000-000000000000");
    CHECK(Guid::kStringLength == 36);
}

TEST_CASE("Parse rejects malformed input rather than guessing") {
    CHECK_FALSE(Guid::Parse("").has_value());
    CHECK_FALSE(Guid::Parse("not-a-guid").has_value());
    CHECK_FALSE(Guid::Parse("00000000-0000-0000-0000-00000000000").has_value());   // short
    CHECK_FALSE(Guid::Parse("00000000-0000-0000-0000-0000000000000").has_value()); // long
    CHECK_FALSE(Guid::Parse("0000000000000000000000000000000000000").has_value()); // no dashes
    CHECK_FALSE(Guid::Parse("gggggggg-0000-0000-0000-000000000000").has_value());  // non-hex
}

TEST_CASE("Parse accepts either case and produces the same value") {
    const auto a = Guid::Parse("01234567-89ab-cdef-0123-456789abcdef");
    const auto b = Guid::Parse("01234567-89AB-CDEF-0123-456789ABCDEF");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == *b);
}

TEST_CASE("Guids order consistently so they can be sorted") {
    const auto low  = Guid::Parse("00000000-0000-0000-0000-000000000001");
    const auto high = Guid::Parse("ffffffff-0000-0000-0000-000000000000");
    REQUIRE(low.has_value());
    REQUIRE(high.has_value());
    CHECK(*low < *high);
    CHECK_FALSE(*high < *low);
    CHECK_FALSE(*low < *low);
}

TEST_CASE("a Guid is usable as a HashMap key") {
    SystemAllocator allocator;
    HashMap<Guid, int> map(allocator);
    const Guid key = Guid::Generate();
    map.Insert(key, 7);
    REQUIRE(map.Find(key) != nullptr);
    CHECK(*map.Find(key) == 7);
    CHECK(map.Find(Guid::Generate()) == nullptr);
}
