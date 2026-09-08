#pragma once

#include <Loader.h>

// A test-only helper over Loader's entry-point tables, shared by this module's test binaries.
//
// **Under Private/ rather than in any one Tests/ directory, and that is what makes it
// shareable.** CMake/MonarcTest.cmake puts a module's Private/ on every test target's include
// path, so Tests/, TestsRuntime/ and TestsDevice/ can each include this; nothing outside the
// module has that directory on its path, and nothing in the shipped module includes this
// header. The alternative was the status quo -- one 13-line function written out twice, which
// had to be kept in step with three X-macro lists in two files.
//
// Tests/TestVulkanLoader.cpp explains why the suites assert *different* things with it: the
// no-Vulkan one can only ever call it on a Loader that was never open, and telling a move that
// clears the source's tables from one that copies them needs a source whose tables were
// populated -- which is TestsRuntime/'s, since a runtime is all that takes. That difference
// justified two sets of assertions. It never justified two copies of the helper.

namespace Monarc::RHI::Detail {

/// True when every entry in every one of the loader's three tables is null.
///
/// Written out through the X-macros rather than asking `IsOpen()`, because "closed" and
/// "holding pointers into a module it no longer owns" are exactly the two states this asserts
/// are the same state -- and a table that kept its pointers turns the next call into a jump
/// into unmapped address space rather than a null dereference. Going through the macro lists
/// also means a function added to any of the three tables is covered here without this file
/// being touched.
[[nodiscard]] inline bool AllTablesEmpty(const Loader& loader) {
    bool empty = true;
#define MONARC_VK_CHECK_NULL(name, requirement) \
    empty = empty && loader.Global().name == nullptr;
    MONARC_VK_GLOBAL_FUNCTIONS(MONARC_VK_CHECK_NULL)
#undef MONARC_VK_CHECK_NULL
#define MONARC_VK_CHECK_NULL(name, requirement) \
    empty = empty && loader.Instance().name == nullptr;
    MONARC_VK_INSTANCE_FUNCTIONS(MONARC_VK_CHECK_NULL)
#undef MONARC_VK_CHECK_NULL
#define MONARC_VK_CHECK_NULL(name, requirement) \
    empty = empty && loader.DebugUtils().name == nullptr;
    MONARC_VK_DEBUG_UTILS_FUNCTIONS(MONARC_VK_CHECK_NULL)
#undef MONARC_VK_CHECK_NULL
    return empty;
}

// There is deliberately no `AllDeviceFunctionsResolved` beside this, though the symmetry
// invites one. A `DeviceFunctions` table belongs to a `VulkanDeviceState`, which is private to
// VulkanDevice.cpp, so no test can reach one to assert over -- and a helper with no caller is
// what this module's own comments say does not belong in it. What covers that table instead is
// that every one of its thirty-two entries has a caller on the readback path in
// TestsDevice/TestVulkanDevice.cpp: an entry point silently skipped by a resolver that returned
// success is a null call through it, not a quiet pass.

}  // namespace Monarc::RHI::Detail
