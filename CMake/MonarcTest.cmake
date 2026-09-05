include_guard(GLOBAL)
include(MonarcTargetOptions)

# Builds one test executable per module, from that module's Tests/ directory, and
# registers it with CTest. Test targets are exempt from the module graph: they are
# not modules, and nothing may depend on them.
function(monarc_test_module module)
    if(NOT MONARC_BUILD_TESTS)
        return()
    endif()

    file(GLOB_RECURSE _test_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp")

    if(NOT _test_sources)
        return()
    endif()

    set(_target "${module}.Tests")
    add_executable(${_target} ${_test_sources})
    target_link_libraries(${_target} PRIVATE ${module} doctest::doctest)

    # doctest forward-declares std::tuple and friends as a compile-speed trick, which
    # MSVC rejects under /W4 /WX (C5285: specialising std templates is forbidden). This
    # is doctest's own documented escape hatch: include the real headers instead.
    target_compile_definitions(${_target} PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    monarc_set_target_options(${_target})
    set_target_properties(${_target} PROPERTIES FOLDER "Tests")

    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES LABELS "unit")
endfunction()
