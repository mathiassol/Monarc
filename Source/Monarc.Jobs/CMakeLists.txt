monarc_module(
    NAME        Monarc.Jobs
    KIND        Runtime
    TIER        0
    PUBLIC_DEPS Monarc.Core)

monarc_test_module(Monarc.Jobs)

# Two of this module's three fatal guards. Tests/TestDeathGuards.cpp says why
# JobSystem::PopQueueLocked's guard is not reachable from any call, legal or illegal, and why
# OnResourceExhausted is provoked at the profile buffer's allocation rather than the job pool's.
# Neither needs a GPU or a display, so both run in CI.
monarc_death_test(
    NAME   Monarc.Jobs.Death.ResourceExhausted
    TARGET Monarc.Jobs.Tests
    GUARD  jobsystem-resource-exhausted
    EXPECT "JobSystem: failed to allocate the profile buffer")

monarc_death_test(
    NAME   Monarc.Jobs.Death.WaitFromWorker
    TARGET Monarc.Jobs.Tests
    GUARD  jobsystem-wait-from-worker
    EXPECT "JobSystem::Wait called from one of this pool's own worker threads")
