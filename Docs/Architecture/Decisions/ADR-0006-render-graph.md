# ADR-0006: The render graph owns all synchronisation

**Status:** Accepted — 2026-09-05

## Context

The vision is unusually specific here: a render graph should be central to rendering,
understanding resource dependencies, lifetimes, transient allocations, aliasing, barriers,
synchronisation, queue usage, execution order, pass culling and parallel recording — and it
should be inspectable and debuggable. High-level code should describe what it needs;
synchronisation should not be manual bookkeeping scattered through every feature.

## Decision

Rendering features declare **passes** stating the resources they read and write. They do not
record barriers, choose queues, allocate transient resources, or decide execution order.

Each frame the graph:

1. builds a dependency graph from the declared reads and writes,
2. culls passes whose outputs nothing consumes,
3. computes resource lifetimes and aliases transient memory across non-overlapping lifetimes,
4. derives barriers in the [ADR-0005](ADR-0005-rhi-sync-model.md) model,
5. assigns queues and inserts cross-queue synchronisation,
6. orders execution and records command lists, in parallel where possible.

The graph is **inspectable**: it can emit its pass list, resource lifetimes, aliasing
decisions and derived barriers as data — for a debug view, and for tests.

## Consequences

**Good.** Synchronisation bugs are fixed in one place rather than per feature per backend,
which is what makes three backends tractable at all. Pass culling and transient aliasing come
free to every feature. Adding a rendering feature does not require understanding barriers.
And because derived barriers are inspectable data, they can be *tested* rather than merely
observed in a capture tool.

**Costs.** Real complexity concentrated in one subsystem, which must be correct before much
can be built on it. A frame's cost is no longer obvious from reading one feature's code. Some
per-frame graph overhead. Debugging a wrong barrier means debugging a derivation, which is
harder than reading a line somebody wrote — mitigated by making the derivation inspectable
from the start rather than as a later addition.

**Scope in M0.** Two or three passes is enough to prove the machinery. The graph must be
structurally real — declaration, culling, lifetimes, derived barriers — but need not yet have
parallel recording or multiple queues.

## Alternatives considered

**Manual barriers in each rendering feature.** Simple at first, and precisely the failure mode
the vision names. It also does not survive three backends.

**Automatic state tracking without a graph.** Produces conservative barriers, because no
component knows the shape of the whole frame.

**Adopt the graph later, once features exist.** Rejected: retrofitting a render graph means
rewriting every feature that had assumed manual synchronisation.
