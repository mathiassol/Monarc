include_guard(GLOBAL)

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

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX
            /permissive-        # conformance mode
            /Zc:__cplusplus     # otherwise __cplusplus reports 199711
            /Zc:preprocessor    # conforming preprocessor
            /utf-8
            /EHsc
            /MP)
        # clang-cl accepts the MSVC flags above but warns about a few it ignores.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-unused-command-line-argument)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()
