monarc_module(
    NAME Monarc.Core
    KIND Runtime
    TIER 0)

monarc_test_module(Monarc.Core)

# The fatal guards, one CTest entry each. Tests/TestDeathGuards.cpp holds the child side and
# says why a guard cannot be tested in process; CMake/MonarcTest.cmake says what the harness
# asserts. No GPU and no display, so all three run in CI.
#
# EXPECT is the guard's own MONARC_CHECK literal, which is what makes each entry a statement
# about *that* guard rather than about the process merely having died.
#
# Guid::Generate's entropy guard is in this module and has no entry: it fires when
# BCryptGenRandom fails, and nothing a test can do makes the platform's preferred RNG fail.
monarc_death_test(
    NAME   Monarc.Core.Death.ArrayAllocationFailed
    TARGET Monarc.Core.Tests
    GUARD  array-allocation-failed
    EXPECT "Array allocation failed")

monarc_death_test(
    NAME   Monarc.Core.Death.HashMapAllocationFailed
    TARGET Monarc.Core.Tests
    GUARD  hashmap-allocation-failed
    EXPECT "HashMap allocation failed")

monarc_death_test(
    NAME   Monarc.Core.Death.StringAllocationFailed
    TARGET Monarc.Core.Tests
    GUARD  string-allocation-failed
    EXPECT "String allocation failed")
