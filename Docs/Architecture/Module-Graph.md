# Module Graph

The authoritative list of Monarc's modules, their tiers, their kinds, and the rules the
build system enforces between them. Prose that contradicts this file is wrong.

## Module kinds

Every module declares exactly one kind. The kind determines what it may depend on, whether
it can ship in a game, and which standard-library policy applies to it.

| Kind | Ships in a game? | May depend on | Library policy |
|---|---|---|---|
| `Runtime` | Yes | `Runtime` only | Restricted subset ([ADR-0003](Decisions/ADR-0003-cpp23-baseline.md)) |
| `Tool` | No | `Runtime`, `Tool` | Full C++23 library |
| `Editor` | No | `Runtime`, `Tool`, `Editor` | Full C++23 library |
| `Test` | No | Anything | Full C++23 library |

**The rule that matters:** a `Runtime` module depending on a `Tool` or `Editor` module is a
build error, not a review comment. This is what makes the vision's "the Editor should not be
pulled into a shipped game" real rather than aspirational.

## The modules

### Tier 0 — Foundation

| Module | Kind | Depends on | Responsibility |
|---|---|---|---|
| `Monarc.Core` | Runtime | — | Platform (files, paths, time, threads, dynamic libraries), memory allocators, containers, strings, math, logging, assertions, GUIDs, `Result` |
| `Monarc.Jobs` | Runtime | `Core` | Task scheduler: jobs, dependency graph, priorities, affinity, instrumentation |

`Monarc.Core` is the only module permitted to contain platform-conditional code. No
`#ifdef _WIN32` appears anywhere else in the engine — that discipline is what keeps macOS
and future platforms reachable ([ADR-0012](Decisions/ADR-0012-backend-rollout.md)).

### Tier 1 — Data

| Module | Kind | Depends on | Responsibility |
|---|---|---|---|
| `Monarc.Reflect` | Runtime | `Core` | Type registry: types, properties, attributes, enums |
| `Monarc.Serialize` | Runtime | `Core`, `Reflect` | Text and binary formats, schemas, versioning, migration |
| `Monarc.Assets` | Runtime | `Core`, `Jobs`, `Reflect`, `Serialize` | Asset identity, typed handles, load/unload/reload, dependency records |

### Tier 2 — Rendering

**These modules depend on each other and on Tier 0 — and on nothing else.** No Tier 1 or
Tier 3 dependency is permitted. See
[ADR-0007](Decisions/ADR-0007-renderer-package-boundary.md).

| Module | Kind | Depends on | Responsibility |
|---|---|---|---|
| `Monarc.RHI` | Runtime | `Core` | Abstract device, capability tiers, resources, command recording, barriers, queues, swapchain |
| `Monarc.RHI.Vulkan` | Runtime | `Core`, `RHI` | Vulkan 1.3 backend |
| `Monarc.RHI.D3D12` | Runtime | `Core`, `RHI` | D3D12 backend (M1) |
| `Monarc.RHI.Metal` | Runtime | `Core`, `RHI` | Metal backend (M2) |
| `Monarc.Shaders` | Runtime | `Core`, `RHI` | Runtime shader objects, binding layouts, pipeline cache |
| `Monarc.Render` | Runtime | `Core`, `Jobs`, `RHI`, `Shaders` | Render graph, passes, materials, views, `RenderScene` |

Backends are selected at runtime from what is linked. A build may include one, several, or
none of them — `Monarc.Host.Headless` includes none.

### Tier 3 — Runtime

| Module | Kind | Depends on | Responsibility |
|---|---|---|---|
| `Monarc.World` | Runtime | `Core`, `Jobs`, `Reflect`, `Serialize`, `Assets` | Actors, Components, transforms, relationships, world kinds, tick phases |
| `Monarc.Engine` | Runtime | `World`, `Render`, `Assets`, `Jobs` | Subsystem lifecycle, frame loop, input, time, **render extraction** |
| `Monarc.Host.Windowed` | Runtime | `Engine`, `Render` | Window, surface, input event loop |
| `Monarc.Host.Headless` | Runtime | `Engine` | Headless/dedicated-server host. **Links no Tier 2 module** |

`Monarc.Engine` is the only module that sees both `Monarc.World` and `Monarc.Render`. It is
the adapter, and that is deliberate — it is where extraction lives
([Frame-Model.md](../Runtime/Frame-Model.md)).

### Tier 4 — Tools

| Module | Kind | Depends on | Responsibility |
|---|---|---|---|
| `Monarc.ShaderCompiler` | Tool | `Core`, `Shaders` | Slang driver, reflection to binding layouts, shader cooking |
| `Monarc.Cook` | Tool | `Core`, `Jobs`, `Assets`, `Serialize` | Importers, intermediate representation, processing, platform cooking, cache |
| `Monarc.Build` | Tool | `Core`, `Cook`, `Assets` | Packaging, dependency walking, export, `explain` |
| `Monarc.Editor` | Editor | Everything above | The authoring environment |
| `Monarc.Hub` | Editor | `Core`, `Build` | Engine versions and projects |

### Game modules

| Module | Kind | Depends on | Responsibility |
|---|---|---|---|
| `Game.<Name>` | Runtime | Tier 0–3 `Runtime` modules | Game code |

A game module cannot reference `Monarc.Editor` or `Monarc.Cook` even by accident: the kind
rule forbids it.

## Declaring a module

Module metadata is declarative and queryable, not buried in build logic:

```cmake
monarc_module(
    NAME         Monarc.Render
    KIND         Runtime
    TIER         2
    PUBLIC_DEPS  Monarc.Core Monarc.RHI Monarc.Shaders
    PRIVATE_DEPS Monarc.Jobs
)
```

CMake is the backend — it gives us the package ecosystem, IDE integration, and the Xcode
generator we will need for Metal. The metadata above is *data*, which means the build can
validate it and the exporter can walk it. See
[ADR-0011](Decisions/ADR-0011-build-system.md).

The build emits `module-graph.json` as an artifact. That file is what
[`monarc explain`](../Product/Build-And-Export.md) reads to answer why something is in an
export.

## Enforced rules

These are build failures or test failures, not conventions:

1. **Acyclic.** The module graph has no cycles.
2. **Downward only.** A module may not depend on a higher tier.
3. **Kind containment.** A `Runtime` module may not depend on a `Tool` or `Editor` module.
4. **Renderer package boundary.** No Tier 2 translation unit may include a header from
   `Monarc.World` or `Monarc.Assets`.
5. **Headless purity.** The headless host binary contains no RHI or graphics-API symbols.
6. **Export purity.** A shipped game binary contains no `Monarc::Editor` or `Monarc::Cook`
   symbols.
7. **Platform containment.** Platform-conditional compilation appears only in
   `Monarc.Core/Platform`.

Rules 4, 5, 6, and 7 are the ones that would decay silently without automation, so they are
gates in [M0](../Milestones/M0-First-Light.md#verification-gates) rather than later additions.

## Third-party dependencies

Monarc owns everything that defines its character and performance: math, containers, memory,
jobs, windowing and input, the RHI, the render graph, the asset pipeline, serialization, and
the world model. Third-party code is confined to deep specialist domains, always behind a
Monarc-owned interface so it can be replaced.

| Domain | Intended library | Status |
|---|---|---|
| Shader compilation | Slang | Chosen ([ADR-0004](Decisions/ADR-0004-slang-shading-language.md)) |
| Editor UI scaffolding | Dear ImGui | Chosen ([ADR-0013](Decisions/ADR-0013-editor-ui.md)) |
| Physics | Jolt | Later; not in M0 |
| Image and font decoding | To be chosen | Later |
| Mesh optimisation | meshoptimizer | Later |
| Audio | To be chosen | Later |

See [ADR-0014](Decisions/ADR-0014-dependency-policy.md) for the policy and its boundaries.
