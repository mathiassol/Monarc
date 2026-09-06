# ADR-0015: Math conventions

**Status:** Accepted — 2026-09-06

## Context

Coordinate handedness, matrix storage order, vector-multiplication convention and depth
range are the classic source of subtle rendering bugs — the kind where an object is mirrored,
or lit from the wrong side, or invisible only in one projection. They are also close to
unchangeable once shaders, importers, cameras and physics all assume them.

They must therefore be written down before any of that exists, not inferred later from
whichever piece of code someone reads first.

Three things constrain the choice more than taste does:

- **glTF is the import format** ([M0](../../Milestones/M0-First-Light.md)). It is right-handed,
  Y-up, -Z forward, with column-major matrices.
- **All three target APIs use a 0..1 depth range.** Vulkan, D3D12 and Metal agree; only
  legacy OpenGL wanted -1..1. There is no conflict to resolve.
- **Slang is the shading language** ([ADR-0004](ADR-0004-slang-shading-language.md)) and can
  be told either matrix layout, so it does not force the decision.

## Decision

| Convention | Choice |
|---|---|
| Handedness | **Right-handed** |
| Up axis | **+Y** |
| Forward axis | **−Z** (so +X is right) |
| Vector convention | **Column vectors**: transform is `M * v` |
| Matrix storage | **Column-major** — `Mat4::columns[i]` is basis vector *i*, translation in `columns[3]` |
| Composition order | `A * B` applies **B first**, then A |
| Depth range | **0 to 1**, near to far |
| Angles | **Radians** in every interface; degrees only at an authoring boundary |
| Default scalar | **`f32`**, with `f64` available where precision genuinely matters |

**Vulkan's inverted clip-space Y is handled in the projection matrix, not in the world
convention.** Vulkan's normalised device coordinates put +Y downward where D3D12 and Metal
put it up. Making that a *world* convention would mean every asset, every camera and every
piece of gameplay code carried a backend detail. It belongs in exactly one function per
backend.

## Consequences

**Good.** Importing glTF needs no basis conversion at all — the format's conventions are
Monarc's, so a mesh's vertices, a node's matrix and a camera's orientation come in
unchanged. Anyone who has debugged a mirrored normal map knows what that is worth. The
column-vector `M * v` form matches the mathematical literature and glm, so external formulae
transcribe directly rather than needing transposition. A single depth convention across all
three backends removes a whole class of per-backend projection bug.

**Costs.** Right-handed with −Z forward is *not* the D3D or Unreal convention (left-handed,
Z-forward, row vectors), so code and formulae taken from those ecosystems need converting,
and developers arriving from them will be briefly surprised. Column-major storage means
`Mat4` indexing reads column-then-row, which is the opposite of how a matrix is written on
paper — a persistent minor friction, mitigated by naming the accessor `columns` so the
layout is impossible to misread as rows.

**What this does not decide.** Whether `Mat4` is SIMD-accelerated is an implementation detail
behind this interface, deliberately deferred — see
[M0](../../Milestones/M0-First-Light.md) — but the types are aligned so that it can be added
without changing their layout or their API.

## Alternatives considered

**Left-handed, Y-up, +Z forward, row vectors (the D3D/Unreal convention).** Familiar to a
large population of graphics programmers, and it makes `v * M` read left-to-right in
composition order. Rejected because every imported glTF asset would need a basis conversion,
and a conversion applied at import is a conversion that will sometimes be applied twice or
not at all. Matching the content format eliminates the failure mode rather than managing it.

**Row-major storage with column vectors.** Possible, and it makes `Mat4` indexing read the
way a matrix looks on paper. Rejected because glTF stores column-major, so the import path
would transpose every matrix — cheap, but another place for an error to hide.

**Depth −1..1.** No target API wants it. Rejected outright.

**Degrees in the public interface.** Friendlier to read in a debugger, and it invites
conversion errors at every internal boundary. Rejected: radians throughout, converted once
at the authoring layer where a human actually types a number.
