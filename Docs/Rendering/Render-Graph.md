# Render Graph

The render graph is the centre of `Monarc.Render`. Rendering features declare what they need;
the graph decides how it happens.

Design decision: [ADR-0006](../Architecture/Decisions/ADR-0006-render-graph.md).

## The contract

A **pass** declares:

- the resources it reads, and how (sampled, storage, attachment, indirect)
- the resources it writes, and how
- whether its resources are transient (owned by the graph) or external (owned elsewhere)
- a recording callback that issues draws and dispatches

A pass does **not**: record barriers, choose a queue, allocate transient resources, or decide
where in the frame it runs. Those are the graph's job, and keeping them out of features is what
makes three backends and a shared synchronisation model possible at all.

## What the graph does each frame

1. **Build** a dependency graph from declared reads and writes.
2. **Cull** passes whose outputs nothing consumes. A feature can declare a pass
   unconditionally and pay nothing when its result is unused.
3. **Compute lifetimes** for every transient resource — first write to last read.
4. **Alias** transient memory across resources whose lifetimes do not overlap.
5. **Derive barriers** in the [RHI's model](RHI.md#barriers-and-synchronisation) from the
   transitions implied by consecutive accesses.
6. **Assign queues** and insert cross-queue synchronisation.
7. **Order and record**, in parallel where the backend and hardware allow.

## Inspectability is a requirement, not a feature

A graph that derives synchronisation must be able to explain what it derived, or debugging a
wrong barrier becomes archaeology. The graph can emit, as data:

- the pass list, in execution order, with culled passes marked
- every resource, its lifetime, and its aliasing group
- every derived barrier, with the accesses that caused it
- queue assignments and cross-queue waits

This serves three purposes: a debug view in the Editor, a diffable artifact when a frame
changes unexpectedly, and — most valuable — **tests that assert on derived barriers** rather
than on pixels. A barrier regression should fail a unit test, not a screenshot comparison.

This is why inspectability is built from the start rather than added when first needed. By the
time it is first needed, it is needed urgently.

## Transient resources

Most render targets and intermediate buffers live for part of one frame. The graph owns them:
a feature asks for a texture with a description, and receives a handle valid for the passes
that use it.

Because the graph knows every lifetime before execution, it can back non-overlapping resources
with the same memory. This is a meaningful memory win at no cost to feature code, and it is
only possible because resources are declared rather than allocated ad hoc.

## Relationship to the frame

The graph is built after [extraction](../Runtime/Frame-Model.md) and consumes only the
immutable render frame. It never reads world state — see
[ADR-0007](../Architecture/Decisions/ADR-0007-renderer-package-boundary.md). Given the same
snapshot it produces the same frame, which is what makes rendering testable and replayable.

## Scope in M0

Structurally real, minimally populated. Declaration, culling, lifetime computation, transient
aliasing, and derived barriers must all exist and be inspectable, exercised by two or three
passes — a depth pass, a forward opaque pass, and presentation.

Deferred: parallel command recording, multiple queues (async compute, transfer), and
sub-pass-level optimisation such as Vulkan's dynamic rendering local read.

Two or three passes is enough to prove the machinery is correct. It is not enough to prove it is
fast, and no claim about performance should be made from M0.
