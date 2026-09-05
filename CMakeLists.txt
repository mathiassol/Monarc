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
    message(FATAL_ERROR
        "Monarc supports: ${MONARC_SUPPORTED_COMPILERS}. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")
include(MonarcTargetOptions)

option(MONARC_BUILD_TESTS "Build Monarc unit tests and architecture gates" ON)

if(MONARC_BUILD_TESTS)
    enable_testing()
endif()

message(STATUS "Monarc ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
