# RHI — Render Hardware Interface

`Monarc.RHI` is the boundary between Monarc's rendering and the platform graphics APIs. Its
public headers contain no Vulkan, D3D12, or Metal types. Backends live in
`Monarc.RHI.{Vulkan,D3D12,Metal}` and are selected at runtime from whatever is linked.

Design decisions: [ADR-0005](../Architecture/Decisions/ADR-0005-rhi-sync-model.md) (barrier
model), [ADR-0012](../Architecture/Decisions/ADR-0012-backend-rollout.md) (rollout order).

## What the RHI is and is not

**It is** a thin, explicit, modern abstraction — device and queues, resources, command
recording, barriers, and presentation. It exposes the shape of the underlying APIs rather than
hiding it.

**It is not** a renderer. It has no concept of a mesh, a material, a camera, or a frame. Those
live in [`Monarc.Render`](Render-Graph.md). If a concept only makes sense to a game, it does not
belong here.

**It is not** a lowest common denominator. Where the APIs genuinely overlap, behaviour is
shared. Where hardware differs, Monarc uses capability tiers. Where one API offers something
unique, there is an extension path rather than a pretence of uniformity.

## Barriers and synchronisation

The barrier model is taken from where D3D12 Enhanced Barriers and Vulkan `synchronization2`
have converged. A barrier carries:

| Field | Meaning |
|---|---|
| `syncBefore`, `syncAfter` | Pipeline stages being synchronised |
| `accessBefore`, `accessAfter` | How memory was and will be accessed |
| `layoutBefore`, `layoutAfter` | Texture layout transition (textures only) |

with buffer, texture, and global variants, and support for split barriers.

Backend mapping:

| Backend | How barriers are expressed |
|---|---|
| Vulkan | `vkCmdPipelineBarrier2` — near one-to-one |
| D3D12 | `Barrier()` with Enhanced Barriers — near one-to-one |
| Metal | Mostly nothing within an encoder; `MTLFence` / `MTLEvent` across encoders |

Metal is a **reduction** of this model, which is the tractable direction. Note the consequence:
the Metal backend discards information the type carries. That is accepted, and is why the model
is shaped from the two APIs that need the detail.

**Application and feature code never writes barriers.** The
[render graph](Render-Graph.md) derives them from declared reads and writes.

## Capability tiers

Hardware genuinely differs, and pretending otherwise produces either slow code everywhere or
broken code somewhere. Monarc queries capabilities and groups them into tiers, so features can
ask a meaningful question rather than probing a dozen booleans.

The development machine deliberately spans two tiers — an **RTX 3070 Ti** and an **Intel UHD
730** — so tier handling is exercised continuously rather than reasoned about.

Tiers cover at least: bindless and descriptor indexing limits, timeline semaphore support,
mesh shading, ray tracing, and available queue families. A feature that requires a tier the
device lacks must degrade or refuse explicitly; silently producing different results across
tiers is the failure mode to avoid.

## Resource binding

Binding follows Slang's `ParameterBlock` model
([ADR-0004](../Architecture/Decisions/ADR-0004-slang-shading-language.md)), which maps to a
descriptor set on Vulkan, a descriptor table on D3D12, and an argument buffer on Metal.

Binding layouts are **generated from shader reflection**, never hand-written. There are no root
signatures or descriptor set layouts maintained by hand, and therefore no category of bug where
shader source and binding declaration drift apart.

Design is **bindless-first** where the capability tier allows: a large descriptor heap indexed
by handle, rather than per-draw descriptor binding. This is the natural model on D3D12 and
Metal, and available on Vulkan via descriptor indexing (core since 1.2). Lower tiers fall back
to conventional binding behind the same interface.

## Vulkan baseline

**Vulkan 1.3 core**, which provides dynamic rendering, `synchronization2`, timeline semaphores,
and descriptor indexing (from 1.2). Vulkan 1.4 features are used opportunistically where
present — push descriptors, `maintenance5` and `maintenance6`, and host image copy are all core
in 1.4.

## Scope in M0

Device and adapter enumeration with capability queries; buffers, textures, samplers, shader
modules and pipelines; command lists with the barrier API above; queues and timeline
synchronisation; swapchain and presentation; a bindless descriptor heap. Vulkan backend only.

Deferred: D3D12 and Metal backends, ray tracing, mesh shaders, multiple queues in anger, device
loss recovery.

## Things that belong here but arrive later

The vision lists these as part of the rendering foundation, and they are noted so that later
work has somewhere natural to belong: device loss and recovery, frame pacing, colour space and
HDR output, GPU memory budgeting and residency, validation integration, and capture and debug
tooling hooks.
