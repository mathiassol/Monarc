# ADR-0009: An explicit render extraction boundary

**Status:** Accepted — 2026-09-05

## Context

The vision asks that the frame and simulation model receive the same care as the renderer, and
that input, fixed-step simulation, variable-step systems, transforms, visibility, animation,
physics, gameplay, render extraction, GPU work and presentation have a comprehensible
relationship. It also asks that editor worlds, play-in-editor worlds, network worlds and
headless worlds not require unrelated assumptions.

[ADR-0007](ADR-0007-renderer-package-boundary.md) forbids the renderer from reading world
state. Something must therefore carry data across that boundary, and *how* it does so
determines whether simulation and rendering can ever run in parallel.

## Decision

There is an explicit **extraction** phase between simulation and rendering:

```
simulate  ->  extract  ->  build graph  ->  execute  ->  present
   |             |
world state   immutable render frame
(mutable)     (snapshot)
```

Extraction reads world state and produces an **immutable snapshot** describing what to draw:
visible instances, their transforms, material and mesh references resolved to renderer
handles, views and cameras. Once produced, the snapshot is not modified, and the renderer
consumes only the snapshot.

The boundary is the same in every world kind. An editor world, a play-in-editor world, a
standalone world and a headless world all run the same phases; a headless world simply has no
extraction consumer.

## Consequences

**Good.** Simulation and rendering can overlap, because rendering never reads mutable game
state — which is what makes the [job system](../../Runtime/Threading.md) useful rather than
decorative. Rendering becomes deterministic given a snapshot, so a frame can be replayed and
tested. The renderer's package boundary is upheld by construction. And all four world kinds
are structurally identical, so play-in-editor is not a special case that drifts from
standalone behaviour.

**Costs.** The snapshot must be built every frame, which costs memory traffic and allocation —
mitigated by allocating it from a [frame allocator](../../Runtime/Memory.md) rather than a
general one. Data needed for rendering must be *deliberately* extracted; adding a feature
means extending extraction, not reaching across. And there is a real duplication of state:
a transform exists in the world and in the snapshot.

**Deferred.** M0 extracts synchronously and does not yet overlap simulation with rendering.
The boundary exists so that the overlap is later a scheduling change rather than a rewrite.

## Alternatives considered

**Render directly from world state.** No snapshot cost, no duplication, and simulation and
rendering can never overlap without locking. Rejected.

**Double-buffer the entire world.** Overlap without an explicit extraction step, at the cost of
duplicating everything rather than just what is drawn, and of making every system aware of
which buffer it is in. Rejected as more invasive for less control.

**Let the renderer hold handles into the world and resolve them late.** Reintroduces the
dependency [ADR-0007](ADR-0007-renderer-package-boundary.md) forbids, and puts world access on
the render thread. Rejected.
