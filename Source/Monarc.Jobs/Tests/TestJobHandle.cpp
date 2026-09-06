// The one translation unit in Monarc.Jobs.Tests that provides doctest's main() -- mirrors
// Monarc.Core.Tests/TestPlaceholder.cpp, which this module has no equivalent placeholder
// file for.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/HashMap.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Jobs/JobHandle.h>

using Monarc::JobHandle;

TEST_CASE("a default handle is invalid") {
    CHECK_FALSE(JobHandle{}.IsValid());
}

TEST_CASE("handles compare by both index and generation") {
    const JobHandle a = JobHandle::ForTesting(3, 1);
    const JobHandle b = JobHandle::ForTesting(3, 1);
    const JobHandle c = JobHandle::ForTesting(3, 2);   // same slot, later generation
    const JobHandle d = JobHandle::ForTesting(4, 1);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.IsValid());
}

TEST_CASE("a handle is hashable so it can key a map") {
    Monarc::SystemAllocator allocator;
    Monarc::HashMap<JobHandle, int> map(allocator);
    const JobHandle handle = JobHandle::ForTesting(7, 3);
    map.Insert(handle, 1);
    REQUIRE(map.Find(handle) != nullptr);
    CHECK(map.Find(JobHandle::ForTesting(7, 4)) == nullptr);
}
