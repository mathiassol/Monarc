// doctest's implementation and `main` used to be here. A3 Task 5 moved both to
// TestDeathGuards.cpp for the reason that file gives: `--monarc-death-guard=<name>` has to be
// handled before doctest starts, or doctest's SEH filter catches the debug break a fatal guard
// raises and reports it as a failed assertion rather than letting the process die. Exactly one
// translation unit may define the implementation, and the file that owns `main` is the natural
// one -- which is also what Monarc.Core.Tests now does.
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
