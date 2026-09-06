#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Jobs/JobProfile.h>
#include <Monarc/Jobs/JobSystem.h>

using Monarc::Array;
using Monarc::JobProfileRecord;
using Monarc::JobSystem;
using Monarc::SystemAllocator;

TEST_CASE("completed jobs are recorded with a name, a worker and a duration") {
    // Threading.md calls visibility a requirement, not a nicety: a scheduler you cannot see
    // into is one you cannot tune.
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64,
                                                .profileCapacity = 128});
    for (int i = 0; i < 8; ++i) {
        const auto handle = jobs.Submit("profiled", [] {});
        REQUIRE(handle.has_value());
        jobs.Wait(*handle);
    }

    Array<JobProfileRecord> records(allocator);
    jobs.CollectProfile(records);
    CHECK(records.Size() == 8);
    for (const JobProfileRecord& record : records) {
        CHECK(record.name == "profiled");
        CHECK(record.workerIndex < jobs.WorkerCount());
        CHECK(record.endTicks >= record.startTicks);
    }
}

TEST_CASE("profiling is off when no capacity is configured") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 2, .maxJobs = 64});
    const auto handle = jobs.Submit("unprofiled", [] {});
    REQUIRE(handle.has_value());
    jobs.Wait(*handle);

    Array<JobProfileRecord> records(allocator);
    jobs.CollectProfile(records);
    CHECK(records.Size() == 0);
}

TEST_CASE("collecting profile records clears them") {
    SystemAllocator allocator;
    JobSystem jobs(allocator, JobSystem::Config{.workerCount = 1, .maxJobs = 64,
                                                .profileCapacity = 64});
    const auto handle = jobs.Submit("once", [] {});
    REQUIRE(handle.has_value());
    jobs.Wait(*handle);

    Array<JobProfileRecord> first(allocator);
    jobs.CollectProfile(first);
    CHECK(first.Size() == 1);

    Array<JobProfileRecord> second(allocator);
    jobs.CollectProfile(second);
    CHECK(second.Size() == 0);
}
