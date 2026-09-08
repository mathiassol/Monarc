#pragma once

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Types.h>

namespace Monarc::RHI::Detail {

/// The two `Array<T>` operations this module needs and `Array<T>` does not have.
///
/// `Array` has `Reserve`, `Emplace` and `Pop` but no `Resize`, and two unrelated things here
/// want one: Vulkan's count-then-fill enumeration idiom, which asks for a count and then wants
/// storage for exactly that many, and the device's fixed-capacity resource pools, which are
/// filled to capacity once at creation and never grown.
///
/// **These lived in VulkanBackend.cpp's anonymous namespace until Task 3, where the second
/// caller appeared.** That file said they would move "into Array if a second one appears"; they
/// moved here instead, and the difference matters twice. `Monarc.Core` is tier 0 and adding a
/// container operation to it is its own change with its own tests, which this task is not. And
/// a function in an anonymous namespace is reachable by no test at all -- the exact category
/// Task 2's review found mutations surviving in. Here, both suites can include this header,
/// which is how `Tests/TestVulkanArrayOps.cpp` exists.
///
/// If a third module wants these, that is when they belong in `Array` itself.

/// Makes `array` hold exactly `count` value-initialised elements, replacing whatever it held.
///
/// Reserves once and then emplaces, so the reallocation happens at most once however far the
/// count is above the current capacity.
template <typename T>
void ResizeTo(Array<T>& array, usize count) {
    array.Clear();
    array.Reserve(count);
    for (usize i = 0; i < count; ++i) {
        array.Emplace();
    }
}

/// Shrinks `array` to `count` elements, destroying the tail. A no-op if it already holds
/// `count` or fewer -- it never grows, which is what makes it the right half of the pair for
/// "Vulkan wrote fewer than it said it would".
template <typename T>
void ShrinkTo(Array<T>& array, usize count) {
    while (array.Size() > count) {
        array.Pop();
    }
}

}  // namespace Monarc::RHI::Detail
