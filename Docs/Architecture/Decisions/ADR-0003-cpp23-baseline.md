# ADR-0003: C++23 baseline, library restricted by module kind

**Status:** Accepted, **conditional** — 2026-09-05. Condition **configured but not yet
observed passing** as of 2026-09-06.

The condition is that a second compiler (Clang) must build the project in CI, because an
MSVC-only build proves nothing about C++23 conformance — MSVC does not accept `/std:c++23`
and silently falls back to `/std:c++latest`, which is *beyond* C++23.

`.github/workflows/ci.yml` now builds and tests four configurations on every push and pull
request — MSVC and Clang, Debug and Release — with `fail-fast` disabled so that when the two
compilers disagree, both results are visible. Warnings are errors in all four.

That workflow has been written and validated locally, but **has not yet run on a GitHub
runner**. Until it has gone green once, this condition is configured rather than met — and
CI's MSVC is an older toolset than the 19.51 used locally, so the first run is a genuine
test rather than a formality. Update this line to **Accepted** when it passes.

## Context

Common advice for a long-lived engine is to hold at C++20. That advice usually conflates two
things with very different portability profiles:

- **C++23 language features** are broadly implemented: MSVC 19.4x+, Clang 17+, GCC 13+.
- **C++23 library features** are fragmented, and worst on the platform we are heading for.
  `std::print`, `std::generator`, `std::flat_map`, `std::stacktrace` and the C++23 ranges
  adaptors lag badly in libc++ and therefore in AppleClang. Metal is expected within a year.

The advice to hold at C++20 is really a warning about the second column. It is aimed at
codebases that lean on the standard library, or teams shipping to consoles today.

Monarc is not that codebase. [ADR-0014](ADR-0014-dependency-policy.md) commits us to owning
containers, strings, allocators and math, because std's allocator model and debug-build
performance are not acceptable in an engine anyway. The fragmented half of C++23 is a half
we were never going to use.

**Verified on this machine:** MSVC 19.51 compiles deducing `this`, `static operator()`,
multidimensional `operator[]`, `if consteval`, `auto(x)`, `[[assume]]`, and `std::expected`
cleanly at `/W4`.

## Decision

**C++23 is the baseline** for all modules. The restriction applies to the *library*, not the
language, and it differs by module kind — which the build system already knows:

| Kind | Language | Standard library |
|---|---|---|
| `Runtime` | Full C++23 | Restricted to the universally-available header-only subset: `<type_traits>`, `<concepts>`, `<utility>`, `<expected>`, `<span>`, `<bit>`, `<atomic>`, `<cstdint>`, `<format>`, and similar. **Banned:** `<iostream>`, `<regex>`, `<print>`, `<generator>`, `<flat_map>`, `<stacktrace>`, and C++23 ranges adaptors |
| `Tool`, `Editor`, `Test` | Full C++23 | Unrestricted. These build only on developer machines with current toolchains |

**Amended 2026-09-05.** This decision originally banned `<format>` alongside `<print>` in
runtime code. That conflated two different things: `<print>` is C++23 and genuinely lags in
libc++, whereas `<format>` is C++20 and widely available. Logging needs type-safe
formatting, and writing a formatter to satisfy a rule we had drawn too broadly would have
been the wrong trade. `<format>` is now permitted in runtime code, on the condition that it
formats into a fixed buffer via `std::format_to_n` rather than allocating a `std::string` —
see `Monarc::Detail::Format` in `Log.h`. If `<format>`'s compile-time cost later becomes a
problem, the formatter sits behind `MONARC_LOG` and can be replaced without touching call
sites.

Additionally:

- **No C++20 modules.** Strict header hygiene and optional unity builds instead. CMake's
  module dependency scanning is disabled (`CXX_SCAN_FOR_MODULES OFF`), which is free build
  time on a six-core machine.
- `/Zc:__cplusplus` is set globally on MSVC.
- **C++26 is not adopted**, but static reflection (P2996) is watched — see
  [ADR-0010](ADR-0010-reflection.md), which is deliberately shaped to accept it later.

## Consequences

**Good.** Deducing `this` removes the CRTP boilerplate that [handle types](ADR-0002-handles-not-pointers.md)
and RHI resource wrappers would otherwise need — const, non-const and rvalue overloads
collapse into one. `std::expected` is the `Result` type directly. `[[assume]]` serves hot
loops; multidimensional `operator[]` serves grid and texture addressing; `static operator()`
serves stateless job functors. The policy is *enforceable*, which "C++20 with selective
C++23" was not — nobody can tell by inspection whether a line is C++20-legal.

**Costs.** We are currently satisfying exactly one compiler, which is a thin basis for a
portability claim. Console toolchains, if Monarc ever targets them, lag; a future downgrade
would be a mechanical port of a handful of language constructs, which is bounded but real.

**The asymmetry that decided it:** choosing C++23 and later needing C++20 is a mechanical
port. Choosing C++20 and later wishing for C++23 means the whole codebase is written in the
more verbose style permanently, because nobody goes back and rewrites working code.

## Alternatives considered

**C++20 baseline.** The defensible choice *if* we do not add a second compiler to CI. The
two decisions are linked, and that trade is recorded here rather than left implicit.

**C++20 floor with opportunistic C++23.** Rejected as unenforceable. It fails at an
unpredictable moment on a compiler nobody ran.

**C++26 with an experimental Clang for reflection.** Rejected: no shipping compiler has
usable P2996, and betting the type system on a fork is not a foundation.
