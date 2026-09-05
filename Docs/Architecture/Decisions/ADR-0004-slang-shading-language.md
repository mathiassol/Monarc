# ADR-0004: Slang as the shading language

**Status:** Accepted — 2026-09-05

## Context

Monarc targets Vulkan, D3D12, and Metal. Shader source must reach SPIR-V, DXIL, and Metal
Shading Language. The vision also requires shader reflection, resource binding conventions,
and pipeline and shader caches as first-class parts of the rendering foundation.

The conventional path is HLSL compiled by DXC to DXIL and SPIR-V, then SPIRV-Cross to
translate SPIR-V into MSL. That is a lossy translation chain, and it is lossy precisely at
the target we can least afford to treat casually.

Slang 2026.13.1 is already installed as part of Vulkan SDK 1.4.357 and has been adopted by
Khronos.

## Decision

**Slang is Monarc's shading language.** `Monarc.ShaderCompiler` drives it at cook time,
producing per-target binary artifacts plus reflection metadata that becomes the binding
layout consumed by `Monarc.Shaders` at runtime.

Shader parameters are declared using Slang's `ParameterBlock`, which maps to the natural
mechanism on each backend without per-backend source:

| Target | `ParameterBlock` becomes |
|---|---|
| Vulkan | Descriptor set |
| D3D12 | Descriptor table |
| Metal | Argument buffer |

Binding layouts are **generated from reflection**, never hand-written. There are no
hand-maintained root signatures or descriptor set layouts to drift out of sync with shader
source.

## Consequences

**Good.** One shader source reaches all three backends without a translation chain. Slang's
modules, generics and interfaces make a shader library composable rather than a pile of
`#include` and `#define`. The reflection API removes an entire category of
binding-mismatch bug. Metal support is a compiler target rather than a porting project.

**Costs.** A dependency on a compiler we do not control, in the most central position in the
rendering pipeline. Slang's Metal target is documented as a work in progress — verified
limitations include no `RaytracingAccelerationStructure`, no conservative rasterisation, and
some unsupported system-value semantics. Rasterisation and compute, which is all M0 and M1
need, are viable. Ray tracing on Metal is not, and that constrains a much later milestone.

**Mitigation.** `Monarc.ShaderCompiler` is a `Tool` module and the cooked shader artifact is
a versioned format. If Slang ever has to be replaced, the change is confined to one tool
module and a recook — not to shader source scattered across the engine.

## Alternatives considered

**HLSL via DXC, plus SPIRV-Cross for Metal.** The conventional choice. Rejected: the Metal
path is a lossy transpile, and reflection would have to be assembled from two sources.

**GLSL via glslang.** Poor D3D12 and Metal story. Rejected.

**Write our own shading language.** Contrary to [ADR-0014](ADR-0014-dependency-policy.md) —
this is a deep specialist domain, and Khronos has adopted a good answer.
