cmake_minimum_required(VERSION 3.28)

project(Monarc
    VERSION 0.1.0
    DESCRIPTION "Monarc game engine"
    LANGUAGES CXX)

# AppleClang is listed deliberately: ADR-0012 schedules a Metal backend once macOS
# hardware is available, and that build reports AppleClang. The previous form was an
# unanchored regex that accepted AppleClang by accident rather than by intent.
set(MONARC_SUPPORTED_COMPILERS MSVC Clang AppleClang)
if(NOT CMAKE_CXX_COMPILER_ID IN_LIST MONARC_SUPPORTED_COMPILERS)
    list(JOIN MONARC_SUPPORTED_COMPILERS ", " MONARC_SUPPORTED_COMPILERS_TEXT)
    message(FATAL_ERROR
        "Monarc supports: ${MONARC_SUPPORTED_COMPILERS_TEXT}. "
        "Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

# monarc_module() sets a FOLDER property per tier; without this it is inert.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")
include(MonarcTargetOptions)

option(MONARC_BUILD_TESTS "Build Monarc unit tests and architecture gates" ON)

if(MONARC_BUILD_TESTS)
    enable_testing()
endif()

message(STATUS "Monarc ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

include(FetchContent)
include(MonarcModule)

# Vulkan-Headers: the API definition, and not a library.
#
# Fetched at a pinned tag rather than located with find_package(Vulkan), for a reason that
# is about CI rather than taste: GitHub's Windows runners have no Vulkan SDK, so
# find_package(Vulkan REQUIRED) would fail at configure time and take all six legs with it.
# An SDK lookup would also make the build depend on whichever SDK a given machine happens
# to have installed, where a pinned tag is the same everywhere. ADR-0014's permissive-licence
# rule is satisfied: Vulkan-Headers is Apache-2.0.
#
# vulkan-sdk-1.4.357.0 matches the SDK installed on the development machine exactly (see
# Docs/Status.md), so the headers cannot describe an API that the local ICDs and validation
# layers do not implement. Monarc requires Vulkan 1.3 core -- the Intel UHD 730 caps at
# 1.3.275 -- and a 1.4 header set is a superset of that; nothing in the engine requires 1.4.
#
# SYSTEM, exactly as doctest below: third-party warnings are not our warnings, and Monarc
# builds at /W4 /WX.
#
# **Only the Vulkan::Headers interface target is ever linked.** Vulkan::Vulkan and
# vulkan-1.lib are deliberately absent from every target in this build: linking the import
# library would turn a missing Vulkan runtime into a Windows loader failure before main,
# which is the exact outcome the loader decision in the Phase A3 plan exists to avoid.
# Monarc.RHI.Vulkan opens vulkan-1.dll itself through Platform::Library and resolves every
# entry point through vkGetInstanceProcAddr, which is also why it compiles with
# VK_NO_PROTOTYPES -- see Source/Monarc.RHI.Vulkan/CMakeLists.txt.
#
# **This block is why the four Clang presets pin CMAKE_C_COMPILER.** Monarc's own project()
# says LANGUAGES CXX, but Vulkan-Headers says project(VULKAN_HEADERS LANGUAGES C CXX), so
# fetching it enables C for the whole build. With CMAKE_CXX_COMPILER pinned to clang-cl and
# CMAKE_C_COMPILER unset, CMake then picks plain `clang` for the C compiler and compiler
# detection fails on the MSVC-style flags it is handed:
# `clang: error: no such file or directory: '/DWIN32'`. CMakePresets.json cannot say this --
# JSON has no comments -- so it is said here, beside the dependency that created the
# requirement. Docs/Status.md's "Known gaps" carries the same note at length.
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        vulkan-sdk-1.4.357.0
    GIT_SHALLOW    TRUE
    SYSTEM)
FetchContent_MakeAvailable(VulkanHeaders)

if(MONARC_BUILD_TESTS)
    # v2.4.12 is the first tag declaring cmake_minimum_required(VERSION 3.5); CMake 4.x
    # hard-rejects anything lower, so v2.4.11 cannot configure here. Fixing it by tag
    # rather than by CMAKE_POLICY_VERSION_MINIMUM, which would be a blanket escape hatch
    # masking the same failure for every future dependency.
    #
    # SYSTEM marks doctest's headers as system headers: third-party warnings are not our
    # warnings, and Monarc builds at /W4 /WX. Standing policy for third-party deps.
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.12
        GIT_SHALLOW    TRUE
        SYSTEM)
    FetchContent_MakeAvailable(doctest)
    include(MonarcTest)
endif()

add_subdirectory(Source/Monarc.Core)
add_subdirectory(Source/Monarc.Jobs)
add_subdirectory(Source/Monarc.RHI)
add_subdirectory(Source/Monarc.RHI.Vulkan)
add_subdirectory(Source/Monarc.Host.Windowed)
add_subdirectory(Source/Monarc.FirstLight)

monarc_validate_modules()

if(MONARC_BUILD_TESTS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME Architecture.Gates
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/Tools/check_architecture.py"
                "${CMAKE_BINARY_DIR}/module-graph.json"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(Architecture.Gates PROPERTIES LABELS "architecture")

    # The gates are string-matching over source trees -- the kind of code that regresses
    # silently, and which has already needed two non-obvious fixes. These tests build real
    # violating trees and assert each gate fires.
    add_test(
        NAME Architecture.GateTests
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/Tools/test_check_architecture.py")
    set_tests_properties(Architecture.GateTests PROPERTIES LABELS "architecture")
endif()
