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
#     either, and gate 7 in Tools/check_architecture.py exempts it from having one.
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

    # ADR-0016: platform code lives in per-platform directories and is selected here, so
    # that a file which cannot compile on this platform is never handed to the compiler.
    # Any directory under Private/Platform/ that is not the current platform is excluded.
    if(WIN32)
        set(_monarc_platform "Windows")
    elseif(APPLE)
        set(_monarc_platform "Mac")
    elseif(UNIX)
        set(_monarc_platform "Linux")
    else()
        message(FATAL_ERROR "${_fn}(${ARG_NAME}): unrecognised target platform")
    endif()
    set(MONARC_PLATFORM_DIR "${_monarc_platform}" CACHE INTERNAL "")

    # An app has no Include/ to glob: it exports nothing, so there is no public header for
    # anyone to include. Globbing it anyway would compile a stray header that is not on
    # any include path, which reads as support for something that does not work.
    set(_globs
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h")
    set(_source_dirs "${CMAKE_CURRENT_SOURCE_DIR}/Private")
    if(NOT _is_app)
        list(APPEND _globs "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h")
        set(_source_dirs "${CMAKE_CURRENT_SOURCE_DIR}/Private or /Include")
    endif()

    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${_globs})

    # Drop sources under any Private/Platform/<other> directory.
    set(_filtered "")
    foreach(_source IN LISTS _sources)
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_source}")
        if(_rel MATCHES "^Private/Platform/([^/]+)/" AND
           NOT CMAKE_MATCH_1 STREQUAL _monarc_platform)
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

    file(WRITE "${CMAKE_BINARY_DIR}/module-graph.json"
"{
  \"engineVersion\": \"${PROJECT_VERSION}\",
  \"generator\": \"monarc_validate_modules\",
  \"modules\": [
${_entries}
  ]
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
