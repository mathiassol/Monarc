# Phase A3 — `Monarc.RHI`, `Monarc.RHI.Vulkan`, and first light

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A window that opens and clears to a known colour, through `Monarc.RHI` and a real Vulkan 1.3 backend — plus an automated, headless colour-readback test that proves the clear actually happened rather than merely that nothing crashed.

**Architecture:** `Monarc.RHI` is an abstract, backend-free interface ([RHI.md](../Rendering/RHI.md)); `Monarc.RHI.Vulkan` implements it; `Monarc.Host.Windowed` owns the window and the platform surface. All three are Tier 2 or 3 and depend only downward ([Module-Graph](../Architecture/Module-Graph.md)). Resources are handles, never pointers ([ADR-0002](../Architecture/Decisions/ADR-0002-handles-not-pointers.md)). Barriers carry the `synchronization2` / Enhanced Barriers shape ([ADR-0005](../Architecture/Decisions/ADR-0005-rhi-sync-model.md)), even though A3 needs only two of them.

**Tech Stack:** C++23, MSVC + Clang (`clang-cl`), CMake + Ninja, doctest, Vulkan SDK 1.4.357.0, raw Win32.

**Plan style:** headers and complete tests, not implementation bodies — as A2a onward.

---

## What makes this phase different from every phase before it

**The verification story changes, and pretending otherwise would be the worst outcome.** A1
through A2d were pure CPU code: every line ran identically on the development machine and on
both CI compilers, so "green on six presets" meant the code was actually exercised. That
stops being true here. GitHub's Windows runners have no GPU and, almost certainly, no Vulkan
ICD at all — so the majority of this phase's code cannot execute in CI.

The failure mode to avoid is a suite that *appears* green in CI while silently running
nothing. Three rules follow, and they shape the task breakdown more than any Vulkan detail:

1. **Split the tests by what they need**, not by what they test. Anything that can be a pure
   function — barrier translation, format tables, tier comparison, adapter deduplication —
   is written as a pure function and tested with no device, so CI genuinely covers it.
2. **A missing device is a `SKIP`, never a `PASS`.** Device tests exit with CTest's
   `SKIP_RETURN_CODE`, so `ctest` prints them as skipped. A suite that cannot run must say so
   in the place people look.
3. **A probe test always runs and always reports.** It prints what it found — loader,
   instance version, adapter list — and passes unconditionally. Its job is to put the truth
   about each machine into the CI log, so "CI has no Vulkan" is an observed fact in the
   record rather than an assumption in a plan.

**Two things about the development machine are already known and constrain the design.** Both
were measured with `vulkaninfo`, not assumed — see [Status.md](../Status.md#hardware):

- **Vulkan 1.3 is the ceiling.** The Intel UHD 730 reports 1.3.275; the RTX 3070 Ti reports
  1.4.351. Requiring 1.4 would silently drop the lower-tier device out of the test matrix,
  which is the entire reason that device is valuable. 1.3 core is the requirement; 1.4
  features are opportunistic and always optional.
- **`vkEnumeratePhysicalDevices` returns the Intel GPU four times here.** All four report the
  same `deviceUUID`; the duplicates come from this machine's virtual display adapters, each
  of which re-registers the Intel ICD. Adapter enumeration must deduplicate, and the test for
  it is not hypothetical — it fails today on the machine the engine is being written on.

---

## File Structure

```
CMake/
  MonarcModule.cmake        + monarc_app(): executables that obey the module graph

Source/Monarc.RHI/          Tier 2, Runtime, deps: Core
  Include/Monarc/RHI/
    Types.h                 formats, extents, enums; no backend types
    Handles.h               TextureHandle, BufferHandle, ... (ADR-0002)
    Barrier.h               the synchronization2-shaped barrier structs
    Capabilities.h          feature queries and tiers
    Adapter.h               AdapterInfo: name, vendor, UUID, tier, api version
    Device.h                IDevice, IQueue, ICommandList interfaces
    Swapchain.h             ISwapchain, SurfaceDescription
    Backend.h               IBackend: enumerate adapters, create a device
  Private/
    Barrier.cpp  Capabilities.cpp  Adapter.cpp
  Tests/
    TestBarrier.cpp  TestCapabilities.cpp  TestAdapter.cpp

Source/Monarc.RHI.Vulkan/   Tier 2, Runtime, deps: Core, RHI
  Include/Monarc/RHI/Vulkan/
    VulkanBackend.h         the only public header: CreateVulkanBackend()
  Private/
    Loader.h  Loader.cpp    vulkan-1.dll via Platform::Library; entry-point table
    Translate.h  Translate.cpp   RHI enum <-> Vk enum, as pure functions
    VulkanBackend.cpp  VulkanDevice.cpp  VulkanCommandList.cpp
    VulkanSwapchain.cpp
    Platform/Windows/VulkanSurface.cpp   VK_KHR_win32_surface (ADR-0016)
  Tests/
    TestVulkanLoader.cpp    device-free: the loader's failure path
    TestVulkanTranslate.cpp device-free: enum translation is total and round-trips
    TestVulkanDevice.cpp    device-required: skips with 77 when there is no device

Source/Monarc.Host.Windowed/  Tier 3, Runtime, deps: Core, RHI
  Include/Monarc/Host/
    Window.h                create, pump, resize/close events, native surface handle
  Private/
    Window.cpp              platform-neutral parts
    Platform/Windows/Window.cpp   raw Win32: window class, message pump (ADR-0016)
  Tests/
    TestWindow.cpp          device-free; window creation is skipped headlessly

Source/Monarc.FirstLight/   Tier 3, Runtime, app, deps: Core, RHI, RHI.Vulkan, Host.Windowed
  Private/Main.cpp          the runnable proof: a window that clears
```

---

## Decisions this plan makes, with reasons

**The Vulkan loader is opened by us, through `Platform::Library`.** No `volk`, no
`vulkan-1.lib` import library. We call `LoadLibrary("vulkan-1.dll")` through A2c's
`Platform::Library`, resolve `vkGetInstanceProcAddr` from it, and build the entry-point table
from there.

The reason is not purity. Linking the import library makes a missing Vulkan runtime a
*Windows loader failure* — the process dies before `main` with a system dialog naming a DLL,
which is useless to a player and unrecoverable by us. Loading it ourselves turns the same
situation into an ordinary `Result` with a message, which is what a shipped game needs and
what [`Monarc.Host.Headless`](../Architecture/Module-Graph.md) needs in order to link no
graphics API at all. It also means backend selection at runtime — which the module graph
already promises — is real rather than aspirational. The cost is an entry-point table; it is
mechanical, and A3's is about sixty functions.

**`ErrorCode` gains one value: `BackendFailure`.** "The graphics API returned an error" and
"this device cannot do that" are different things and lead to different handling. `Unsupported`
keeps the second meaning; `BackendFailure` takes the first, and carries the `VkResult` name in
its message. Overloading one code for both would make every caller guess.

**Adapters are deduplicated on `deviceUUID`, and the dedupe is a pure function.**
`VkPhysicalDeviceIDProperties::deviceUUID` is core since Vulkan 1.1 and is the identity the
spec actually guarantees — device name is not unique, and PCI info is an extension. The dedupe
takes a list of `AdapterInfo` and returns a list, so it is testable in CI with no device at
all, and the real enumeration path is a thin wrapper over it.

**Validation errors are fatal in Debug, and that is the point.** A validation layer whose
output scrolls past is worse than no validation, because it produces false confidence. The
debug messenger logs everything and, for `ERROR` severity, follows the house pattern —
`MONARC_CHECK` with a message, then `MONARC_DEBUG_BREAK()` and `std::abort()`, exactly as
[`JobSystem::Wait`](../Runtime/Threading.md#wait-is-for-the-owning-thread-not-a-worker) does,
and for the same reason: a check that only reports does not change what the program does.
Release builds create no messenger and load no layers.

**Present uses binary semaphores; frame pacing uses a timeline semaphore.** Timeline
semaphores are core in 1.3 and are the right tool for "has frame N-2 finished on the GPU",
but `vkQueuePresentKHR` accepts only binary semaphores. Using each where it belongs is
honest; pretending the timeline covers presentation would not survive contact with the
swapchain.

**Two frames in flight.** Enough to overlap CPU and GPU, few enough that latency stays
comprehensible while there is no frame pacing work yet. Frame pacing is explicitly deferred
([RHI.md](../Rendering/RHI.md#things-that-belong-here-but-arrive-later)).

**FIFO present mode, `B8G8R8A8_UNORM`, sRGB non-linear colour space.** FIFO is the only mode
Vulkan guarantees. UNORM rather than an sRGB format is deliberate for A3: clear-value
semantics on sRGB images are a genuine subtlety, and A3 has no business deciding colour
management before there is anything to manage. It is called out under open questions below
rather than guessed at.

**`monarc_app()` is new, and apps are graph leaves.** `monarc_module()` only makes static
libraries, so there is currently no way to declare an executable that the architecture gates
can see. `monarc_app()` takes the same `NAME`/`KIND`/`TIER`/`*_DEPS` metadata, registers the
same way, and produces an executable — with one extra rule: **nothing may depend on an app**,
enforced in `monarc_validate_modules()`. Apps stay console-subsystem for now, because A3's
diagnostics are more valuable than a hidden console window.

**Gate 10's name is wrong and A3 corrects it.** The gate is documented as "platform
conditionals only in `Monarc.Core/Platform`", but what it actually enforces — and has enforced
since A2c — is path-shaped and module-agnostic: platform conditionals are permitted only under
`Private/Platform/<Platform>/`, in any module. That broader rule is the correct one, and A3 is
the phase that proves it, because `Monarc.Host.Windowed` and `Monarc.RHI.Vulkan` both need
genuinely platform-specific code and neither is `Monarc.Core`. The mechanism is unchanged; the
gate's name, [Module-Graph](../Architecture/Module-Graph.md) rule 7, and
[M0](../Milestones/M0-First-Light.md) gate 10 are corrected to describe it.

---

## Task 1: `monarc_app()`, three module skeletons, and the graph that holds them

The first real test of the module graph beyond a single edge: three new modules across two
tiers, one of them an executable, and the first Tier 3 → Tier 2 dependency in the project.

- [ ] Add `monarc_app()` to `CMake/MonarcModule.cmake`, sharing `monarc_module()`'s argument
      parsing, validation, registration and `module-graph.json` entry, but emitting
      `add_executable`. Mark apps in the graph with `"app": true`.
- [ ] Extend `monarc_validate_modules()` with the leaf rule: it is a configure-time
      `FATAL_ERROR` for any module to list an app in `PUBLIC_DEPS` or `PRIVATE_DEPS`.
- [ ] Create `Monarc.RHI` (Runtime, tier 2, deps `Monarc.Core`) with `Types.h`, `Handles.h`
      and nothing else yet.
- [ ] Create `Monarc.RHI.Vulkan` (Runtime, tier 2, deps `Monarc.Core`, `Monarc.RHI`) with
      `VulkanBackend.h` declaring `CreateVulkanBackend()` and a stub returning
      `ErrorCode::Unsupported`.
- [ ] Create `Monarc.Host.Windowed` (Runtime, tier 3, deps `Monarc.Core`, `Monarc.RHI`) with
      `Window.h` and no implementation yet.
- [ ] Create the `Monarc.FirstLight` app (Runtime, tier 3, deps `Monarc.Core`, `Monarc.RHI`,
      `Monarc.RHI.Vulkan`, `Monarc.Host.Windowed`) whose `main` prints the backend's error and
      exits non-zero. That is a real runnable proof: the graph links end to end.
- [ ] Add `Monarc.RHI` and `Monarc.RHI.Vulkan` to gate 3's Tier 2 boundary list in
      `Tools/check_architecture.py`, and add a gate test that a Tier 2 module including a
      Tier 3 header fails.
- [ ] Add gate tests for the app leaf rule: an app is accepted; a module depending on an app
      is rejected.

**Verification.** `module-graph.json` records six modules and the edges above.
`ctest` is green on all six presets. Then, deliberately: make `Monarc.RHI` depend on
`Monarc.Host.Windowed` and confirm the tier rule fires; make `Monarc.RHI` depend on
`Monarc.FirstLight` and confirm the leaf rule fires; revert both. **Do not skip this** — gate
10 was vacuously green for four phases precisely because nobody had made it fail on purpose.

---

## Task 2: The loader, the instance, and honest adapter enumeration

- [ ] `Loader.h` / `Loader.cpp`: open `vulkan-1.dll` through `Platform::Library`, resolve
      `vkGetInstanceProcAddr`, and populate global, instance and device entry-point tables
      from an X-macro list. Every failure is a `Result` with a message naming what was missing.
- [ ] Instance creation: application info at API version 1.3, `VK_KHR_surface` +
      `VK_KHR_win32_surface`, and in Debug `VK_EXT_debug_utils` plus
      `VK_LAYER_KHRONOS_validation`. Missing layers are a warning and a degraded instance, not
      a failure — a machine without the SDK still runs the game.
- [ ] The debug messenger, with the abort-on-`ERROR` behaviour described above.
- [ ] `AdapterInfo` and `DeduplicateAdapters(Span<const AdapterInfo>)` as a pure function.
- [ ] `Capabilities.h`: the tier query. A3 needs only enough to distinguish the two local
      devices — API version, device type, descriptor-indexing limits, timeline semaphore
      support, queue families — with room for mesh/ray-tracing tiers later.
- [ ] `Monarc.FirstLight` gains an `--adapters` mode that prints the enumerated list.

### Device-free tests (these run in CI)

- [ ] `TestVulkanLoader.cpp`: opening a library name that does not exist returns
      `ErrorCode::NotFound` with a message naming it, and does not crash — the same shape as
      `Platform::Library`'s own tests.
- [ ] `TestAdapter.cpp`: `DeduplicateAdapters` collapses four entries sharing one UUID into
      one and keeps the first; preserves order otherwise; keeps two entries whose UUIDs differ
      by a single byte; handles an empty list. **Build the four-identical-Intel case from the
      real UUID** recorded in [Status.md](../Status.md#hardware) — this is a regression test
      for an observed defect, not a hypothetical.
- [ ] `TestCapabilities.cpp`: tier comparison is a total order; a device missing a required
      feature never compares as meeting the tier that requires it.
- [ ] `TestVulkanTranslate.cpp`: every RHI enumerator maps to exactly one Vulkan enumerator
      and the mapping is exhaustive — a `switch` with no `default`, so adding an enumerator
      later is a compile error rather than a silent fall-through.

### Device-required test

- [ ] `TestVulkanDevice.cpp` gains its first case: enumeration finds at least one adapter, and
      **no two adapters in the returned list share a UUID**. Registered with
      `SKIP_RETURN_CODE 77` and the `gpu` label.
- [ ] `Monarc.RHI.Vulkan.Probe`: a separate always-passing CTest entry that prints loader
      status, instance version and the raw and deduplicated adapter lists.

**Verification.** Locally, `--adapters` prints **two** adapters — the RTX 3070 Ti and one
Intel UHD 730 — where raw enumeration returns five. Capture both lists in the task notes. In
CI, the probe runs and its output is read; whatever it says about the runners goes into
Status.md as fact.

---

## Task 3: Device, command lists, barriers, and a colour that can be proved

This is the task that makes the phase automatable. Everything here runs without a window.

- [ ] `IDevice`, `IQueue`, `ICommandList` in `Monarc.RHI`, and their Vulkan implementations:
      logical device creation with a graphics queue, command pools per frame, command buffer
      recording, submission, and a timeline semaphore for frame completion.
- [ ] `Barrier.h`: the `syncBefore`/`syncAfter`, `accessBefore`/`accessAfter`,
      `layoutBefore`/`layoutAfter` structs from [ADR-0005](../Architecture/Decisions/ADR-0005-rhi-sync-model.md),
      with buffer, texture and global variants. A3 implements translation for all of it and
      *uses* two barriers; the rest is exercised by the pure-function tests.
- [ ] Texture creation and a linear host-visible staging buffer, enough for readback.
- [ ] `BeginRendering`/`EndRendering` on top of Vulkan 1.3 dynamic rendering — no
      `VkRenderPass`, no `VkFramebuffer`, anywhere in the backend.

### Device-free tests

- [ ] `TestBarrier.cpp`: a barrier with no state change is representable and is not silently
      dropped; the struct round-trips through translation unchanged; a texture barrier without
      a layout pair is rejected at the type level rather than at runtime.

### Device-required tests

- [ ] Create a device on every deduplicated adapter in turn — **both** local GPUs, not just
      the default — and destroy it cleanly.
- [ ] **The readback test.** Create an `R8G8B8A8_UNORM` texture, clear it to
      `(64/255, 128/255, 192/255, 1.0)` through the RHI, barrier it to transfer-source, copy
      to a host-visible buffer, and assert the bytes are exactly `(64, 128, 192, 255)`. Exact
      values, chosen to be representable without rounding: a tolerance here would hide the
      channel-order and colour-space bugs this test exists to catch.
- [ ] Run the readback on both adapters. A result that differs between vendors is a finding,
      not a flake.
- [ ] Validation produces zero errors across the whole suite — which the fatal messenger
      already guarantees, but assert the messenger was installed, so a build that quietly
      failed to load the layer cannot pass as clean.

**Verification.** `ctest -L gpu` green locally on both adapters; `ctest` in CI reports the gpu
tests as **skipped**, not passed. Confirm the skip appears in CI's output by reading it.

### Notes carried over from Task 2's code-quality review

Two things Task 2's review found and deliberately left alone, because Task 3 is where they
stop being premature. Neither is a defect today.

- [ ] **Consolidate the resolve macros.** `MONARC_VK_RESOLVE_GLOBAL` and
      `MONARC_VK_RESOLVE_INSTANCE` in `Private/Loader.cpp` differ only in the table they write
      and two words of wording. A third copy for the device table is the point at which
      parameterising pays — not before, which is why Task 2 left two.

      Related, and larger: today "optional" means "its own table". That works for the one
      extension Monarc asks for (`VK_EXT_debug_utils`, whose absence is not an error) and
      multiplies badly once mesh-shader, ray-tracing and swapchain entry points arrive with
      *per-adapter* availability. `Private/Loader.h`'s three-table comment anticipates the
      device table but not per-function optionality; a resolved-or-null flag per entry, or a
      `Resolve(..., Required | Optional)` parameter, is the shape to consider when the device
      table lands.
- [ ] **Extract the messenger block from `BringUp`.** `Private/VulkanBackend.cpp`'s messenger
      creation needs only `instance`, `messengerInfo` and the loader, and comes out cleanly.
      `BringUp`'s remaining length is partly forced and should stay: `messengerInfo`,
      `enabledExtensions` and `applicationInfo` must all outlive the `vkCreateInstance` call
      that reads them, so they cannot move into helpers that return.

---

## Task 4: A window, a surface, a swapchain, and first light

- [ ] `Window.h`: create with a size and title, pump messages, report resize and close, and
      expose a native surface handle as an opaque `void*` pair so the header names no Win32
      type ([ADR-0016](../Architecture/Decisions/ADR-0016-platform-code-selection.md)).
- [ ] `Private/Platform/Windows/Window.cpp`: window class registration, `WndProc`, the message
      pump, DPI awareness, and translation of Win32 messages into Monarc events.
- [ ] `Private/Platform/Windows/VulkanSurface.cpp`: `vkCreateWin32SurfaceKHR`, and the
      queue-family presentation-support query. This is the only file in the backend that
      includes `<windows.h>`.
- [ ] `ISwapchain`: create from a surface, acquire, present, and recreate. Handle
      `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` from both acquire and present.
- [ ] **Handle minimisation.** A minimised window reports a `0 x 0` extent, and creating a
      swapchain with it is invalid. The loop must park — pump messages, present nothing — and
      resume on restore. This is a real bug in most first attempts and is cheaper to write now
      than to debug later.
- [ ] `Monarc.FirstLight`'s main loop: acquire, barrier undefined → colour attachment, begin
      rendering with a clear, end, barrier → present, submit, present, advance the timeline.
- [ ] Clean shutdown: wait on the timeline, destroy in reverse order, and exit zero.

### Tests

- [ ] `TestWindow.cpp`, device-free: a window created and destroyed leaves no registered class
      behind; a resize event carries the size the window actually became, not the size
      requested. Skips (77) when there is no interactive session.
- [ ] Device-required: create a swapchain against a real window, acquire and present ten
      frames headlessly-but-really, and assert no validation output and no `OUT_OF_DATE` loop.

**Verification.** Run `Monarc.FirstLight`. A window opens and is filled with the clear colour.
Resize it, maximise it, minimise and restore it, and drag it between the two monitors — no
validation errors, no crash, no hang. Then **capture a frame in RenderDoc** on each adapter in
turn and confirm the clear appears as one `vkCmdBeginRendering` with the expected clear value.
That capture is the proof that the pixels came from where we think they did.

---

## Task 5: Close-out — make the gaps visible, then write them down

- [ ] Correct gate 10's name in `Tools/check_architecture.py` to describe what it enforces —
      platform conditionals only under `Private/Platform/<Platform>/`, in any module — and
      update [Module-Graph](../Architecture/Module-Graph.md) rule 7 and
      [M0](../Milestones/M0-First-Light.md) gate 10 to match. Add a gate test that a platform
      conditional in a *non-Core* module outside such a directory still fails.
- [ ] Confirm gate 3 (renderer package boundary) now has real Tier 2 modules to check, and
      make it fail on purpose once before trusting it.
- [ ] Read CI's probe output and record in [Status.md](../Status.md) exactly what the runners
      have: loader present or not, instance version, adapters. No guesses.
- [ ] Update [Status.md](../Status.md) with an "A3 delivered" section: what runs, what is
      skipped in CI and why, and the RenderDoc captures taken.
- [ ] Update [RHI.md](../Rendering/RHI.md) where A3 turned an intention into a fact —
      particularly the loader decision and the adapter-identity rule.
- [ ] Update [M0](../Milestones/M0-First-Light.md) to mark A3 complete.

### Notes carried over from Task 2's code-quality review

- [ ] **Decide whether `TestsRuntime/` earns a third binary — after reading the probe's real CI
      output, not before.** Six cases in `TestsDevice/TestVulkanDevice.cpp` need a Vulkan
      **runtime**, not a **device**: the three `Loader` move cases, "opening a library that is
      not the Vulkan loader still fails on a machine that has one", the instance-version case,
      and the validation-implication case. They sit behind `main`'s `adapters.IsEmpty()` gate,
      so on a machine with `vulkan-1.dll` and no registered ICD all six are skipped though
      every one could have run. Whether that is worth a third outcome depends entirely on what
      the runners actually have, which is the checkbox above this one.

      Related, and independent of that decision: **in CI the X-macro resolution loops are
      compiled and never executed**, because the device-free suite stops at `Loader::Open`'s
      second check — no `vulkan-1.dll`, no resolution. A test-only DLL exporting a stub
      `vkGetInstanceProcAddr` that returns null for one named function would make the
      "returned null for X" branch executable in CI, and would also make `Loader.cpp`'s
      table-clearing on partial failure observable. Task 2 proved that path by mutation
      instead, which is not the same as covering it.

### A note carried over from Task 3

- [ ] **Add a death-test harness, and cover the fatal guards with it.** Monarc now has four
      guards that deliberately end the process, and **not one of them has a case in any
      suite**: `Array<T>::OnAllocationFailed`, `JobSystem::Wait`'s worker guard,
      `JobSystem::PopQueueLocked`, and `ICommandList::Barrier`'s stale-handle refusal. Each
      was verified once, by hand, with a scratch program that was then deleted — so nothing
      stops a later edit from turning any of them back into a silent `return`, which is
      exactly the regression three separate reviews on this branch have already caught in
      other forms.

      The shape is a registered CTest entry that runs a child process and asserts a non-zero
      exit with an expected message. It covers all four at once, and **three of the four need
      no GPU**, so most of it runs in CI. Worth its own pass rather than a comment in four
      files saying "not testable" — the guards are testable, just not in-process, and the
      difference is a harness nobody has written yet.

---

## Definition of done

- `Monarc.FirstLight` opens a window that clears to a known colour, on both local adapters.
- The colour is proved by an automated readback test asserting exact bytes, not by looking.
- A RenderDoc capture exists for each adapter.
- Resize, maximise, minimise, restore and monitor-drag all survive with zero validation output.
- CI is green on all six presets, with gpu tests reported as **skipped** and the probe's
  findings recorded in Status.md.
- Every new gate has been made to fail on purpose at least once.
- `module-graph.json` records six modules, and the app leaf rule is enforced.

---

## What A3 deliberately excludes

The render graph (that is A4 — A3 records commands directly, which is exactly the thing A4
replaces), shaders and pipelines, vertex buffers, descriptor sets and bindless heaps, any
second backend, multiple queues, device-loss recovery, frame pacing, HDR and colour
management, `Monarc.Host.Headless`, and input beyond the window events needed to close and
resize a window.

## Open questions this plan does not settle

- **Clear-value semantics on sRGB formats.** Whether a clear value is interpreted as linear
  and encoded, or written through unchanged, is a real subtlety with real vendor history. A3
  avoids it by using UNORM throughout. Phase B decides it deliberately, with a readback test
  on an sRGB target, once there is a shader and a reason to care.
- **Whether the entry-point table should be generated.** Sixty functions is comfortable by
  hand; six hundred is not. Revisit when the backend grows, not before.
- **Whether `Monarc.Host.Windowed` keeps its Tier 3 home once `Monarc.Engine` exists.** It is
  declared with `Core` + `RHI` deps here because `Engine` and `Render` do not exist yet; the
  [Module-Graph](../Architecture/Module-Graph.md) entry that adds them is Phase D's, and the
  tier does not change.
