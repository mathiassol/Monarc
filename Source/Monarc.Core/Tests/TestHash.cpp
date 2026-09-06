#include <doctest/doctest.h>

#include <Monarc/Core/Hash.h>

#include <string_view>

using Monarc::Hash;
using Monarc::HashBytes;
using Monarc::Hasher;
using Monarc::u32;
using Monarc::u64;

namespace {

/// A user type with no built-in Hasher -- exercises the customisation point from outside
/// Hash.h. In an unnamed namespace so it does not collide with any other test's local types
/// once every Tests/*.cpp is linked into one executable.
struct Key {
    int a;
    int b;
};

}  // namespace

/// Specialised at namespace scope, as Hash.h's contract requires. Combines the two members
/// into disjoint halves of a u64 rather than XORing them together, because XOR is
/// commutative and would make the hash order-insensitive -- exactly what the test below
/// checks it is not.
template <>
struct Monarc::Hasher<Key> {
    u64 operator()(const Key& key) const { return HashBytes(&key, sizeof(key)); }
};

TEST_CASE("HashBytes is deterministic and order-sensitive") {
    const char a[] = "monarc";
    const char b[] = "monrac";
    CHECK(HashBytes(a, 6) == HashBytes(a, 6));
    CHECK(HashBytes(a, 6) != HashBytes(b, 6));
}

TEST_CASE("HashBytes distinguishes length even with a shared prefix") {
    CHECK(HashBytes("ab", 2) != HashBytes("abc", 3));
}

TEST_CASE("hashing an empty range is well defined") {
    CHECK(HashBytes(nullptr, 0) == HashBytes("", 0));
}

TEST_CASE("Hasher handles strings through the generic entry point") {
    CHECK(Hash(std::string_view("engine")) == Hash(std::string_view("engine")));
    CHECK(Hash(std::string_view("engine")) != Hash(std::string_view("Engine")));
}

TEST_CASE("integers hash without collapsing neighbouring values") {
    // A weak integer hash sends 1, 2, 3 to adjacent buckets, which degrades linear probing
    // exactly when keys are sequential ids -- the common case in an engine.
    const u64 h1 = Hash(u64{1});
    const u64 h2 = Hash(u64{2});
    const u64 h3 = Hash(u64{3});
    CHECK(h1 != h2);
    CHECK(h2 != h3);
    // Neighbouring inputs must differ in the high bits, not merely the low ones.
    CHECK((h1 >> 32) != (h2 >> 32));
}

TEST_CASE("different integer widths reach the same entry point") {
    CHECK(Hash(u32{7}) == Hash(u32{7}));
    CHECK(Hash(u64{7}) == Hash(u64{7}));
}

TEST_CASE("pointers are hashable") {
    int  value = 0;
    int* p     = &value;
    CHECK(Hash(p) == Hash(p));
    CHECK(Hash(p) != Hash(static_cast<int*>(nullptr)));
}

TEST_CASE("Hasher can be specialised for a user type") {
    // Specialisation lives above, in the Monarc namespace; see Hash.h's contract.
    CHECK(Hasher<Key>{}(Key{1, 2}) == Hasher<Key>{}(Key{1, 2}));
    CHECK(Hasher<Key>{}(Key{1, 2}) != Hasher<Key>{}(Key{2, 1}));
}
