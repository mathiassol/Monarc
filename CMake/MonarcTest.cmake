include_guard(GLOBAL)
include(MonarcTargetOptions)

# The shared body of monarc_test_module() and monarc_device_test_module(): build one test
# executable from `source_dir` under the calling module's directory, link doctest and the
# module, and hand it back through `out_target`. Nothing is registered with CTest here --
# registration is the only thing the two callers actually differ in, and it is the thing that
# matters.
#
# Underscore-prefixed because it is not part of the vocabulary a CMakeLists.txt should use, the
# same convention _monarc_declare follows in MonarcModule.cmake.
function(_monarc_add_test_binary module source_dir target_suffix out_target)
    set(${out_target} "" PARENT_SCOPE)
    if(NOT MONARC_BUILD_TESTS)
        return()
    endif()

    file(GLOB_RECURSE _test_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}/*.cpp")

    if(NOT _test_sources)
        return()
    endif()

    set(_target "${module}.${target_suffix}")
    add_executable(${_target} ${_test_sources})
    target_link_libraries(${_target} PRIVATE ${module} doctest::doctest)

    # A module's own tests may include its private headers. target_link_libraries above
    # propagates only the module's PUBLIC include directory (its Include/), so without this a
    # test could not reach the code that is deliberately not part of the module's interface --
    # Monarc.RHI.Vulkan's Loader.h and Translate.h are both private, and both are exactly the
    # kind of pure, table-driven code that most wants a test. Nothing outside the module gains
    # anything from this: no other target has this directory on its include path.
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")

    # doctest forward-declares std::tuple and friends as a compile-speed trick, which
    # MSVC rejects under /W4 /WX (C5285: specialising std templates is forbidden). This
    # is doctest's own documented escape hatch: include the real headers instead.
    target_compile_definitions(${_target} PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    monarc_set_target_options(${_target})
    set_target_properties(${_target} PROPERTIES FOLDER "Tests")

    set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()

# Builds one test executable per module, from that module's Tests/ directory, and
# registers it with CTest. Test targets are exempt from the module graph: they are
# not modules, and nothing may depend on them.
#
# Everything under Tests/ must run on a machine with no GPU, no Vulkan driver and no display.
# Anything that needs a device goes in TestsDevice/ instead -- see below.
function(monarc_test_module module)
    _monarc_add_test_binary(${module} "Tests" "Tests" _target)
    if(NOT _target)
        return()
    endif()

    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES LABELS "unit")
endfunction()

# Builds a module's device-required tests, from its TestsDevice/ directory, as a **separate
# binary and a separate CTest entry** from its unit tests.
#
# This exists for one property, and it is the most important one in Phase A3:
# **SKIP_RETURN_CODE 77 means a machine with no device reports Skipped and never Passed.**
# GitHub's Windows runners have no GPU and almost certainly no Vulkan ICD, so most of
# Monarc.RHI.Vulkan cannot execute in CI at all. A suite that appears green while silently
# running nothing is worse than a red one, because it produces confidence nobody checked. CTest
# has a vocabulary for "could not run" and this uses it, so the fact shows up in the place
# people look.
#
# A separate binary rather than a doctest filter within one, because the decision has to be
# made *before any test runs*: the entry point in TestsDevice/ brings up the backend, and if
# there is no Vulkan runtime or no adapter it returns 77 without registering a single result.
# A filter inside a passing binary would report "0 tests, all passed".
#
# Label `gpu`, so `ctest -L gpu` runs exactly these and `ctest -LE gpu` excludes them.
function(monarc_device_test_module module)
    _monarc_add_test_binary(${module} "TestsDevice" "DeviceTests" _target)
    if(NOT _target)
        return()
    endif()

    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES
        LABELS "gpu"
        SKIP_RETURN_CODE 77)
endfunction()

# Registers a CTest entry whose job is to print, not to assert.
#
# The third of Phase A3's three test outcomes. A probe runs on every machine, reports what it
# found -- loader present or not, instance version, the raw and deduplicated adapter lists --
# and exits zero regardless of what it finds. Its value is that it puts the truth about each
# machine into the CI log, so "CI has no Vulkan" becomes an observed fact in the record rather
# than an assumption in a plan. It is deliberately not the gate for its own findings: the
# device tests above are that.
#
# **"Always passes" needs one caveat, and it is not a small one.** A probe exits zero on any
# *finding*, including no Vulkan at all -- but the program still runs Monarc's own code, and in
# Debug it runs it with validation on and the fatal messenger installed. A VALIDATION-type
# error from the layer therefore stops the process at 0x80000003 and CTest reports the entry
# **Failed**. That is right, and it is worth stating rather than leaving as a surprise: what
# the probe declines to gate on is what it observed about the machine, not whether Monarc used
# Vulkan correctly while observing it.
#
# Label `probe`, so it is neither `unit` nor `gpu` and neither `ctest -L unit` nor
# `ctest -LE gpu` misfiles it.
function(monarc_probe_test name)
    if(NOT MONARC_BUILD_TESTS)
        return()
    endif()

    add_test(NAME ${name} COMMAND ${ARGN})
    set_tests_properties(${name} PROPERTIES LABELS "probe")
endfunction()
