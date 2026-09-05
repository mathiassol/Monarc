#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <utility>

using Monarc::Array;
using Monarc::SystemAllocator;

namespace {

/// Counts construction and destruction so the tests can prove element lifetimes are correct.
struct Tracked {
    static int s_alive;
    int        value;

    explicit Tracked(int v = 0) : value(v) { ++s_alive; }
    Tracked(const Tracked& other) : value(other.value) { ++s_alive; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++s_alive; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --s_alive; }
};

int Tracked::s_alive = 0;

}  // namespace

TEST_CASE("a new Array is empty and holds no memory") {
    SystemAllocator allocator;
    Array<int> array(allocator);
    CHECK(array.Size() == 0);
    CHECK(array.Capacity() == 0);
    CHECK(array.IsEmpty());
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Push appends and grows") {
    SystemAllocator allocator;
    Array<int> array(allocator);

    for (int i = 0; i < 10; ++i) {
        array.Push(i * i);
    }

    REQUIRE(array.Size() == 10);
    CHECK(array.Capacity() >= 10);
    CHECK(array[0] == 0);
    CHECK(array[3] == 9);
    CHECK(array[9] == 81);
    CHECK(allocator.BytesAllocated() > 0);
}

TEST_CASE("Reserve allocates without changing size") {
    SystemAllocator allocator;
    Array<int> array(allocator);

    array.Reserve(64);
    CHECK(array.Size() == 0);
    CHECK(array.Capacity() >= 64);

    const int* before = array.Data();
    for (int i = 0; i < 64; ++i) {
        array.Push(i);
    }
    CHECK(array.Data() == before);   // no reallocation within reserved capacity
}

TEST_CASE("Array destroys its elements") {
    SystemAllocator allocator;
    CHECK(Tracked::s_alive == 0);
    {
        Array<Tracked> array(allocator);
        for (int i = 0; i < 5; ++i) {
            array.Emplace(i);
        }
        CHECK(Tracked::s_alive == 5);
    }
    CHECK(Tracked::s_alive == 0);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Clear destroys elements but keeps capacity") {
    SystemAllocator allocator;
    Array<Tracked> array(allocator);
    for (int i = 0; i < 4; ++i) {
        array.Emplace(i);
    }
    const Monarc::usize capacity = array.Capacity();

    array.Clear();
    CHECK(array.Size() == 0);
    CHECK(Tracked::s_alive == 0);
    CHECK(array.Capacity() == capacity);
}

TEST_CASE("Pop removes and destroys the last element") {
    SystemAllocator allocator;
    Array<Tracked> array(allocator);
    array.Emplace(1);
    array.Emplace(2);
    CHECK(Tracked::s_alive == 2);

    array.Pop();
    CHECK(array.Size() == 1);
    CHECK(Tracked::s_alive == 1);
    CHECK(array[0].value == 1);
}

TEST_CASE("moving an Array transfers ownership and leaves the source empty") {
    SystemAllocator allocator;
    Array<int> source(allocator);
    source.Push(7);
    source.Push(8);
    const int* data = source.Data();

    Array<int> moved(std::move(source));
    CHECK(moved.Size() == 2);
    CHECK(moved.Data() == data);   // no reallocation
    CHECK(moved[1] == 8);
    CHECK(source.Size() == 0);
    CHECK(source.Data() == nullptr);
}

TEST_CASE("move assignment releases the target's existing memory") {
    SystemAllocator allocator;
    Array<Tracked> target(allocator);
    target.Emplace(1);
    target.Emplace(2);

    Array<Tracked> source(allocator);
    source.Emplace(3);

    target = std::move(source);
    CHECK(target.Size() == 1);
    CHECK(target[0].value == 3);
    CHECK(Tracked::s_alive == 1);   // the two originals were destroyed
}

TEST_CASE("Array is range-for iterable") {
    SystemAllocator allocator;
    Array<int> array(allocator);
    array.Push(1);
    array.Push(2);
    array.Push(3);

    int sum = 0;
    for (int value : array) {
        sum += value;
    }
    CHECK(sum == 6);
}

TEST_CASE("Array frees everything it allocated") {
    SystemAllocator allocator;
    {
        Array<int> array(allocator);
        array.Reserve(256);
        for (int i = 0; i < 256; ++i) {
            array.Push(i);
        }
    }
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("pushing an existing element survives the grow it triggers") {
    // array.Push(array[0]) is legal, and when it forces a reallocation the argument
    // aliases memory the grow is about to free. Constructing the new element after the
    // move would read freed storage -- previously observed as MSVC's 0xDDDDDDDD poison.
    SystemAllocator allocator;
    Array<int> array(allocator);
    array.Reserve(1);
    array.Push(42);
    REQUIRE(array.Size() == array.Capacity());   // the next push must reallocate

    array.Push(array[0]);

    REQUIRE(array.Size() == 2);
    CHECK(array[0] == 42);
    CHECK(array[1] == 42);
}

TEST_CASE("self-push survives a grow for non-trivial element types too") {
    SystemAllocator allocator;
    Array<Tracked> array(allocator);
    array.Reserve(1);
    array.Emplace(7);
    REQUIRE(array.Size() == array.Capacity());

    array.Push(array[0]);

    REQUIRE(array.Size() == 2);
    CHECK(array[0].value == 7);
    CHECK(array[1].value == 7);
}

TEST_CASE("capacity is bounded so the byte size cannot overflow") {
    // Reserve of a count whose byte size wraps used to "succeed" with a tiny buffer while
    // Capacity() reported room for ~1e18 elements.
    SystemAllocator allocator;
    Array<int> array(allocator);
    CHECK(Array<int>::MaxCapacity() == static_cast<Monarc::usize>(-1) / sizeof(int));
    CHECK(Array<int>::MaxCapacity() < static_cast<Monarc::usize>(-1));
    // A request above the ceiling is fatal by contract, so it is not exercised here;
    // the bound itself is what stops the wrap.
    CHECK(array.Capacity() == 0);
}
