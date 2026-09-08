# Phase A4 — the render graph

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The same window, the same clear — but the two barriers A3 wrote by hand are now *derived* by the graph from declared reads and writes, and nothing outside `Monarc.Render` writes a barrier again.

**Architecture:** `Monarc.Render` is Tier 2 and owns all synchronisation ([ADR-0006](../Architecture/Decisions/ADR-0006-render-graph.md)). Features declare passes; the graph builds a dependency graph, culls, computes lifetimes, groups aliases, derives barriers in the [ADR-0005](../Architecture/Decisions/ADR-0005-rhi-sync-model.md) model, orders execution and records. It is inspectable by requirement, not as a debug feature ([Render-Graph.md](../Rendering/Render-Graph.md)).

**Tech Stack:** C++23, MSVC + Clang (`clang-cl`), CMake + Ninja, doctest, Vulkan 1.3 through `Monarc.RHI`.

**Plan style:** headers and complete tests, not implementation bodies — as A2a onward.

---

## What makes this phase different from A3

**A3's verification problem is inverted, and that should shape everything.** In A3 the majority
of the code could not run in CI, so the phase was built around splitting tests by what they
need and making a missing device a visible `SKIP`. A4 is the opposite: **the graph's entire
value is a pure function.** Declaration → cull → lifetimes → alias groups → derived barriers
is computation over declarations, with no device anywhere in it. It is the most important code
in the rendering layer and it is *fully* CI-coverable.

Two consequences:

1. **Almost nothing here has an excuse to be device-gated.** A device is needed only to *run*
   the recorded frame. If a test needs a GPU to check a derivation, the derivation has been
   entangled with execution and the design is wrong. Treat a device-required test in Tasks 1–3
   as a design smell to investigate, not a fact to accept.
2. **The crown jewel is testable by assertion rather than by capture.** Render-Graph.md says
   this outright — *"tests that assert on derived barriers rather than on pixels. A barrier
   regression should fail a unit test, not a screenshot comparison."* A4 is where that stops
   being an aspiration.

**A3 left behind exactly the ground truth A4 needs.** `Build/Captures/` holds two RenderDoc
captures with XML dumps, one per adapter, of the hand-written frame: **one**
`vkCmdBeginRendering` with `LOAD_OP_CLEAR` and clear value `0.25098040699958801`, and
**exactly two** `vkCmdPipelineBarrier2` — `UNDEFINED → COLOR_ATTACHMENT_OPTIMAL` before, and
`COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR` after. That is not a plausible reference, it is a
measured one.

So A4 has an unusually sharp definition of correct: **the graph must derive those two barriers
and no others, from a declaration that mentions no barrier at all.** Assert it device-free
against the captured values, then confirm on a device that the swapchain readback is still
byte-exact and a fresh capture matches A3's.

---

## File Structure

```
Source/Monarc.Render/          Tier 2, Runtime, deps: Core, RHI
  Include/Monarc/Render/
    ResourceId.h            graph-resource identity: a declaration, not a live resource
    Access.h                how a pass touches a resource, in ADR-0005's vocabulary
    PassBuilder.h           what a pass declares: reads, writes, transients, a callback
    RenderGraph.h           Build -> Compile -> Execute, and the inspection accessors
    GraphInspection.h       the emitted data: passes, lifetimes, alias groups, barriers
  Private/
    ResourceRegistry.cpp    declared resources and their descriptions
    Compile.cpp             dependency build, culling, lifetimes, alias grouping
    DeriveBarriers.cpp      the crown jewel, and a pure function
    Execute.cpp             the only file that touches an RHI command list
    GraphInspection.cpp     formatting the inspection data
  Tests/
    TestResourceId.cpp  TestAccess.cpp  TestPassDeclaration.cpp
    TestCull.cpp  TestLifetimes.cpp  TestAliasing.cpp
    TestDeriveBarriers.cpp   <- the most important test file in the phase
    TestGraphInspection.cpp
  TestsDevice/
    TestGraphExecution.cpp   device-required: the graph actually records and runs

Source/Monarc.FirstLight/
  Private/Main.cpp          stops writing barriers; declares a pass instead
```

---

## Decisions this plan makes, with reasons

**`Monarc.Render` depends on `Core` and `RHI`, and not yet on `Jobs` or `Shaders`.**
[Module-Graph](../Architecture/Module-Graph.md) shows its destination as `Core`, `Jobs`,
`RHI`, `Shaders`. `Monarc.Shaders` is Phase B and does not exist. `Monarc.Jobs` exists, but
its only use here would be parallel command recording, which ADR-0006 defers past M0 — so it
arrives with its first user, the rule this codebase has applied to every enumerator and field
since A2. Declaring an edge nothing traverses is the thing three reviews on the A3 branch
objected to.

**A graph resource is not an RHI resource, and the types must not be confusable.** A pass
declares a `TextureId` — an identity within one graph build — and the graph decides what
physical resource backs it, or whether one is needed at all after culling. An `RHI::TextureHandle`
is a live GPU resource. Conflating them makes culling and aliasing unrepresentable, so
`ResourceId.h` gets its own tagged handle type and the compiler keeps them apart, in the shape
`Monarc/RHI/Handles.h` already uses.

**External resources are declared, not smuggled.** The swapchain image belongs to the
swapchain; the graph must not own or alias it, and must know its incoming layout is
`Undefined` and its outgoing layout must be `Present`. That is an *import* with a stated
before-and-after, and it is exactly what makes the two barriers derivable.

**Aliasing is decided and reported, and deliberately not yet honoured.** The graph computes
lifetimes and groups transients whose lifetimes do not overlap, and emits those groups as
inspection data. The RHI still backs every transient with its own allocation, because A3's
memory is one `VkDeviceMemory` per resource — a placeholder A3 recorded as such. Honouring an
alias group needs sub-allocation, which is the memory-allocator work A3 deferred and A4 is not
the place to do.

**Say this loudly in the code and in Status.md**: the aliasing *decision* is real and tested;
the memory saving is not yet realised. A green aliasing test must not be readable as "aliasing
works". This is the M0 principle — thin but structurally correct — and the honest half of it is
the part that usually goes unwritten.

**Inspection data is the test surface, not a debug view.** `GraphInspection.h` is a first-class
deliverable of Task 1, not a Task 5 nicety. Every subsequent task's tests assert on it. If the
inspection output cannot express something the graph decided, the graph has a decision nobody
can test — which is the failure mode Render-Graph.md predicts becomes urgent exactly when it is
first needed.

**The graph is built directly by the app in A4, and that is a stated seam.** Render-Graph.md's
destination is a graph built after extraction, consuming only the immutable render frame. There
is no `RenderScene` and no extraction until Phase B/D. So `Monarc.FirstLight` declares its pass
itself. Say so where the entry point is, the way A3's `VulkanBackend.h` states its `IBackend`
seam.

**No pass may record a barrier, and that should be structurally hard rather than a rule.**
The recording callback receives something narrower than a raw `ICommandList&` if that can be
arranged cheaply — a wrapper that offers drawing and forbids `Barrier`. If it cannot be
arranged without contorting the interface, say so and rely on review, but consider it first:
ADR-0006's central promise is that features do not write barriers, and a promise the compiler
keeps is worth more than one a comment makes.

---

## Task 1: The module, the declaration API, and inspection

- [ ] `Monarc.Render` (Runtime, tier 2, deps `Monarc.Core`, `Monarc.RHI`). Confirm
      `module-graph.json` records the edges and every gate still passes.
- [ ] `ResourceId.h`: tagged graph-resource identities, distinct from `RHI` handles.
- [ ] `Access.h`: how a pass touches a resource, expressed in ADR-0005's stage/access
      vocabulary so derivation has something to derive *from*. Attachment, sampled, storage
      and indirect reads and writes; the membership rule is `Barrier.h`'s — the model arrives
      whole where a value costs one switch row, and an access whose backend behaviour nothing
      implements waits.
- [ ] `PassBuilder.h`: what a pass declares — reads, writes, transient creation, resource
      import, and a recording callback.
- [ ] `RenderGraph.h`: the three-phase shape (declare, compile, execute) with compile and
      execute separable, because everything worth testing happens in compile.
- [ ] `GraphInspection.h` + `GraphInspection.cpp`: the emitted data — pass list in execution
      order with culled passes marked, every resource with its lifetime and alias group, every
      derived barrier **with the accesses that caused it**, and queue assignments when those
      exist. Stable, diffable text, and structured enough to assert on field by field rather
      than by string matching.

**Device-free tests.** Declaration round-trips through inspection unchanged. A pass declaring
the same resource as both read and write is representable and reported (it is legal — a
read-modify-write attachment). Declaring a resource no pass created is an error naming it.
`ResourceId` and `RHI::TextureHandle` are not interconvertible — a `static_assert`, and
demonstrate the diagnostic.

**Verification.** Six presets green. Then deliberately: make `Monarc.Render` depend on
`Monarc.Host.Windowed` and confirm gate 3 fires; add a `Monarc/Assets/` include and confirm it
fires too. **Do not skip this** — gate 10 was vacuously green for four phases because nobody
made it fail on purpose, and gate 3 had nothing real to forbid until A3.

---

## Task 2: Compile — dependency, culling, lifetimes, alias groups

All of this is pure computation over declarations. If any of it needs a device, stop and say so.

- [ ] Build the dependency graph from declared reads and writes. Detect a cycle and report it
      naming the passes, rather than looping or asserting.
- [ ] Cull passes whose outputs nothing consumes, transitively. A pass writing only to a
      culled pass's input is itself culled; a pass writing an *imported* resource is never
      culled, because something outside the graph consumes it.
- [ ] Compute each transient's lifetime — first write to last read — in execution order.
- [ ] Group transients whose lifetimes do not overlap **and whose descriptions are
      compatible**. Two textures of different formats or extents cannot share memory whatever
      their lifetimes say; the grouping must consider both.

### Device-free tests

- [ ] Culling: a diamond, a chain, a pass whose only consumer is culled, a pass writing an
      imported resource (never culled), and a graph where everything is culled.
- [ ] Lifetimes: a transient written and read in the same pass; one read several passes later;
      one written twice; and one whose last reader is culled — its lifetime must shrink.
- [ ] Aliasing: two non-overlapping lifetimes group; two overlapping do not; two
      non-overlapping with incompatible descriptions do not; and a resource never read groups
      with nothing.
- [ ] Cycles: a two-pass cycle and a three-pass cycle each report the passes involved.

**Every one of these must be able to fail.** Six cases were deleted on the A3 branch for
reporting green having asserted nothing, and 45 assertions for being tautologies. Before
keeping an assertion, ask what source change turns it red — and mutation-test the suite you end
up with rather than reading it.

---

## Task 3: Derive the barriers

The most important task in the phase, and entirely a pure function.

- [ ] Derive barriers from the transitions implied by consecutive accesses to each resource:
      stage and access scopes from the two accesses, layout from the two usages, in ADR-0005's
      model. An imported resource's declared incoming and outgoing states are the first and
      last transitions.
- [ ] Emit each derived barrier through inspection **with the accesses that caused it**, so a
      wrong barrier is debuggable as a derivation rather than as archaeology.
- [ ] A transition that is not a transition — same stage, same access, same layout — must be
      representable and must not be emitted. An unnecessary barrier is a performance bug, and a
      silently dropped necessary one is a correctness bug; the test suite must distinguish them.

### Device-free tests

- [ ] **The A3 equivalence test, and it is the phase's headline.** Given the declaration
      `Monarc.FirstLight` will make — one pass writing an imported swapchain image, incoming
      layout `Undefined`, outgoing `Present`, cleared on load — the graph derives **exactly
      two** barriers: `Undefined → ColorAttachment` with the stage and access scopes A3 used,
      and `ColorAttachment → Present` with A3's. Assert against the values in
      `Build/Captures/*.xml`, and cite the capture in the test so the reference is traceable.
- [ ] Write-after-write on the same resource yields one barrier; read-after-read yields none;
      read-after-write and write-after-read each yield one with the right direction.
- [ ] A resource with three consecutive accesses yields two barriers, not one merged or three.
- [ ] Culled passes contribute no barriers.
- [ ] A barrier is never derived for a resource a pass declared and no pass used.

**Prove the derivation can fail.** Mutate the derivation — swap a layout pair, drop a stage,
merge two barriers into one — and show which assertion catches each. A3's reviews found that
six mutations went undetected in code whose comments claimed coverage; the derivation is where
that would hurt most.

---

## Task 4: Execute, and light the window through the graph

- [ ] `Execute.cpp` records the compiled graph into an `ICommandList` through `Monarc.RHI` —
      the only file in the module that touches one. Barriers come from the derivation; passes
      contribute only their callbacks.
- [ ] Transient resources are created and destroyed around the frame, each with its own
      allocation, with the aliasing gap stated where the allocation happens.
- [ ] `Monarc.FirstLight` **stops writing barriers and stops calling `BeginRendering`
      directly.** It declares a pass that writes the imported swapchain image with a clear
      load-op, and the graph does the rest. Deleting that hand-written code is the point of the
      phase; leaving it beside the graph as a fallback would defeat it.

### Device-required tests

- [ ] The graph records and submits a frame on every deduplicated adapter, and the swapchain
      readback is still **exactly** `(192, 128, 64, 255)` BGRA on both — the same assertion A3
      passed, now through derived barriers.
- [ ] Zero validation output across the suite, and assert the messenger was installed so a
      build that failed to load the layer cannot pass as clean.
- [ ] A graph with a transient written by one pass and read by another runs clean on a device —
      the read-after-write barrier the derivation produced is the one the driver wanted.

**Verification.** Run `Monarc.FirstLight`: a window opens and clears, resize and minimise still
survive, exit zero. Then **capture a frame in RenderDoc on each adapter and compare against
A3's captures** — the same one `vkCmdBeginRendering`, the same clear value, the same two
`vkCmdPipelineBarrier2` with the same layouts. A3's capture is the reference; a difference is a
finding, not a variation.

---

## Task 5: Close-out

- [ ] Confirm gate 3 has a third tier-2 module to police, and make it fail on purpose once.
- [ ] Update [Render-Graph.md](../Rendering/Render-Graph.md) where A4 turned an intention into
      a fact, and state plainly which of its seven steps exist and which do not — parallel
      recording, queue assignment and sub-pass optimisation are all still deferred, and the
      aliasing decision is computed but not honoured.
- [ ] Update [Status.md](../Status.md) with an "A4 delivered" section: what runs, what CI
      covers (which should be most of it, unlike A3), the captures compared, and **what A4 does
      not prove** — nothing about performance, nothing about a second backend, nothing about
      transient memory actually being saved.
- [ ] Update [M0](../Milestones/M0-First-Light.md) to mark A4 complete.
- [ ] Read CI's output on the merge and confirm the device-free half genuinely covers the
      graph. If the ratio of CI-covered to device-gated coverage is not much better than A3's,
      that is a finding about the design, not about the tests.

---

## Definition of done

- `Monarc.FirstLight` clears the window through the graph, and contains **no barrier code**.
- The two barriers A3 hand-wrote are derived, asserted device-free against A3's captures, and
  confirmed on a device by a byte-exact readback and a matching fresh capture.
- Culling, lifetimes, alias grouping and derivation each have tests that were made to fail.
- The inspection output can express every decision the graph makes, and the tests assert on it
  rather than on strings.
- CI is green on all six presets, with the graph's compile half genuinely exercised there.
- Gate 3 has been made to fail on purpose with three real tier-2 modules.

---

## What A4 deliberately excludes

Parallel command recording and `Monarc.Jobs` (ADR-0006 defers it; it arrives with its first
user). Multiple queues and cross-queue synchronisation. Honouring alias groups in memory —
the decision is computed, the sub-allocation is not built. Shaders, pipelines, draws and
`Monarc.Shaders` — Phase B. `RenderScene` and the extraction boundary — Phase B/D. Depth and
forward passes, which need shaders. Sub-pass-level optimisation such as dynamic rendering local
read. Any performance claim whatsoever: two or three passes prove the machinery is correct and
say nothing about whether it is fast.

## Open questions this plan does not settle

- **What a pass declaring two accesses with disagreeing layouts should mean.** Raised by Task
  1, which found the state representable and refused by nothing. Colour attachment read plus
  write on one resource is legal — both want the same layout — and is tested. But
  `DepthStencilAttachmentRead` plus `DepthStencilAttachmentWrite` on one resource asks for two
  layouts at once, and nothing rejects it. Task 1 deliberately did not guess the rule because
  A4 has no depth pass; the derivation in Task 3 is where it becomes answerable, since that is
  the code with an opinion about what layout a resource is in.
- **A device-free stub `ICommandList`, which Tasks 3 and 4 want anyway.** Task 1 reports that
  `RenderGraph::Execute`'s stub is unreachable from any test, because it takes `RHI::IDevice&`
  and `RHI::ICommandList&` and a device-free test has neither. That is the right signature —
  weakening it to a nullable context struct to serve a test would be the tail wagging the dog
  — but it means recording is untestable without a GPU. A fake `ICommandList` that records
  what it was asked to do turns "the graph emitted exactly these two barriers, in this order,
  around this rendering pass" into a device-free assertion, which is strictly stronger than
  inspecting the derivation and hoping execution matches it. Decide it in Task 3, where the
  first thing worth asserting about recording exists.

  **The dead surface is wider than `Execute`'s four lines, and Task 3 should inherit the whole
  list rather than a corner of it.** A review of Task 1 walked it: nothing anywhere constructs
  a `PassCommandList`, so its private constructor and its `Commands()` accessor are uncalled
  too — both are pinned by `static_assert`s, which is a different thing from being run.
  `Detail::IPassRecord::Invoke` and `Detail::PassRecord::Invoke` are uncalled for the same
  reason: nothing has a `PassCommandList&` to invoke a callback with. What *is* covered is the
  stored callable's construction and destruction — `PassBuilder::Record`'s placement new and
  the slot's occupancy by the round-trip case, and `DestroyRecords`' virtual destructor call by
  *"a recording callback is destroyed by Reset and by the destructor"*, whose `Witness` counts
  its own destructions. So the gap is precisely the invocation path and nothing either side of
  it. One stub `ICommandList` closes all five at once, which is part of why it is worth
  building.
- **Whether the recording callback can be given a barrier-free command list cheaply.** Worth
  attempting in Task 1 and abandoning if it distorts the interface — but a compiler-enforced
  version of ADR-0006's central promise is worth an attempt.
- **How the graph should express a resource whose first access is a read.** Legal for an
  imported resource with a known incoming state; an error for a transient nothing wrote.
  Task 2's culling tests will force the distinction; decide it there rather than here.
- **Whether alias grouping should consider memory *size* as well as format and extent.** Two
  compatible descriptions is the conservative rule and the right starting point; a real
  allocator may want a looser one keyed on size and alignment. Revisit when the allocator
  exists rather than guessing its requirements now.
