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

**"Does not record barriers" is a compiler's rule and not a reviewer's, as of A4.** A recording
callback is handed a `PassCommandList` rather than an RHI command list, and that type exposes no
way to reach the list it wraps — so a pass cannot write a barrier, rather than being asked not
to. What it does not yet expose is any *other* recording call either: A4 has no draws to forward,
so the callback's vocabulary arrives with Phase B's first one.

## What the graph does each frame

Phase A4 built the first version of this, so the seven steps are no longer all intentions.
Each is marked with what exists; [Status.md](../Status.md#a4-delivered) is the record of what
the delivery actually proves, and this list stays the description of the destination.

1. **Build** a dependency graph from declared reads and writes. — *Exists.*
2. **Cull** passes whose outputs nothing consumes. A feature can declare a pass
   unconditionally and pay nothing when its result is unused. — *Exists.*
3. **Compute lifetimes** for every transient resource — first write to last read. — *Exists.*
4. **Alias** transient memory across resources whose lifetimes do not overlap. — *Decided and
   not honoured.* The groups are computed and reported; every transient still gets its own
   allocation. See [Transient resources](#transient-resources).
5. **Derive barriers** in the [RHI's model](RHI.md#barriers-and-synchronisation) from the
   transitions implied by consecutive accesses. — *Exists, and is what A4 was for.*
6. **Assign queues** and insert cross-queue synchronisation. — *Does not exist.* Every pass is
   assigned the graphics queue and the assignment is reported, because there is one queue to
   assign; there is no second queue and therefore no cross-queue synchronisation to insert.
   Both arrive with the first pass that has a reason to be elsewhere.
7. **Order and record**, in parallel where the backend and hardware allow. — *Ordering and
   recording exist, single-threaded.* Parallel recording needs `Monarc.Jobs`, which ADR-0006
   defers past M0, so the graph does not depend on it yet.

## Inspectability is a requirement, not a feature

A graph that derives synchronisation must be able to explain what it derived, or debugging a
wrong barrier becomes archaeology. The graph can emit, as data:

- the pass list, in execution order, with culled passes marked — *emitted*
- every resource, its lifetime, and its aliasing group — *emitted*
- every derived barrier, with the accesses that caused it — *emitted*
- queue assignments and cross-queue waits — *the assignment is emitted, and says "graphics"
  for every pass because that is the only queue; there are no cross-queue waits to emit*

This serves three purposes: a debug view in the Editor, a diffable artifact when a frame
changes unexpectedly, and — most valuable — **tests that assert on derived barriers** rather
than on pixels. A barrier regression should fail a unit test, not a screenshot comparison.

This is why inspectability is built from the start rather than added when first needed. By the
time it is first needed, it is needed urgently.

**The third purpose is the one A4 turned into a fact.** The derivation runs with no device, so
the two barriers that used to be hand-written are asserted against Phase A3's measured capture
values in an ordinary unit test. There is no Editor debug view yet — that arrives with the
Editor — and the text rendering exists as the diffable artifact.

## Transient resources

Most render targets and intermediate buffers live for part of one frame. The graph owns them:
a feature asks for a texture with a description, and receives a handle valid for the passes
that use it.

Because the graph knows every lifetime before execution, it can back non-overlapping resources
with the same memory. This is a meaningful memory win at no cost to feature code, and it is
only possible because resources are declared rather than allocated ad hoc.

**The decision is real; the memory win is not yet.** A4 computes the groups and reports them,
and then creates one texture per transient, because the RHI's memory is one allocation per
resource and sharing one needs sub-allocation from an allocator that does not exist. So a
populated alias group says "these two were computed to be able to share memory", never "these
two shared memory", and a green aliasing test says nothing about bytes. Honouring the grouping
is the memory-allocator work A3 deferred.

## Relationship to the frame

The graph is built after [extraction](../Runtime/Frame-Model.md) and consumes only the
immutable render frame. It never reads world state — see
[ADR-0007](../Architecture/Decisions/ADR-0007-renderer-package-boundary.md). Given the same
snapshot it produces the same frame, which is what makes rendering testable and replayable.

**There is no extraction yet, so in A4 the application declares the frame itself.** There is no
`RenderScene` and no immutable render frame until Phase B/D; `Monarc.FirstLight` builds the graph
directly. The half of the paragraph above that A4 does hold is the one that matters here — the
graph reads nothing but its own declarations, which is why every stage above `Execute` is a pure
function and runs on a machine with no GPU.

## Scope in M0

Structurally real, minimally populated. Declaration, culling, lifetime computation, transient
aliasing, and derived barriers must all exist and be inspectable, exercised by two or three
passes — a depth pass, a forward opaque pass, and presentation.

**A4 ships one pass**, and the two that are missing are the two that need shaders. A depth pass
and a forward opaque pass both draw, and there is no `Monarc.Shaders`, no pipeline and no draw
call until Phase B; the graph's machinery is exercised by a clear through a declared attachment
instead, with multi-pass frames — including a transient written by one pass and read by another
— covered by tests rather than by the app. The remaining passes arrive with the shaders that
give them something to do.

Deferred: parallel command recording, multiple queues (async compute, transfer), and
sub-pass-level optimisation such as Vulkan's dynamic rendering local read.

Two or three passes is enough to prove the machinery is correct. It is not enough to prove it is
fast, and no claim about performance should be made from M0.
