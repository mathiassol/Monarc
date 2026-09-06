#include <doctest/doctest.h>

#include <Monarc/Core/Containers/HashMap.h>
#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <utility>

using Monarc::HashMap;
using Monarc::StringView;
using Monarc::SystemAllocator;
using Monarc::u64;
using Monarc::usize;

namespace {

/// Counts lifetimes so the tests can prove values are destroyed exactly once.
struct Tracked {
    static int s_alive;
    int        value;

    explicit Tracked(int v = 0) : value(v) { ++s_alive; }
    Tracked(const Tracked& o) : value(o.value) { ++s_alive; }
    Tracked(Tracked&& o) noexcept : value(o.value) { ++s_alive; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --s_alive; }
};

int Tracked::s_alive = 0;

/// Sends every key to the same bucket, forcing the probe sequence to be exercised.
struct Collide {
    int v;
};

// Declared here, inside the anonymous namespace, rather than after it: Collide's operator==
// must live in Collide's own namespace to be found by argument-dependent lookup from
// HashMap<Collide, int>'s template code, which is instantiated from namespace Monarc and so
// cannot see an unqualified operator== declared only at this file's enclosing scope. Declaring
// it out here compiles under MSVC's lenient two-phase lookup but is rejected by clang-cl
// ("neither visible in the template definition nor found by argument-dependent lookup") --
// found by building both, which is what ADR-0003's second compiler is for.
static bool operator==(const Collide& a, const Collide& b) { return a.v == b.v; }

/// Home bucket is always the final slot, so any probe chain wraps to index 0 immediately.
/// This is what exercises backward-shift deletion across the array boundary -- the case a
/// raw-index comparison gets wrong while a masked-distance comparison gets right.
struct WrapToEnd {
    int v;
};

static bool operator==(const WrapToEnd& a, const WrapToEnd& b) { return a.v == b.v; }

}  // namespace

template <>
struct Monarc::Hasher<Collide> {
    u64 operator()(const Collide&) const { return 0; }
};

template <>
struct Monarc::Hasher<WrapToEnd> {
    u64 operator()(const WrapToEnd&) const { return ~u64{0}; }
};

TEST_CASE("a new map is empty and holds no memory") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    CHECK(map.Size() == 0);
    CHECK(map.IsEmpty());
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Insert then Find round-trips") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    map.Insert(1, 100);
    map.Insert(2, 200);

    REQUIRE(map.Size() == 2);
    const int* a = map.Find(1);
    REQUIRE(a != nullptr);
    CHECK(*a == 100);
    CHECK(*map.Find(2) == 200);
    CHECK(map.Find(3) == nullptr);
}

TEST_CASE("inserting an existing key replaces its value without growing the map") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    map.Insert(1, 100);
    map.Insert(1, 999);
    CHECK(map.Size() == 1);
    CHECK(*map.Find(1) == 999);
}

TEST_CASE("Contains agrees with Find") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    map.Insert(5, 50);
    CHECK(map.Contains(5));
    CHECK_FALSE(map.Contains(6));
}

TEST_CASE("Remove deletes only the requested key") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    for (int i = 0; i < 10; ++i) {
        map.Insert(i, i * 10);
    }
    CHECK(map.Remove(4));
    CHECK_FALSE(map.Remove(4));      // already gone
    CHECK(map.Size() == 9);
    CHECK(map.Find(4) == nullptr);
    for (int i = 0; i < 10; ++i) {
        if (i != 4) {
            REQUIRE(map.Find(i) != nullptr);
            CHECK(*map.Find(i) == i * 10);
        }
    }
}

TEST_CASE("the map survives growth across many rehashes") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i) {
        map.Insert(i, i * 3);
    }
    CHECK(map.Size() == kCount);
    for (int i = 0; i < kCount; ++i) {
        REQUIRE(map.Find(i) != nullptr);
        CHECK(*map.Find(i) == i * 3);
    }
}

TEST_CASE("colliding keys all remain findable") {
    // Every key hashes to bucket 0, so this exercises the probe sequence directly.
    SystemAllocator allocator;
    HashMap<Collide, int> map(allocator);
    for (int i = 0; i < 50; ++i) {
        map.Insert(Collide{i}, i);
    }
    CHECK(map.Size() == 50);
    for (int i = 0; i < 50; ++i) {
        REQUIRE(map.Find(Collide{i}) != nullptr);
        CHECK(*map.Find(Collide{i}) == i);
    }
}

TEST_CASE("removal from a fully-colliding map does not strand later entries") {
    // This is the case tombstone-free deletion gets wrong if backward shifting is buggy:
    // removing from the middle of a probe chain must not make its tail unreachable.
    SystemAllocator allocator;
    HashMap<Collide, int> map(allocator);
    for (int i = 0; i < 20; ++i) {
        map.Insert(Collide{i}, i);
    }
    for (int i = 0; i < 20; i += 2) {
        CHECK(map.Remove(Collide{i}));
    }
    CHECK(map.Size() == 10);
    for (int i = 1; i < 20; i += 2) {
        REQUIRE(map.Find(Collide{i}) != nullptr);
        CHECK(*map.Find(Collide{i}) == i);
    }
}

TEST_CASE("values are destroyed exactly once") {
    SystemAllocator allocator;
    CHECK(Tracked::s_alive == 0);
    {
        HashMap<int, Tracked> map(allocator);
        for (int i = 0; i < 50; ++i) {
            map.Insert(i, Tracked{i});
        }
        CHECK(Tracked::s_alive == 50);
        CHECK(map.Remove(10));
        CHECK(Tracked::s_alive == 49);
    }
    CHECK(Tracked::s_alive == 0);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Clear destroys values and empties the map") {
    SystemAllocator allocator;
    HashMap<int, Tracked> map(allocator);
    for (int i = 0; i < 8; ++i) {
        map.Insert(i, Tracked{i});
    }
    map.Clear();
    CHECK(map.Size() == 0);
    CHECK(Tracked::s_alive == 0);
    CHECK(map.Find(0) == nullptr);
}

TEST_CASE("String keys work through the Hasher specialisation") {
    SystemAllocator allocator;
    HashMap<StringView, int> map(allocator);
    map.Insert("alpha", 1);
    map.Insert("beta", 2);
    REQUIRE(map.Find("alpha") != nullptr);
    CHECK(*map.Find("alpha") == 1);
    CHECK(*map.Find("beta") == 2);
    CHECK(map.Find("gamma") == nullptr);
}

TEST_CASE("moving a map transfers ownership and leaves the source empty") {
    SystemAllocator allocator;
    HashMap<int, int> source(allocator);
    source.Insert(1, 10);
    HashMap<int, int> moved(std::move(source));
    CHECK(moved.Size() == 1);
    CHECK(*moved.Find(1) == 10);
    CHECK(source.Size() == 0);
    CHECK(source.Find(1) == nullptr);
}

TEST_CASE("iteration visits every entry exactly once") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    for (int i = 0; i < 20; ++i) {
        map.Insert(i, i);
    }
    int count = 0;
    int sum   = 0;
    for (const auto& entry : map) {
        ++count;
        sum += entry.value;
    }
    CHECK(count == 20);
    CHECK(sum == 190);   // 0..19
}

TEST_CASE("a map releases everything it allocated") {
    SystemAllocator allocator;
    {
        HashMap<int, int> map(allocator);
        for (int i = 0; i < 500; ++i) {
            map.Insert(i, i);
        }
    }
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("removal across the array boundary does not strand a wrapped entry") {
    // Every key homes to the last slot, so entries spill past the end into index 0 onward.
    // Removing from the middle of that chain makes the backward-shift scan cross the
    // boundary; comparing raw indices instead of masked distances strands the tail.
    SystemAllocator allocator;
    HashMap<WrapToEnd, int> map(allocator);
    for (int i = 0; i < 12; ++i) {
        map.Insert(WrapToEnd{i}, i);
    }
    REQUIRE(map.Size() == 12);

    for (int i = 0; i < 12; i += 3) {
        CHECK(map.Remove(WrapToEnd{i}));
    }
    CHECK(map.Size() == 8);

    for (int i = 0; i < 12; ++i) {
        if (i % 3 == 0) {
            CHECK(map.Find(WrapToEnd{i}) == nullptr);
        } else {
            REQUIRE(map.Find(WrapToEnd{i}) != nullptr);
            CHECK(*map.Find(WrapToEnd{i}) == i);
        }
    }
}

TEST_CASE("replacing a key destroys the old value exactly once") {
    // The replace path is otherwise only exercised with int, so a leaked or double-destroyed
    // value would go unnoticed -- silently, which is the worst kind of destructor bug.
    SystemAllocator allocator;
    CHECK(Tracked::s_alive == 0);
    {
        HashMap<int, Tracked> map(allocator);
        map.Insert(1, Tracked{10});
        CHECK(Tracked::s_alive == 1);

        map.Insert(1, Tracked{20});
        CHECK(map.Size() == 1);
        CHECK(map.Find(1)->value == 20);
        CHECK(Tracked::s_alive == 1);   // the old value went, the new one stayed

        for (int i = 0; i < 5; ++i) {
            map.Insert(1, Tracked{i});
        }
        CHECK(map.Size() == 1);
        CHECK(Tracked::s_alive == 1);
    }
    CHECK(Tracked::s_alive == 0);
}
