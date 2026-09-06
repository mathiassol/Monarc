# ADR-0011: CMake backend with declarative module metadata

**Status:** Accepted — 2026-09-05

## Context

The build system carries more weight in Monarc than in most projects. It must enforce the
module graph ([ADR-0001](ADR-0001-layered-modules.md)), keep compile times controllable on a
six-core machine, support Windows now and macOS within a year, produce reproducible builds, and
supply the data that lets an export explain itself.

Serious engines often write their own build tool — Unreal Build Tool is the canonical example —
because the module graph then becomes queryable data rather than logic buried in build scripts.
That is genuinely valuable. It is also a large amount of work that produces no rendering.

## Decision

**CMake is the backend; module metadata is declarative data on top of it.**

```cmake
monarc_module(
    NAME         Monarc.Render
    KIND         Runtime
    TIER         2
    PUBLIC_DEPS  Monarc.Core Monarc.RHI Monarc.Shaders
    PRIVATE_DEPS Monarc.Jobs
)
```

`monarc_module` validates the declaration against the rules in
[Module-Graph.md](../Module-Graph.md) and fails the configure step on a violation. It also
records the declaration, and the build emits **`module-graph.json`** as an artifact — the file
that `monarc explain` and the export tooling read.

Configuration is via CMake presets. Presets deliberately pin **no absolute tool paths**: one
set has to serve developer machines, CI, and macOS later, and a preset naming
`C:/Program Files/LLVM/bin/clang-cl.exe` serves exactly one of those. Ninja and `clang-cl`
are expected on `PATH` instead; `CMakeUserPresets.json` (gitignored) is the escape hatch for
a machine that needs overrides.

One local caveat this creates: Git Bash puts MSYS2's `g++` ahead of MSVC on `PATH`, so builds
must still run from a Visual Studio developer environment (`vcvars64.bat`). The presets choose
the generator and build type; the environment chooses the compiler.

Standing build settings: `/Zc:__cplusplus`, `CXX_SCAN_FOR_MODULES OFF`, C++23,
warnings-as-errors, and unity builds available per module but off by default.

## Consequences

**Good.** We keep CMake's ecosystem, IDE integration, and — importantly — the Xcode generator
we will need for Metal. Module metadata is data, so the build can validate it and tools can
walk it, which is most of what a custom build tool would have bought. No new language or tool
in the developer loop.

**Costs.** CMake is the constraint we inherit: its language is unpleasant, and complex logic in
`monarc_module` will be harder to maintain than the equivalent in a real programming language.
Some validations are more natural as a separate step over `module-graph.json` than as CMake
logic, so enforcement is split between configure-time and test-time. If Monarc ever outgrows
CMake, the migration is real work — though the declarative metadata is the part that would
survive, which is the point of separating it from build logic.

## Alternatives considered

**A custom build tool generating Ninja directly (UBT-style).** The right answer at Unreal's
scale, and it buys precise control over module compilation, codegen hooks, and export trimming.
Rejected for now on cost: it produces no engine, and CMake plus declarative metadata captures
most of the benefit. Revisit if configure times or trimming precision become real problems.

**Plain CMake with folder conventions.** Rejected — the module graph would live in build logic
where nothing can query it, and [ADR-0001](ADR-0001-layered-modules.md)'s enforcement would
have nothing to enforce against.

**A separate manifest format (TOML or JSON) with a generator producing CMake.** Cleaner
metadata, at the cost of an extra indirection that makes build failures harder to trace. The
CMake-function form is already declarative enough to query, so the indirection buys little.
Rejected for now; the metadata could be lifted out later without changing consumers.

**Bazel or another hermetic build system.** Strong reproducibility, poor fit for graphics
development on Windows and macOS, and a steep operational cost for one developer. Rejected.
