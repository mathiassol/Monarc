# ADR-0012: Backend order — Vulkan, then D3D12, then Metal

**Status:** Accepted — 2026-09-05

## Context

The vision states that designing for all three graphics backends begins immediately, while
implementing every backend at once is "a rollout choice to make intelligently".

The available facts:

- Vulkan SDK 1.4.357 is installed, with validation layers, gfxreconstruct capture, and
  SPIRV-Tools.
- The machine has an **RTX 3070 Ti and an Intel UHD 730** — two vendors and two capability
  tiers, on one desk.
- The Windows SDK ships D3D12 headers, so D3D12 needs no new hardware or SDK.
- There is **no macOS machine**, and one is expected within roughly a year.

## Decision

**Vulkan first.** Best tooling for bringing up a renderer from nothing — validation layers that
explain mistakes, and capture tools that show what actually happened. Two vendors and two tiers
available immediately, which keeps capability tiers honest rather than theoretical.

**D3D12 immediately after M0, before anything is built on top of the RHI.** This is not a
platform-coverage decision, it is a **design-validation** decision. D3D12's binding and
synchronisation models differ from Vulkan's in exactly the ways that expose a dishonest
abstraction. An RHI validated against one backend is not validated, and every week it goes
unchallenged it accumulates Vulkan assumptions that become expensive to remove. The gate is an
image comparison: the same frame on both backends, within tolerance.

**Metal when the hardware arrives.** Designed for from the start — see
[ADR-0005](ADR-0005-rhi-sync-model.md) for the barrier model that treats Metal as a
simplification, and [ADR-0004](ADR-0004-slang-shading-language.md) for shaders that already
target MSL — but honestly recorded as unimplemented and unproven until it runs.

Two disciplines keep the Metal path viable while no Mac exists:

1. **Clang builds in CI** ([ADR-0003](ADR-0003-cpp23-baseline.md)) — catches MSVC-specific code
   long before an Apple toolchain would.
2. **Platform-conditional compilation only under `Private/Platform/<Platform>/`** — no
   `#ifdef _WIN32` anywhere else in any module, enforced as gate 10.

**Amended 2026-09-08.** Discipline 2 originally read "only in `Monarc.Core/Platform`". That
was true of the tree when this was written and was never what the gate enforced: gate 10's
exemption has been the *path* `Private/Platform/<Platform>/`, in any module, since A2c, and
[ADR-0016](ADR-0016-platform-code-selection.md) states the general form. Phase A3 made the
narrow wording actively wrong — `Monarc.RHI.Vulkan` and `Monarc.Host.Windowed` both hold
genuinely platform-specific code, and neither is `Monarc.Core` — so the discipline is restated
here as the rule the gate has always applied. Nothing about the mechanism changed.

## Consequences

**Good.** The first backend has the best debugging story, which matters most when nothing works
yet. The RHI is challenged by a genuinely different API while it is still cheap to change.
Metal arrives as a backend implementation rather than a porting project. The claim "Monarc
supports three backends" is never made before it is true.

**Costs.** D3D12 immediately after M0 delays visible engine features by a chunk of work that
adds no capability a user can see — this is the price of the abstraction being real, and it is
paid deliberately. Metal remains unproven for a year, so some Metal-shaped design decisions are
educated guesses that may need revision; the RHI's cross-backend gates are what will surface
that. Vulkan 1.3 as a floor excludes older hardware.

## Alternatives considered

**D3D12 first.** Defensible on Windows — better native debugging via PIX, and the platform's
own API. Rejected because Vulkan's validation layers are more instructive during bring-up, and
because we would still need the same second-backend validation afterwards.

**All three at once.** Rejected: triples the cost of every RHI change at the moment the RHI is
least settled.

**Vulkan only until the engine is featureful, then port.** The most common plan, and the one
that produces an RHI shaped entirely around one API. Rejected — this is the specific outcome
scheduling D3D12 early is meant to prevent.

**MoltenVK for macOS instead of a Metal backend.** Ships sooner, and gives up the honest
platform integration the vision asks for while adding a translation layer we do not control.
Rejected as a destination; acceptable as a stopgap if a Mac arrives before the backend is ready.
