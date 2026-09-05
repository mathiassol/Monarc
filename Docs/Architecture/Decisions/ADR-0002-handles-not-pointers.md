# ADR-0002: Handles, not pointers, for all engine identity

**Status:** Accepted — 2026-09-05

## Context

The vision states that runtime code should use stable handles or resource identities with
explicit loading, unloading, reload and failure behaviour "rather than treating raw pointers
as the identity of content". It separately requires that asset identity survive renames,
moves, reimports and engine evolution, and that the public gameplay API not assume C++ will
be the only way to write logic forever.

These read as three requirements. They are largely one decision.

## Decision

Assets, GPU resources, actors, and components are referenced by **typed handles** carrying
an index and a generation counter:

```cpp
template <typename T>
struct Handle {
    uint32_t index;
    uint32_t generation;
};
```

Resolving a handle checks the generation against the slot. A stale handle resolves to a
failure, not to whatever now occupies the memory. Raw pointers may exist as short-lived
locals within a single scope, but never as stored identity, never in serialized data, and
never across a frame boundary.

## Consequences

**Good.** Dangling references become *detectable* rather than undefined. Storage can be
reorganised — pooled, sorted, made data-oriented — without breaking any reference, which is
what allows [the authoring model to differ from the storage model](../Overview.md#authoring-model-versus-storage-model).
References serialize stably. Hot-reload can swap the object behind a handle. Most
importantly for the long term: **handles marshal trivially across a language boundary, and
pointers do not** — this is the concrete decision that keeps a future scripting language
possible.

**Costs.** An indirection on every access, and a resolve step that can fail and must be
handled. Systems that need tight inner loops must resolve once and iterate over storage
directly rather than resolving per element — a discipline, and occasionally an annoyance.

## Alternatives considered

**Raw pointers with careful ownership.** Fastest to access, and forecloses the scripting
boundary, safe hot-reload, and storage reorganisation simultaneously. Rejected.

**Reference counting (`shared_ptr`-style).** Solves lifetime but not identity, serialization,
relocation, or language marshalling — and adds atomic traffic. Rejected.

**Handles only for assets, pointers for actors and components.** Tempting, and it is where
the pressure will come from during implementation. Rejected because the gameplay layer is
precisely the layer that must eventually be reachable from another language.
