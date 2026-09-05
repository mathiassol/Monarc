# ADR-0001: Layered modules with build-enforced kinds

**Status:** Accepted — 2026-09-05

## Context

The vision requires that the Editor, Hub, and development-only systems not be pulled into a
shipped game merely because they share a repository, and that this separation be "real and
visible". It also requires that a headless or dedicated-server runtime can exist without
graphics.

Layering by convention decays. Every engine that relies on reviewers noticing a bad
`#include` eventually acquires the coupling it meant to avoid, because the cost of each
individual violation is small and the cost is only paid later, collectively.

## Decision

Monarc is organised into five tiers with strictly downward dependencies. Every module
declares a **kind** — `Runtime`, `Tool`, `Editor`, or `Test` — and the build system rejects:

- any cycle in the module graph,
- any dependency on a higher tier,
- any `Runtime` module depending on a `Tool` or `Editor` module.

The producer/consumer split is expressed as **pairs of modules** (`Monarc.Cook` /
`Monarc.Assets`, `Monarc.ShaderCompiler` / `Monarc.Shaders`) rather than as folders or
preprocessor flags. Headless is a distinct host module that links no rendering modules at
all, rather than a build flag that disables them.

The full graph is in [Module-Graph.md](../Module-Graph.md).

## Consequences

**Good.** The separation is checkable rather than aspirational. Compile times stay bounded
because the graph is shallow and explicit. Exported games can be verified free of tool code
by inspecting symbols. Because the two halves of each pair cannot reach into each other, the
cooked data format is forced to become a genuine, versioned interface.

**Costs.** More modules than a monolith needs. Some code that "obviously" wants to be shared
between a cooker and its runtime must instead be placed in a lower tier or duplicated
deliberately. Adding a module has ceremony.

## Alternatives considered

**A single library with folder conventions.** Fastest to start, and the standard outcome is
that the editor ends up linked into shipped games. Rejected because the vision names this as
a specific thing to prevent.

**Runtime checks or lint rules instead of build failures.** Weaker: lint can be silenced and
warnings accumulate. A build error is unambiguous.

**Header-only, tier-free organisation.** Rejected on compile time alone — the development
machine has six cores, so the module graph is also a build-performance decision.
