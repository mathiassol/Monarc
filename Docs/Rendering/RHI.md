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

## How the Vulkan runtime is loaded

**Monarc opens `vulkan-1.dll` itself, through `Platform::Library`, and resolves every entry
point through `vkGetInstanceProcAddr`.** No `volk`, and no `vulkan-1.lib` import library
anywhere in the build. Implemented in Phase A3 Task 2 (`Monarc.RHI.Vulkan/Private/Loader.h`).

The reason is not purity. Linking the import library makes a missing Vulkan runtime a *Windows
loader failure* — the process dies before `main` with a system dialog naming a DLL, which is
useless to a player and unrecoverable by us. Loading it ourselves makes the same situation an
ordinary `Result` with a message naming the library, which is what a shipped game needs and
what [`Monarc.Host.Headless`](../Architecture/Module-Graph.md) needs in order to link no
graphics API at all.

Two things make that a property rather than a discipline:

- **`VK_NO_PROTOTYPES`** is defined on `Monarc.RHI.Vulkan`, so the headers declare no global
  `vkCreateInstance`. A direct call does not compile, rather than compiling and inviting
  someone to "fix" the link by adding the import library.
- **Only the `Vulkan::Headers` interface target is linked**, from a pinned `FetchContent` tag
  rather than `find_package(Vulkan)`. A CI runner with no SDK still configures and builds — see
  [ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md).

## Adapter identity is the device UUID, and only that

**Adapters are deduplicated on `VkPhysicalDeviceIDProperties::deviceUUID`.** It is core since
Vulkan 1.1 and is the identity the spec actually guarantees; a device *name* is not unique, and
PCI bus information comes from an extension a driver need not implement.
`DeduplicateAdapters` is a pure function over a mutable span — order-preserving, keeping the
first of each UUID, allocating nothing — so it is tested in CI with no device present at all,
and the real enumeration path is a thin wrapper over it.

This is not a precaution. `vkEnumeratePhysicalDevices` returns **five** physical devices on
the development machine for **two** GPUs: the Intel part appears four times, once per virtual
display adapter installed on that machine, all four reporting one `deviceUUID`. Without
deduplication the adapter list a player sees has four identical rows in it. Measured, with
both lists captured, in [Status.md](../Status.md#hardware).

## Capability tiers

Hardware genuinely differs, and pretending otherwise produces either slow code everywhere or
broken code somewhere. Monarc queries capabilities and groups them into tiers, so features can
ask a meaningful question rather than probing a dozen booleans.

The development machine deliberately spans two tiers — an **RTX 3070 Ti** and an **Intel UHD
730** — so tier handling is exercised continuously rather than reasoned about.

The ladder as A3 Task 2 built it, least to most capable:

| Tier | Requires |
|---|---|
| `Unsupported` | nothing; the floor every device meets |
| `Baseline` | Vulkan 1.3 core — dynamic rendering, `synchronization2`, timeline semaphores — and a graphics queue family |
| `Bindless` | `Baseline`, plus descriptor indexing in the shape a heap needs: non-uniform indexing, a runtime-sized array, partially bound descriptors, and enough of them |
| `Advanced` | `Bindless`, plus mesh shading and ray tracing |

`MeetsTier(capabilities, tier)` is defined as `DetermineTier(capabilities) >= tier` rather than
the other way round, so monotonicity is structural: reaching a tier means having fallen through
the tests for every tier below it, and "meets a tier without meeting the one below" is
unrepresentable rather than merely avoided.

**Two things in that table were decided by measurement rather than by design**, and both
changed the shape:

- **Device type is not a tier input**, though the phase plan listed it as one. The Intel UHD
  730 reports every descriptor-indexing feature and limit the RTX 3070 Ti does — both at
  `maxDescriptorSetUpdateAfterBindSampledImages = 1048576` — so which side of a PCIe bus a
  device sits on predicts nothing about the tiers Monarc defines. It stays a fact on
  `AdapterInfo` rather than a requirement.
- **`Advanced` exists because of that same measurement.** Descriptor indexing does not separate
  the two local devices; mesh shading and ray tracing do. Without a third rung the tier query
  could not tell an Ampere card from an integrated Xe-LP part, which would have made the whole
  mechanism decorative on the one machine that can exercise it.

A tier above `Advanced` arrives with the first feature that needs one. A feature that requires
a tier the device lacks must degrade or refuse explicitly; silently producing different results
across tiers is the failure mode to avoid.

## Validation is fatal in Debug, and that is the point

A validation layer whose output scrolls past is worse than no validation, because it produces
false confidence. The `VK_EXT_debug_utils` messenger logs every severity it is given and, for
an ERROR-severity message of VALIDATION type, stops the process — `MONARC_CHECK` with the
message, then `MONARC_DEBUG_BREAK()` and `std::abort()`, exactly as
[`JobSystem::Wait`](../Runtime/Threading.md#wait-is-for-the-owning-thread-not-a-worker) does
and for the same reason: a check that only reports does not change what the program does.

Loader diagnostics are marked GENERAL rather than VALIDATION and are logged without stopping
anything — an implicit overlay layer built against an older API version is a fact about a
machine, not a bug in Monarc. A validation finding is a bug in Monarc's use of Vulkan, every
time.

The messenger's create-info is also chained into `VkInstanceCreateInfo::pNext`, and that turns
out to be load-bearing rather than good practice. Measured with a deliberately invalid
`VkApplicationInfo::sType`: with the chain, the error reaches the callback and the process
stops. Without it, the layer prints the same error to stderr on its own, `vkCreateInstance`
*succeeds*, and the process exits zero.

Release builds create no messenger and load no layers. Asking for validation is not getting it,
either: a machine with no Vulkan SDK has neither the layer nor the extension, and that is a
warning and a degraded instance rather than a failure. The backend reports which of the two
actually happened, so a build that quietly failed to load the layer is distinguishable from one
that never asked.

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
and descriptor indexing (from 1.2). The instance is created at `VK_API_VERSION_1_3`, and 1.3 is
the floor every device must meet. Vulkan 1.4 features are used opportunistically where
present — push descriptors, `maintenance5` and `maintenance6`, and host image copy are all core
in 1.4.

1.3 rather than 1.4 is measured, not conservative: the development machine's Intel UHD 730
reports 1.3.275 while its RTX 3070 Ti reports 1.4.351, so requiring 1.4 would silently drop the
lower-tier device out of the test matrix — which is the entire reason that device is valuable.
The *loader* reports 1.4.357 on that machine, and the two numbers are deliberately kept apart:
it is the device's version that decides what may be called on it.

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
