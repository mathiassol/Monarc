# Monarc.RHI is a public dependency because VulkanBackend.h names RHI types -- AdapterInfo,
# ApiVersion, Capabilities -- in its own public interface. It was declared PUBLIC in Task 1
# ahead of that being true, so that the graph and module-graph.json matched the plan from the
# first commit; as of Task 2 it is load-bearing. Note that gate 3 polices this module because
# it says TIER 2, not because of this edge: the gate keys off tier and never reads
# dependencies.
monarc_module(
    NAME        Monarc.RHI.Vulkan
    KIND        Runtime
    TIER        2
    PUBLIC_DEPS Monarc.Core Monarc.RHI)

# Vulkan::Headers is the *only* Vulkan target linked anywhere in this build, and it is PRIVATE.
#
# PRIVATE matters twice over. Vulkan is an implementation detail of this module -- no Vulkan
# type appears in its public header (Docs/Rendering/RHI.md; ADR-0014 rule 1) -- and making the
# headers public would hand consumers Vulkan declarations *without* VK_NO_PROTOTYPES below,
# which is the one combination that undoes the loader decision.
#
# Vulkan::Vulkan and vulkan-1.lib are deliberately absent. See the FetchContent block in the
# top-level CMakeLists.txt for why: an import library makes a missing Vulkan runtime a Windows
# loader failure before main.
target_link_libraries(Monarc.RHI.Vulkan PRIVATE Vulkan::Headers)

# VK_NO_PROTOTYPES turns the loader decision from a discipline into a compiler-enforced
# property: without it the headers declare the global vkCreateInstance and friends, and the
# first accidental direct call would either fail to link or, worse, invite someone to "fix" it
# by linking the import library. With it there is no such declaration, so every call has to go
# through Loader's dispatch tables and a direct call does not compile at all.
#
# MONARC_VULKAN_VALIDATION is what ValidationDefault() reads, and it is set here rather than in
# the public header so that exactly one translation unit knows the answer -- a consumer built
# with different flags cannot then disagree with the backend about whether validation is on.
# Written as an ordinary expression in C++ rather than an #if, so a build that failed to define
# it does not compile instead of quietly defaulting to off.
target_compile_definitions(Monarc.RHI.Vulkan PRIVATE
    VK_NO_PROTOTYPES
    $<$<CONFIG:Debug>:MONARC_VULKAN_VALIDATION=1>
    $<$<NOT:$<CONFIG:Debug>>:MONARC_VULKAN_VALIDATION=0>)

# VK_USE_PLATFORM_WIN32_KHR is deliberately NOT set here. It is defined inside exactly two
# translation units -- Private/Platform/Windows/VulkanPlatform.cpp and
# Private/Platform/Windows/VulkanSurface.cpp, both selected by directory rather than by an
# #ifdef -- and in no header, so no platform-neutral source in this module ever sees a Win32
# declaration. See those files' own comments, Private/VulkanPlatform.h, and ADR-0016.
# (This said "VulkanPlatform.cpp and nowhere else" until Task 4 added the second file.)

# Three test outcomes, and they are three because a suite that silently runs nothing while
# showing green is the failure mode this whole phase is shaped to avoid.
#
#   1. Tests/       -- device-free, label `unit`, runs everywhere including CI.
#   2. TestsDevice/ -- device-required, label `gpu`, SKIP_RETURN_CODE 77, so a machine with no
#                      Vulkan device reports Skipped and never Passed.
#   3. The probe     -- always runs, always passes, prints what it found. Registered from
#                      Source/Monarc.FirstLight/CMakeLists.txt, because `--adapters` is already
#                      exactly that program and a second implementation of it would be a second
#                      thing to keep true.
monarc_test_module(Monarc.RHI.Vulkan)
monarc_device_test_module(Monarc.RHI.Vulkan)

# Both test binaries compile against Private/ headers -- Loader.h and Translate.h are where the
# entry-point tables and the pure translation functions live, and neither belongs in a public
# header. monarc_test_module() puts the module's Private/ directory on the test target's
# include path for exactly this; what these two lines add is the Vulkan headers themselves and
# the same VK_NO_PROTOTYPES the module is built with. Without the definition being identical,
# a test would see prototypes the module does not and the two would disagree about what a
# direct call means.
foreach(_test_target Monarc.RHI.Vulkan.Tests Monarc.RHI.Vulkan.DeviceTests)
    if(TARGET ${_test_target})
        target_link_libraries(${_test_target} PRIVATE Vulkan::Headers)
        target_compile_definitions(${_test_target} PRIVATE VK_NO_PROTOTYPES)
    endif()
endforeach()

# The three fatal guards in this module that a test can reach, one CTest entry each.
#
# GPU, so each one carries the `gpu` label and SKIP_RETURN_CODE 77 -- the child is the device
# suite's own binary, whose main already returns 77 for a missing Vulkan runtime, a failed
# instance or an empty adapter list, and Tools/run_death_test.py propagates that. A machine with
# no device reports these Skipped, never Passed.
#
# The debug messenger's own fatal path has no entry. TestsDevice/TestVulkanDevice.cpp says why
# at the head of its death-guard section: the callback is a file-static in an anonymous
# namespace, reaching it means provoking a real validation finding rather than calling anything,
# and it exists only in Debug with the layer installed.
monarc_death_test(
    NAME   Monarc.RHI.Vulkan.Death.BarrierInsideRendering
    TARGET Monarc.RHI.Vulkan.DeviceTests
    GUARD  barrier-inside-rendering
    EXPECT "ICommandList::Barrier(GlobalBarrier) on a list that cannot record one"
    GPU)

monarc_death_test(
    NAME   Monarc.RHI.Vulkan.Death.BufferBarrierStaleHandle
    TARGET Monarc.RHI.Vulkan.DeviceTests
    GUARD  buffer-barrier-stale-handle
    EXPECT "ICommandList::Barrier(BufferBarrier) names a buffer this device does not have"
    GPU)

monarc_death_test(
    NAME   Monarc.RHI.Vulkan.Death.TextureBarrierStaleHandle
    TARGET Monarc.RHI.Vulkan.DeviceTests
    GUARD  texture-barrier-stale-handle
    EXPECT "ICommandList::Barrier(TextureBarrier) names a texture this device does not have"
    GPU)
