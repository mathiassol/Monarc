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

include(MonarcModule)

if(MONARC_BUILD_TESTS)
    include(FetchContent)
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
