include_guard(GLOBAL)

# Sanitizer to build with, e.g. "address". Empty means none.
#
# Worth having because this codebase has produced three aliasing use-after-free hazards in
# three containers -- Array<T>, String, and HashMap -- each caught by careful review. A
# sanitizer catches that whole class mechanically, which matters more as the platform layer
# and the job system arrive and the bugs stop being visible by reading.
set(MONARC_SANITIZE "" CACHE STRING "Sanitizer to enable: address, or empty for none")
set_property(CACHE MONARC_SANITIZE PROPERTY STRINGS "" "address")

if(MONARC_SANITIZE)
    # AddressSanitizer on Windows requires the dynamic CRT, in every configuration.
    # Forced here rather than left to the preset so a sanitizing build cannot be
    # accidentally configured against the static or debug CRT.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)

    # CMake links through lld-link directly rather than through the clang-cl driver, so
    # -fsanitize=address never gets translated into a runtime library and the link fails
    # on undefined __asan_* symbols. Ask the compiler where its runtime lives instead of
    # hardcoding a path that embeds the LLVM major version.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
            OUTPUT_VARIABLE MONARC_CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _resource_dir_result)
        if(NOT _resource_dir_result EQUAL 0)
            message(FATAL_ERROR "could not determine Clang's resource directory")
        endif()
        set(MONARC_ASAN_DIR "${MONARC_CLANG_RESOURCE_DIR}/lib/windows")
        set(MONARC_ASAN_LIB   "${MONARC_ASAN_DIR}/clang_rt.asan_dynamic-x86_64.lib")
        set(MONARC_ASAN_THUNK "${MONARC_ASAN_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
        set(MONARC_ASAN_DLL   "${MONARC_ASAN_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        foreach(_needed "${MONARC_ASAN_LIB}" "${MONARC_ASAN_THUNK}" "${MONARC_ASAN_DLL}")
            if(NOT EXISTS "${_needed}")
                message(FATAL_ERROR "sanitizer runtime not found: ${_needed}")
            endif()
        endforeach()
    endif()

    message(STATUS "Sanitizer: ${MONARC_SANITIZE} (dynamic CRT; runtime from ${MONARC_ASAN_DIR})")
endif()

# Applied to every Monarc target. Keeps compiler settings in exactly one place.
function(monarc_set_target_options target)
    target_compile_features(${target} PUBLIC cxx_std_23)

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        # ADR-0003: we do not use C++20 modules. CMake 4.2 enables dependency
        # scanning by default at C++23; turning it off is free build time.
        CXX_SCAN_FOR_MODULES OFF)

    target_compile_definitions(${target} PUBLIC
        $<$<CONFIG:Debug>:MONARC_ENABLE_ASSERTS=1>
        $<$<NOT:$<CONFIG:Debug>>:MONARC_ENABLE_ASSERTS=0>)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX
            /permissive-        # conformance mode
            /Zc:__cplusplus     # otherwise __cplusplus reports 199711
            /Zc:preprocessor    # conforming preprocessor
            /utf-8
            /EHsc)
        # No /MP: it parallelises multiple sources within a single cl.exe invocation,
        # but Ninja invokes cl.exe once per source file, so it never has anything to
        # do. Build parallelism comes from Ninja's own scheduler.
        # clang-cl accepts the MSVC flags above but warns about a few it ignores.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-unused-command-line-argument)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()

    if(MONARC_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=${MONARC_SANITIZE})

        get_target_property(_target_type ${target} TYPE)
        if(_target_type STREQUAL "EXECUTABLE")
            # Incremental linking is incompatible with ASan instrumentation.
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
            target_link_libraries(${target} PRIVATE
                "${MONARC_ASAN_LIB}" "${MONARC_ASAN_THUNK}")
            # The runtime DLL must sit beside the executable, or it will not start.
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${MONARC_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copying the AddressSanitizer runtime beside ${target}")
        endif()
    endif()
endfunction()
