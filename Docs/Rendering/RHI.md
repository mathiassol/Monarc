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

**Implemented whole in Phase A3 Task 3** (`Monarc.RHI/Include/Monarc/RHI/Barrier.h`): fifteen
pipeline stages, eighteen accesses and eight texture layouts, every one translated in both
directions and tested with no device present, though A3 records three barriers. The membership
rule for those lists is *not* "what A3 uses" — it is Vulkan 1.3 **core** only, and only stages
Monarc has a plan for. So `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` arrives with the swapchain rather
than here, and the ray-tracing and mesh-shading stages arrive with a phase that schedules them.
This is the opposite of the rule `Format` in `Types.h` states for itself, and deliberately: a
format costs a size claim nothing verifies, where a stage costs one row in one switch.

**A texture barrier without its layout pair does not compile.** `GlobalBarrier` and
`BufferBarrier` are aggregates whose fields all default to `None` — so `GlobalBarrier{}` is the
barrier that changes nothing, which a render graph will produce for a pass with no state to
change, and which the backend records rather than dropping. `TextureBarrier` is not an
aggregate and has no default constructor: its constructor takes all seven fields with no
defaults.

That asymmetry is forced rather than chosen. `TextureLayout::Undefined` is a *legitimate*
before-layout — it is what a freshly created texture is in — so a barrier whose layouts had
been left to a zero-initialised default is byte-for-byte identical to one whose author meant
`Undefined`, and there is nothing left for a run-time check to look at. Distinct `enum class`
types with no implicit conversion between them are the other half: seven arguments with the
stages shifted up into the layouts' places is also a compile error, which is the omission a
count-based check would miss. All four ways of omitting the pair were tried on both compilers;
the diagnostics are in [Status.md](../Status.md#a3-task-3-delivered).

Split barriers are the one part of the model not yet expressed. Both APIs write one as a pair
of halves sharing an identity, and nothing in Monarc issues the first half yet: A3 records
commands directly, and A4's render graph is what will have a reason to overlap a transition with
unrelated work.

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
ordinary `Result` a caller can act on, which is what a shipped game needs and what
[`Monarc.Host.Headless`](../Architecture/Module-Graph.md) needs in order to link no graphics API
at all.

Two things make that a property rather than a discipline:

- **`VK_NO_PROTOTYPES`** is defined on `Monarc.RHI.Vulkan`, so the headers declare no global
  `vkCreateInstance`. A direct call does not compile, rather than compiling and inviting
  someone to "fix" the link by adding the import library.
- **Only the `Vulkan::Headers` interface target is linked**, from a pinned `FetchContent` tag
  rather than `find_package(Vulkan)`. A CI runner with no SDK still configures and builds — see
  [ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md).

### The lifecycle is a factory, and the messages are literals

`Detail::Loader::Open` and `VulkanBackend::Create` are static functions returning a
`Result<T>`, the same shape `Platform::Library::Open` and `Host::Window::Create` use. Teardown
(`Close`, `Shutdown`) is safe to call unconditionally and repeatedly, and bringing a loader or
a backend back up means calling the factory again rather than re-initialising the object.
`VulkanBackend` allocates its state inside `Create`, before anything is constructed, so a
backend with no state is not a thing a caller can hold.

`Error::message` is a non-owning view over storage that must outlive the error, and a factory
that returns *no object* on failure has nowhere to point but static storage. So every failure
message here is a string literal: the name of the missing entry point (from the X-macro's
`#name`), the name of the missing instance extension (`VK_KHR_SURFACE_EXTENSION_NAME` and the
platform's own are both literals), the `VkResult`'s spelling (`Detail::ToString` returns a
pointer to one), and a bare sentence for the library that would not open — matching
`Platform::Library::Open`'s own `"could not load library"`, since the library name is
caller-supplied and the caller already has it.

Composition goes to `MONARC_LOG` at the failure site instead: which library, which operation,
the numeric `VkResult` alongside its spelling, and the version actually reported. **Nothing in
Monarc branches on message text** — the `ErrorCode` carries what is branchable and the log
carries what a human reads.

A note on how thin the safety net is here. A message that *did* dangle — re-pointed at a
buffer inside the backend's state and read after that state was freed — produced no
`clang-asan` report at all when it was tried: the freed bytes were still there and the read
went unnoticed. The tests therefore compare the whole message text rather than searching it,
because that comparison is the only thing that catches it.

## The device, its queue, and its command lists

`IDevice`, `IQueue` and `ICommandList` (`Monarc.RHI/Include/Monarc/RHI/Device.h`, Phase A3
Task 3) are abstract interfaces, and they are the ones [`Monarc.Render`](Render-Graph.md)
consumes in Phase B rather than a step towards them: a call site holding an `IDevice&` should
not have to change.

**There is no `IBackend` yet, and no owning handle for an `IDevice`.**
`VulkanBackend::CreateDevice` returns a concrete `Result<VulkanDevice>` — polymorphic *use*
without polymorphic *ownership*. Two reasons, and the first is the load-bearing one: an
`IBackend`'s only justification for being virtual is runtime backend selection, and there is
one backend. The second is that `Monarc.Core` has no owning-pointer type and designing one
properly is its own piece of work — `IAllocator::Deallocate` needs the size and alignment of
what it frees, and for a polymorphic type those are the derived class's rather than
`sizeof(Base)`, so the deleter has to carry values captured where the object was made.

Both arrive with the second backend, which
[ADR-0012](../Architecture/Decisions/ADR-0012-backend-rollout.md) schedules immediately after
M0. **The three interfaces do not change when they do**; what changes is the backend's own
factory shape. That is a stated seam rather than an unstated one, and the headers say so.

**Rendering is Vulkan 1.3 dynamic rendering, and there is no `VkRenderPass` or `VkFramebuffer`
anywhere in the backend.** `ICommandList::BeginRendering` names attachments at record time, so
nothing is created ahead of a frame, nothing has to be invalidated when a texture is recreated,
and there are no pipeline/pass compatibility rules to encode. D3D12 and Metal already work this
way, so this is the model all three share rather than Vulkan's older shape emulated on the
other two. `grep -rni "renderpass\|framebuffer\|render pass" Source/` matches four lines, all
comments.

**There is no clear command.** A clear is a `LoadOp::Clear` on an attachment, because that is
the path the swapchain clear and the render graph both take — a separate clear entry point
would be a second code path with no shipped caller, and a test of it would read as coverage
while the real path stayed unexercised.

**Frame completion is a timeline semaphore, and there are no binary semaphores.**
`IQueue::Submit` returns the value its submission will signal; `IDevice::BeginFrame` waits on
the value its frame slot's previous submission signalled before resetting that slot's command
pool, which is what makes the reset legal — resetting a pool whose buffers are still executing
is undefined behaviour. Two frames in flight, one command pool each. Binary semaphores arrive
with presentation, because `vkQueuePresentKHR` accepts only those.

**One `Begin`, one `End` and one `Submit` per `BeginFrame`**, and a second of any of them is
refused with `ErrorCode::InvalidArgument` rather than attempted: only `BeginFrame` resets the
pool, so only `BeginFrame` makes a list recordable and submittable again — waiting for the work
to finish does not. A frame loop that comes round without calling it gets a `Status` and not
undefined behaviour, in every configuration and not only where a validation layer is loaded.
`ICommandList` in `Monarc/RHI/Device.h` lists the states each call refuses.

**Resources are handles from device-owned, generation-checked, fixed-capacity pools.** Capacity
comes from a `DeviceConfig` at device creation and is never grown, which is
[`JobSystem`](../Runtime/Threading.md)'s discipline and is here for the same reason: a pool that
reallocated would move a slot out from under every handle naming it. A full pool reports
`ErrorCode::OutOfMemory`; a stale handle resolves to a failure and never to the slot's new
occupant ([ADR-0002](../Architecture/Decisions/ADR-0002-handles-not-pointers.md)).

### GPU memory is one allocation per resource, and that is a placeholder

A3 allocates a `VkDeviceMemory` per texture and per buffer. It is honest for a phase that
creates one of each, and it will not survive the first scene with real assets: drivers cap
`maxMemoryAllocationCount` (4096 on much hardware), each allocation has real cost, and nothing
sub-allocates or pools.

What makes it safe to ship is the shape of the interface rather than the strategy behind it.
`CreateTexture` takes a `TextureDescription` and returns a handle, so a sub-allocating allocator
replaces one function's body without touching a call site. What would have been wrong is an
interface that never took a description.
[ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md)'s dependency table has no
VMA row, and adding one is a decision for whoever needs the second allocation strategy.

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
  the two local devices; mesh shading and ray tracing do. Stopping the table at `Bindless` —
  which is where it would have ended without this measurement — would have left the tier query
  unable to tell an Ampere card from an integrated Xe-LP part, making the whole mechanism
  decorative on the one machine that can exercise it.

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

The type qualifier is an **accepted narrowing**, not a free one. A finding the layer chose to
type GENERAL rather than VALIDATION would be logged at Error and would not stop the process,
indistinguishable here from the overlay-layer diagnostics. Khronos types its findings
VALIDATION, so the risk is small against a certain cost: keying on severity alone would abort
on every one of the several GENERAL loader errors a machine with implicit overlay layers emits
per process start, which is a fatal messenger that cannot be run at all. Nothing tests this
edge, because whether a finding is typed GENERAL is the layer's choice and not Monarc's to
provoke.

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
