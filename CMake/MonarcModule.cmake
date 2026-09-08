include_guard(GLOBAL)
include(MonarcTargetOptions)

set(MONARC_VALID_KINDS Runtime Tool Editor Test)

# What each kind is permitted to depend on. ADR-0001.
set(MONARC_KIND_ALLOWED_Runtime Runtime)
set(MONARC_KIND_ALLOWED_Tool    Runtime Tool)
set(MONARC_KIND_ALLOWED_Editor  Runtime Tool Editor)
set(MONARC_KIND_ALLOWED_Test    Runtime Tool Editor Test)

define_property(GLOBAL PROPERTY MONARC_ALL_MODULES
    BRIEF_DOCS "Names of every module declared through monarc_module or monarc_app")

define_property(GLOBAL PROPERTY MONARC_ALL_TEST_TARGETS
    BRIEF_DOCS "Names of every test executable built by CMake/MonarcTest.cmake")

# The name of this platform's directory under Private/Platform/.
#
# ADR-0016: platform code lives in per-platform directories and is selected by the build, so
# that a file which cannot compile on this platform is never handed to the compiler. A function
# rather than a variable because MonarcTest.cmake needs the same answer for a module's
# TestSupport/ tree, and two copies of this `if` chain would be two things to keep true.
function(monarc_platform_directory out_var)
    if(WIN32)
        set(${out_var} "Windows" PARENT_SCOPE)
    elseif(APPLE)
        set(${out_var} "Mac" PARENT_SCOPE)
    elseif(UNIX)
        set(${out_var} "Linux" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "monarc_platform_directory: unrecognised target platform")
    endif()
endfunction()

# **`Private/**/TestSupport/` is compiled into a module's test binaries and not into the module.**
#
# The convention exists because of a measured cost. `Monarc.Host.Windowed`'s
# `Detail::WindowTestHooks` -- ten functions, no shipped caller, there so that no test in the
# repository includes `<Windows.h>` -- were compiled into the module, so the linker pulled them
# into every app that links it: `dumpbin /imports` on the Release `Monarc.FirstLight.exe` listed
# `GDI32.dll` plus a dozen USER32 imports no shipped path calls. Moving them into the two test
# directories that need them was considered and rejected (four are not wrappers a test could
# write for itself, and it would put Win32 in two test files and outside the per-platform
# directory ADR-0016 confines it to), so what is needed is a translation unit the module does
# not glob and its test binaries do.
#
# A directory and not a filename suffix, because a directory composes with the rules already in
# force: nested inside `Private/Platform/<Platform>/` it inherits the platform selection below
# *and* stays inside gate 10's exemption, so a test-only Win32 file is still selected by
# directory and still governed by the same conditional rule as the shipping one.
#
# Two places, one name:
#
#   Private/TestSupport/                     platform-neutral test-only code
#   Private/Platform/<Platform>/TestSupport/ this platform's test-only code
#
# Both are read by every test binary of the owning module and by nothing else -- no other target
# has this module's Private/ on its include path -- so a header may live there too.
set(MONARC_TEST_SUPPORT_DIR "TestSupport")

# Declares a static library that participates in the module graph.
function(monarc_module)
    _monarc_declare(LIBRARY ${ARGN})
endfunction()

# Declares an executable that participates in the module graph.
#
# Same metadata, same validation, same module-graph.json entry as monarc_module() -- an app
# is a node in the graph, not an exception to it, which is the whole reason it goes through
# here rather than calling add_executable() directly. Two things differ:
#
#   * It emits add_executable(), and is marked "app": true in module-graph.json.
#   * Nothing may depend on it (enforced in monarc_validate_modules). An app is a link
#     target, not an interface: it has no consumers, so it has no Include/ directory
#     either -- and gate 13 in Tools/check_architecture.py forbids it one rather than
#     merely excusing its absence, because a header under an app's Include/ is never
#     globbed here and so would be governed by the other gates while never compiling.
#
# Apps stay console-subsystem for now: A3's diagnostics are worth more than a hidden
# console window.
function(monarc_app)
    _monarc_declare(EXECUTABLE ${ARGN})
endfunction()

# The shared body of monarc_module() and monarc_app(). target_type is LIBRARY or EXECUTABLE.
#
# Underscore-prefixed because it is not part of the vocabulary a CMakeLists.txt should use:
# a module declares itself as a module or as an app, and the target type follows from that
# rather than being chosen separately.
function(_monarc_declare target_type)
    if(target_type STREQUAL "EXECUTABLE")
        set(_fn "monarc_app")
        set(_is_app TRUE)
        set(_noun "app")
    elseif(target_type STREQUAL "LIBRARY")
        set(_fn "monarc_module")
        set(_is_app FALSE)
        set(_noun "module")
    else()
        message(FATAL_ERROR "_monarc_declare: unknown target type '${target_type}'")
    endif()

    cmake_parse_arguments(ARG "" "NAME;KIND;TIER" "PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

    # Reject anything cmake_parse_arguments did not recognise. Without this, a typo such
    # as PUBLIC_DEP silently lands in ARG_UNPARSED_ARGUMENTS, the dependency is dropped,
    # and the graph reports "rules satisfied" while missing an edge -- which would make
    # every guarantee in ADR-0001 unreliable for the sake of one character.
    if(ARG_UNPARSED_ARGUMENTS)
        list(JOIN ARG_UNPARSED_ARGUMENTS " " _unparsed)
        message(FATAL_ERROR
            "${_fn}(${ARG_NAME}): unrecognised arguments: ${_unparsed}")
    endif()
    if(ARG_KEYWORDS_MISSING_VALUES)
        list(JOIN ARG_KEYWORDS_MISSING_VALUES ", " _empty_keywords)
        message(FATAL_ERROR
            "${_fn}(${ARG_NAME}): keywords given with no value: ${_empty_keywords}")
    endif()

    if(NOT ARG_NAME)
        message(FATAL_ERROR "${_fn}: NAME is required")
    endif()
    if(NOT ARG_KIND IN_LIST MONARC_VALID_KINDS)
        list(JOIN MONARC_VALID_KINDS ", " _valid_kinds)
        message(FATAL_ERROR
            "${_fn}(${ARG_NAME}): KIND '${ARG_KIND}' must be one of: ${_valid_kinds}")
    endif()
    if(NOT DEFINED ARG_TIER)
        message(FATAL_ERROR "${_fn}(${ARG_NAME}): TIER is required")
    endif()
    if(NOT ARG_TIER MATCHES "^[0-4]$")
        message(FATAL_ERROR "${_fn}(${ARG_NAME}): TIER must be 0-4, got '${ARG_TIER}'")
    endif()

    monarc_platform_directory(_monarc_platform)
    set(MONARC_PLATFORM_DIR "${_monarc_platform}" CACHE INTERNAL "")

    # An app has no Include/ to glob: it exports nothing, so there is no public header for
    # anyone to include. Globbing one anyway would put a header in the target's source list
    # where it changes nothing -- a .h there is never compiled -- while reading as support
    # for a public interface an app does not have. Not globbing it is not enough on its own
    # either, since check_architecture.py's gates walk Include/ for every module: the layout
    # gate therefore forbids an app's Include/ outright rather than tolerating its absence.
    set(_globs
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h")
    set(_source_dirs "${CMAKE_CURRENT_SOURCE_DIR}/Private")
    if(NOT _is_app)
        list(APPEND _globs "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h")
        set(_source_dirs "${CMAKE_CURRENT_SOURCE_DIR}/Private or /Include")
    endif()

    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${_globs})

    # Drop sources under any Private/Platform/<other> directory, and under any TestSupport/
    # directory at all -- the latter is the whole of what makes the convention above real on
    # this side. See MONARC_TEST_SUPPORT_DIR for why the cost of not doing it was measurable.
    set(_filtered "")
    foreach(_source IN LISTS _sources)
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_source}")
        if(_rel MATCHES "^Private/Platform/([^/]+)/" AND
           NOT CMAKE_MATCH_1 STREQUAL _monarc_platform)
            continue()
        endif()
        # Anchored to Private/, deliberately. A TestSupport/ directory under Include/ is then
        # just a public directory with an odd name: globbed and governed like any other, which
        # is the honest outcome. Dropping it from the glob instead would leave a header that no
        # target ever compiles while gates 3 and 10 go on reading it -- the "simultaneously
        # governed and dead" trap gate 13 exists to close for an app's Include/.
        if(_rel MATCHES "^Private/(.*/)?${MONARC_TEST_SUPPORT_DIR}/")
            continue()
        endif()
        list(APPEND _filtered "${_source}")
    endforeach()
    set(_sources ${_filtered})

    if(NOT _sources)
        message(FATAL_ERROR "${_fn}(${ARG_NAME}): no sources found under ${_source_dirs}")
    endif()

    if(_is_app)
        add_executable(${ARG_NAME} ${_sources})
        target_include_directories(${ARG_NAME}
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
    else()
        add_library(${ARG_NAME} STATIC ${_sources})
        target_include_directories(${ARG_NAME}
            PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Include"
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
    endif()

    if(ARG_PUBLIC_DEPS)
        target_link_libraries(${ARG_NAME} PUBLIC ${ARG_PUBLIC_DEPS})
    endif()
    if(ARG_PRIVATE_DEPS)
        target_link_libraries(${ARG_NAME} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()

    monarc_set_target_options(${ARG_NAME})
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "Tier ${ARG_TIER}")

    # Record metadata so validation and JSON emission can read it back.
    set_property(GLOBAL APPEND PROPERTY MONARC_ALL_MODULES ${ARG_NAME})
    set_property(GLOBAL PROPERTY MONARC_MOD_${ARG_NAME}_KIND "${ARG_KIND}")
    set_property(GLOBAL PROPERTY MONARC_MOD_${ARG_NAME}_TIER "${ARG_TIER}")
    set_property(GLOBAL PROPERTY MONARC_MOD_${ARG_NAME}_PUBLIC "${ARG_PUBLIC_DEPS}")
    set_property(GLOBAL PROPERTY MONARC_MOD_${ARG_NAME}_PRIVATE "${ARG_PRIVATE_DEPS}")
    set_property(GLOBAL PROPERTY MONARC_MOD_${ARG_NAME}_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(GLOBAL PROPERTY MONARC_MOD_${ARG_NAME}_APP "${_is_app}")

    message(STATUS "  ${_noun} ${ARG_NAME} [${ARG_KIND}, tier ${ARG_TIER}]")
endfunction()

# Called once from the top-level CMakeLists after every module is declared.
# Enforces the rules CMake can check directly; cycles and source-level rules are
# checked by Tools/check_architecture.py (see Task 4).
function(monarc_validate_modules)
    get_property(_modules GLOBAL PROPERTY MONARC_ALL_MODULES)
    set(_errors "")

    foreach(_m IN LISTS _modules)
        get_property(_kind GLOBAL PROPERTY MONARC_MOD_${_m}_KIND)
        get_property(_tier GLOBAL PROPERTY MONARC_MOD_${_m}_TIER)
        get_property(_pub  GLOBAL PROPERTY MONARC_MOD_${_m}_PUBLIC)
        get_property(_priv GLOBAL PROPERTY MONARC_MOD_${_m}_PRIVATE)
        set(_deps ${_pub} ${_priv})

        foreach(_d IN LISTS _deps)
            if(NOT _d IN_LIST _modules)
                list(APPEND _errors
                     "${_m} depends on '${_d}', which is not a Monarc module")
                continue()
            endif()

            get_property(_dkind GLOBAL PROPERTY MONARC_MOD_${_d}_KIND)
            get_property(_dtier GLOBAL PROPERTY MONARC_MOD_${_d}_TIER)
            get_property(_dapp  GLOBAL PROPERTY MONARC_MOD_${_d}_APP)

            # Rule: kind containment (ADR-0001)
            if(NOT _dkind IN_LIST MONARC_KIND_ALLOWED_${_kind})
                list(APPEND _errors
                     "${_m} [${_kind}] may not depend on ${_d} [${_dkind}]")
            endif()

            # Rule: downward only
            if(_dtier GREATER _tier)
                list(APPEND _errors
                     "${_m} [tier ${_tier}] may not depend on ${_d} [tier ${_dtier}]")
            endif()

            # Rule: apps are graph leaves. An app is a link target rather than an
            # interface -- depending on one would mean linking a second main(), and would
            # make "why is this in the export" unanswerable, because an app is where a
            # dependency chain ends rather than something a chain can pass through.
            if(_dapp)
                list(APPEND _errors
                     "${_m} may not depend on ${_d}, which is an app: apps are graph leaves")
            endif()
        endforeach()
    endforeach()

    if(_errors)
        list(JOIN _errors "\n  - " _joined)
        message(FATAL_ERROR "Module graph violations:\n  - ${_joined}")
    endif()

    # Emit module-graph.json. Built as a string because CMake's string(JSON ...)
    # can read JSON but cannot construct it.
    set(_entries "")
    foreach(_m IN LISTS _modules)
        get_property(_kind GLOBAL PROPERTY MONARC_MOD_${_m}_KIND)
        get_property(_tier GLOBAL PROPERTY MONARC_MOD_${_m}_TIER)
        get_property(_pub  GLOBAL PROPERTY MONARC_MOD_${_m}_PUBLIC)
        get_property(_priv GLOBAL PROPERTY MONARC_MOD_${_m}_PRIVATE)
        get_property(_dir  GLOBAL PROPERTY MONARC_MOD_${_m}_DIR)
        get_property(_app  GLOBAL PROPERTY MONARC_MOD_${_m}_APP)
        file(RELATIVE_PATH _reldir "${CMAKE_SOURCE_DIR}" "${_dir}")

        set(_pubjson "")
        foreach(_d IN LISTS _pub)
            list(APPEND _pubjson "\"${_d}\"")
        endforeach()
        list(JOIN _pubjson ", " _pubjson)

        set(_privjson "")
        foreach(_d IN LISTS _priv)
            list(APPEND _privjson "\"${_d}\"")
        endforeach()
        list(JOIN _privjson ", " _privjson)

        # Written for every module, not only for apps: module-graph.json is what
        # `monarc explain` reads, and a reader of the file should not have to know that an
        # absent key means false. Consumers still default it, so a graph written before
        # this key existed still loads.
        if(_app)
            set(_appjson "true")
        else()
            set(_appjson "false")
        endif()

        list(APPEND _entries
"    {
      \"name\": \"${_m}\",
      \"kind\": \"${_kind}\",
      \"tier\": ${_tier},
      \"app\": ${_appjson},
      \"directory\": \"${_reldir}\",
      \"publicDeps\": [${_pubjson}],
      \"privateDeps\": [${_privjson}]
    }")
    endforeach()
    list(JOIN _entries ",\n" _entries)

    # Test targets, and the link line each one actually has.
    #
    # **Not part of the graph, and deliberately a sibling key rather than a module row.** A test
    # target is not a module: nothing may depend on one, it has no tier of its own, and
    # `monarc explain` reads the "modules" array to answer why something is in an export -- a
    # question no test target participates in. It is recorded because gate 14 needs it: the only
    # thing that stops a tier-2 test from reaching a tier-3 header is which libraries it links,
    # and CMake is the only place that knows.
    #
    # Read here rather than in _monarc_add_test_binary because a module's CMakeLists.txt appends
    # to the link line after that function returns -- Monarc.Host.Windowed's device suite and
    # Monarc.RHI.Vulkan's two suites all do. This function runs once from the top-level
    # CMakeLists after every add_subdirectory, which is the first moment the answer is final.
    #
    # LINK_LIBRARIES is the *direct* link line, which is the right granularity: a transitive
    # edge belongs to some module's own dependencies and is already policed above. Everything on
    # it is emitted, Monarc module or not (doctest::doctest, Vulkan::Headers), because this key
    # says what the target links and the gate decides what that means -- a filter here that was
    # wrong would leave the gate reading an empty list and passing vacuously.
    set(_test_entries "")
    get_property(_test_targets GLOBAL PROPERTY MONARC_ALL_TEST_TARGETS)
    foreach(_t IN LISTS _test_targets)
        get_property(_owner GLOBAL PROPERTY MONARC_TEST_${_t}_MODULE)
        get_target_property(_links ${_t} LINK_LIBRARIES)
        set(_linkjson "")
        if(_links)
            foreach(_l IN LISTS _links)
                list(APPEND _linkjson "\"${_l}\"")
            endforeach()
        endif()
        list(JOIN _linkjson ", " _linkjson)
        list(APPEND _test_entries
"    {
      \"name\": \"${_t}\",
      \"module\": \"${_owner}\",
      \"links\": [${_linkjson}]
    }")
    endforeach()
    list(JOIN _test_entries ",\n" _test_entries)
    if(_test_entries)
        set(_test_entries "\n${_test_entries}\n  ")
    endif()

    file(WRITE "${CMAKE_BINARY_DIR}/module-graph.json"
"{
  \"engineVersion\": \"${PROJECT_VERSION}\",
  \"generator\": \"monarc_validate_modules\",
  \"modules\": [
${_entries}
  ],
  \"testTargets\": [${_test_entries}]
}
")

    list(LENGTH _modules _count)
    if(_count EQUAL 0)
        message(WARNING
            "monarc_validate_modules(): no modules were declared. "
            "Is an add_subdirectory() missing?")
        return()
    endif()
    set(_noun "modules")
    if(_count EQUAL 1)
        set(_noun "module")
    endif()
    message(STATUS "Module graph: ${_count} ${_noun}, rules satisfied")
endfunction()
