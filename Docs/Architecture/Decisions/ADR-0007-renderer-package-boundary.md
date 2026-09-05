# ADR-0007: The renderer may not depend on world or assets

**Status:** Accepted — 2026-09-05

## Context

The vision requires the Renderer to be "designed as though it could be consumed as a
high-quality C++ package", dealing in useful engine concepts rather than scattered API calls,
with backend code confined behind the RHI.

A package boundary that is merely intended is not a package boundary. The pressure to let the
renderer "just look at" an Actor's transform, or "just load" a texture asset, arrives early
and continuously — and each individual concession is reasonable on its own.

## Decision

Tier 2 — `Monarc.RHI`, its backends, `Monarc.Shaders`, and `Monarc.Render` — **depends on
each other and on Tier 0, and on nothing else**. Concretely, no Tier 2 translation unit may
include a header from `Monarc.Reflect`, `Monarc.Serialize`, `Monarc.World`, or
`Monarc.Assets`.

The Tier 1 exclusion is worth stating explicitly, because it is the one that will feel
arbitrary: the renderer has no business knowing about reflection or serialization either. A
renderer that can serialize itself is a renderer with opinions about asset formats.

The renderer owns its own scene representation, `RenderScene`, and receives plain descriptors
and byte spans. `Monarc.Engine` is the sole adapter turning world state into that
representation, at the [extraction boundary](ADR-0009-render-extraction.md).

This is enforced by a test over the include graph, not by review.

## Consequences

**Good.** The renderer genuinely can be consumed as a standalone package. Rendering becomes
testable without a world, and worlds testable without a GPU — which also means most of the
engine's tests need no graphics device at all. The boundary fails loudly and immediately: the
first `#include` that crosses it breaks the build that day, instead of the coupling spreading
quietly for a year.

**Costs.** A permanent adapter layer in `Monarc.Engine`, and some duplication of concepts
across the boundary — a "material" means something slightly different to the world and to the
renderer. Any feature needing new data in the renderer must thread it through extraction
rather than reading it directly. This cost is paid continuously, and was accepted knowingly.

## Alternatives considered

**Let the renderer read world state directly.** Less code, faster feature work, and the
renderer stops being a package. Rejected — this is the vision's explicit requirement.

**Interfaces that the world implements for the renderer.** Inverts the dependency on paper
while preserving it in practice, and adds virtual dispatch to a hot path. Rejected.

**Allow a dependency on `Monarc.Assets` only.** Superficially reasonable, since the renderer
needs meshes and textures. Rejected: it would couple the renderer to the cooked asset format,
which is among the things that most needs to stay replaceable. The renderer takes byte spans;
the engine decides where the bytes came from.
