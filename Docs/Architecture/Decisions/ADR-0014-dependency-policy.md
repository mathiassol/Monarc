# ADR-0014: Own the core, license the specialists

**Status:** Accepted — 2026-09-05

## Context

"From-scratch engine" can mean several things, and the difference changes the project's
timeline by years. It might mean *not built on Unity or Unreal*, or it might mean *no
third-party code at all*. The vision says both "from-scratch game engine" and "not a library
that sits beside an otherwise separate game" — which are claims about ownership of the
architecture, not necessarily about every line of code.

## Decision

**Monarc owns everything that defines its character and performance. Third-party code is
confined to deep specialist domains, always behind a Monarc-owned interface.**

Monarc writes:

- math, containers, strings, allocators
- the job system and threading model
- windowing and input
- the RHI and all graphics backends
- the render graph and high-level renderer
- the asset pipeline, serialization, and versioning
- the world model, the editor's model layer, and the build and export system

Third-party code is acceptable for:

| Domain | Intended | Rationale |
|---|---|---|
| Shader compilation | Slang | Khronos-adopted, and a compiler is its own multi-year project ([ADR-0004](ADR-0004-slang-shading-language.md)) |
| Editor UI scaffolding | Dear ImGui | Explicitly transitional ([ADR-0013](ADR-0013-editor-ui.md)) |
| Unit testing | doctest | Single-header MIT framework; compiles far faster than Catch2, which matters on six cores. Test-only, never linked into a shipped target |
| Graphics API definitions | Vulkan-Headers | Not a library at all — an API definition, and therefore the platform interface, in the same sense `windows.h` is. Header-only, Apache-2.0, no code linked. Pinned to a tag (`vulkan-sdk-1.4.357.0`) rather than found through `find_package(Vulkan)`, so the build is reproducible on a machine with no SDK — which is every CI runner |
| Physics | Jolt | Deep specialist domain; correctness and stability take years |
| Image and font decoding | To be chosen | Format-compliance work with no architectural content |
| Mesh optimisation | meshoptimizer | Well-studied algorithms, no architectural content |
| Audio | To be chosen | Platform backends and DSP; specialist |

Two rules govern every inclusion:

1. **Behind a Monarc interface.** No third-party type appears in a Monarc public header.
   Replacing the library must be an internal change.
2. **Permissive licence only.** MIT, Apache-2.0, BSD, zlib. Nothing that constrains what a
   Monarc user may ship.

Rule 1 needs one clarification that the Vulkan-Headers row above is the first case of. A
graphics API's *headers* are not third-party code sitting behind an interface — they are the
interface, to the operating system, and there is no version of "own the RHI" that involves
writing our own `VkInstanceCreateInfo`. What rule 1 governs there is that no Vulkan type
appears in a `Monarc.RHI` public header, which
[RHI.md](../../Rendering/RHI.md) already requires and
[gate 3](../../Milestones/M0-First-Light.md) polices. `Monarc.RHI.Vulkan`'s own public header
names no Vulkan type either; the headers are private to its implementation.

The linking rule that goes with it is stricter than the licence rule and matters more:
**Monarc links the `Vulkan::Headers` interface target and never `Vulkan::Vulkan` or
`vulkan-1.lib`.** An import library would make a missing Vulkan runtime a Windows loader
failure before `main`, where opening the runtime through `Platform::Library` makes it an
ordinary `Result` — see the loader decision in the Phase A3 plan.

Note that windowing and input are on the *own it* side, which is the line most projects draw
differently. They sit closer to engine architecture than they appear: they own the event loop,
frame pacing, and the platform surface the swapchain is built on — and they are the first thing
a new platform requires.

## Consequences

**Good.** The engine's performance character, allocator model and iteration behaviour are ours,
which is exactly what cannot be retrofitted. Debug-build performance is under our control —
std containers alone would make debug builds unusable in an engine. Dependency count stays low,
so builds stay reproducible and porting stays feasible. Where we do take a library, it is in a
domain where writing our own would be worse, not merely slower.

**Costs.** Substantially more work before the engine does anything a game needs — containers
and math are a real chunk of M0 that produces no visible feature. Our containers will have
bugs that `std::vector` does not. Windowing and input in particular are unglamorous work that
must be redone per platform. And "own the core" is a line that will be argued at every
boundary; the table above exists so those arguments are short.

## Alternatives considered

**Library-first for everything solved** (SDL3 for windowing and input, GLM for math, and so on).
Fastest route to a working engine. Rejected: the engine's seams and performance character would
be partly someone else's, and SDL's event and window model would shape our frame loop rather
than the reverse.

**Purist — write literally everything**, including PNG decoding, font rasterisation, physics and
audio. Maximum ownership and the best learning. Rejected: it adds years before the engine does
anything a game needs, and format compliance and physics stability are domains where our own
version would be strictly worse for a long time.

**Own the core now, revisit per domain as it arrives.** This is effectively the accepted
decision — the table is expected to grow, and each addition should be recorded here with its
rationale rather than absorbed silently.
