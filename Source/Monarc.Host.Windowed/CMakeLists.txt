# Tier 3, depending on Core and RHI. Docs/Architecture/Module-Graph.md shows this module's
# eventual dependencies as Engine and Render; neither exists yet, and the entry that adds
# them is Phase D's. The tier does not change either way -- see the open questions in
# Docs/Plans/2026-09-06-phase-a3-rhi-and-first-light.md.
monarc_module(
    NAME        Monarc.Host.Windowed
    KIND        Runtime
    TIER        3
    PUBLIC_DEPS Monarc.Core Monarc.RHI)

monarc_test_module(Monarc.Host.Windowed)
monarc_device_test_module(Monarc.Host.Windowed)

# **The unit tests can be Skipped as well as Passed or Failed, which monarc_test_module() does
# not arrange on its own** -- and they need to be, for a reason no other suite in the tree has.
# Everything under Tests/ elsewhere is a pure function or a container; these open real windows,
# and a process with no interactive session cannot. `Detail::WindowPlatform::HasInteractiveSession`
# says what that means mechanically; TestWindow.cpp's main() checks it and returns 77 before
# doctest runs a case, exactly as the device suites do.
#
# The label stays `unit`, because that is what these are: they need no GPU, no Vulkan and no
# driver, and on a machine with a desktop they run in CI. What they need is a screen to put a
# window on, which is a third axis from "needs a device" and is why this is a property on the
# existing entry rather than a fourth test outcome.
#
# Guarded on the target existing, because monarc_test_module() registers nothing when
# MONARC_BUILD_TESTS is off -- the same guard Monarc.RHI.Vulkan/CMakeLists.txt uses for its
# per-test-target settings.
if(TARGET Monarc.Host.Windowed.Tests)
    set_tests_properties(Monarc.Host.Windowed.Tests PROPERTIES SKIP_RETURN_CODE 77)
endif()

# The device-required suite is where Task 4's headline evidence lives: a swapchain against a
# real window, ten frames acquired and presented, the acquired image copied back and asserted
# to the byte, and the window's own pixels read off the screen.
#
# **It links Monarc.RHI.Vulkan, which this module does not depend on**, and that is deliberate
# rather than a graph violation. Test targets are exempt from the module graph -- they are not
# modules and nothing may depend on them (CMake/MonarcTest.cmake says so) -- and the direction
# here is the harmless one: Monarc.Host.Windowed is tier 3 and Monarc.RHI.Vulkan is tier 2, so
# a test of the tier-3 module reaching *down* to tier 2 is the way the graph already allows
# dependencies to run.
#
# The alternative was putting these cases in Monarc.RHI.Vulkan/TestsDevice/, where the swapchain
# itself lives. That would have made a tier-2 module's tests link a tier-3 module -- upward, and
# the direction the graph exists to forbid -- so the tests live with the window instead. Which
# is also where the thing that makes them hard to write lives: driving resize, minimise,
# maximise and a monitor drag from code needs Win32, and `Detail::WindowTestHooks` is in this
# module.
if(TARGET Monarc.Host.Windowed.DeviceTests)
    target_link_libraries(Monarc.Host.Windowed.DeviceTests PRIVATE Monarc.RHI.Vulkan)
endif()
