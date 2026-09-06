#pragma once

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Types.h>

namespace Monarc {

/// One completed job, as recorded by JobSystem when Config::profileCapacity is nonzero --
/// see JobSystem::CollectProfile, which is how these leave the system.
///
/// `name` is the same non-owning view that was passed to Submit/SubmitAfter/
/// SubmitWithPriority, so it carries the same lifetime assumption as everywhere else this
/// codebase stores a StringView without owning it: it must reference a string literal or
/// otherwise long-lived storage, because JobSystem keeps no copy.
///
/// `startTicks`/`endTicks` are raw Platform::Time::Ticks() readings bracketing the job's
/// own callable -- nothing about locking, queueing or dispatch is included, only the
/// callable's own execution. Stored as ticks rather than converted to seconds so that
/// recording a completion costs one clock read on each side and nothing else; a consumer
/// that wants seconds calls Platform::Time::TicksToSeconds(endTicks - startTicks) itself,
/// paying that conversion only for records someone actually looks at.
struct JobProfileRecord {
    StringView name;
    u32        workerIndex = 0;
    u64        startTicks  = 0;
    u64        endTicks    = 0;
};

}  // namespace Monarc
