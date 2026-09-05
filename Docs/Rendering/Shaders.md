# Shaders

Monarc's shading language is **Slang**
([ADR-0004](../Architecture/Decisions/ADR-0004-slang-shading-language.md)). Shader work is split
across the Tool/Runtime pair described in
[Overview.md](../Architecture/Overview.md#runtime-and-tool-come-in-pairs):

| Module | Kind | Responsibility |
|---|---|---|
| `Monarc.ShaderCompiler` | Tool | Drives Slang, extracts reflection, writes cooked shader artifacts |
| `Monarc.Shaders` | Runtime | Loads cooked artifacts, creates pipelines, caches them |

A shipped game contains only `Monarc.Shaders`. It cannot compile a shader, and does not need to.

## The pipeline

```
.slang source
   -> Monarc.ShaderCompiler (Slang)
      -> target binary   (SPIR-V | DXIL | MSL)
      -> reflection data (binding layout, parameter blocks, entry points)
   -> cooked shader artifact  [versioned format]
      -> Monarc.Shaders at runtime
         -> pipeline object (cached by hash)
```

Reflection is extracted at cook time and **stored in the artifact**. The runtime never needs
the Slang compiler to know how to bind a shader's parameters, which is what keeps the compiler
out of shipped games.

## Parameter passing

Shader parameters are declared with Slang's `ParameterBlock`, which maps to each backend's
natural mechanism without per-backend source:

| Target | `ParameterBlock` becomes |
|---|---|
| Vulkan | Descriptor set |
| D3D12 | Descriptor table |
| Metal | Argument buffer |

This is the single most valuable property Slang gives us: one parameter model across three
backends, rather than three binding conventions to keep in agreement.

**Binding layouts are generated from reflection, never written by hand.** No root signatures or
descriptor set layouts are maintained manually, which removes the entire class of bug where
shader source and binding declaration drift apart.

## Caching

Two caches, keyed by content:

- **Cooked shader cache** (tool side) — keyed like any other asset
  ([ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md)): source content, compile
  options, Slang version, target, engine version. Recooking with nothing changed does no work.
- **Pipeline cache** (runtime side) — keyed by the hash of the shader artifact plus pipeline
  state, and backed by the backend's own pipeline cache where one exists.

## Known limitations of the Metal target

Verified from Slang's documentation, and recorded here because they constrain later milestones
rather than M0:

- `RaytracingAccelerationStructure` is **not supported** — ray tracing on Metal is not currently
  possible through Slang.
- Conservative rasterisation is unavailable; `SV_InnerCoverage` is always false.
- Some system-value semantics are unsupported (`SV_FragInvocationCount`, `SV_FragSize`).
- `SubpassInput` is fragment-only and cannot be nested in a `ParameterBlock`.

Rasterisation and compute — everything M0 and M1 need — are viable.

## Scope in M0

One shader: static mesh with a base-colour texture. Compiled to SPIR-V, with reflection driving
binding layout, loaded through the cooked artifact format, and cached.

Deferred: shader variants and permutations, hot reload, shader graph or material authoring,
include and module libraries beyond what one shader needs.

**Hot reload is deliberately deferred but shapes the design.** Because `Monarc.Shaders` loads a
versioned artifact and caches pipelines by hash, hot reload later means invalidating a cache
entry and recompiling in the Editor — not restructuring the shader system. The Editor already
links `Monarc.ShaderCompiler`, so nothing new needs to be linked to make it work.
