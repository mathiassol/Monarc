# ADR-0008: GUID identity, content-addressed cook cache

**Status:** Accepted — 2026-09-05

## Context

The vision requires that asset identity survive renames, moves, reimports and engine
evolution, and that the pipeline be "data-oriented, incremental, deterministic, cacheable, and
prepared for streaming". It also requires that exporting be able to explain why an asset was
included and what the final game actually contains.

Two problems hide inside this, and they need different mechanisms:

- **Identity** — what a reference points to, stably, forever.
- **Freshness** — whether a cooked artifact is still valid.

Using a file path for identity fails the first (renames break references). Using a timestamp
for freshness fails determinism (timestamps change without content changing, and differ
across machines).

## Decision

**Identity is a GUID**, assigned on first import and stored in a `.meta` sidecar next to the
source asset:

```
Content/Models/Barrel.gltf
Content/Models/Barrel.gltf.meta     <- GUID + import settings
```

References between assets, and from scenes to assets, store the GUID. Renaming or moving the
source file carries the `.meta` with it and changes nothing.

**Freshness is content-addressed.** A cooked artifact is keyed by a hash of:

```
source content + import settings + cooker version + target platform + engine version
```

Any of those changing invalidates the artifact; none of them changing means the cook is a
no-op. This makes the pipeline incremental and deterministic by the same mechanism, rather
than trading one against the other.

**Dependencies are recorded as data.** Each cook records what the asset referenced and what it
was built from. `Monarc.Build` walks those records from root references, which is what lets
[`monarc explain`](../../Product/Build-And-Export.md) print the chain of references that put
an asset into an export.

## Consequences

**Good.** Renames, moves and reimports do not break references. Two clean cooks produce
identical bytes, so cooked output can be cached, shared, and compared between machines. A
recook after no changes performs no work. Exports contain only what is reachable, and can
justify every inclusion. The same dependency records later serve streaming.

**Costs.** A sidecar file per asset, which must not be lost — deleting a `.meta` orphans its
GUID and breaks every reference to it, so `.meta` files are project data and must be committed.
Hashing costs IO on large sources, mitigated by caching hashes against file size and
modification time as a *hint* while keeping content as the source of truth. Cooker and engine
versions participate in the key, so bumping either invalidates the whole cache — deliberate,
and the reason those versions must be meaningful rather than incidental.

## Alternatives considered

**Path-based identity.** Simplest, and every rename becomes a broken reference. Rejected — the
vision names this specifically.

**Timestamp-based freshness.** Cheap and non-deterministic, so cooked output cannot be shared
between machines or verified. Rejected.

**Identity stored inside the source file.** Impossible in general — we do not control the
format of a `.gltf` or `.png`, and rewriting artists' source files to inject identity is
hostile. Rejected.

**A central identity database instead of sidecars.** One file to lose instead of many, and it
merges badly in version control. Sidecars move with their asset and conflict only when the
asset itself conflicts. Rejected.
