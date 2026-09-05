#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>

#include <cstring>
#include <string_view>

using Monarc::SystemAllocator;
using Monarc::uptr;
using Monarc::usize;

TEST_CASE("SystemAllocator returns usable, correctly aligned memory") {
    SystemAllocator allocator;

    void* block = allocator.Allocate(256, 64);
    REQUIRE(block != nullptr);
    CHECK(reinterpret_cast<uptr>(block) % 64 == 0);

    std::memset(block, 0xAB, 256);
    CHECK(static_cast<unsigned char*>(block)[255] == 0xAB);

    allocator.Deallocate(block, 256, 64);
}

TEST_CASE("SystemAllocator tracks outstanding bytes") {
    SystemAllocator allocator;
    CHECK(allocator.BytesAllocated() == 0);

    void* a = allocator.Allocate(100, 16);
    CHECK(allocator.BytesAllocated() == 100);

    void* b = allocator.Allocate(50, 16);
    CHECK(allocator.BytesAllocated() == 150);

    allocator.Deallocate(a, 100, 16);
    CHECK(allocator.BytesAllocated() == 50);

    allocator.Deallocate(b, 50, 16);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("SystemAllocator tolerates deallocating null") {
    SystemAllocator allocator;
    allocator.Deallocate(nullptr, 0, 16);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("SystemAllocator reports a name") {
    SystemAllocator allocator;
    CHECK(std::string_view(allocator.Name()) == "System");
}

TEST_CASE("a SystemAllocator is usable through the IAllocator interface") {
    SystemAllocator concrete;
    Monarc::IAllocator& allocator = concrete;

    void* block = allocator.Allocate(32, alignof(double));
    REQUIRE(block != nullptr);
    CHECK(allocator.BytesAllocated() == 32);
    allocator.Deallocate(block, 32, alignof(double));
    CHECK(allocator.BytesAllocated() == 0);
}
