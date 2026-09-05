# Memory

The vision asks that memory be intentional from day one — that CPU, GPU, frame, persistent,
asset, and transient memory have understandable ownership and lifetime, and that budgets,
alignment, reclamation, profiling and telemetry not be afterthoughts.

The reason this cannot wait: code written against an implicit global allocator has to be
rewritten to accept an explicit one. Every system, everywhere. Passing an allocator from the
start costs a parameter.

## Allocator strategies

| Strategy | Lifetime | Used for |
|---|---|---|
| **General** | Arbitrary | Long-lived engine state, editor data. The fallback, not the default |
| **Linear / arena** | Reset as a whole | Loading, cooking, any phase with a clear end |
| **Frame** | One frame | The [render frame snapshot](Frame-Model.md#the-extraction-boundary), per-frame scratch, command data |
| **Pool** | Per-object, uniform size | Actors, components, GPU resource records — anything with handles |
| **GPU** | Explicit | Device memory, sub-allocated. Separate budgets from CPU |
| **Transient GPU** | Part of one frame | Render graph render targets and intermediates, aliased across non-overlapping lifetimes |

## Allocators are passed, not assumed

Systems that allocate take an allocator. There is no ambient global `new` in engine code.

This is the rule that makes everything else possible — budgets, per-subsystem reporting,
swapping a strategy, and testing a system under a constrained allocator all require that the
system does not choose its own allocator. It is also the rule most likely to be quietly broken
under deadline pressure, so it is worth stating plainly and checking in review.

Pool allocation pairs naturally with
[handles](../Architecture/Decisions/ADR-0002-handles-not-pointers.md): a pool slot's index is
the handle's index, and the generation counter lives beside it. Handle resolution and pool
lookup are the same operation.

## Frame memory and the extraction snapshot

The per-frame render snapshot is the clearest case for a frame allocator. It is built every
frame, read once, and discarded — genuinely per-frame garbage. From a frame allocator its cost
is a pointer bump and a reset; from a general allocator it would be thousands of allocations
and frees per second for no reason.

This is also why [ADR-0009](../Architecture/Decisions/ADR-0009-render-extraction.md)'s snapshot
cost is acceptable at all. The two decisions depend on each other.

## GPU memory is separate

GPU memory has its own budgets, its own residency concerns, and failure modes CPU memory does
not have. It is tracked separately, sub-allocated from larger device allocations rather than
allocated per resource, and the [render graph](../Rendering/Render-Graph.md) aliases transient
GPU memory across resources whose lifetimes do not overlap.

## Budgets, profiling, telemetry

Every allocator is tagged and reports its usage. That gives, without extra machinery:

- **Budgets** per subsystem, and a clear answer to which subsystem exceeded one
- **Reports** by category rather than one total that says nothing actionable
- **Leak detection** — an arena or pool that does not return to empty when it should
- **Telemetry** over time, to see growth rather than a single snapshot

Alignment is explicit in the allocator interface. Over-aligned types, SIMD data, and GPU-visible
memory all have real alignment requirements, and discovering them through a crash on one
platform is avoidable.

## Scope in M0

The allocator interface, with general, linear/arena, frame, and pool implementations. Allocators
passed explicitly throughout engine code. Tagging and per-allocator usage reporting. Explicit
alignment. GPU memory sub-allocated with its own tracking, and transient GPU aliasing in the
render graph.

Deferred: a custom general-purpose allocator replacing the system one, memory defragmentation,
residency management and eviction, detailed telemetry over time, allocation hot-spot profiling.

The interface is what matters in M0. A general allocator that wraps the system allocator is
fine — replacing it later is an internal change *because* it sits behind the interface. What
could not be changed later is code that never took an allocator in the first place.
