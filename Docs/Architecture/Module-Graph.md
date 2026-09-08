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

`Monarc.Core` was the only module with platform code until Phase A3, and the rule was written
as though that were the rule. It is not: **platform-conditional compilation is confined by
*directory*, in every module** — a `#ifdef _WIN32` is legitimate only under
`Private/Platform/<Platform>/`, and nowhere else in any module, `Monarc.Core` included. That is
what keeps macOS and future platforms reachable
([ADR-0012](Decisions/ADR-0012-backend-rollout.md);
[ADR-0016](Decisions/ADR-0016-platform-code-selection.md)), and A3 is what made the broader
statement necessary rather than merely more accurate: `Monarc.RHI.Vulkan` and
`Monarc.Host.Windowed` both contain genuinely platform-specific code and neither is
`Monarc.Core`. Gate 10 has enforced the directory rule since A2c; its *name* said
"Core/Platform" until A3 Task 5 corrected it.

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
| `Monarc.Host.Windowed` | Runtime | `RHI`, `Engine`, `Render` | Window, surface, input event loop |
| `Monarc.Host.Headless` | Runtime | `Engine` | Headless/dedicated-server host. **Links no Tier 2 module** |

`Monarc.Host.Windowed` is declared with `Core` + `RHI` deps today, because `Engine` and
`Render` do not exist yet. The row above is its destination, not its present state; the tier
is the same either way, and the entry that adds those two edges is Phase D's.

The `RHI` edge in that row is deliberate and permanent, not a leftover. It changes nothing
about what is reachable — `Monarc.Render` depends on `Monarc.RHI` *publicly*, so a windowed
host sees RHI headers transitively whether or not it says so — and a window that must
recreate a swapchain when the OS resizes it is RHI-aware by nature. Declaring the edge states
a dependency that already exists rather than inheriting one silently. It costs headless
nothing: `Monarc.Host.Headless` does not depend on `Monarc.Host.Windowed`, so rule 5 is
untouched.

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

### Apps

An **app** is an executable that is a node in the module graph rather than an exception to
it: same kind, same tier, same dependency rules, same `module-graph.json` entry, marked
`"app": true`. It is declared with `monarc_app()` instead of `monarc_module()`.

Two things follow from an app being a link target rather than an interface, and both are
enforced:

- **Nothing may depend on an app** — rule 8 below. An app is where a dependency chain ends,
  not something a chain passes through, and depending on one would mean linking a second
  `main()`.
- **An app has no `Include/` directory** — rule 9 below, and the layout gate *forbids* one
  rather than merely excusing its absence. An app exports nothing, so there is no public
  header for anyone to include. Merely exempting it would leave a trap: `monarc_app()` does
  not glob an app's `Include/`, so nothing in the build would ever read a header placed
  there, while the gates that walk a module's sources went on policing it — they scan
  `Include/` for every module, app or not. A file simultaneously governed and dead.

| App | Kind | Tier | Depends on | Responsibility |
|---|---|---|---|---|
| `Monarc.FirstLight` | Runtime | 3 | `Core`, `RHI`, `RHI.Vulkan`, `Host.Windowed` | A window that clears to a known colour: M0's first-light proof |

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

An executable is declared the same way, with `monarc_app()`:

```cmake
monarc_app(
    NAME         Monarc.FirstLight
    KIND         Runtime
    TIER         3
    PRIVATE_DEPS Monarc.Core Monarc.RHI Monarc.RHI.Vulkan Monarc.Host.Windowed
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
4. **Renderer package boundary.** No Tier 2 translation unit may include a header from a
   Tier 1 or Tier 3 module — today `Monarc.World`, `Monarc.Assets` and
   `Monarc.Host.Windowed`.
5. **Headless purity.** The headless host binary contains no RHI or graphics-API symbols.
6. **Export purity.** A shipped game binary contains no `Monarc::Editor` or `Monarc::Cook`
   symbols.
7. **Platform containment.** Platform-conditional compilation appears only under a module's
   `Private/Platform/<Platform>/` directory — in **any** module, not only `Monarc.Core`, and
   not in a public header or in a platform-neutral file sitting beside those directories.
8. **Apps are leaves.** No module may list an app in `PUBLIC_DEPS` or `PRIVATE_DEPS`.
9. **Layout.** Every module has a `Private/` directory; a library has an `Include/`
   directory and an app does not.

All of these would decay silently without automation, and all but rule 2 have a numbered gate
in [M0](../Milestones/M0-First-Light.md#verification-gates): rule 6 is gate 1, rules 1 and 3
share gate 2, rule 4 is gate 3, rule 5 is gate 9, rule 7 is gate 10, rule 8 is gate 12 and
rule 9 is gate 13. Rule 2 is the exception because CMake refuses it at configure time, which
is earlier than a gate could catch it.

The two mechanisms divide the rules as follows. `monarc_validate_modules()` refuses rules 2,
3 and 8 at configure time, which is where a developer wants to hear about them.
`Tools/check_architecture.py` checks rules 1, 4, 7, 8 and 9 against the emitted
`module-graph.json` and the source tree, and runs under `ctest`.

**Rule 8 is the one checked in both places, deliberately.** `module-graph.json` is the
product [`monarc explain`](../Product/Build-And-Export.md) reads, so a stale or hand-edited
graph would otherwise answer "why is this in the export" from a shape CMake never approved.
Rule 1 goes the other way: CMake does no cycle detection at all, so the gate is the only
thing enforcing it.

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
