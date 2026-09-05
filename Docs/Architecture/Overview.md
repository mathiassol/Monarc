# Architecture Overview

This document describes the shape of Monarc and the ideas that hold it together. The
per-decision reasoning lives in [Decisions](Decisions/); the authoritative module list lives
in [Module-Graph.md](Module-Graph.md).

## The organising idea

Monarc is layered, and **dependencies flow strictly downward**. That is not unusual. What
makes it load-bearing here is that the rule is *enforced by the build system and tested*,
not maintained by discipline. Three constraints do most of the work:

1. Every module declares a **kind**. A `Runtime` module may never depend on a `Tool` or
   `Editor` module. The build rejects it.
2. **The renderer is a package.** Tier 2 depends only on Tier 0. It cannot see the world or
   asset layers, so it stays genuinely reusable.
3. **Headless is a host, not a flag.** A dedicated server does not disable rendering; it
   never links the rendering modules at all.

Each of these is checked by an automated gate — see
[M0 — First Light](../Milestones/M0-First-Light.md#verification-gates). Architecture that is
only described tends to decay; architecture that fails a build does not.

## Tiers

```
Tier 4  Tools        ShaderCompiler   Cook   Build   Editor   Hub
                          |             |      |       |       |
Tier 3  Runtime      World  ->  Engine  ->  Host.Windowed / Host.Headless
                          |             |
Tier 2  Rendering    RHI -> RHI.{Vulkan, D3D12, Metal}   Shaders   Render
                          |
Tier 1  Data         Reflect   Serialize   Assets
                          |
Tier 0  Foundation   Core   Jobs
```

- **Tier 0 — Foundation.** No engine concepts. Platform access, memory, containers, math,
  logging, IDs, and the job scheduler. Everything depends on this; it depends on nothing.
- **Tier 1 — Data.** Type reflection, serialization, asset identity and loading. The
  vocabulary for describing and persisting engine data.
- **Tier 2 — Rendering.** The RHI, its backends, the shader system, and the high-level
  renderer. Depends only on Tier 0 — see [the package boundary](#the-renderer-is-a-package).
- **Tier 3 — Runtime.** The world model, the assembled engine, and the application hosts.
- **Tier 4 — Tools.** Everything that authors or produces content, and never ships.

## Runtime and Tool come in pairs

The separation between what ships and what authors is expressed as **pairs of modules**,
not as folders or preprocessor flags:

| Produces (Tool) | Consumes (Runtime) |
|---|---|
| `Monarc.ShaderCompiler` | `Monarc.Shaders` |
| `Monarc.Cook` | `Monarc.Assets` |

The Editor links both halves. A shipped game links only the right-hand column.

This has a consequence worth stating plainly: because the halves cannot reach into each
other, **the cooked data format becomes a real interface**. It has to be written down,
versioned, and migrated, because the only thing connecting producer and consumer is the
bytes on disk. That is the discipline the vision asks for, obtained structurally rather
than by intention.

A second consequence is that the two kinds can carry **different standard-library policies**
— see [ADR-0003](Decisions/ADR-0003-cpp23-baseline.md). Runtime code is restricted to the
portable subset; tool code, which only ever builds on developer machines, is not.

## The renderer is a package

`Monarc.Render`, `Monarc.RHI`, its backends, and `Monarc.Shaders` depend on **each other and
on Tier 0 — and on nothing else**. In particular they may not include anything from
`Monarc.Reflect`, `Monarc.Serialize`, `Monarc.World`, or `Monarc.Assets`.

The renderer therefore owns its own scene representation — `RenderScene` — and receives
plain descriptors and byte spans. `Monarc.Engine` is the adapter that turns world state into
that representation, at a deliberate [extraction boundary](../Runtime/Frame-Model.md).

This costs a layer of adapter code, permanently. It buys three things:

- The renderer can genuinely be consumed as a standalone C++ package, as the vision requires.
- Rendering can be tested without a world, and worlds can be tested without a GPU.
- The boundary fails loudly. If rendering ever needs to know what an Actor is, a gate breaks
  the build that day, instead of the coupling spreading quietly for a year.

See [ADR-0007](Decisions/ADR-0007-renderer-package-boundary.md).

## Identity is a handle, never a pointer

Assets, GPU resources, actors, and components are referenced by typed handles carrying an
index and a generation counter. Not raw pointers.

This single choice pays into most of the vision's harder requirements at once: dangling
references become detectable rather than undefined; storage can be reorganised for cache
behaviour without breaking references; references serialize stably across runs and engine
versions; hot-reload can swap the thing behind a handle; and — most importantly for the
long term — **handles marshal trivially across a language boundary, and pointers do not**.

The vision asks that the gameplay API not assume C++ forever. Handles are the concrete
decision that keeps that door open. See [ADR-0002](Decisions/ADR-0002-handles-not-pointers.md).

## Authoring model versus storage model

Actors provide identity and gameplay presence; Components provide capabilities. That is how
developers think and author, and it is what the public API looks like.

It is explicitly **not** a statement about how runtime systems store their hot data. Systems
are free to keep data-oriented or hybrid representations where that improves cache
behaviour, iteration, or parallelism. The handle rule above is what makes this separation
possible: because nothing holds a pointer into storage, storage can change.

One departure from Unreal: **transform lives on the Actor**, not on a root component. The
vision defines Actors as providing presence, and transform *is* presence. This removes a
layer of indirection Unreal carries for historical reasons. See
[World-Model.md](../Runtime/World-Model.md).

## Honest abstraction over graphics APIs

The RHI is modelled on where D3D12 and Vulkan have genuinely converged — Enhanced Barriers
and `synchronization2` are close enough in shape to share one barrier model — rather than on
a lowest common denominator or a fantasy that all three APIs are alike. Metal then reads as
a *simplification* of that model rather than a mismatch.

Where hardware genuinely differs, Monarc uses **capability tiers**, and where an API offers
something unique, an **extension path** rather than a pretence of uniformity. See
[RHI.md](../Rendering/RHI.md) and [ADR-0005](Decisions/ADR-0005-rhi-sync-model.md).

Synchronisation is not scattered through rendering features. The
[render graph](../Rendering/Render-Graph.md) derives it from declared reads and writes, so
sync bugs are fixed once rather than per-feature per-backend.

## Content is cooked, never loaded raw

The runtime never loads a source art file. The journey is:

```
source asset -> importer -> intermediate representation -> processing
             -> platform-specific cooked asset -> runtime loading
```

Identity is a GUID stored in a `.meta` sidecar, so it survives renames, moves, and
reimports. The cook cache is content-addressed on `(source content, import settings, cooker
version, platform, engine version)`, which is what makes cooking incremental and
deterministic at the same time.

Because dependencies are recorded as data, the exporter can *explain itself*: given an asset
in the output, it can print the reference chain back to a root. See
[Asset-Pipeline.md](../Content/Asset-Pipeline.md).

## What this buys, and what it costs

**Buys:** parts of the engine can be replaced without a rewrite; compile and iteration times
stay controllable on modest hardware; exported games contain only what they need; a second
language, a second backend, or a different storage strategy are additions rather than
upheavals.

**Costs:** adapter code at the renderer boundary; a cooked format that must be versioned and
migrated because it is a real interface; more modules than a monolith would need; and the
discipline to keep the gates green.

These are deliberate trades, recorded in [Decisions](Decisions/) so that a future change of
mind is an informed one.
