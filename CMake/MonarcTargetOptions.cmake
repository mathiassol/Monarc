include_guard(GLOBAL)

# Sanitizer to build with: "address", "undefined", or empty for none.
#
# Worth having because this codebase has produced three aliasing use-after-free hazards in
# three containers -- Array<T>, String, and HashMap -- each caught by careful review. A
# sanitizer catches that whole class mechanically, which matters more as the platform layer
# and the job system arrive and the bugs stop being visible by reading.
#
# ThreadSanitizer is deliberately absent: clang-cl rejects -fsanitize=thread for the MSVC
# target and LLVM ships no TSan runtime for Windows. Data races in Monarc.Jobs will need a
# Linux CI leg or macOS, not a preset.
set(MONARC_SANITIZE "" CACHE STRING "Sanitizer to enable: address, undefined, or empty")
set_property(CACHE MONARC_SANITIZE PROPERTY STRINGS "" "address" "undefined")

if(MONARC_SANITIZE)
    if(NOT MONARC_SANITIZE MATCHES "^(address|undefined)$")
        message(FATAL_ERROR
            "MONARC_SANITIZE must be 'address' or 'undefined', got '${MONARC_SANITIZE}'")
    endif()

    # The CRT choice is per-sanitizer, and getting it wrong fails the link with
    # "/failifmismatch: mismatch detected for 'RuntimeLibrary'" rather than anything
    # informative. ASan's runtime is a DLL and needs the dynamic CRT; UBSan ships only a
    # static standalone runtime and needs the static one. Forced here rather than left to
    # the preset, so a sanitizing build cannot be configured against the wrong CRT.
    if(MONARC_SANITIZE STREQUAL "address")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
    endif()

    # CMake links through lld-link directly rather than through the clang-cl driver, so
    # -fsanitize=... never gets translated into a runtime library and the link fails on
    # undefined __asan_*/__ubsan_* symbols. Ask the compiler where its runtime lives
    # instead of hardcoding a path that embeds the LLVM major version.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
            OUTPUT_VARIABLE MONARC_CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _resource_dir_result)
        if(NOT _resource_dir_result EQUAL 0)
            message(FATAL_ERROR "could not determine Clang's resource directory")
        endif()
        set(_rt "${MONARC_CLANG_RESOURCE_DIR}/lib/windows")

        set(MONARC_SANITIZE_LIBS "")
        set(MONARC_SANITIZE_DLLS "")
        if(MONARC_SANITIZE STREQUAL "address")
            list(APPEND MONARC_SANITIZE_LIBS
                "${_rt}/clang_rt.asan_dynamic-x86_64.lib"
                "${_rt}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
            list(APPEND MONARC_SANITIZE_DLLS "${_rt}/clang_rt.asan_dynamic-x86_64.dll")
        else()
            list(APPEND MONARC_SANITIZE_LIBS
                "${_rt}/clang_rt.ubsan_standalone-x86_64.lib"
                "${_rt}/clang_rt.ubsan_standalone_cxx-x86_64.lib")
        endif()

        foreach(_needed ${MONARC_SANITIZE_LIBS} ${MONARC_SANITIZE_DLLS})
            if(NOT EXISTS "${_needed}")
                message(FATAL_ERROR "sanitizer runtime not found: ${_needed}")
            endif()
        endforeach()
    endif()

    message(STATUS "Sanitizer: ${MONARC_SANITIZE} "
                   "(CRT: ${CMAKE_MSVC_RUNTIME_LIBRARY}; runtime from ${_rt})")
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
            # C4062: a `default`-less switch over an enum that omits an enumerator. This is
            # not redundant with /W4 -- C4062 is one of MSVC's off-by-default warnings and
            # /W4 does not turn it on, so without this line cl compiles a non-exhaustive
            # switch silently while clang-cl rejects the same file (-Wswitch, on at /W4).
            # Verified on a minimal translation unit: `cl /W4 /WX` exits clean, `cl /W4 /WX
            # /w44062` gives "warning C4062: enumerator 'E::C' in switch of enum 'E' is not
            # handled" through C2220. Monarc relies on that error as the mechanism that
            # forces a new enumerator to be given a case -- Monarc.RHI/Private/Types.cpp,
            # Monarc.Core/Private/Error.cpp and Log.cpp all write their switches without a
            # `default` for exactly that reason -- and a guarantee that holds on one of two
            # supported compilers is not one a developer on msvc-debug can lean on.
            #
            # Deliberately not /w44061, which fires even when a `default` IS present: it
            # would force every defensive `default` to enumerate all cases and delete the
            # distinction between "forgot an enumerator" and "chose to handle the rest in
            # one place". C4062 fires only where the code has already opted in by omitting
            # `default`, which is the signal actually wanted.
            /w44062
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

        # UBSan prints and continues by default, so a violation would leave the process
        # exiting zero and CI green. Make it fatal, or it is not a gate.
        if(MONARC_SANITIZE STREQUAL "undefined")
            target_compile_options(${target} PRIVATE
                -fno-sanitize-recover=undefined)
        endif()

        get_target_property(_target_type ${target} TYPE)
        if(_target_type STREQUAL "EXECUTABLE")
            # Incremental linking is incompatible with sanitizer instrumentation.
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
            target_link_libraries(${target} PRIVATE ${MONARC_SANITIZE_LIBS})
            # Any runtime DLL must sit beside the executable, or it will not start.
            foreach(_dll ${MONARC_SANITIZE_DLLS})
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                            "${_dll}" "$<TARGET_FILE_DIR:${target}>"
                    COMMENT "Copying the ${MONARC_SANITIZE} sanitizer runtime beside ${target}")
            endforeach()
        endif()
    endif()
endfunction()
