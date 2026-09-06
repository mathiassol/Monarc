# M0 — First Light

The first end-to-end proof. A project is created in the Hub, opened in the Editor, has content
imported through the real toolchain, renders through the intended runtime path, plays in the
editor, runs standalone, and exports to a clean build.

## The governing principle

**Build the spine thin but structurally correct.**

Every module exists with its real intended public interface. Implementations are minimal.
Nothing is faked in a way that would need architectural rework later.

The distinction that matters: a general allocator wrapping the system allocator is *thin* — it
sits behind the real interface and can be replaced. Code that never took an allocator is
*wrong*, and no amount of later work makes it right without touching every call site. M0 accepts
the former everywhere and the latter nowhere.

## What is in scope per module

| Module | In M0 | Deferred |
|---|---|---|
| `Core` | File IO, paths, time, threads, dynamic libraries; allocator interface with general, arena, frame and pool; `Array`, `String`, `HashMap`, `Span`; scalar math; log categories; asserts; GUID; `Result` | SIMD math behind the same API, custom general allocator, full profiler |
| `Jobs` | Thread pool, dependency graph, wait handles, priorities, instrumentation — **used by the cooker** | Fibers, work stealing, broad cancellation |
| `Reflect` | Types, properties (name, type, offset, attributes), enums | Methods, dynamic invoke, codegen |
| `Serialize` | Text format for authored data, versioned binary for cooked data, schema versions, migration with at least one real migration test | Zero-copy loading, network encodings, compression |
| `Assets` | GUID identity, typed handles, load/unload/reload, explicit load states, dependency records | Streaming, bundles, async priorities |
| `RHI` | Device, adapters, capability tiers; buffers, textures, samplers, shaders, pipelines; command lists with the Enhanced-Barriers-shaped API; queues, timeline sync; swapchain; bindless heap | D3D12 and Metal backends, ray tracing, mesh shaders, device loss |
| `RHI.Vulkan` | Vulkan 1.3 backend | — |
| `ShaderCompiler` / `Shaders` | Slang to SPIR-V with reflection driving binding layout; cooked shader artifact; pipeline cache | Variants and permutations, hot reload |
| `Render` | `RenderScene`; render graph with declaration, culling, lifetimes, transient aliasing, derived barriers, inspectability; two or three passes | Lighting, shadows, post-processing, culling, parallel recording, multiple queues |
| `World` | Actor with transform and hierarchy, Component, `StaticMeshComponent`, `CameraComponent`, world storage, tick phases, all four world kinds | Physics, animation, spatial queries, replication, prefabs |
| `Engine` | Subsystem lifecycle, phased frame loop, input, time, render extraction to an immutable snapshot | Overlapped simulation and rendering, parallel extraction |
| `Host.Windowed` / `Host.Headless` | Own window and input platform code; headless links no Tier 2 | Frame pacing, latency work |
| `Cook` | Importer interface, glTF static mesh, image import; IR, processing, platform cook; content-addressed incremental cache; dependency records; `.meta` sidecars | Texture compression, audio, animation, LODs, file watching |
| `Build` | Export a Windows standalone; reachability-based asset selection; manifest; `monarc explain` | Other platforms, patches, DLC, installers, signing |
| `Editor` | Viewport, outliner, inspector, content browser, log; command stack with undo/redo; scene save and load; import triggering a cook; play-in-editor | Gizmos beyond translation, docking persistence, first-party UI, accessibility |
| `Hub` | Create project from template, list projects and versions, launch Editor | Version downloads, upgrades, multiple templates |

Textures cook to uncompressed RGBA8 with mips. The cooked format supports compressed formats
from the start, so BC7 later is a cooker change rather than a format change.

## Phases

Each phase ends in something that runs. That is what stops a large first milestone from
stalling — there is always a working thing, and always a place to redirect.

| Phase | Contents | Runnable proof |
|---|---|---|
| **A1** | Build system, `monarc_module`, architecture gates, Core's memory and diagnostics foundation | A build that enforces the architecture, and a tested `Monarc.Core` — [plan](../Plans/2026-09-05-phase-a1-foundation.md) |
| **A2a** | `Hash`, `String`, `HashMap` | The vocabulary types the rest of Core is written in — [plan](../Plans/2026-09-06-phase-a2a-strings-and-maps.md) |
| **A2b** | Math — vectors, matrices, quaternions, transforms | Testable math with the conventions of [ADR-0015](../Architecture/Decisions/ADR-0015-math-conventions.md) locked in by tests — [plan](../Plans/2026-09-06-phase-a2b-math.md) |
| **A2c** | Platform — files, paths, time, threads, dynamic libraries, GUID | The OS boundary, selected by directory rather than `#ifdef` ([ADR-0016](../Architecture/Decisions/ADR-0016-platform-code-selection.md)) — [plan](../Plans/2026-09-06-phase-a2c-platform.md) |
| **A2d** | `Monarc.Jobs` | A parallel job graph with dependencies, under test — the first module beyond `Monarc.Core` — [plan](../Plans/2026-09-06-phase-a2d-jobs.md) |
| **A3** | `RHI`, `RHI.Vulkan`, `Host.Windowed` | A window with a cleared screen, via the RHI directly, plus a headless readback test that proves the colour — [plan](../Plans/2026-09-06-phase-a3-rhi-and-first-light.md) |
| **A4** | Minimal render graph | A window opens and the screen clears **through the real render graph** |
| **B** | `ShaderCompiler`, `Shaders`, `Render` | A cube renders from in-memory data via `RenderScene` and a Slang shader |
| **C** | `Reflect`, `Serialize`, `Assets`, `Cook` | A glTF mesh and texture are imported, cooked, loaded by handle, and rendered |
| **D** | `World`, `Engine` | An Actor with a `StaticMeshComponent` and a camera, in a saved scene |
| **E** | `Editor` | Viewport, inspector, undo/redo, content browser, play-in-editor |
| **F** | `Hub`, `Build` | Project created from the Hub; standalone runs; export verified clean |

## Verification gates

These are what make M0 a proof rather than a demo. Each is automated — a test or a build
failure, not a judgement.

| # | Gate | Guards |
|---|---|---|
| 1 | No `Monarc::Editor` or `Monarc::Cook` symbols in a shipped game binary | [ADR-0001](../Architecture/Decisions/ADR-0001-layered-modules.md), export purity |
| 2 | Module graph is acyclic; no `Runtime` module depends on `Tool` or `Editor` | [ADR-0001](../Architecture/Decisions/ADR-0001-layered-modules.md) |
| 3 | No Tier 2 translation unit includes a header from Tier 1 or Tier 3 — `Monarc.Reflect`, `Monarc.Serialize`, `Monarc.World`, `Monarc.Assets` | [ADR-0007](../Architecture/Decisions/ADR-0007-renderer-package-boundary.md) |
| 4 | Two clean cooks produce identical output hashes | [ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md), determinism |
| 5 | Cook, change nothing, cook again: zero work performed | [ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md), incrementality |
| 6 | Rename a source asset, reimport: the scene still resolves it | [ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md), identity |
| 7 | The same scene yields identical actor and component state in Editor, Play, Standalone and Headless worlds | [World kinds](../Runtime/World-Model.md#world-kinds) |
| 8 | `monarc explain <asset>` prints a reference chain to a root | [Build-And-Export](../Product/Build-And-Export.md) |
| 9 | The headless binary contains no RHI or graphics-API symbols | [Module-Graph](../Architecture/Module-Graph.md), headless purity |
| 10 | Platform-conditional compilation appears only in `Monarc.Core/Platform` | [ADR-0012](../Architecture/Decisions/ADR-0012-backend-rollout.md), macOS viability |
| 11 | A serialized schema migration round-trips older data forward | [Serialization](../Content/Serialization.md) |

Gates 1, 3, 9, and 10 are the ones that would decay silently. They should be red-green from the
first week they can be, rather than added once they would already fail.

## Prerequisites

- ~~Install the LLVM toolchain and add a Clang build to CI.~~ **Done 2026-09-06.** Clang
  22.1.8 locally, and CI builds MSVC and Clang in Debug and Release on every push, so
  [ADR-0003](../Architecture/Decisions/ADR-0003-cpp23-baseline.md)'s condition is met.
- ~~Install RenderDoc before Phase A3.~~ **Done 2026-09-06.** RenderDoc 1.46, vendor-neutral,
  so it can capture on the Intel UHD 730 as well as the NVIDIA card. Windows long paths are
  enabled too. Phase A3 has no outstanding prerequisites — the Vulkan SDK (1.4.357.0) and
  both devices are verified working; see [Status.md](../Status.md#hardware) for the two
  device facts that constrain the backend.

## Honest assessment of size

**M0 is large.** It is a real engine skeleton, not a weekend. The phases exist so that there is
always something running and a boundary at which to redirect.

If it needs to shrink, the honest place to cut is **Phase E**: a leaner Editor with viewport and
inspector only, dropping the content browser, still proves every architectural boundary. Cutting
Phase C would be a false economy — the asset identity, cook, and dependency layer is where the
hard design decisions live, and it is the most expensive thing to retrofit.

## What M0 does not prove

Worth stating so no claim outruns the evidence:

- **Nothing about performance.** Two or three render passes prove the graph is correct, not fast.
- **Nothing about the RHI's honesty.** An abstraction validated against one backend is not
  validated. That is what D3D12 immediately after M0 is for
  ([ADR-0012](../Architecture/Decisions/ADR-0012-backend-rollout.md)).
- **Nothing about Metal.** Designed for, unimplemented, unproven.
- **Nothing about scale.** One mesh and one texture do not exercise streaming, budgets, or large
  scenes.
