# Monarc Documentation

Monarc is a from-scratch game engine and a complete product: a Hub that manages engine
versions and projects, an Editor for authoring, a runtime, a renderer, an asset toolchain,
and a build system that exports clean standalone games.

Start with [Prompt.md](Prompt.md) — the vision. It is the stable document. Everything else
here describes how we are choosing to realise it, and may mature as the engine does.

## Reading path

If you are new to the project, read in this order:

1. [Prompt.md](Prompt.md) — the vision and its ambitions
2. [Architecture/Overview.md](Architecture/Overview.md) — the shape of the system and the ideas holding it together
3. [Architecture/Module-Graph.md](Architecture/Module-Graph.md) — every module, its tier, its kind, its dependencies
4. [Milestones/M0-First-Light.md](Milestones/M0-First-Light.md) — the first end-to-end proof
5. [Status.md](Status.md) — what is actually true today

## Map

### Architecture
- [Overview](Architecture/Overview.md) — layering, the package boundary, enforcement
- [Module Graph](Architecture/Module-Graph.md) — the authoritative module list and dependency rules
- [Decisions](Architecture/Decisions/) — one ADR per load-bearing decision, with alternatives and consequences

### Rendering
- [RHI](Rendering/RHI.md) — the hardware interface, capability tiers, the barrier model
- [Render Graph](Rendering/Render-Graph.md) — passes, resources, derived synchronisation
- [Shaders](Rendering/Shaders.md) — Slang, reflection-driven binding, caches

### Content
- [Asset Pipeline](Content/Asset-Pipeline.md) — source to cooked, identity, cache, dependencies
- [Serialization](Content/Serialization.md) — formats, schemas, versioning, migration

### Runtime
- [World Model](Runtime/World-Model.md) — Actors, Components, relationships, world kinds
- [Frame Model](Runtime/Frame-Model.md) — phases, simulation, extraction, presentation
- [Threading](Runtime/Threading.md) — the job system and ownership rules
- [Memory](Runtime/Memory.md) — allocator strategies, budgets, lifetimes

### Product
- [Hub](Product/Hub.md) — engine versions and projects
- [Editor](Product/Editor.md) — the authoring environment
- [Build and Export](Product/Build-And-Export.md) — packaging, stripping, explainability

### Milestones and plans
- [M0 — First Light](Milestones/M0-First-Light.md) — the first end-to-end proof
- [Plans](Plans/) — task-by-task implementation plans, one per phase
  - [Phase A1 — Foundation and the Build Gates](Plans/2026-09-05-phase-a1-foundation.md)
  - [Phase A2a — Hash, String, and HashMap](Plans/2026-09-06-phase-a2a-strings-and-maps.md)
  - [Phase A2b — Math](Plans/2026-09-06-phase-a2b-math.md)

A milestone says *what* and *why*; a plan says *how*, in executable steps. Plans are written
one phase ahead, not all at once, because later phases are shaped by what earlier ones teach.

## Conventions in these documents

- **Decisions live in ADRs.** If a document states a rule, the ADR explains why and what
  we gave up. Change the ADR, not just the prose.
- **Links are relative Markdown**, so they resolve both in Obsidian and on GitHub.
- **Docs describe intent; `Status.md` describes reality.** Keeping those separate is what
  stops this directory from becoming aspirational fiction.

There is also an Obsidian Canvas, [Monarc-Architecture.canvas](Monarc-Architecture.canvas),
which visualises the relationships between these documents. It supports the Markdown; it
does not replace it.
