# RHI — Render Hardware Interface

`Monarc.RHI` is the boundary between Monarc's rendering and the platform graphics APIs. Its
public headers contain no Vulkan, D3D12, or Metal types. Backends live in
`Monarc.RHI.{Vulkan,D3D12,Metal}` and are selected at runtime from whatever is linked.

**That first claim is structural rather than a discipline, as of Phase A3, and it is
checkable.** `Monarc.RHI.Vulkan` links only `Vulkan::Headers` and links it `PRIVATE`, so a
consumer of the backend's public header is never handed a Vulkan declaration. Every `Vk…` or
`VK_…` token under `Monarc.RHI/Include/` and `Monarc.RHI.Vulkan/Include/` is in a comment —
fifty-three mentions, no declarations — and `VK_USE_PLATFORM_WIN32_KHR` is defined in exactly
two translation units, both under `Private/Platform/Windows/`, of which one includes
`<Windows.h>` and the other reaches Win32 through `vulkan_win32.h`.

Design decisions: [ADR-0005](../Architecture/Decisions/ADR-0005-rhi-sync-model.md) (barrier
model), [ADR-0012](../Architecture/Decisions/ADR-0012-backend-rollout.md) (rollout order).

**A note on what "implemented" means in this document.** Everything below marked as a phase's
work is running and tested; where a number, a byte or a driver behaviour is stated, it was
measured on the development machine's two GPUs and [Status.md](../Status.md) holds the run. The
sections on resource binding and on what arrives later are still design, and say so.

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

with buffer, texture, and global variants. Split barriers are part of the model's *design* and
are not expressed yet — see below.

**Implemented whole in Phase A3 Task 3** (`Monarc.RHI/Include/Monarc/RHI/Barrier.h`): fifteen
pipeline stages, eighteen accesses and nine texture layouts, every one translated in both
directions and tested with no device present. What the engine *records* is fewer, and the
distinction is worth keeping: `Monarc.FirstLight` issues two texture barriers a frame — into
`ColorAttachment` before the clear and into `PresentSource` before the present — and the
readback test adds a third, a buffer barrier into the host stage that makes the copy's writes
visible to a mapped read.

The membership rule for those lists is *not* "what A3 uses" — it is Vulkan 1.3 **core** only,
and only stages Monarc has a plan for. Which is why the ray-tracing and mesh-shading stages are
still absent: they arrive with a phase that schedules them. `PresentSource` is the exception
that proves the rule rather than breaking it — it needs `VK_KHR_swapchain`, so it was
deliberately left out of Task 3 and arrived in Task 4 with `ISwapchain`, which is what makes
the count nine rather than eight. This is the opposite of the rule `Format` in `Types.h` states
for itself, and deliberately: a format costs a size claim nothing verifies, where a stage costs
one row in one switch.

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

**Frame completion is a timeline semaphore; presentation adds binary ones, each where it
belongs.** `IQueue::Submit` returns the value its submission will signal;
`IDevice::BeginFrame` waits on the value its frame slot's previous submission signalled before
resetting that slot's command pool, which is what makes the reset legal — resetting a pool
whose buffers are still executing is undefined behaviour. Two frames in flight, one command
pool each.

Binary semaphores arrived with the swapchain in A3 Task 4, because `vkQueuePresentKHR` accepts
only those: an acquire semaphore per frame in flight and a render-finished semaphore per
swapchain image, both owned by the swapchain and destroyed with it. The timeline is still what
says whether frame N-2 has finished, and it is *also* what makes reusing an acquire semaphore
safe — the swapchain records the timeline value of the submission that waited on each one and
waits for it before handing that semaphore to another acquire. Making the timeline cover
presentation was never available; using each where it belongs is the whole decision.

**One `Begin`, one `End` and one `Submit` per `BeginFrame`**, and a second of any of them is
refused with `ErrorCode::InvalidArgument` rather than attempted: only `BeginFrame` resets the
pool, so only `BeginFrame` makes a list recordable and submittable again — waiting for the work
to finish does not. A frame loop that comes round without calling it gets a `Status` and not
undefined behaviour, in every configuration and not only where a validation layer is loaded.
`ICommandList` in `Monarc/RHI/Device.h` lists the states each call refuses.

**A command list is in exactly one of five states, and the backend switches over them
exhaustively.** Task 3 tracked the machine in four booleans and closing the table found three
reachable holes, each a named VUID in Debug and silent undefined behaviour in Release; Task 4
made it one `enum class State` — `Reset`, `Recording`, `Rendering`, `Recorded`, `Submitted` —
with every switch `default`-less, so `/w44062` makes an unaudited method a compile error where
four booleans could not. The fifth state is `Rendering`, which was the sub-state Task 3 kept in
a fourth bool; the swapchain adds no list state at all, because an acquired image is an ordinary
texture handle. `VulkanCommandList`'s class comment in
`Monarc.RHI.Vulkan/Private/VulkanDeviceState.h` holds the table and the transitions.

**Resources are handles from device-owned, generation-checked, fixed-capacity pools.** Capacity
comes from a `DeviceConfig` at device creation and is never grown, which is
[`JobSystem`](../Runtime/Threading.md)'s discipline and is here for the same reason: a pool that
reallocated would move a slot out from under every handle naming it. A full pool reports
`ErrorCode::OutOfMemory`; a stale handle resolves to a failure and never to the slot's new
occupant ([ADR-0002](../Architecture/Decisions/ADR-0002-handles-not-pointers.md)).

**Where the answer cannot be a `Status`, it is the end of the process — and as of A3 Task 5
there is a test that says so.** `ICommandList::Barrier` returns void, and a dropped barrier is
not a refused operation but a synchronisation hole whose symptom is wrong pixels or a GPU hang
on some driver days later. So a barrier naming a resource this device does not have, or one
whose handle is stale, ends the process; so does a barrier attempted inside a rendering
instance, where `vkCmdPipelineBarrier2` has no legal call to make at all. Those three refusals
have a CTest entry each, under the `gpu` label: a child process reaches the guard with an
assert handler that declines to break, and the harness requires a non-zero exit and the guard's
own message. Turning any of the three into a plain `return` turns its entry red, which is the
regression a comment alone could not stop. `Tools/run_death_test.py` holds the conditions.

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

## The swapchain, and how a surface reaches it

`ISwapchain` and `SurfaceDescription` (`Monarc.RHI/Include/Monarc/RHI/Swapchain.h`, Phase A3
Task 4) are the presentation half, and both are backend-free.

**A surface is two opaque `void*` and nothing knows what a window is.** `SurfaceDescription`
carries a native window handle and the module or display connection it belongs to, and neither
the RHI's headers nor the backend's may name a platform type
([ADR-0016](../Architecture/Decisions/ADR-0016-platform-code-selection.md);
[ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md) rule 1). That is
structural rather than stylistic: gate 3 forbids a Tier 2 translation unit from including
`Monarc/Host/`, so a `Window&` could not be passed to the backend even if someone wanted to.
The app takes the handles out of the window and puts them in the description;
`Monarc.RHI.Vulkan/Private/Platform/Windows/VulkanSurface.cpp` turns them into a
`VkSurfaceKHR`, and it is the only file in the backend that includes `<Windows.h>`.

**An acquired image is a `TextureHandle` from the device's own pool.** So `Barrier`,
`BeginRendering` and `CopyTextureToBuffer` work on one unchanged, with ADR-0002's generation
checking intact — which is what lets the swapchain readback that proves the clear reuse the
same copy path an offscreen texture does, with no entry point of its own. `ISwapchain::Recreate`
releases every slot, so a handle held across a resize is stale rather than naming the slot's new
occupant.

**FIFO, `B8G8R8A8_UNORM`, `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`, opaque composition — negotiated
against the surface and refused rather than substituted.** FIFO is the only present mode Vulkan
guarantees and sRGB-non-linear the only colour space; UNORM rather than an sRGB format keeps A3
out of deciding clear-value semantics on sRGB images, which is stated as an open question rather
than guessed at. A surface offering none of them reports `ErrorCode::Unsupported`, because a
swapchain silently created in another format would make every byte the suite asserts a statement
about a format nobody chose.

**Swapchain images are not frames in flight.** The image count is `minImageCount + 1`, clamped
to `maxImageCount` when that is non-zero, and belongs to the surface; `kFramesInFlight` is
Monarc's own CPU/GPU overlap. On the development machine they are 3 and 2, and nothing derives
either from the other — code that had tied them together would be wrong only on a driver
reporting a different minimum, which is the kind of bug that surfaces on someone else's machine.

**`VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` are handled from both acquire and present**
— four cases, not two. Out-of-date from acquire means there is no image and nothing may be
presented; suboptimal means the image is valid and *must* be presented, because abandoning a
signalled acquire semaphore is the one mistake here a validation layer catches late and a
release build not at all. `ISwapchain::NeedsRecreation()` is sticky and set by any of the four,
so a loop that checks it once a frame cannot miss one.

**A minimised window reports `0 x 0` and the frame loop parks.** `Recreate` refuses an empty
extent — before tearing anything down, so a loop that asks at the wrong moment keeps the
swapchain it had — and the parking belongs to the caller, because only the caller knows how to
wait for the window to come back.

**Presentation support is per adapter and per surface, and it is measured.**
`VulkanBackend::AdapterCanPresent` asks `vkGetPhysicalDeviceSurfaceSupportKHR` about the
graphics queue family device creation would actually pick. On the development machine both
adapters can present to a window on either monitor — see [Status.md](../Status.md), which
records it as a finding rather than an assumption.

`VK_KHR_swapchain` is the only device extension Monarc enables, and it is enabled per adapter
rather than required: presentation is a window-system capability, not a tier, so a compute-only
or headless device stays usable for the offscreen work it is perfectly capable of and only
swapchain creation reports about it.

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

**This is the one fatal guard in the tree that A3 Task 5's death-test harness does not cover,
and the reason is worth stating rather than leaving as a gap in a table.** The three others in
this document — `Barrier`'s two stale-handle refusals and the rendering-instance one — each got
a CTest entry, because a test can *call* them. This one cannot be called: the callback is a
file-static in an anonymous namespace, so reaching it means provoking a genuine
VALIDATION-severity finding, which means deliberately misusing Vulkan in a way Monarc's own
guards do not already refuse first. It exists only in Debug, and only where the layer is
installed, so an entry for it could run on neither the Release presets nor a machine without
the SDK. What it has instead is the pNext measurement above, which is what proves the mechanism
rather than the abort.

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
