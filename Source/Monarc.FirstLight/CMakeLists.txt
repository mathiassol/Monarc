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

# Monarc.FirstLight itself is deliberately not a CTest entry. It is an integration proof that
# currently exits non-zero on purpose -- the window arrives in Task 4 -- so registering it
# would fail the suite for the one reason the suite must stay sensitive to.
#
# Its `--adapters` mode is a different program with a different contract: it reports what
# Vulkan sees on this machine and exits zero whatever it finds, including on a machine with no
# Vulkan at all. That is exactly the probe the Phase A3 plan asks for, so it is registered as
# one rather than reimplemented as one -- a second copy of "print the adapter lists" would be
# a second thing to keep true, and the probe's whole value is that its output is trustworthy.
#
# Always runs, always passes, and its findings are read rather than asserted on. The gate for
# adapter enumeration is Monarc.RHI.Vulkan.DeviceTests, which reports Skipped when there is no
# device; this entry is what makes "the runner has no Vulkan" an observed fact in the CI log
# instead of an assumption in a plan.
monarc_probe_test(Monarc.RHI.Vulkan.Probe $<TARGET_FILE:Monarc.FirstLight> --adapters)
