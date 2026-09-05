cmake_minimum_required(VERSION 3.28)

project(Monarc
    VERSION 0.1.0
    DESCRIPTION "Monarc game engine"
    LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC|Clang")
    message(FATAL_ERROR
        "Monarc supports MSVC and Clang. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")
include(MonarcTargetOptions)

option(MONARC_BUILD_TESTS "Build Monarc unit tests and architecture gates" ON)

if(MONARC_BUILD_TESTS)
    enable_testing()
endif()

message(STATUS "Monarc ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
