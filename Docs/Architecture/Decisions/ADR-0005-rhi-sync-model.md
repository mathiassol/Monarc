# ADR-0005: RHI barrier model from the D3D12/Vulkan convergence

**Status:** Accepted — 2026-09-05

## Context

The vision asks that the RHI be "small, coherent, and honest about the differences between
graphics APIs" — sharing behaviour where the APIs genuinely overlap, using capability tiers
where hardware differs, and giving advanced features an extension path rather than
pretending platforms are identical.

Synchronisation is where RHI abstractions usually go wrong. The three APIs appear to differ
profoundly, which tempts designers either into a lowest-common-denominator model that wastes
performance, or into a per-backend model that pushes synchronisation into every rendering
feature.

That appearance is out of date. D3D12 **Enhanced Barriers** and Vulkan **`synchronization2`**
have genuinely converged: both express a barrier as source and destination *stage*, source
and destination *access*, and for textures a *layout* transition, with support for split
barriers. Metal differs — within a command encoder it needs almost no explicit barriers, and
across encoders it uses `MTLFence` and `MTLEvent`.

## Decision

The RHI's barrier type is modelled directly on the **Enhanced Barriers / `synchronization2`
shared shape**: sync-before, sync-after, access-before, access-after, and for textures
layout-before and layout-after, with buffer, texture, and global variants.

Metal is then implemented as a **simplification** of that model: barriers within an encoder
mostly collapse to nothing, and cross-encoder barriers become fences and events. Reducing
information is straightforward; the reverse — inventing stage and access detail that a
lowest-common-denominator model had discarded — is not.

Vulkan's baseline is **1.3 core** (dynamic rendering, `synchronization2`, timeline
semaphores, descriptor indexing from 1.2), with 1.4 features taken opportunistically.

Application code does not write barriers. The [render graph](ADR-0006-render-graph.md)
derives them.

## Consequences

**Good.** The abstraction is honest about two of three backends because it is *shaped from*
them rather than imposed above them. Nothing is discarded on the two backends that matter
first. Metal's implementation is a reduction, which is the easy direction.

**Costs.** The RHI's barrier type carries more detail than Metal needs, so the Metal backend
contains translation logic that throws information away. Vulkan 1.3 as a floor excludes older
hardware and drivers — acceptable, and validated locally against two vendors and two
capability tiers (RTX 3070 Ti and Intel UHD 730).

**The honesty test.** D3D12 is scheduled immediately after M0, before the RHI ossifies around
Vulkan assumptions — see [ADR-0012](ADR-0012-backend-rollout.md). An abstraction validated
against a single backend is not validated.

## Alternatives considered

**A lowest-common-denominator barrier model.** Simpler, and it discards exactly the
information Vulkan and D3D12 need in order to be fast. Rejected.

**Per-backend synchronisation with no shared model.** Honest, but pushes synchronisation into
every rendering feature — the specific outcome the vision says to avoid.

**Fully automatic tracking with no explicit barrier concept.** Attractive, but state tracking
without a global view of the frame produces conservative, slow barriers. The render graph
provides that global view, so the barrier model underneath it can stay explicit.
