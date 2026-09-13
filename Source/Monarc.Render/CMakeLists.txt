monarc_module(
    NAME        Monarc.Render
    KIND        Runtime
    TIER        2
    PUBLIC_DEPS Monarc.Core Monarc.RHI)

monarc_test_module(Monarc.Render)
monarc_device_test_module(Monarc.Render)

# The device suite needs a real Vulkan implementation, which this module does not depend on and
# must not: `Monarc.Render` is tier 2 and talks to `Monarc.RHI`'s interfaces only, which is the
# whole reason `Compile` runs in CI with no GPU.
#
# **Linking the backend into the *test target* is the allowed direction and gate 14 checks it.**
# Test targets are not modules and nothing may depend on them (CMake/MonarcTest.cmake says so),
# so what governs this line is rule 10: a test target links only what its module may depend on.
# `Monarc.RHI.Vulkan` is tier 2, the same tier as `Monarc.Render`, so the edge is one the graph
# already allows. What it would refuse is `Monarc.Host.Windowed` at tier 3 -- which is precisely
# why the swapchain-through-the-graph case lives in that module's device suite instead of this
# one, and why every case here is headless.
#
# Guarded on the target existing, because monarc_device_test_module() registers nothing when
# MONARC_BUILD_TESTS is off.
if(TARGET Monarc.Render.DeviceTests)
    target_link_libraries(Monarc.Render.DeviceTests PRIVATE Monarc.RHI.Vulkan)
endif()
