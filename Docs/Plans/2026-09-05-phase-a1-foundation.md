# Phase A1 — Foundation and the Build Gates

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CMake build that mechanically enforces Monarc's module graph, plus `Monarc.Core`'s memory and diagnostics foundation — allocators, containers, logging, asserts, and `Result` — all under test.

**Architecture:** Modules are declared with a `monarc_module()` CMake function carrying `KIND` and `TIER`. Direct rule violations (bad kind, upward tier dependency, unknown dependency) fail at configure time. Structural rules that CMake handles badly (cycles, source-level include rules, platform containment) are checked by `Tools/check_architecture.py`, wired into CTest so they fail a test run. The build emits `module-graph.json`, which is the artifact `monarc explain` will later read. `Monarc.Core` never allocates through an ambient global: every allocating type takes an `IAllocator&`.

**Tech Stack:** C++23, MSVC 19.51 + Clang 22.1.8 (`clang-cl`), CMake 4.2 + Ninja, doctest for unit tests, Python 3.14 for architecture gates.

**Reference docs:** [Module-Graph](../Architecture/Module-Graph.md), [ADR-0001](../Architecture/Decisions/ADR-0001-layered-modules.md), [ADR-0003](../Architecture/Decisions/ADR-0003-cpp23-baseline.md), [ADR-0011](../Architecture/Decisions/ADR-0011-build-system.md), [Memory](../Runtime/Memory.md), [M0](../Milestones/M0-First-Light.md).

---

## File Structure

```
CMakeLists.txt                          top level: options, toolchain settings, validation call
CMakePresets.json                       pinned toolchains: msvc-debug, msvc-release, clang-debug
CMake/
  MonarcModule.cmake                    monarc_module(), monarc_validate_modules(), JSON emission
  MonarcTargetOptions.cmake             warnings, C++23, /Zc:__cplusplus, scan-for-modules off
  MonarcTest.cmake                      monarc_test_module() and doctest wiring
Tools/
  check_architecture.py                 gates: cycles, kind/tier, include rules, platform containment
Source/
  Monarc.Core/
    CMakeLists.txt
    Include/Monarc/Core/
      Types.h                           fixed-width aliases
      Assert.h                          MONARC_ASSERT / MONARC_CHECK, installable handler
      Error.h                           ErrorCode, Error, Result<T>, Status
      Log.h                             categories, levels, MONARC_LOG
      Memory/Allocator.h                IAllocator
      Memory/SystemAllocator.h          general-purpose, wraps aligned malloc
      Memory/ArenaAllocator.h           linear bump allocator with Reset
      Containers/Array.h                dynamic array over an IAllocator
    Private/
      Assert.cpp  Error.cpp  Log.cpp
      Memory/SystemAllocator.cpp  Memory/ArenaAllocator.cpp
    Tests/
      TestAssert.cpp  TestError.cpp  TestSystemAllocator.cpp
      TestArenaAllocator.cpp  TestArray.cpp  TestLog.cpp
```

**Rationale for the split:** each header is one concept, so a file stays small enough to hold in context. `Memory/` and `Containers/` are separate directories because they will each grow to a dozen files by M0's end, and because the include-rule gate operates on paths.

---

## Task 1: Repository skeleton that configures and builds

**Files:**
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `CMake/MonarcTargetOptions.cmake`

- [ ] **Step 1: Create the shared target-options module**

`CMake/MonarcTargetOptions.cmake`:

```cmake
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
endfunction()
```

- [ ] **Step 2: Create the top-level CMakeLists**

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.28)

project(Monarc
    VERSION 0.1.0
    DESCRIPTION "Monarc game engine"
    LANGUAGES CXX)

# AppleClang is listed deliberately: ADR-0012 schedules a Metal backend once macOS
# hardware is available, and that build reports AppleClang. An unanchored regex would
# accept AppleClang by accident rather than by intent.
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
```

- [ ] **Step 3: Create CMakePresets.json**

`CMakePresets.json`:

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 28, "patch": 0 },
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/Build/${presetName}",
      "cacheVariables": {
        "CMAKE_MAKE_PROGRAM": "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe",
        "MONARC_BUILD_TESTS": "ON"
      }
    },
    {
      "name": "msvc-debug",
      "inherits": "base",
      "displayName": "MSVC Debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" }
    },
    {
      "name": "msvc-release",
      "inherits": "base",
      "displayName": "MSVC Release",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" }
    },
    {
      "name": "clang-debug",
      "inherits": "base",
      "displayName": "Clang Debug (ADR-0003 second compiler)",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_CXX_COMPILER": "C:/Program Files/LLVM/bin/clang-cl.exe"
      }
    }
  ],
  "buildPresets": [
    { "name": "msvc-debug",   "configurePreset": "msvc-debug" },
    { "name": "msvc-release", "configurePreset": "msvc-release" },
    { "name": "clang-debug",  "configurePreset": "clang-debug" }
  ],
  "testPresets": [
    {
      "name": "msvc-debug",
      "configurePreset": "msvc-debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "clang-debug",
      "configurePreset": "clang-debug",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

> The absolute paths to Ninja and `clang-cl` are correct for this machine and are
> deliberately committed so a fresh clone builds without setup. Another machine overrides
> them in `CMakeUserPresets.json`, which is already gitignored.

- [ ] **Step 4: Configure and verify**

All build commands must run from a Visual Studio developer environment, because Git Bash puts
MSYS2's `g++` ahead of MSVC on `PATH`. From PowerShell:

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-debug'
```

Expected: `Monarc 0.1.0 | MSVC 19.51.36244.0`, then `Configuring done` and `Generating done`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt CMakePresets.json CMake/MonarcTargetOptions.cmake
git commit -m "build: top-level CMake project with pinned toolchain presets"
```

---

## Task 2: `monarc_module()` with configure-time rule enforcement

**Files:**
- Create: `CMake/MonarcModule.cmake`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the module function**

`CMake/MonarcModule.cmake`:

```cmake
include_guard(GLOBAL)
include(MonarcTargetOptions)

set(MONARC_VALID_KINDS Runtime Tool Editor Test)

# What each kind is permitted to depend on. ADR-0001.
set(MONARC_KIND_ALLOWED_Runtime Runtime)
set(MONARC_KIND_ALLOWED_Tool    Runtime Tool)
set(MONARC_KIND_ALLOWED_Editor  Runtime Tool Editor)
set(MONARC_KIND_ALLOWED_Test    Runtime Tool Editor Test)

define_property(GLOBAL PROPERTY MONARC_ALL_MODULES
    BRIEF_DOCS "Names of every module declared through monarc_module")

function(monarc_module)
    cmake_parse_arguments(ARG "" "NAME;KIND;TIER" "PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

    if(NOT ARG_NAME)
        message(FATAL_ERROR "monarc_module: NAME is required")
    endif()
    if(NOT ARG_KIND IN_LIST MONARC_VALID_KINDS)
        message(FATAL_ERROR
            "monarc_module(${ARG_NAME}): KIND '${ARG_KIND}' must be one of: ${MONARC_VALID_KINDS}")
    endif()
    if(NOT DEFINED ARG_TIER)
        message(FATAL_ERROR "monarc_module(${ARG_NAME}): TIER is required")
    endif()
    if(NOT ARG_TIER MATCHES "^[0-4]$")
        message(FATAL_ERROR "monarc_module(${ARG_NAME}): TIER must be 0-4, got '${ARG_TIER}'")
    endif()

    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h")

    if(NOT _sources)
        message(FATAL_ERROR "monarc_module(${ARG_NAME}): no sources found under "
                            "${CMAKE_CURRENT_SOURCE_DIR}/Private or /Include")
    endif()

    add_library(${ARG_NAME} STATIC ${_sources})

    target_include_directories(${ARG_NAME}
        PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")

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

    message(STATUS "  module ${ARG_NAME} [${ARG_KIND}, tier ${ARG_TIER}]")
endfunction()
```

- [ ] **Step 2: Write the validation function in the same file**

Append to `CMake/MonarcModule.cmake`:

```cmake
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
        endforeach()
    endforeach()

    if(_errors)
        list(JOIN _errors "\n  - " _joined)
        message(FATAL_ERROR "Module graph violations:\n  - ${_joined}")
    endif()

    list(LENGTH _modules _count)
    message(STATUS "Module graph: ${_count} modules, rules satisfied")
endfunction()
```

- [ ] **Step 3: Wire it into the top level**

Modify `CMakeLists.txt` — replace the `if(MONARC_BUILD_TESTS)` block and everything after it with:

```cmake
option(MONARC_BUILD_TESTS "Build Monarc unit tests and architecture gates" ON)

if(MONARC_BUILD_TESTS)
    enable_testing()
endif()

message(STATUS "Monarc ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

include(MonarcModule)

add_subdirectory(Source/Monarc.Core)

monarc_validate_modules()
```

- [ ] **Step 4: Prove the rules actually fire**

Create a throwaway module that violates a rule, and confirm configure fails. Create
`Source/Monarc.Core/CMakeLists.txt` temporarily as:

```cmake
monarc_module(
    NAME Monarc.Core
    KIND Runtime
    TIER 0
    PUBLIC_DEPS Monarc.DoesNotExist)
```

You also need a source file for the glob to succeed — create `Source/Monarc.Core/Private/Placeholder.cpp` containing:

```cpp
namespace Monarc { int PlaceholderSymbol = 0; }
```

Run:

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-debug'
```

Expected: `CMake Error` containing
`Monarc.Core depends on 'Monarc.DoesNotExist', which is not a Monarc module`.

This is the point of the task — a rule you have not seen fail is a rule you do not have.

- [ ] **Step 5: Fix the module declaration and reconfigure**

Replace `Source/Monarc.Core/CMakeLists.txt` with:

```cmake
monarc_module(
    NAME Monarc.Core
    KIND Runtime
    TIER 0)
```

Run the configure command from Step 4 again.

Expected: `module Monarc.Core [Runtime, tier 0]` and `Module graph: 1 modules, rules satisfied`.

- [ ] **Step 6: Commit**

```bash
git add CMake/MonarcModule.cmake CMakeLists.txt Source/Monarc.Core/CMakeLists.txt Source/Monarc.Core/Private/Placeholder.cpp
git commit -m "build: monarc_module with configure-time kind and tier enforcement"
```

---

## Task 3: Emit `module-graph.json`

**Files:**
- Modify: `CMake/MonarcModule.cmake`

The [Build and Export](../Product/Build-And-Export.md) design depends on the module graph
being queryable data. Emitting it now means `monarc explain` later reads a file that has
existed and been correct since the first module.

- [ ] **Step 1: Add JSON emission to `monarc_validate_modules`**

In `CMake/MonarcModule.cmake`, insert this immediately before the final
`message(STATUS "Module graph: ...")` line inside `monarc_validate_modules()`:

```cmake
    # Emit module-graph.json. Built as a string because CMake's string(JSON ...)
    # can read JSON but cannot construct it.
    set(_entries "")
    foreach(_m IN LISTS _modules)
        get_property(_kind GLOBAL PROPERTY MONARC_MOD_${_m}_KIND)
        get_property(_tier GLOBAL PROPERTY MONARC_MOD_${_m}_TIER)
        get_property(_pub  GLOBAL PROPERTY MONARC_MOD_${_m}_PUBLIC)
        get_property(_priv GLOBAL PROPERTY MONARC_MOD_${_m}_PRIVATE)
        get_property(_dir  GLOBAL PROPERTY MONARC_MOD_${_m}_DIR)
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

        list(APPEND _entries
"    {
      \"name\": \"${_m}\",
      \"kind\": \"${_kind}\",
      \"tier\": ${_tier},
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
```

- [ ] **Step 2: Reconfigure and inspect the output**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-debug' && cat Build/msvc-debug/module-graph.json
```

Expected exactly:

```json
{
  "engineVersion": "0.1.0",
  "generator": "monarc_validate_modules",
  "modules": [
    {
      "name": "Monarc.Core",
      "kind": "Runtime",
      "tier": 0,
      "directory": "Source/Monarc.Core",
      "publicDeps": [],
      "privateDeps": []
    }
  ]
}
```

- [ ] **Step 3: Verify it parses as JSON**

```bash
python -c "import json;d=json.load(open('Build/msvc-debug/module-graph.json'));print(f\"{len(d['modules'])} module(s), engine {d['engineVersion']}\")"
```

Expected: `1 module(s), engine 0.1.0`

- [ ] **Step 4: Commit**

```bash
git add CMake/MonarcModule.cmake
git commit -m "build: emit module-graph.json as a build artifact"
```

---

## Task 4: Architecture gates as CTest tests

**Files:**
- Create: `Tools/check_architecture.py`
- Modify: `CMakeLists.txt`

This implements M0 gates 2, 3, 7 and 10. Gates 1, 5 and 9 need modules that do not exist yet;
they are added in later phases against this same harness.

- [ ] **Step 1: Write the gate script**

`Tools/check_architecture.py`:

```python
#!/usr/bin/env python3
"""Monarc architecture gates.

Checks the structural rules that CMake cannot check directly. Run by CTest, so a
violation fails a test run rather than merely being noticed in review.

Usage:  check_architecture.py <module-graph.json> <repo-root>
"""
from __future__ import annotations

import json
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".hpp", ".inl", ".cpp"}

# Rule 4 of Module-Graph.md, generalised: for each tier, the tiers it may NOT
# reference at source level. Tier 2 is the renderer package boundary (ADR-0007).
FORBIDDEN_INCLUDES = {
    2: ["Monarc/Reflect/", "Monarc/Serialize/", "Monarc/Assets/", "Monarc/World/"],
}

# Rule 7: platform-conditional compilation is confined to Core/Platform.
#
# Deliberately excludes _MSC_VER and __clang__. Those are *compiler* macros, not platform
# macros, and ADR-0012's rule is about platform portability. Compiler differences are
# legitimate anywhere -- that is what building with two compilers under /WX is for.
PLATFORM_MACROS = re.compile(
    r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b("
    r"_WIN32|_WIN64|__APPLE__|__linux__|__ANDROID__|TARGET_OS_[A-Z]+"
    r")\b"
)
PLATFORM_EXEMPT_DIRS = ("Source/Monarc.Core/Include/Monarc/Core/Platform",
                        "Source/Monarc.Core/Private/Platform")


class Gate:
    def __init__(self, number: int, name: str) -> None:
        self.number, self.name, self.failures = number, name, []

    def fail(self, message: str) -> None:
        self.failures.append(message)

    def report(self) -> bool:
        status = "PASS" if not self.failures else "FAIL"
        print(f"[{status}] Gate {self.number}: {self.name}")
        for f in self.failures:
            print(f"         {f}")
        return not self.failures


def iter_sources(root: pathlib.Path, module_dir: str):
    base = root / module_dir
    if not base.is_dir():
        return
    for path in base.rglob("*"):
        if path.suffix in SOURCE_SUFFIXES and path.is_file():
            yield path


def gate_acyclic(modules: dict) -> Gate:
    gate = Gate(2, "module graph is acyclic")
    colour: dict[str, int] = {}          # 0 = visiting, 1 = done

    def visit(name: str, stack: list[str]) -> None:
        state = colour.get(name)
        if state == 1:
            return
        if state == 0:
            cycle = " -> ".join(stack[stack.index(name):] + [name])
            gate.fail(f"cycle: {cycle}")
            return
        colour[name] = 0
        for dep in modules[name]["publicDeps"] + modules[name]["privateDeps"]:
            if dep in modules:
                visit(dep, stack + [name])
        colour[name] = 1

    for name in modules:
        visit(name, [])
    return gate


def gate_package_boundary(modules: dict, root: pathlib.Path) -> Gate:
    gate = Gate(3, "renderer package boundary (tier 2 sees no tier 1 or 3)")
    for name, mod in modules.items():
        forbidden = FORBIDDEN_INCLUDES.get(mod["tier"])
        if not forbidden:
            continue
        for path in iter_sources(root, mod["directory"]):
            for lineno, line in enumerate(
                    path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if not line.lstrip().startswith("#include"):
                    continue
                for bad in forbidden:
                    if bad in line:
                        rel = path.relative_to(root).as_posix()
                        gate.fail(f"{rel}:{lineno} includes {bad} (module {name})")
    return gate


def gate_platform_containment(modules: dict, root: pathlib.Path) -> Gate:
    gate = Gate(10, "platform conditionals only in Core/Platform")
    for mod in modules.values():
        for path in iter_sources(root, mod["directory"]):
            rel = path.relative_to(root).as_posix()
            if any(rel.startswith(d) for d in PLATFORM_EXEMPT_DIRS):
                continue
            for lineno, line in enumerate(
                    path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if PLATFORM_MACROS.match(line):
                    gate.fail(f"{rel}:{lineno} {line.strip()}")
    return gate


def gate_layout(modules: dict, root: pathlib.Path) -> Gate:
    gate = Gate(7, "every module follows the Include/Private layout")
    for name, mod in modules.items():
        base = root / mod["directory"]
        if not (base / "Include").is_dir():
            gate.fail(f"{name}: missing {mod['directory']}/Include")
        if not (base / "Private").is_dir():
            gate.fail(f"{name}: missing {mod['directory']}/Private")
    return gate


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    graph_path, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
    graph = json.loads(graph_path.read_text(encoding="utf-8"))
    modules = {m["name"]: m for m in graph["modules"]}

    print(f"Monarc architecture gates | {len(modules)} module(s) | engine {graph['engineVersion']}")

    gates = [
        gate_acyclic(modules),
        gate_package_boundary(modules, root),
        gate_layout(modules, root),
        gate_platform_containment(modules, root),
    ]

    passed = all(g.report() for g in gates)
    print("all gates passed" if passed else "ARCHITECTURE GATES FAILED")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Wire the gates into CTest**

In `CMakeLists.txt`, replace the final `monarc_validate_modules()` line with:

```cmake
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
endif()
```

- [ ] **Step 3: Create the directory layout the gate expects**

The layout gate requires `Include/` and `Private/`. Create the public header directory and
move the placeholder:

```bash
mkdir -p Source/Monarc.Core/Include/Monarc/Core Source/Monarc.Core/Tests
```

- [ ] **Step 4: Run the gates and verify they pass**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-debug && ctest --preset msvc-debug'
```

Expected output includes:

```
[PASS] Gate 2: module graph is acyclic
[PASS] Gate 3: renderer package boundary (tier 2 sees no tier 1 or 3)
[PASS] Gate 7: every module follows the Include/Private layout
[PASS] Gate 10: platform conditionals only in Core/Platform
all gates passed
100% tests passed, 0 tests failed out of 1
```

- [ ] **Step 5: Prove gate 10 fires**

Temporarily add this to `Source/Monarc.Core/Private/Placeholder.cpp`:

```cpp
#ifdef _WIN32
namespace Monarc { int WindowsOnlySymbol = 1; }
#endif
```

Run the ctest command from Step 4.

Expected: `[FAIL] Gate 10` naming `Source/Monarc.Core/Private/Placeholder.cpp:2`, and
`ARCHITECTURE GATES FAILED`. Then **remove those three lines** and re-run to confirm PASS.

- [ ] **Step 6: Commit**

```bash
git add Tools/check_architecture.py CMakeLists.txt
git commit -m "build: architecture gates for cycles, package boundary, layout, platform containment"
```

---

## Task 5: Test harness with doctest

**Files:**
- Create: `CMake/MonarcTest.cmake`
- Modify: `CMakeLists.txt`
- Modify: `Source/Monarc.Core/CMakeLists.txt`
- Create: `Source/Monarc.Core/Tests/TestPlaceholder.cpp`
- Modify: `Docs/Architecture/Decisions/ADR-0014-dependency-policy.md`

doctest is a single-header MIT test framework. It compiles far faster than Catch2, which
matters on six cores. A test framework is commodity, not engine character, so
[ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md) permits it — but the
table there must record it.

- [ ] **Step 1: Add doctest via FetchContent**

In `CMakeLists.txt`, insert immediately after the `include(MonarcModule)` line:

```cmake
if(MONARC_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(doctest)
    include(MonarcTest)
endif()
```

- [ ] **Step 2: Write the test-module helper**

`CMake/MonarcTest.cmake`:

```cmake
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
    monarc_set_target_options(${_target})
    set_target_properties(${_target} PROPERTIES FOLDER "Tests")

    add_test(NAME ${_target} COMMAND ${_target})
    set_tests_properties(${_target} PROPERTIES LABELS "unit")
endfunction()
```

- [ ] **Step 3: Write a failing test**

`Source/Monarc.Core/Tests/TestPlaceholder.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("the test harness runs") {
    CHECK(2 + 2 == 5);
}
```

Add to the end of `Source/Monarc.Core/CMakeLists.txt`:

```cmake
monarc_test_module(Monarc.Core)
```

- [ ] **Step 4: Run and verify it fails**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-debug && cmake --build --preset msvc-debug && ctest --preset msvc-debug'
```

Expected: `Monarc.Core.Tests` **fails**, reporting `2 + 2 == 5` with `values: 4 == 5`.
This confirms failures actually surface rather than being swallowed.

- [ ] **Step 5: Correct the test and re-run**

Change the assertion in `TestPlaceholder.cpp`:

```cpp
TEST_CASE("the test harness runs") {
    CHECK(2 + 2 == 4);
}
```

Run the command from Step 4.

Expected: `100% tests passed, 0 tests failed out of 2` (the gates plus this).

- [ ] **Step 6: Record doctest in ADR-0014**

In `Docs/Architecture/Decisions/ADR-0014-dependency-policy.md`, add a row to the
third-party table, immediately after the "Editor UI scaffolding" row:

```markdown
| Unit testing | doctest | Single-header MIT framework; compiles far faster than Catch2, which matters on six cores. Test-only, never linked into a shipped target |
```

- [ ] **Step 7: Commit**

```bash
git add CMake/MonarcTest.cmake CMakeLists.txt Source/Monarc.Core/CMakeLists.txt Source/Monarc.Core/Tests/TestPlaceholder.cpp Docs/Architecture/Decisions/ADR-0014-dependency-policy.md
git commit -m "test: doctest harness with per-module test executables"
```

---

## Task 6: `Types.h` and testable assertions

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Types.h`
- Create: `Source/Monarc.Core/Include/Monarc/Core/Assert.h`
- Create: `Source/Monarc.Core/Private/Assert.cpp`
- Create: `Source/Monarc.Core/Tests/TestAssert.cpp`
- Delete: `Source/Monarc.Core/Private/Placeholder.cpp`

Assertions are installed with a replaceable handler specifically so they can be tested. An
assertion mechanism nobody has verified fires is not a safety net.

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestAssert.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>

#include <string>

namespace {

struct CapturedAssert {
    bool        fired = false;
    std::string expression;
    std::string message;
    int         line = 0;
};

CapturedAssert g_captured;

bool CaptureHandler(const char* expr, const char* /*file*/, int line, const char* message) {
    g_captured.fired      = true;
    g_captured.expression = expr;
    g_captured.message    = message ? message : "";
    g_captured.line       = line;
    return false;   // false: do not break into the debugger
}

// Installs the capturing handler and restores the previous one on scope exit.
struct ScopedCapture {
    Monarc::AssertHandler previous;
    ScopedCapture() : previous(Monarc::SetAssertHandler(&CaptureHandler)) {
        g_captured = CapturedAssert{};
    }
    ~ScopedCapture() { Monarc::SetAssertHandler(previous); }
};

}  // namespace

TEST_CASE("MONARC_CHECK does nothing when the condition holds") {
    ScopedCapture capture;
    MONARC_CHECK(1 + 1 == 2, "arithmetic still works");
    CHECK_FALSE(g_captured.fired);
}

TEST_CASE("MONARC_CHECK reports expression and message when it fails") {
    ScopedCapture capture;
    MONARC_CHECK(1 + 1 == 3, "arithmetic broke");
    REQUIRE(g_captured.fired);
    CHECK(g_captured.expression == "1 + 1 == 3");
    CHECK(g_captured.message == "arithmetic broke");
    CHECK(g_captured.line > 0);
}

TEST_CASE("MONARC_CHECK evaluates its condition exactly once") {
    ScopedCapture capture;
    int calls = 0;
    auto bump = [&calls] { ++calls; return true; };
    MONARC_CHECK(bump(), "should evaluate once");
    CHECK(calls == 1);
}

TEST_CASE("SetAssertHandler returns the handler it replaced") {
    Monarc::AssertHandler first  = Monarc::SetAssertHandler(&CaptureHandler);
    Monarc::AssertHandler second = Monarc::SetAssertHandler(first);
    CHECK(second == &CaptureHandler);
}
```

- [ ] **Step 2: Run and verify it fails to compile**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug'
```

Expected: build failure, `Cannot open include file: 'Monarc/Core/Assert.h'`.

- [ ] **Step 3: Write `Types.h`**

`Source/Monarc.Core/Include/Monarc/Core/Types.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace Monarc {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;
using iptr  = std::intptr_t;
using uptr  = std::uintptr_t;

}  // namespace Monarc
```

- [ ] **Step 4: Write `Assert.h`**

`Source/Monarc.Core/Include/Monarc/Core/Assert.h`:

```cpp
#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc {

/// Returns true if the caller should break into the debugger.
/// Replaceable so that assertion behaviour itself can be tested.
using AssertHandler = bool (*)(const char* expression,
                               const char* file,
                               int         line,
                               const char* message);

/// Installs a handler and returns the previous one. Not thread-safe; intended for
/// process start-up and for tests.
AssertHandler SetAssertHandler(AssertHandler handler);

namespace Detail {
bool OnAssertFailed(const char* expression, const char* file, int line, const char* message);
}  // namespace Detail

}  // namespace Monarc

#if defined(_MSC_VER)
#    define MONARC_DEBUG_BREAK() __debugbreak()
#else
#    define MONARC_DEBUG_BREAK() __builtin_trap()
#endif

/// Checked in every configuration except Shipping. Use for conditions whose violation
/// means the program is already wrong.
#define MONARC_CHECK(expression, message)                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            if (::Monarc::Detail::OnAssertFailed(#expression, __FILE__, __LINE__,         \
                                                 (message))) {                            \
                MONARC_DEBUG_BREAK();                                                     \
            }                                                                             \
        }                                                                                 \
    } while (false)

/// Checked only when MONARC_ENABLE_ASSERTS is on. Use for expensive invariants.
#if MONARC_ENABLE_ASSERTS
#    define MONARC_ASSERT(expression, message) MONARC_CHECK(expression, message)
#else
#    define MONARC_ASSERT(expression, message) ((void)sizeof(!(expression)))
#endif
```

> `MONARC_DEBUG_BREAK` branches on `_MSC_VER`, which is a **compiler** macro, not a platform
> macro. Gate 10's regex deliberately excludes compiler macros, so this does not violate
> platform containment — and it should not, because `Assert.h` must work on every platform
> under both compilers. `clang-cl` defines `_MSC_VER` for MSVC compatibility and supports
> `__debugbreak()`, so both of our compilers take the first branch.

- [ ] **Step 5: Write `Assert.cpp`**

`Source/Monarc.Core/Private/Assert.cpp`:

```cpp
#include <Monarc/Core/Assert.h>

#include <cstdio>

namespace Monarc {
namespace {

bool DefaultAssertHandler(const char* expression, const char* file, int line,
                          const char* message) {
    std::fprintf(stderr, "[assert] %s:%d: (%s) %s\n", file, line, expression,
                 message ? message : "");
    std::fflush(stderr);
    return true;   // break into the debugger
}

AssertHandler g_handler = &DefaultAssertHandler;

}  // namespace

AssertHandler SetAssertHandler(AssertHandler handler) {
    AssertHandler previous = g_handler;
    g_handler = handler ? handler : &DefaultAssertHandler;
    return previous;
}

namespace Detail {

bool OnAssertFailed(const char* expression, const char* file, int line, const char* message) {
    return g_handler(expression, file, line, message);
}

}  // namespace Detail
}  // namespace Monarc
```

- [ ] **Step 6: Define `MONARC_ENABLE_ASSERTS`**

In `CMake/MonarcTargetOptions.cmake`, add this inside `monarc_set_target_options`, just after
the `set_target_properties(...)` call:

```cmake
    target_compile_definitions(${target} PUBLIC
        $<$<CONFIG:Debug>:MONARC_ENABLE_ASSERTS=1>
        $<$<NOT:$<CONFIG:Debug>>:MONARC_ENABLE_ASSERTS=0>)
```

- [ ] **Step 7: Remove the placeholder and build**

```bash
git rm Source/Monarc.Core/Private/Placeholder.cpp
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-debug && cmake --build --preset msvc-debug && ctest --preset msvc-debug'
```

Expected: `100% tests passed, 0 tests failed out of 2`, with `TestAssert.cpp`'s four cases
passing and all four architecture gates green.

- [ ] **Step 8: Commit**

```bash
git add Source/Monarc.Core CMake/MonarcTargetOptions.cmake
git commit -m "core: fixed-width types and assertions with a replaceable handler"
```

---

## Task 7: `Error` and `Result<T>`

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Error.h`
- Create: `Source/Monarc.Core/Private/Error.cpp`
- Create: `Source/Monarc.Core/Tests/TestError.cpp`

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestError.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Error.h>

#include <string_view>

namespace {

Monarc::Result<int> Divide(int numerator, int denominator) {
    if (denominator == 0) {
        return Monarc::Err(Monarc::ErrorCode::InvalidArgument, "division by zero");
    }
    return numerator / denominator;
}

Monarc::Status Validate(int value) {
    if (value < 0) {
        return Monarc::Err(Monarc::ErrorCode::InvalidArgument, "value must be non-negative");
    }
    return {};
}

}  // namespace

TEST_CASE("Result carries a value on success") {
    auto result = Divide(10, 2);
    REQUIRE(result.has_value());
    CHECK(*result == 5);
}

TEST_CASE("Result carries code and message on failure") {
    auto result = Divide(10, 0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Monarc::ErrorCode::InvalidArgument);
    CHECK(result.error().message == "division by zero");
}

TEST_CASE("Status expresses success with no value") {
    CHECK(Validate(3).has_value());
    auto bad = Validate(-1);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == Monarc::ErrorCode::InvalidArgument);
}

TEST_CASE("ToString names every error code") {
    using Monarc::ErrorCode;
    CHECK(std::string_view(Monarc::ToString(ErrorCode::Unknown)) == "Unknown");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::InvalidArgument)) == "InvalidArgument");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::OutOfMemory)) == "OutOfMemory");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::NotFound)) == "NotFound");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::AlreadyExists)) == "AlreadyExists");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::IoFailure)) == "IoFailure");
    CHECK(std::string_view(Monarc::ToString(ErrorCode::Unsupported)) == "Unsupported");
}

TEST_CASE("errors propagate through and_then without losing detail") {
    auto chained = Divide(10, 0).and_then(
        [](int v) -> Monarc::Result<int> { return v * 2; });
    REQUIRE_FALSE(chained.has_value());
    CHECK(chained.error().message == "division by zero");
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug'
```

Expected: `Cannot open include file: 'Monarc/Core/Error.h'`.

- [ ] **Step 3: Write `Error.h`**

`Source/Monarc.Core/Include/Monarc/Core/Error.h`:

```cpp
#pragma once

#include <Monarc/Core/Types.h>

#include <expected>
#include <string_view>

namespace Monarc {

enum class ErrorCode : u32 {
    Unknown = 0,
    InvalidArgument,
    OutOfMemory,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    IoFailure,
    Unsupported,
};

const char* ToString(ErrorCode code);

/// An error value. `message` is a non-owning view and must reference storage that
/// outlives the error — in practice a string literal. This is deliberate: error paths
/// do not allocate.
struct Error {
    ErrorCode        code    = ErrorCode::Unknown;
    std::string_view message = {};
};

template <typename T>
using Result = std::expected<T, Error>;

/// A fallible operation returning no value on success.
using Status = std::expected<void, Error>;

[[nodiscard]] inline std::unexpected<Error> Err(ErrorCode code,
                                                std::string_view message = {}) {
    return std::unexpected(Error{code, message});
}

}  // namespace Monarc
```

- [ ] **Step 4: Write `Error.cpp`**

`Source/Monarc.Core/Private/Error.cpp`:

```cpp
#include <Monarc/Core/Error.h>

namespace Monarc {

const char* ToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Unknown:          return "Unknown";
        case ErrorCode::InvalidArgument:  return "InvalidArgument";
        case ErrorCode::OutOfMemory:      return "OutOfMemory";
        case ErrorCode::NotFound:         return "NotFound";
        case ErrorCode::AlreadyExists:    return "AlreadyExists";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::IoFailure:        return "IoFailure";
        case ErrorCode::Unsupported:      return "Unsupported";
    }
    return "Unknown";
}

}  // namespace Monarc
```

- [ ] **Step 5: Build and run**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug && ctest --preset msvc-debug'
```

Expected: `100% tests passed`, with the five `TestError` cases passing.

- [ ] **Step 6: Commit**

```bash
git add Source/Monarc.Core
git commit -m "core: Error, Result<T> and Status over std::expected"
```

---

## Task 8: `IAllocator` and `SystemAllocator`

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Memory/Allocator.h`
- Create: `Source/Monarc.Core/Include/Monarc/Core/Memory/SystemAllocator.h`
- Create: `Source/Monarc.Core/Private/Memory/SystemAllocator.cpp`
- Create: `Source/Monarc.Core/Tests/TestSystemAllocator.cpp`

[Memory.md](../Runtime/Memory.md) requires allocators to be passed rather than assumed, to
report usage, and to take alignment explicitly. `Allocate` has no default alignment argument
on purpose: default arguments on virtual functions bind statically and are a known trap.

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestSystemAllocator.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>

#include <cstring>
#include <string_view>

using Monarc::SystemAllocator;
using Monarc::usize;
using Monarc::uptr;

TEST_CASE("SystemAllocator returns usable, correctly aligned memory") {
    SystemAllocator allocator;

    void* block = allocator.Allocate(256, 64);
    REQUIRE(block != nullptr);
    CHECK(reinterpret_cast<uptr>(block) % 64 == 0);

    std::memset(block, 0xAB, 256);
    CHECK(static_cast<unsigned char*>(block)[255] == 0xAB);

    allocator.Deallocate(block, 256, 64);
}

TEST_CASE("SystemAllocator tracks outstanding bytes") {
    SystemAllocator allocator;
    CHECK(allocator.BytesAllocated() == 0);

    void* a = allocator.Allocate(100, 16);
    CHECK(allocator.BytesAllocated() == 100);

    void* b = allocator.Allocate(50, 16);
    CHECK(allocator.BytesAllocated() == 150);

    allocator.Deallocate(a, 100, 16);
    CHECK(allocator.BytesAllocated() == 50);

    allocator.Deallocate(b, 50, 16);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("SystemAllocator tolerates deallocating null") {
    SystemAllocator allocator;
    allocator.Deallocate(nullptr, 0, 16);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("SystemAllocator reports a name") {
    SystemAllocator allocator;
    CHECK(std::string_view(allocator.Name()) == "System");
}

TEST_CASE("a SystemAllocator is usable through the IAllocator interface") {
    SystemAllocator concrete;
    Monarc::IAllocator& allocator = concrete;

    void* block = allocator.Allocate(32, alignof(double));
    REQUIRE(block != nullptr);
    CHECK(allocator.BytesAllocated() == 32);
    allocator.Deallocate(block, 32, alignof(double));
    CHECK(allocator.BytesAllocated() == 0);
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug'
```

Expected: `Cannot open include file: 'Monarc/Core/Memory/SystemAllocator.h'`.

- [ ] **Step 3: Write `Allocator.h`**

`Source/Monarc.Core/Include/Monarc/Core/Memory/Allocator.h`:

```cpp
#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc {

/// Interface every Monarc allocator implements.
///
/// Alignment is always explicit. There is no default argument, because default
/// arguments on virtual functions bind to the static type and silently disagree
/// across an override.
class IAllocator {
public:
    virtual ~IAllocator() = default;

    IAllocator(const IAllocator&)            = delete;
    IAllocator& operator=(const IAllocator&) = delete;

    /// Returns nullptr on failure. `alignment` must be a power of two.
    [[nodiscard]] virtual void* Allocate(usize size, usize alignment) = 0;

    /// `size` and `alignment` must match the original call. Passing nullptr is a no-op.
    virtual void Deallocate(void* pointer, usize size, usize alignment) = 0;

    /// Bytes currently outstanding. Every allocator reports usage so that budgets and
    /// per-subsystem reports need no extra machinery — see Docs/Runtime/Memory.md.
    [[nodiscard]] virtual usize BytesAllocated() const = 0;

    [[nodiscard]] virtual const char* Name() const = 0;

protected:
    IAllocator() = default;
};

/// True when `value` is a power of two, which every alignment must be.
[[nodiscard]] constexpr bool IsPowerOfTwo(usize value) {
    return value != 0 && (value & (value - 1)) == 0;
}

/// Rounds `value` up to the next multiple of `alignment`.
[[nodiscard]] constexpr usize AlignUp(usize value, usize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace Monarc
```

- [ ] **Step 4: Write `SystemAllocator.h`**

`Source/Monarc.Core/Include/Monarc/Core/Memory/SystemAllocator.h`:

```cpp
#pragma once

#include <Monarc/Core/Memory/Allocator.h>

#include <atomic>

namespace Monarc {

/// General-purpose allocator backed by the platform's aligned allocation.
///
/// This is the fallback, not the default. Systems with a clear lifetime should use an
/// arena or pool instead. It sits behind IAllocator so replacing it later with a custom
/// general allocator is an internal change.
class SystemAllocator final : public IAllocator {
public:
    SystemAllocator() = default;

    [[nodiscard]] void* Allocate(usize size, usize alignment) override;
    void                Deallocate(void* pointer, usize size, usize alignment) override;
    [[nodiscard]] usize BytesAllocated() const override;
    [[nodiscard]] const char* Name() const override { return "System"; }

    /// Process-wide instance, for the small number of places that genuinely have no
    /// allocator to hand — chiefly start-up.
    static SystemAllocator& Get();

private:
    std::atomic<usize> m_bytesAllocated{0};
};

}  // namespace Monarc
```

- [ ] **Step 5: Write `SystemAllocator.cpp`**

`Source/Monarc.Core/Private/Memory/SystemAllocator.cpp`:

```cpp
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <Monarc/Core/Assert.h>

#include <cstdlib>
#include <new>

namespace Monarc {

void* SystemAllocator::Allocate(usize size, usize alignment) {
    MONARC_CHECK(IsPowerOfTwo(alignment), "alignment must be a power of two");
    if (size == 0) {
        return nullptr;
    }

    // std::aligned_alloc requires size to be a multiple of alignment; operator new
    // with an alignment argument has no such constraint and is available everywhere
    // we build.
    void* pointer = ::operator new(size, std::align_val_t{alignment}, std::nothrow);
    if (pointer != nullptr) {
        m_bytesAllocated.fetch_add(size, std::memory_order_relaxed);
    }
    return pointer;
}

void SystemAllocator::Deallocate(void* pointer, usize size, usize alignment) {
    if (pointer == nullptr) {
        return;
    }
    ::operator delete(pointer, std::align_val_t{alignment});
    m_bytesAllocated.fetch_sub(size, std::memory_order_relaxed);
}

usize SystemAllocator::BytesAllocated() const {
    return m_bytesAllocated.load(std::memory_order_relaxed);
}

SystemAllocator& SystemAllocator::Get() {
    static SystemAllocator instance;
    return instance;
}

}  // namespace Monarc
```

- [ ] **Step 6: Build and run**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-debug && cmake --build --preset msvc-debug && ctest --preset msvc-debug'
```

Expected: `100% tests passed`, with the five `TestSystemAllocator` cases passing.

- [ ] **Step 7: Commit**

```bash
git add Source/Monarc.Core
git commit -m "core: IAllocator interface and tracked SystemAllocator"
```

---

## Task 9: `ArenaAllocator`

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Memory/ArenaAllocator.h`
- Create: `Source/Monarc.Core/Private/Memory/ArenaAllocator.cpp`
- Create: `Source/Monarc.Core/Tests/TestArenaAllocator.cpp`

This is also the frame allocator from [Frame-Model.md](../Runtime/Frame-Model.md). The
extraction snapshot will be built from one of these, which is what makes its per-frame cost a
pointer bump.

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestArenaAllocator.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Memory/ArenaAllocator.h>

#include <cstddef>
#include <string_view>
#include <vector>

using Monarc::ArenaAllocator;
using Monarc::uptr;
using Monarc::usize;

namespace {
constexpr usize kCapacity = 1024;
}

TEST_CASE("ArenaAllocator hands out sequential aligned blocks") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    void* first = arena.Allocate(16, 16);
    REQUIRE(first != nullptr);
    CHECK(reinterpret_cast<uptr>(first) % 16 == 0);

    void* second = arena.Allocate(16, 16);
    REQUIRE(second != nullptr);
    CHECK(second != first);
    CHECK(reinterpret_cast<uptr>(second) > reinterpret_cast<uptr>(first));
}

TEST_CASE("ArenaAllocator honours over-alignment") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    arena.Allocate(1, 1);                     // deliberately misalign the cursor
    void* aligned = arena.Allocate(8, 64);
    REQUIRE(aligned != nullptr);
    CHECK(reinterpret_cast<uptr>(aligned) % 64 == 0);
}

TEST_CASE("ArenaAllocator returns nullptr when exhausted rather than overrunning") {
    std::vector<std::byte> backing(64);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    CHECK(arena.Allocate(32, 1) != nullptr);
    CHECK(arena.Allocate(1024, 1) == nullptr);
    CHECK(arena.BytesAllocated() == 32);      // the failed request changes nothing
}

TEST_CASE("Deallocate is a no-op and Reset reclaims everything") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    void* block = arena.Allocate(128, 16);
    CHECK(arena.BytesAllocated() == 128);

    arena.Deallocate(block, 128, 16);
    CHECK(arena.BytesAllocated() == 128);     // arenas do not free individually

    arena.Reset();
    CHECK(arena.BytesAllocated() == 0);

    void* reused = arena.Allocate(128, 16);
    CHECK(reused == block);                   // same memory handed out again
}

TEST_CASE("ArenaAllocator remembers its high-water mark across resets") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "Test");

    arena.Allocate(500, 1);
    CHECK(arena.HighWaterMark() == 500);

    arena.Reset();
    arena.Allocate(100, 1);
    CHECK(arena.BytesAllocated() == 100);
    CHECK(arena.HighWaterMark() == 500);      // budgeting needs the peak, not the current
}

TEST_CASE("ArenaAllocator reports capacity and name") {
    std::vector<std::byte> backing(kCapacity);
    ArenaAllocator arena(backing.data(), backing.size(), "FrameArena");
    CHECK(arena.Capacity() == kCapacity);
    CHECK(std::string_view(arena.Name()) == "FrameArena");
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug'
```

Expected: `Cannot open include file: 'Monarc/Core/Memory/ArenaAllocator.h'`.

- [ ] **Step 3: Write `ArenaAllocator.h`**

`Source/Monarc.Core/Include/Monarc/Core/Memory/ArenaAllocator.h`:

```cpp
#pragma once

#include <Monarc/Core/Memory/Allocator.h>

#include <cstddef>

namespace Monarc {

/// Linear bump allocator over a caller-supplied buffer.
///
/// Individual deallocation is not supported: Deallocate is a no-op and Reset reclaims
/// the whole arena at once. This is the right shape for anything with a clear phase
/// boundary — loading, cooking, and the per-frame render snapshot.
///
/// The arena does not own its backing memory, so its lifetime must not exceed the
/// buffer's.
class ArenaAllocator final : public IAllocator {
public:
    ArenaAllocator(void* buffer, usize capacity, const char* name);

    [[nodiscard]] void* Allocate(usize size, usize alignment) override;

    /// No-op. Arenas reclaim through Reset.
    void Deallocate(void* pointer, usize size, usize alignment) override;

    [[nodiscard]] usize       BytesAllocated() const override { return m_offset; }
    [[nodiscard]] const char* Name() const override { return m_name; }

    /// Reclaims the entire arena. Does not run destructors — arena-allocated types must
    /// be trivially destructible, or destroyed explicitly before Reset.
    void Reset() { m_offset = 0; }

    [[nodiscard]] usize Capacity() const { return m_capacity; }

    /// Peak usage since construction, surviving Reset. This is the number budgeting needs.
    [[nodiscard]] usize HighWaterMark() const { return m_highWaterMark; }

private:
    std::byte*  m_base          = nullptr;
    usize       m_capacity      = 0;
    usize       m_offset        = 0;
    usize       m_highWaterMark = 0;
    const char* m_name          = "Arena";
};

}  // namespace Monarc
```

- [ ] **Step 4: Write `ArenaAllocator.cpp`**

`Source/Monarc.Core/Private/Memory/ArenaAllocator.cpp`:

```cpp
#include <Monarc/Core/Memory/ArenaAllocator.h>

#include <Monarc/Core/Assert.h>

namespace Monarc {

ArenaAllocator::ArenaAllocator(void* buffer, usize capacity, const char* name)
    : m_base(static_cast<std::byte*>(buffer)),
      m_capacity(capacity),
      m_name(name != nullptr ? name : "Arena") {
    MONARC_CHECK(buffer != nullptr || capacity == 0, "arena needs a backing buffer");
}

void* ArenaAllocator::Allocate(usize size, usize alignment) {
    MONARC_CHECK(IsPowerOfTwo(alignment), "alignment must be a power of two");
    if (size == 0) {
        return nullptr;
    }

    const uptr  base    = reinterpret_cast<uptr>(m_base);
    const uptr  current = base + m_offset;
    const uptr  aligned = static_cast<uptr>(AlignUp(static_cast<usize>(current), alignment));
    const usize padding = static_cast<usize>(aligned - current);

    // Checked before advancing, so an exhausted arena leaves its cursor untouched.
    if (padding > m_capacity - m_offset || size > m_capacity - m_offset - padding) {
        return nullptr;
    }

    m_offset += padding + size;
    if (m_offset > m_highWaterMark) {
        m_highWaterMark = m_offset;
    }
    return reinterpret_cast<void*>(aligned);
}

void ArenaAllocator::Deallocate(void* /*pointer*/, usize /*size*/, usize /*alignment*/) {
    // Intentionally empty: arenas reclaim through Reset.
}

}  // namespace Monarc
```

- [ ] **Step 5: Build and run**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug && ctest --preset msvc-debug'
```

Expected: `100% tests passed`, with the six `TestArenaAllocator` cases passing. The
exhaustion test is the important one — it proves the arena refuses rather than overruns.

- [ ] **Step 6: Commit**

```bash
git add Source/Monarc.Core
git commit -m "core: ArenaAllocator with alignment, exhaustion safety and high-water tracking"
```

---

## Task 10: `Array<T>`

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Containers/Array.h`
- Create: `Source/Monarc.Core/Tests/TestArray.cpp`

The first container. It takes an `IAllocator&` at construction, which is the pattern every
allocating type in Monarc follows. Copying is deleted: an allocating copy should be
deliberate and visible, so duplication goes through an explicit `Clone`.

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestArray.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <utility>

using Monarc::Array;
using Monarc::SystemAllocator;

namespace {

/// Counts construction and destruction so the tests can prove element lifetimes are correct.
struct Tracked {
    static int s_alive;
    int        value;

    explicit Tracked(int v = 0) : value(v) { ++s_alive; }
    Tracked(const Tracked& other) : value(other.value) { ++s_alive; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++s_alive; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --s_alive; }
};

int Tracked::s_alive = 0;

}  // namespace

TEST_CASE("a new Array is empty and holds no memory") {
    SystemAllocator allocator;
    Array<int> array(allocator);
    CHECK(array.Size() == 0);
    CHECK(array.Capacity() == 0);
    CHECK(array.IsEmpty());
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Push appends and grows") {
    SystemAllocator allocator;
    Array<int> array(allocator);

    for (int i = 0; i < 10; ++i) {
        array.Push(i * i);
    }

    REQUIRE(array.Size() == 10);
    CHECK(array.Capacity() >= 10);
    CHECK(array[0] == 0);
    CHECK(array[3] == 9);
    CHECK(array[9] == 81);
    CHECK(allocator.BytesAllocated() > 0);
}

TEST_CASE("Reserve allocates without changing size") {
    SystemAllocator allocator;
    Array<int> array(allocator);

    array.Reserve(64);
    CHECK(array.Size() == 0);
    CHECK(array.Capacity() >= 64);

    const int* before = array.Data();
    for (int i = 0; i < 64; ++i) {
        array.Push(i);
    }
    CHECK(array.Data() == before);   // no reallocation within reserved capacity
}

TEST_CASE("Array destroys its elements") {
    SystemAllocator allocator;
    CHECK(Tracked::s_alive == 0);
    {
        Array<Tracked> array(allocator);
        for (int i = 0; i < 5; ++i) {
            array.Emplace(i);
        }
        CHECK(Tracked::s_alive == 5);
    }
    CHECK(Tracked::s_alive == 0);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Clear destroys elements but keeps capacity") {
    SystemAllocator allocator;
    Array<Tracked> array(allocator);
    for (int i = 0; i < 4; ++i) {
        array.Emplace(i);
    }
    const Monarc::usize capacity = array.Capacity();

    array.Clear();
    CHECK(array.Size() == 0);
    CHECK(Tracked::s_alive == 0);
    CHECK(array.Capacity() == capacity);
}

TEST_CASE("Pop removes and destroys the last element") {
    SystemAllocator allocator;
    Array<Tracked> array(allocator);
    array.Emplace(1);
    array.Emplace(2);
    CHECK(Tracked::s_alive == 2);

    array.Pop();
    CHECK(array.Size() == 1);
    CHECK(Tracked::s_alive == 1);
    CHECK(array[0].value == 1);
}

TEST_CASE("moving an Array transfers ownership and leaves the source empty") {
    SystemAllocator allocator;
    Array<int> source(allocator);
    source.Push(7);
    source.Push(8);
    const int* data = source.Data();

    Array<int> moved(std::move(source));
    CHECK(moved.Size() == 2);
    CHECK(moved.Data() == data);   // no reallocation
    CHECK(moved[1] == 8);
    CHECK(source.Size() == 0);
    CHECK(source.Data() == nullptr);
}

TEST_CASE("move assignment releases the target's existing memory") {
    SystemAllocator allocator;
    Array<Tracked> target(allocator);
    target.Emplace(1);
    target.Emplace(2);

    Array<Tracked> source(allocator);
    source.Emplace(3);

    target = std::move(source);
    CHECK(target.Size() == 1);
    CHECK(target[0].value == 3);
    CHECK(Tracked::s_alive == 1);   // the two originals were destroyed
}

TEST_CASE("Array is range-for iterable") {
    SystemAllocator allocator;
    Array<int> array(allocator);
    array.Push(1);
    array.Push(2);
    array.Push(3);

    int sum = 0;
    for (int value : array) {
        sum += value;
    }
    CHECK(sum == 6);
}

TEST_CASE("Array frees everything it allocated") {
    SystemAllocator allocator;
    {
        Array<int> array(allocator);
        array.Reserve(256);
        for (int i = 0; i < 256; ++i) {
            array.Push(i);
        }
    }
    CHECK(allocator.BytesAllocated() == 0);
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug'
```

Expected: `Cannot open include file: 'Monarc/Core/Containers/Array.h'`.

- [ ] **Step 3: Write `Array.h`**

`Source/Monarc.Core/Include/Monarc/Core/Containers/Array.h`:

```cpp
#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Memory/Allocator.h>

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Monarc {

/// Dynamic array over an explicit allocator.
///
/// Copying is deleted: an allocating copy should be deliberate and visible. Duplicate
/// through an explicit Clone when one is needed.
template <typename T>
class Array {
public:
    explicit Array(IAllocator& allocator) : m_allocator(&allocator) {}

    ~Array() {
        Clear();
        Release();
    }

    Array(const Array&)            = delete;
    Array& operator=(const Array&) = delete;

    Array(Array&& other) noexcept
        : m_allocator(other.m_allocator),
          m_data(other.m_data),
          m_size(other.m_size),
          m_capacity(other.m_capacity) {
        other.m_data     = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }

    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            Clear();
            Release();
            m_allocator      = other.m_allocator;
            m_data           = other.m_data;
            m_size           = other.m_size;
            m_capacity       = other.m_capacity;
            other.m_data     = nullptr;
            other.m_size     = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    void Reserve(usize newCapacity) {
        if (newCapacity <= m_capacity) {
            return;
        }

        T* newData = static_cast<T*>(m_allocator->Allocate(newCapacity * sizeof(T), alignof(T)));
        MONARC_CHECK(newData != nullptr, "Array allocation failed");
        if (newData == nullptr) {
            return;
        }

        for (usize i = 0; i < m_size; ++i) {
            std::construct_at(newData + i, std::move(m_data[i]));
            std::destroy_at(m_data + i);
        }

        Release();
        m_data     = newData;
        m_capacity = newCapacity;
    }

    T& Push(const T& value) { return Emplace(value); }
    T& Push(T&& value) { return Emplace(std::move(value)); }

    template <typename... Args>
    T& Emplace(Args&&... args) {
        if (m_size == m_capacity) {
            Reserve(m_capacity == 0 ? kInitialCapacity : m_capacity * 2);
        }
        T* slot = std::construct_at(m_data + m_size, std::forward<Args>(args)...);
        ++m_size;
        return *slot;
    }

    void Pop() {
        MONARC_CHECK(m_size > 0, "Pop on an empty Array");
        if (m_size == 0) {
            return;
        }
        --m_size;
        std::destroy_at(m_data + m_size);
    }

    /// Destroys every element. Capacity is retained.
    void Clear() {
        for (usize i = 0; i < m_size; ++i) {
            std::destroy_at(m_data + i);
        }
        m_size = 0;
    }

    [[nodiscard]] T& operator[](usize index) {
        MONARC_CHECK(index < m_size, "Array index out of range");
        return m_data[index];
    }

    [[nodiscard]] const T& operator[](usize index) const {
        MONARC_CHECK(index < m_size, "Array index out of range");
        return m_data[index];
    }

    [[nodiscard]] usize Size() const { return m_size; }
    [[nodiscard]] usize Capacity() const { return m_capacity; }
    [[nodiscard]] bool  IsEmpty() const { return m_size == 0; }

    [[nodiscard]] T*       Data() { return m_data; }
    [[nodiscard]] const T* Data() const { return m_data; }

    [[nodiscard]] T*       begin() { return m_data; }
    [[nodiscard]] T*       end() { return m_data + m_size; }
    [[nodiscard]] const T* begin() const { return m_data; }
    [[nodiscard]] const T* end() const { return m_data + m_size; }

private:
    static constexpr usize kInitialCapacity = 8;

    void Release() {
        if (m_data != nullptr) {
            m_allocator->Deallocate(m_data, m_capacity * sizeof(T), alignof(T));
            m_data     = nullptr;
            m_capacity = 0;
        }
    }

    IAllocator* m_allocator = nullptr;
    T*          m_data      = nullptr;
    usize       m_size      = 0;
    usize       m_capacity  = 0;
};

}  // namespace Monarc
```

- [ ] **Step 4: Build and run**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug && ctest --preset msvc-debug'
```

Expected: `100% tests passed`, with the ten `TestArray` cases passing. The lifetime tests
(`Tracked::s_alive` returning to zero) are the ones that matter — they prove destructors run
and memory is returned.

- [ ] **Step 5: Commit**

```bash
git add Source/Monarc.Core
git commit -m "core: Array<T> over an explicit allocator, with element lifetime tests"
```

---

## Task 11: Logging

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Log.h`
- Create: `Source/Monarc.Core/Private/Log.cpp`
- Create: `Source/Monarc.Core/Tests/TestLog.cpp`
- Modify: `Docs/Architecture/Decisions/ADR-0003-cpp23-baseline.md`

**An ADR correction is part of this task.** ADR-0003 bans `std::print`/`std::format` in
runtime paths. That was over-broad: `<print>` is C++23 and genuinely lags in libc++, but
`<format>` is C++20 and widely available. Logging needs type-safe formatting, and building a
formatter to satisfy a rule we wrote too broadly would be the wrong trade. Formatting happens
into a fixed stack buffer via `std::format_to_n`, so the runtime log path does not allocate.

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestLog.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Log.h>

#include <string>
#include <vector>

namespace {

struct CapturedLine {
    std::string category;
    Monarc::LogLevel level;
    std::string message;
};

std::vector<CapturedLine> g_lines;

void CaptureSink(const Monarc::LogRecord& record) {
    g_lines.push_back({std::string(record.category), record.level, std::string(record.message)});
}

struct ScopedSink {
    Monarc::LogSink previous;
    ScopedSink() : previous(Monarc::SetLogSink(&CaptureSink)) { g_lines.clear(); }
    ~ScopedSink() { Monarc::SetLogSink(previous); }
};

MONARC_LOG_CATEGORY(LogTest, Info);

}  // namespace

TEST_CASE("MONARC_LOG delivers category, level and formatted message") {
    ScopedSink sink;
    MONARC_LOG(LogTest, Warning, "disk {} is {}% full", "C:", 91);

    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0].category == "LogTest");
    CHECK(g_lines[0].level == Monarc::LogLevel::Warning);
    CHECK(g_lines[0].message == "disk C: is 91% full");
}

TEST_CASE("messages below the category's minimum level are dropped") {
    ScopedSink sink;
    MONARC_LOG(LogTest, Trace, "should not appear");
    MONARC_LOG(LogTest, Debug, "should not appear either");
    MONARC_LOG(LogTest, Info, "should appear");

    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0].message == "should appear");
}

TEST_CASE("a category's minimum level can be changed at runtime") {
    ScopedSink sink;
    const Monarc::LogLevel original = LogTest.minLevel;

    LogTest.minLevel = Monarc::LogLevel::Error;
    MONARC_LOG(LogTest, Warning, "filtered out");
    CHECK(g_lines.empty());

    LogTest.minLevel = original;
    MONARC_LOG(LogTest, Warning, "now visible");
    CHECK(g_lines.size() == 1);
}

TEST_CASE("arguments are not evaluated when the message is filtered out") {
    ScopedSink sink;
    int calls = 0;
    auto expensive = [&calls] { ++calls; return 42; };

    MONARC_LOG(LogTest, Trace, "value {}", expensive());
    CHECK(calls == 0);

    MONARC_LOG(LogTest, Info, "value {}", expensive());
    CHECK(calls == 1);
}

TEST_CASE("over-long messages are truncated rather than overflowing") {
    ScopedSink sink;
    const std::string huge(4000, 'x');
    MONARC_LOG(LogTest, Info, "{}", huge);

    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0].message.size() < huge.size());
    CHECK(g_lines[0].message.size() <= Monarc::kMaxLogMessageLength);
}

TEST_CASE("ToString names every level") {
    using Monarc::LogLevel;
    CHECK(std::string_view(Monarc::ToString(LogLevel::Trace)) == "Trace");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Debug)) == "Debug");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Info)) == "Info");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Warning)) == "Warning");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Error)) == "Error");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Fatal)) == "Fatal");
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug'
```

Expected: `Cannot open include file: 'Monarc/Core/Log.h'`.

- [ ] **Step 3: Write `Log.h`**

`Source/Monarc.Core/Include/Monarc/Core/Log.h`:

```cpp
#pragma once

#include <Monarc/Core/Types.h>

#include <format>
#include <string_view>
#include <utility>

namespace Monarc {

enum class LogLevel : u8 {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

const char* ToString(LogLevel level);

/// Longest message a single log call can emit. Formatting happens into a stack buffer
/// of this size, so the runtime log path performs no allocation; longer messages are
/// truncated.
inline constexpr usize kMaxLogMessageLength = 1024;

/// A category groups related messages and carries its own threshold, so verbosity is
/// controlled per subsystem rather than globally.
struct LogCategory {
    const char* name;
    LogLevel    minLevel;
};

struct LogRecord {
    std::string_view category;
    LogLevel         level;
    std::string_view message;
    const char*      file;
    int              line;
};

using LogSink = void (*)(const LogRecord& record);

/// Installs a sink and returns the previous one.
LogSink SetLogSink(LogSink sink);

namespace Detail {
void Emit(const LogCategory& category, LogLevel level, const char* file, int line,
          std::string_view message);

template <typename... Args>
void Format(const LogCategory& category, LogLevel level, const char* file, int line,
            std::format_string<Args...> fmt, Args&&... args) {
    char        buffer[kMaxLogMessageLength];
    const auto  result = std::format_to_n(buffer, kMaxLogMessageLength, fmt,
                                          std::forward<Args>(args)...);
    const usize written = static_cast<usize>(result.out - buffer);
    Emit(category, level, file, line, std::string_view(buffer, written));
}
}  // namespace Detail

}  // namespace Monarc

/// Declares a log category. Place at namespace scope.
#define MONARC_LOG_CATEGORY(CategoryName, MinimumLevel)                                   \
    inline ::Monarc::LogCategory CategoryName{#CategoryName, ::Monarc::LogLevel::MinimumLevel}

/// Logs a formatted message. Arguments are not evaluated when the level is filtered out,
/// so expensive arguments cost nothing in a build that discards them.
#define MONARC_LOG(Category, Level, ...)                                                  \
    do {                                                                                  \
        if (::Monarc::LogLevel::Level >= (Category).minLevel) {                           \
            ::Monarc::Detail::Format((Category), ::Monarc::LogLevel::Level, __FILE__,     \
                                     __LINE__, __VA_ARGS__);                              \
        }                                                                                 \
    } while (false)
```

- [ ] **Step 4: Write `Log.cpp`**

`Source/Monarc.Core/Private/Log.cpp`:

```cpp
#include <Monarc/Core/Log.h>

#include <cstdio>

namespace Monarc {
namespace {

void ConsoleSink(const LogRecord& record) {
    std::fprintf(record.level >= LogLevel::Error ? stderr : stdout,
                 "[%-7s] %.*s: %.*s\n",
                 ToString(record.level),
                 static_cast<int>(record.category.size()), record.category.data(),
                 static_cast<int>(record.message.size()), record.message.data());
}

LogSink g_sink = &ConsoleSink;

}  // namespace

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:   return "Trace";
        case LogLevel::Debug:   return "Debug";
        case LogLevel::Info:    return "Info";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Error:   return "Error";
        case LogLevel::Fatal:   return "Fatal";
    }
    return "Unknown";
}

LogSink SetLogSink(LogSink sink) {
    LogSink previous = g_sink;
    g_sink = sink != nullptr ? sink : &ConsoleSink;
    return previous;
}

namespace Detail {

void Emit(const LogCategory& category, LogLevel level, const char* file, int line,
          std::string_view message) {
    g_sink(LogRecord{category.name, level, message, file, line});
}

}  // namespace Detail
}  // namespace Monarc
```

- [ ] **Step 5: Build and run**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build --preset msvc-debug && ctest --preset msvc-debug'
```

Expected: `100% tests passed`, with the six `TestLog` cases passing.

- [ ] **Step 6: Correct ADR-0003**

In `Docs/Architecture/Decisions/ADR-0003-cpp23-baseline.md`, find the `Runtime` row of the
decision table and replace its "Standard library" cell text:

```
Restricted to the universally-available header-only subset: `<type_traits>`, `<concepts>`, `<utility>`, `<expected>`, `<span>`, `<bit>`, `<atomic>`, `<cstdint>`, `<format>`, and similar. **Banned:** `<iostream>`, `<regex>`, `<print>`, `<generator>`, `<flat_map>`, `<stacktrace>`, and C++23 ranges adaptors
```

Then add this paragraph immediately below that table:

```markdown
**Amended 2026-09-05.** This decision originally banned `<format>` alongside `<print>` in
runtime code. That conflated two different things: `<print>` is C++23 and genuinely lags in
libc++, whereas `<format>` is C++20 and widely available. Logging needs type-safe
formatting, and writing a formatter to satisfy a rule we had drawn too broadly would have
been the wrong trade. `<format>` is now permitted in runtime code, on the condition that it
formats into a fixed buffer via `std::format_to_n` rather than allocating a `std::string` —
see `Monarc::Detail::Format` in `Log.h`. If `<format>`'s compile-time cost later becomes a
problem, the formatter sits behind `MONARC_LOG` and can be replaced without touching call
sites.
```

- [ ] **Step 7: Commit**

```bash
git add Source/Monarc.Core Docs/Architecture/Decisions/ADR-0003-cpp23-baseline.md
git commit -m "core: categorised logging with non-allocating formatting; amend ADR-0003 on <format>"
```

---

## Task 12: Verify against both compilers and close out A1

**Files:**
- Modify: `Docs/Status.md`

ADR-0003's whole justification is that a second compiler catches MSVC-specific divergence
early. That only holds if the Clang build is actually run.

- [ ] **Step 1: Configure and build with Clang**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset clang-debug && cmake --build --preset clang-debug'
```

Expected: `Monarc 0.1.0 | Clang 22.1.8`, then a clean build with **zero warnings** — the
target options set `/WX`, so any warning is an error.

If Clang reports warnings MSVC did not, fix the code rather than suppressing the warning.
That divergence is the entire reason this build exists.

- [ ] **Step 2: Run the full suite under Clang**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && ctest --preset clang-debug'
```

Expected: `100% tests passed, 0 tests failed out of 2` — `Architecture.Gates` and
`Monarc.Core.Tests`.

- [ ] **Step 3: Confirm the MSVC Release build is also clean**

```bash
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc-release && cmake --build --preset msvc-release && ctest --output-on-failure --test-dir Build/msvc-release'
```

Expected: clean build, all tests pass. This matters because `MONARC_ENABLE_ASSERTS` is 0 in
non-Debug configurations, so `MONARC_ASSERT` compiles out — the `((void)sizeof(!(expression)))`
form keeps the expression type-checked without evaluating it, and this build proves it.

- [ ] **Step 4: Update Status.md**

In `Docs/Status.md`, replace the entire "## Implementation progress" section with:

```markdown
## Implementation progress

| Phase | Contents | State |
|---|---|---|
| A1 | Build system, module gates, Core memory + diagnostics | **Complete** |
| A2 | Rest of Core (String, HashMap, math, GUID, platform), Jobs | Not started |
| A3 | RHI, Vulkan backend, Host.Windowed | Not started |
| A4 | Minimal render graph | Not started |
| B | ShaderCompiler, Shaders, Render | Not started |
| C | Reflect, Serialize, Assets, Cook | Not started |
| D | World, Engine | Not started |
| E | Editor | Not started |
| F | Hub, Build and export | Not started |

### A1 delivered

- `monarc_module()` with configure-time enforcement of kind and tier rules
- `module-graph.json` emitted as a build artifact
- Architecture gates 2, 3, 7 and 10 running under CTest, each verified to fail when violated
- `Monarc.Core`: `Types`, `Assert` (replaceable handler), `Error`/`Result`/`Status`,
  `IAllocator`, `SystemAllocator`, `ArenaAllocator`, `Array<T>`, categorised logging
- Verified under MSVC Debug, MSVC RelWithDebInfo, and Clang Debug, warnings-as-errors
```

Then replace the "## Verification gates" section with:

```markdown
## Verification gates

Four of M0's eleven gates are implemented and running under CTest as
`Architecture.Gates`: acyclicity (2), renderer package boundary (3), module layout (7), and
platform containment (10). Gate 3 currently passes vacuously — no tier 2 module exists yet —
which is the intended state: it will go red the first time the boundary is crossed.

The remaining gates need modules that do not exist yet and are added against this same
harness: export purity (1), cook determinism (4), cook incrementality (5), asset identity
across rename (6), world-kind parity (7 in M0's numbering), export explainability (8),
headless purity (9), and schema migration (11).
```

- [ ] **Step 5: Confirm the docs still link correctly**

```bash
python Tools/check_doc_links.py Docs
```

Expected: `all links resolve`, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add Docs/Status.md
git commit -m "docs: record Phase A1 complete, verified under MSVC and Clang"
```

---

## Definition of done

A1 is complete when all of the following are true:

1. `cmake --preset msvc-debug && cmake --build --preset msvc-debug && ctest --preset msvc-debug` passes.
2. The same holds for `clang-debug` and for `msvc-release`.
3. Every build is warning-free (`/WX` is on, so this is enforced rather than checked).
4. `Build/<preset>/module-graph.json` exists and parses.
5. All four architecture gates pass, and gates 2 and 10 have each been *observed to fail*
   when deliberately violated (Tasks 2 and 4).
6. `Docs/Status.md` reflects reality.

## What A1 deliberately does not include

Stated so nothing is assumed finished that is not:

- **No math, strings, or hash maps.** Those are A2, alongside `Monarc.Jobs`.
- **No platform layer.** File IO, time, threads and dynamic library loading are A2. The
  platform-containment gate is in place and passes vacuously until then, which is the point.
- **No pool allocator.** It pairs with handles, and arrives when handles do.
- **No CI.** The Clang build is run manually in Task 12. Automating it is worth doing once
  there is somewhere to run it.
- **Nothing renders.** A window and a cleared screen are A3 and A4.
