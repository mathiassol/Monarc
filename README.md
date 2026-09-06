# Monarc

[![CI](https://github.com/mathiassol/Monarc/actions/workflows/ci.yml/badge.svg)](https://github.com/mathiassol/Monarc/actions/workflows/ci.yml)

A game engine written from scratch in C++23, built as a complete product rather than a
library that sits beside a game:

```
Monarc Hub → choose an engine version → open the Monarc Editor
           → make a game → run it → export a standalone build
```

**Status: very early.** The architecture is designed and recorded, and the foundation
is under test on two compilers. **Nothing renders yet.** See [Docs/Status.md](Docs/Status.md)
for an honest account of what is actually true, kept deliberately separate from what is
intended.

## What exists today

- A build system that **mechanically enforces** the module graph. Every module declares a
  kind (`Runtime`/`Tool`/`Editor`/`Test`) and a tier; a `Runtime` module depending on an
  `Editor` module is a configure-time error, not a review comment.
- Four architecture gates under CTest — dependency cycles, the renderer package boundary,
  module layout, and platform-conditional containment. Each was deliberately violated and
  observed failing before being trusted.
- `Monarc.Core`: explicit allocators (system, arena), `Array<T>` over them, assertions with
  a replaceable handler, `Result<T>`/`Status`, and categorised non-allocating logging.
- CI building and testing MSVC and Clang, Debug and Release, on every push.

## Building

**Requires:** Visual Studio 2022 or later with the C++ toolset, CMake 3.28+, Ninja, and
Python 3.10+. Clang (`clang-cl`) additionally for the Clang presets.

Builds must run from a Visual Studio developer environment, because the presets choose the
generator and build type but the environment chooses the compiler.

```bat
call "%ProgramFiles%\Microsoft Visual Studio\<version>\<edition>\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

Presets: `msvc-debug`, `msvc-release`, `clang-debug`, `clang-release`. They pin no absolute
tool paths, so `ninja` and `clang-cl` must be resolvable from `PATH`; override anything
machine-specific in `CMakeUserPresets.json`, which is gitignored.

All builds are warnings-as-errors under both compilers.

## Layout

```
CMake/        monarc_module(), target options, test wiring
Source/       engine modules, one directory each
Tools/        architecture gates and documentation checks
Docs/         architecture, decisions, and status
```

## Documentation

Start at [Docs/README.md](Docs/README.md), which maps the rest. The three that constrain
everything else:

- [Architecture/Overview.md](Docs/Architecture/Overview.md) — the shape of the system
- [Architecture/Module-Graph.md](Docs/Architecture/Module-Graph.md) — modules and the rules between them
- [Architecture/Decisions/](Docs/Architecture/Decisions/) — an ADR per load-bearing decision, each recording what was rejected and what it cost

[Docs/Prompt.md](Docs/Prompt.md) is the original vision, kept unchanged as the stable
reference the rest is measured against.

## Licence

None yet, which means default copyright — all rights reserved. If you want others to be
able to use, fork, or contribute to this, it needs an explicit licence.
