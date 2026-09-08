include_guard(GLOBAL)
include(MonarcTargetOptions)
# For monarc_platform_directory() and MONARC_TEST_SUPPORT_DIR, which the TestSupport/ glob
# below needs. Included explicitly rather than relying on the top-level CMakeLists including
# MonarcModule first -- both files carry include_guard(GLOBAL), so this costs nothing.
include(MonarcModule)

# **Every CTest entry gets a TIMEOUT, because a hang is a failure mode this tree has already
# produced and nothing was bounding it.** Disabling `WindowPlatform::Pump`'s null-handle guard
# makes `Monarc.Host.Windowed.Tests` block forever in `WaitMessage`; CTest does report that as
# `(Timeout)` rather than as slowness, but only after **its own default of 1500 seconds** --
# so a single hang cost 25 minutes per preset, six presets deep in CI, and
# .github/workflows/ci.yml sets no `timeout-minutes` either.
#
# The numbers are measured, not guessed. Slowest real run of each kind across all six presets
# (msvc-debug/release, clang-debug/release/asan/ubsan) on this machine:
#
#   Monarc.Host.Windowed.DeviceTests   8.79 s   <- slowest of all; presents ~90 frames
#   Monarc.RHI.Vulkan.DeviceTests      2.64 s
#   Monarc.Host.Windowed.Tests         1.00 s   <- slowest unit suite; opens real windows
#   Monarc.RHI.Vulkan.Probe            0.45 s
#   Monarc.RHI.Vulkan.RuntimeTests     0.05 s   <- opens a library; creates no instance at all
#   every other unit suite            <0.25 s
#
# A3 Task 5 added two more kinds, at opposite ends. The runtime suite is the cheapest entry in
# the tree -- it opens a library and creates nothing. A death test is slower than it looks: its
# child ends at an unhandled debug break, and Windows Error Reporting spends about two and a
# half seconds on that before the process is gone. Measured on msvc-debug -- the device-free entries 2.4 to
# 2.7 s each and the three device ones 3.6 to 5.4 s, the difference being Vulkan bring-up. So
# 60 s is ~11x the slowest of them, and Tools/run_death_test.py's own 30 s timeout fires first
# anyway, which is deliberate: the harness's message about a guard that hung is more use than
# CTest's about an entry that took too long.
#
# The device figure understates the *worst* case and the headroom is sized for that rather than
# for what was observed. The screen-capture case presents in batches until two readings agree,
# up to 25 batches of 20 frames; it settled after 2 here, so the same suite can legitimately
# present about 580 frames instead of about 90. Under FIFO that is display-rate bound: roughly
# 18 s at 60 Hz and 27 s at 30 Hz. 180 s is therefore ~20x the measured run and ~6.7x that
# worst case, which leaves a hang detectable in three minutes instead of twenty-five while
# still not policing a slow machine. 60 s for the rest is 60x their measured worst.
set(MONARC_TEST_TIMEOUT_SECONDS 60)
set(MONARC_DEVICE_TEST_TIMEOUT_SECONDS 180)

# The shared body of monarc_test_module(), monarc_runtime_test_module() and
# monarc_device_test_module(): build one test executable from `source_dir` under the calling
# module's directory, link doctest and the module, and hand it back through `out_target`.
# Nothing is registered with CTest here -- registration is the only thing the three callers
# actually differ in, and it is the thing that matters.
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

    # The module's TestSupport/ trees, compiled into this binary rather than into the module.
    # MONARC_TEST_SUPPORT_DIR in MonarcModule.cmake holds the convention and the measurement
    # that motivated it; monarc_module() drops exactly these files.
    #
    # Both trees are globbed for every test binary of the module, so a helper several suites
    # need is written once. The per-platform one is named directly rather than filtered afterwards,
    # which is what keeps a Windows-only file from ever reaching a macOS compiler.
    monarc_platform_directory(_monarc_platform)
    file(GLOB_RECURSE _support_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/${MONARC_TEST_SUPPORT_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/Platform/${_monarc_platform}/${MONARC_TEST_SUPPORT_DIR}/*.cpp")

    set(_target "${module}.${target_suffix}")
    add_executable(${_target} ${_test_sources} ${_support_sources})
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

    # Recorded so that gate 14 can read this target's link line back.
    #
    # **The link line is the whole lever, which is why this exists.** Every source-reading gate
    # skips Tests/, TestsRuntime/ and TestsDevice/ by design (see MODULE_SOURCE_DIRS in
    # Tools/check_architecture.py): test targets are not modules. Gate 3 therefore cannot see a
    # tier-2 *test* that includes Monarc/Host/ -- and what stopped one was not a rule but an
    # accident, that _monarc_add_test_binary links only the module under test, so the header is
    # not on the include path. One target_link_libraries line lifts that, exactly as
    # Monarc.Host.Windowed/CMakeLists.txt already does in the allowed direction. Recording the
    # link line turns the accident into a checkable rule; A3 Task 5's entry in Docs/Status.md
    # weighs this against the two alternatives.
    #
    # The links themselves are read at emission time rather than here, because a module's
    # CMakeLists.txt adds to them *after* this function returns.
    set_property(GLOBAL APPEND PROPERTY MONARC_ALL_TEST_TARGETS ${_target})
    set_property(GLOBAL PROPERTY MONARC_TEST_${_target}_MODULE "${module}")

    set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()

# Builds one test executable per module, from that module's Tests/ directory, and
# registers it with CTest. Test targets are exempt from the module graph: they are
# not modules, and nothing may depend on them.
#
# Everything under Tests/ must run on a machine with no GPU, no Vulkan driver, no Vulkan
# runtime library and no display. Anything that needs a device goes in TestsDevice/, and
# anything that needs `vulkan-1.dll` but neither an instance nor an adapter goes in
# TestsRuntime/ -- both below, both reporting Skipped rather than Passed where they cannot run.
function(monarc_test_module module)
    _monarc_add_test_binary(${module} "Tests" "Tests" _target)
    if(NOT _target)
        return()
    endif()

    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES
        LABELS "unit"
        TIMEOUT ${MONARC_TEST_TIMEOUT_SECONDS})
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
        SKIP_RETURN_CODE 77
        TIMEOUT ${MONARC_DEVICE_TEST_TIMEOUT_SECONDS})
endfunction()

# Builds a module's tests that need a Vulkan **runtime** but no **device**, from its
# TestsRuntime/ directory, as a third binary and a third CTest entry.
#
# **This exists because CI turned out to be exactly the machine this describes, which was not
# what the phase plan assumed.** The A3 plan expected GitHub's Windows runners to have no
# `vulkan-1.dll` at all, so that everything loader-shaped was device-gated and unreachable
# there. Measured on run 34241306503 instead: the loader opens, every global entry point
# resolves, and bring-up refuses later, at `VK_KHR_surface`, because no ICD is registered. So
# there is a real middle tier -- a runtime with no instance and no adapter -- and four cases
# that had been sitting behind the device suite's `adapters.IsEmpty()` gate could have been
# running in CI all along. Docs/Status.md holds the measurement and which four.
#
# The gate is `Loader::Open` rather than a device, and it is made in `main` before doctest runs,
# for the same reason monarc_device_test_module() gives: a filter inside a binary that had
# already started reports "0 tests, all passed", and SKIP_RETURN_CODE 77 is the vocabulary for
# "could not run". A machine with no Vulkan at all reports Skipped here; a machine with a loader
# and no ICD runs every case; a machine with a device runs them too.
#
# Label `runtime`, so `ctest -L runtime` runs exactly these and neither `-L unit` nor `-L gpu`
# claims them. `ctest -LE gpu` now runs them, which is the point.
function(monarc_runtime_test_module module)
    _monarc_add_test_binary(${module} "TestsRuntime" "RuntimeTests" _target)
    if(NOT _target)
        return()
    endif()

    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES
        LABELS "runtime"
        SKIP_RETURN_CODE 77
        TIMEOUT ${MONARC_TEST_TIMEOUT_SECONDS})
endfunction()

# Registers one CTest entry per fatal guard: a child process that invokes the guard, and a
# harness that asserts the process died with the guard's own message.
#
# **Monarc's guards that deliberately end the process had no case in any suite until A3 Task 5**,
# and they cannot have an in-process one: the assertion is that the process stops. Each was
# verified once by hand with a scratch program that was then deleted, which is exactly the shape
# of evidence three separate reviews on this branch have caught decaying in other forms.
#
# `TARGET` is an existing test binary -- a mode flag on one is far cheaper than a target per
# guard, and it means the guard is reached through the same linkage the rest of the suite
# already has. `GUARD` names the case its `Tests/TestDeathGuards.cpp` registers; `EXPECT` is a
# substring of what the child must print, and is the guard's own `MONARC_CHECK` literal so that
# the test says *which* guard fired.
#
# `GPU` puts the entry behind the `gpu` label and SKIP_RETURN_CODE 77, the same treatment
# monarc_device_test_module() gives a device suite: the harness propagates a child's 77, so a
# machine with no adapter reports Skipped rather than Passed. Without GPU the entry is labelled
# `unit` and runs in CI, which is where most of these belong -- five of the eight guards that
# can be reached at all need no GPU.
#
# Tools/run_death_test.py holds the four conditions and why each is load-bearing. Its own
# timeout is shorter than the TIMEOUT set here so that a guard edited into a hang is reported by
# the harness's message rather than by CTest's.
function(monarc_death_test)
    if(NOT MONARC_BUILD_TESTS)
        return()
    endif()

    cmake_parse_arguments(ARG "GPU" "NAME;TARGET;GUARD;EXPECT" "" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS)
        list(JOIN ARG_UNPARSED_ARGUMENTS " " _unparsed)
        message(FATAL_ERROR "monarc_death_test(${ARG_NAME}): unrecognised arguments: ${_unparsed}")
    endif()
    foreach(_required NAME TARGET GUARD EXPECT)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "monarc_death_test: ${_required} is required")
        endif()
    endforeach()
    if(NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR
            "monarc_death_test(${ARG_NAME}): ${ARG_TARGET} is not a target. Death tests run an "
            "existing test binary in a child process, so the binary has to exist first.")
    endif()

    add_test(NAME ${ARG_NAME}
        COMMAND ${MONARC_PYTHON_EXECUTABLE}
                "${MONARC_TOOLS_DIR}/run_death_test.py"
                "$<TARGET_FILE:${ARG_TARGET}>"
                "${ARG_GUARD}"
                "${ARG_EXPECT}")

    if(ARG_GPU)
        set_tests_properties(${ARG_NAME} PROPERTIES
            LABELS "gpu"
            SKIP_RETURN_CODE 77
            TIMEOUT ${MONARC_TEST_TIMEOUT_SECONDS})
    else()
        set_tests_properties(${ARG_NAME} PROPERTIES
            LABELS "unit"
            TIMEOUT ${MONARC_TEST_TIMEOUT_SECONDS})
    endif()
endfunction()

# Registers a CTest entry whose job is to print, not to assert.
#
# The fourth of Phase A3's four test outcomes. A probe runs on every machine, reports what it
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
    set_tests_properties(${name} PROPERTIES
        LABELS "probe"
        TIMEOUT ${MONARC_TEST_TIMEOUT_SECONDS})
endfunction()
