# Status

**What is actually true right now.** Intent lives in the other documents; this file is the
honest account. Update it when reality changes, not when a plan is written.

_Last updated: 2026-09-05_

## Summary

The architecture is designed and recorded, and **Phase A1 is complete**: the build
mechanically enforces the module graph, and `Monarc.Core` has its memory and diagnostics
foundation under test on two compilers.

Next is **Phase A2** — the rest of `Monarc.Core` (strings, hash maps, math, GUIDs, the
platform layer) and `Monarc.Jobs`. See
[M0 — First Light](Milestones/M0-First-Light.md) for how the phases fit together.

Nothing renders yet. That is Phase A3 and A4.

## Verified environment

Confirmed by direct testing on the development machine, not assumed:

| Component | Version | Notes |
|---|---|---|
| Compiler | MSVC 19.51 (toolset 14.51, VS 2026 Community) | Compiles C++23 language features cleanly at `/W4` — verified: deducing `this`, `static operator()`, multidimensional `operator[]`, `if consteval`, `auto(x)`, `[[assume]]`, `std::expected` |
| Second compiler | Clang 22.1.8 (`clang-cl`, standalone LLVM) | Builds the whole project and full test suite warning-free at `/WX`, output identical to MSVC. **ADR-0003's condition is only partly met**: it requires Clang *in CI*, and no CI exists — the Clang build is run by hand. See [Known gaps](#known-gaps) |
| Build | CMake 4.2.1 + Ninja 1.13.2 (standalone, on `PATH`) | Presets pin no absolute tool paths, so one set serves this machine, CI, and macOS later |
| Vulkan | SDK 1.4.357.0 | Found automatically by CMake's `find_package(Vulkan)`. Validation layers, gfxreconstruct, SPIRV-Tools present |
| Shaders | Slang 2026.13.1 (in the Vulkan SDK), DXC 1.9, glslang | |
| Windows SDK | 10.0.26100.0 | D3D12 headers present |
| Other | Python 3.14.7, Node 22.15, .NET 9 + 10 | |

### Continuous integration

`.github/workflows/ci.yml` builds and tests four configurations on every push and pull
request — MSVC and Clang, Debug and Release — plus a documentation-link check.
`fail-fast` is disabled so that when the compilers disagree, both results are visible.
First run green, 2026-09-06.

The runner's toolchain differs from this machine usefully: the same MSVC family
(19.51.36256 vs 19.51.36244 here) but **Clang 20.1.8 against 22.1.8 locally**, so the
pairing spans two Clang major versions. That is better coverage than the MSVC spread that
was expected and did not materialise.

### Hardware

- **NVIDIA RTX 3070 Ti** (Ampere) — high capability tier: bindless, mesh shaders, ray tracing
- **Intel UHD 730** (Xe-LP, integrated) — a genuinely useful *second vendor and lower tier*
  on the same machine, for keeping capability tiers honest rather than theoretical
- Intel i5-11400, 6 cores / 12 threads, 32 GB RAM — modest, so **compile-time discipline is
  a design constraint, not a virtue**

### Known gaps

- **No macOS machine.** Metal is designed for but unimplemented and unproven. Expected
  within a year — see [ADR-0012](Architecture/Decisions/ADR-0012-backend-rollout.md).
- **No graphics debugger.** NVIDIA Nsight Graphics is installed, but it is NVIDIA-only and
  so cannot inspect frames on the Intel UHD 730 — the device that keeps the capability
  tiers honest. RenderDoc is needed before Phase A rendering work begins.
- **No compiler cache.** Neither `sccache` nor `ccache` is present. On six cores this is
  worth having before the module count grows.

## Toolchain quirks worth remembering

- MSVC reports `__cplusplus == 199711` unless built with `/Zc:__cplusplus`. Set it globally.
- **MSVC 19.51 does not accept `/std:c++23` at all.** Passed explicitly it warns
  `D9002: ignoring unknown option` and silently drops to C++14. CMake's `CXX_STANDARD 23`
  therefore falls back to `/std:c++latest`, under which `__cplusplus` reports `202400L` —
  a value *beyond* C++23. `clang-cl` reports exactly `202302L`.

  The consequence is worth stating plainly: **a successful MSVC build is not evidence that
  code is C++23-conforming.** MSVC will happily accept post-C++23 features that Clang
  rejects. This is precisely the divergence
  [ADR-0003](Architecture/Decisions/ADR-0003-cpp23-baseline.md) makes the Clang build
  conditional on catching, and it means that build is load-bearing rather than belt-and-braces.
- `/MP` is inert under Ninja and is deliberately absent from the compiler flags. It
  parallelises multiple sources within a single `cl.exe` invocation, and Ninja invokes
  `cl.exe` once per file. Build parallelism comes from Ninja's scheduler.
- CMake 4.2 enables C++20 module dependency scanning by default at C++23. We do not use
  modules ([ADR-0003](Architecture/Decisions/ADR-0003-cpp23-baseline.md)), so
  `CXX_SCAN_FOR_MODULES OFF` is free build time.
- Git Bash puts MSYS2's `g++` ahead of MSVC on `PATH`. Builds must run from a VS developer
  environment (`vcvars64.bat`) or via CMake presets that pin the toolchain.
- **`clang-cl` does not accept `/std:c++23`** — it silently ignores the flag (emitting only
  an "argument unused" warning) and falls back to C++17, which then fails with a wall of
  confusing errors. Use `/std:c++latest`, or let CMake's `CXX_STANDARD 23` pick the flag.
- `core.autocrlf` is `true` on this machine. `.gitattributes` overrides it with
  `text=auto eol=lf` so line-ending behaviour does not depend on developer config.
- The only `python` on `PATH` is MSYS2's (3.14.7), and it ships **without `pip`**. Both
  scripts in `Tools/` are therefore written against the standard library alone, and must
  stay that way — `find_package(Python3)` resolves to this interpreter. A separate CPython
  3.12 exists under `%LOCALAPPDATA%\Programs\Python` if a script ever genuinely needs a
  package.
- Ninja 1.13.2 and LLVM's `bin` are on `PATH` as of 2026-09-06, which is what lets
  `CMakePresets.json` pin no absolute tool paths.
- Windows long paths are **not** enabled (`LongPathsEnabled=0`, `core.longpaths` unset).
  Deep module trees plus Ninja build directories approach the 260-character limit.

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

- `monarc_module()` enforcing kind and tier rules at configure time, and rejecting
  unrecognised arguments so a typo cannot silently drop a dependency edge
- `module-graph.json` emitted as a build artifact
- Four architecture gates running under CTest, each verified to fail when violated
- `Monarc.Core`: `Types`, `Assert` (replaceable handler), `Error`/`Result`/`Status`,
  `IAllocator`, `SystemAllocator`, `ArenaAllocator`, `Array<T>`, categorised logging
- Verified under MSVC Debug, MSVC RelWithDebInfo, and Clang Debug, warnings-as-errors
  — 43 doctest cases (126 assertions) plus the 4 architecture gates, 100% passing on a
  clean rebuild of all three presets

### Known gaps in A1, carried into A2

A cross-cutting review at the end of A1 found no live defects, but three things worth
carrying forward rather than rediscovering:

- **`Tools/check_architecture.py` has no tests of its own.** It is the mechanism this whole
  phase exists to prove works, and it has already needed two non-obvious fixes — a bare
  prefix match that would have exempted a sibling `Private/PlatformUtils/`, and a regex
  defeated by a backslash-continued `#if`. It is string-matching over source trees, which is
  exactly the code that regresses silently.
- **`Array<T>`'s abort-on-allocation-failure contract is untested.** Its most important
  safety property is the one thing no test exercises, because triggering it kills the test
  process. Making it testable means routing through a replaceable fatal handler, the way
  `Assert.h` already does — worth doing when a second container needs the same treatment.
- **`MONARC_ASSERT` has no production call sites yet.** Everything real uses `MONARC_CHECK`.
  Not a misuse — A1 has no expensive invariant to check — but the distinction will quietly
  erode unless A2's hash tables and job graphs actually use it.

## Verification gates

Four of M0's eleven gates are implemented and running under CTest as
`Architecture.Gates`: acyclicity (2), renderer package boundary (3), module layout (7), and
platform containment (10). Gate 3 currently passes vacuously — no tier 2 module exists yet —
which is the intended state: it will go red the first time the boundary is crossed.

The remaining gates need modules that do not exist yet and are added against this same
harness: export purity (1), cook determinism (4), cook incrementality (5), asset identity
across rename (6), world-kind parity, export explainability (8), headless purity (9), and
schema migration (11).
