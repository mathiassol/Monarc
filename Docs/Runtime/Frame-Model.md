# Frame Model

The vision asks that the frame and simulation model receive the same care as the renderer, and
that input, simulation, transforms, visibility, extraction, GPU work and presentation have a
comprehensible relationship. This document is that relationship.

Design decision: [ADR-0009](../Architecture/Decisions/ADR-0009-render-extraction.md).

## The phases

```
  input
    |
  fixed-step simulation   (zero or more times per frame)
    |
  variable-step update
    |
  transform propagation
    |
  visibility
    |
  EXTRACT  --------------->  immutable render frame
    |                              |
  (next frame may begin)      build render graph
                                   |
                                execute
                                   |
                                present
```

The **extraction boundary** is the important line. To its left, mutable world state. To its
right, an immutable snapshot. Nothing on the right reads anything on the left.

## Fixed and variable steps

Simulation that must be deterministic and stable — physics, and gameplay that depends on it —
runs at a fixed timestep, zero or more times per frame depending on accumulated time.
Everything else runs once per frame with the real delta.

Both exist from the start because retrofitting a fixed step into a variable-step codebase means
auditing every system for frame-rate dependence. Declaring which phase a system belongs to is
cheap; discovering it later is not.

## The extraction boundary

Extraction reads world state and produces an **immutable render frame**: visible instances,
their world transforms, mesh and material references resolved to renderer handles, and views.

Once produced it is never modified. The renderer consumes only the snapshot, which:

- lets simulation and rendering **overlap**, since rendering never touches mutable state;
- makes rendering **deterministic** given a snapshot, so frames can be replayed and tested;
- upholds the [renderer package boundary](../Architecture/Decisions/ADR-0007-renderer-package-boundary.md)
  by construction rather than by discipline.

The snapshot is allocated from a [frame allocator](Memory.md), not a general one. It is
per-frame garbage by design, and treating it that way keeps the cost to a pointer bump.

## The main thread has a job

The vision warns against the main thread becoming "the place where every system eventually
waits". So its role is defined rather than residual:

**The main thread owns:** platform and OS message pumping, input collection, presentation, and
the frame's phase sequencing.

**The main thread does not own:** simulation work, extraction of large scenes, command
recording, or asset loading. Those belong to [`Monarc.Jobs`](Threading.md), with the main thread
scheduling and joining rather than performing.

M0 does much of this synchronously. The phases exist so that moving work off the main thread is
later a scheduling change rather than a restructuring.

## The same phases in every world kind

Editor, play-in-editor, standalone, and headless worlds run the same phase sequence. An editor
world does not simulate; a headless world has no extraction consumer. Neither is a different
frame model — see [World-Model.md](World-Model.md#world-kinds).

## Frame pacing and presentation

Noted rather than built. Presentation mode, vsync behaviour, frame pacing, and latency are
properties of the windowed host and the swapchain, and belong in
[`Monarc.RHI`](../Rendering/RHI.md) and `Monarc.Host.Windowed`. M0 presents simply and makes no
claims about latency.

## Scope in M0

All phases exist and run in order, mostly synchronously. Extraction produces a real immutable
snapshot from a frame allocator. Fixed and variable steps are distinguished, with at least one
system in each.

Deferred: overlapping simulation with rendering, parallel extraction, parallel command
recording, frame pacing, and latency work.
