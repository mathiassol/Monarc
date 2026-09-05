#include <doctest/doctest.h>

#include <Monarc/Core/Memory/ArenaAllocator.h>

#include <cstddef>
#include <string_view>
#include <vector>

using Monarc::ArenaAllocator;
using Monarc::uptr;
using Monarc::usize;

namespace {
constexpr usize kCapacity = 1024;
}

TEST_CASE("ArenaAllocator hands out sequential aligned blocks") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    void* first = arena.Allocate(16, 16);
    REQUIRE(first != nullptr);
    CHECK(reinterpret_cast<uptr>(first) % 16 == 0);

    void* second = arena.Allocate(16, 16);
    REQUIRE(second != nullptr);
    CHECK(second != first);
    CHECK(reinterpret_cast<uptr>(second) > reinterpret_cast<uptr>(first));
}

TEST_CASE("ArenaAllocator honours over-alignment") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    (void)arena.Allocate(1, 1);                // deliberately misalign the cursor
    void* aligned = arena.Allocate(8, 64);
    REQUIRE(aligned != nullptr);
    CHECK(reinterpret_cast<uptr>(aligned) % 64 == 0);
}

TEST_CASE("ArenaAllocator returns nullptr when exhausted rather than overrunning") {
    std::vector<std::byte> backing(64);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    CHECK(arena.Allocate(32, 1) != nullptr);
    CHECK(arena.Allocate(1024, 1) == nullptr);
    CHECK(arena.BytesAllocated() == 32);      // the failed request changes nothing
}

TEST_CASE("Deallocate is a no-op and Reset reclaims everything") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    void* block = arena.Allocate(128, 16);
    CHECK(arena.BytesAllocated() == 128);

    arena.Deallocate(block, 128, 16);
    CHECK(arena.BytesAllocated() == 128);     // arenas do not free individually

    arena.Reset();
    CHECK(arena.BytesAllocated() == 0);

    void* reused = arena.Allocate(128, 16);
    CHECK(reused == block);                   // same memory handed out again
}

TEST_CASE("ArenaAllocator remembers its high-water mark across resets") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    (void)arena.Allocate(500, 1);
    CHECK(arena.HighWaterMark() == 500);

    arena.Reset();
    (void)arena.Allocate(100, 1);
    CHECK(arena.BytesAllocated() == 100);
    CHECK(arena.HighWaterMark() == 500);      // budgeting needs the peak, not the current
}

TEST_CASE("ArenaAllocator reports capacity and name") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "FrameArena");
    CHECK(arena.Capacity() == kCapacity);
    CHECK(std::string_view(arena.Name()) == "FrameArena");
}
