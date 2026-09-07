// Device-free tests for Private/ArrayOps.h.
//
// **These exist because the two functions used to be unreachable.** They lived in
// VulkanBackend.cpp's anonymous namespace, where no test could call them -- the exact category
// Task 2's code-quality review found behavioural mutations surviving in. Task 3 gave them a
// second caller (the device's resource pools), which is what moved them into a private header;
// being callable from a test is the part that was worth having anyway.
//
// Over `int` rather than over a Vulkan structure, deliberately: what these two do is arithmetic
// on an `Array<T>`, and a Vulkan type in the test would suggest the behaviour depended on one.

#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <ArrayOps.h>

using Monarc::RHI::Detail::ResizeTo;
using Monarc::RHI::Detail::ShrinkTo;

TEST_CASE("ResizeTo replaces the contents with value-initialised elements") {
    Monarc::SystemAllocator  allocator;
    Monarc::Array<int>       array(allocator);

    array.Push(11);
    array.Push(22);
    REQUIRE(array.Size() == 2);

    ResizeTo(array, 4);

    REQUIRE(array.Size() == 4);
    for (Monarc::usize i = 0; i < array.Size(); ++i) {
        // Value-initialised, not left holding what was there. Vulkan's count-then-fill idiom
        // hands the resulting buffer straight to a driver that may write fewer elements than
        // it said, and the device's resource pools read every slot's `live` flag before
        // anything sets one -- so uninitialised storage here is a stale-adapter or
        // phantom-resource bug rather than an untidiness.
        CHECK(array[i] == 0);
    }
}

TEST_CASE("ResizeTo to zero empties the array") {
    Monarc::SystemAllocator allocator;
    Monarc::Array<int>      array(allocator);
    array.Push(7);

    ResizeTo(array, 0);

    // The case Vulkan enumeration actually hits on a machine with no devices: a count of zero,
    // and an array that must come back empty rather than keeping its previous contents.
    CHECK(array.Size() == 0);
    CHECK(array.IsEmpty());
}

TEST_CASE("ResizeTo reserves once rather than growing as it fills") {
    Monarc::SystemAllocator allocator;
    Monarc::Array<int>      array(allocator);

    ResizeTo(array, 100);

    // Exactly the capacity asked for, which is what says `Reserve` ran before the loop rather
    // than the loop growing the array through Array's own doubling -- that would leave a
    // capacity of 128 here, and would mean the device's pools reallocated part-way through
    // being filled.
    CHECK(array.Size() == 100);
    CHECK(array.Capacity() == 100);
}

TEST_CASE("ShrinkTo drops the tail and never grows") {
    Monarc::SystemAllocator allocator;
    Monarc::Array<int>      array(allocator);
    for (int value = 0; value < 5; ++value) {
        array.Push(value);
    }

    ShrinkTo(array, 3);
    REQUIRE(array.Size() == 3);

    // The elements kept are the *first* three. A version that popped from the front -- or that
    // shrank by rebuilding -- would pass the size assertion and lose the wrong ones, which for
    // `DeduplicateAdapters`' survivors is the whole answer.
    CHECK(array[0] == 0);
    CHECK(array[1] == 1);
    CHECK(array[2] == 2);

    // Asking for more than it holds is a no-op and not a resize. `EnumerateInto` calls this
    // with the count Vulkan reported, which can equal what the array already holds, and the
    // device's `EnumerateAdapters` calls it with the deduplicated count.
    ShrinkTo(array, 10);
    CHECK(array.Size() == 3);
    CHECK(array[2] == 2);

    ShrinkTo(array, 0);
    CHECK(array.IsEmpty());
}
