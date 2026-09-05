# Serialization

Serialization is foundational rather than incidental in Monarc, because the
[Tool/Runtime split](../Architecture/Overview.md#runtime-and-tool-come-in-pairs) means cooked
data is the *only* connection between a cooker and its runtime. The format is a real interface
and is treated as one.

Built on [`Monarc.Reflect`](../Architecture/Decisions/ADR-0010-reflection.md), so types
describe themselves once and serialization follows.

## Two formats, different jobs

| | Text | Binary |
|---|---|---|
| Used for | Scenes, `.meta` files, project files, editor config | Cooked assets, save games, network data |
| Read by | Humans, version control, tools | The runtime |
| Optimised for | Diffability, mergeability, readability | Load speed, size, zero-copy where possible |
| Written by | Editor and tools | Cooker |

Authored data is text so that version control works properly on it — a scene change should be a
readable diff, and two people editing different actors should not produce a binary conflict.
Runtime data is binary because load time and size matter and nobody reads it.

## Versioning from the first commit

Every serialized schema carries a version. Every format carries a header identifying its schema
and engine version. Migrations are registered functions that upgrade older data forward.

This exists from the beginning, before there is anything to migrate. The reason is simple: the
first time it is genuinely needed is the first time data already exists in the wild, and by then
adding it is a data-loss risk rather than a design choice.

Rules:

- A schema version is bumped when its layout or meaning changes.
- A migration is written in the same change that bumps the version.
- Loading data newer than the running engine understands **fails explicitly**. It does not guess.
- Loading data older than the current version migrates, or fails with a clear account of what
  is missing.

## Integrity

Cooked and saved data carries enough to detect damage rather than crash on it: a magic number
and format version, a content hash, and explicit lengths on variable-size sections. A corrupt
asset should produce a diagnosable failure and a failed load state, not undefined behaviour.

## Shared foundations, deliberately separate schemas

Asset data, save-game data, and network data share the same serialization machinery — the same
reflection, the same primitive encodings, the same versioning discipline.

They do **not** share schemas. The vision is explicit that these should not become accidentally
coupled, and the reason is that they change for unrelated reasons and on different schedules: an
asset format changes when the cooker improves, a save format when gameplay changes, a network
format when protocol compatibility requires it. Coupling them means one of those changes
breaking the other two.

## Handles serialize as identity

Because [identity is a handle, not a pointer](../Architecture/Decisions/ADR-0002-handles-not-pointers.md),
references serialize naturally. An asset reference serializes as its
[GUID](../Architecture/Decisions/ADR-0008-asset-identity.md); an intra-scene reference
serializes as a stable local id. Neither serializes as an address, so neither depends on load
order or memory layout.

## Scope in M0

A text reader and writer for authored data, and a versioned binary format for cooked data — both
driven by the type registry. Schema versions and migration registration in place, exercised by
at least one real migration test so the mechanism is proven rather than merely present. Magic
numbers, version headers, and content hashes on binary data.

Deferred: zero-copy and memory-mapped loading, schema evolution tooling, network encodings,
compression.
