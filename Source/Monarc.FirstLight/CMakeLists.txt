# An app: an executable that is a node in the module graph rather than an exception to it.
# Nothing may depend on it, which monarc_validate_modules() enforces, and it has no Include/
# directory because it exports nothing for anyone to include.
#
# PRIVATE_DEPS throughout, not PUBLIC: public linkage exists to propagate usage requirements
# to consumers, and a leaf has none.
monarc_app(
    NAME         Monarc.FirstLight
    KIND         Runtime
    TIER         3
    PRIVATE_DEPS Monarc.Core Monarc.RHI Monarc.RHI.Vulkan Monarc.Host.Windowed)

# No monarc_test_module(): Monarc.FirstLight has no Tests/ directory, and would be the wrong
# place for one. It is an integration proof that currently exits non-zero on purpose, so
# registering it with CTest would fail the suite for the one reason the suite must stay
# sensitive to. Task 4 gives it a window and a zero exit; the tests that assert on that live
# in the modules doing the work.
