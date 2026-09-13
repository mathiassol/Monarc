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
      naming the passes, rather than looping or asserting. **The vehicle exists**: one
      `GraphDiagnostic` per pass in the cycle, all carrying the same `group`, plus a new
      `DiagnosticKind`. `GraphDiagnostic::group` argues why a group id and not a second `pass`
      field or an inline pass list, and *"diagnostics of one report carry their group, and
      overlapping reports stay apart"* in Tests/TestGraphInspection.cpp already renders the
      shape by hand — including two cycles that share a pass, which is the case one `pass` field
      per row cannot express.
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
- [ ] Cycles: a two-pass cycle and a three-pass cycle each report the passes involved, one
      diagnostic per member sharing a `group`; and two cycles in one graph stay apart. A cycle
      longer than the diagnostics pool has room for must still leave every surviving row
      grouped, with `diagnosticsDropped` non-zero — the accounting the group id was chosen to
      compose with.

**Every one of these must be able to fail.** Six cases were deleted on the A3 branch for
reporting green having asserted nothing, and 45 assertions for being tautologies. Before
keeping an assertion, ask what source change turns it red — and mutation-test the suite you end
up with rather than reading it.

---

## Task 3: Derive the barriers

The most important task in the phase, and entirely a pure function.

- [x] Derive barriers from the transitions implied by consecutive accesses to each resource:
      stage and access scopes from the two accesses, layout from the two usages, in ADR-0005's
      model. An imported resource's declared incoming and outgoing states are the first and
      last transitions.
- [x] Emit each derived barrier through inspection **with the accesses that caused it**, so a
      wrong barrier is debuggable as a derivation rather than as archaeology.
- [x] A transition that is not a transition — same stage, same access, same layout — must be
      representable and must not be emitted. An unnecessary barrier is a performance bug, and a
      silently dropped necessary one is a correctness bug; the test suite must distinguish them.

### Device-free tests

- [x] **The A3 equivalence test, and it is the phase's headline.** Given the declaration
      `Monarc.FirstLight` will make — one pass writing an imported swapchain image, incoming
      layout `Undefined`, outgoing `Present`, cleared on load — the graph derives **exactly
      two** barriers: `Undefined → ColorAttachment` with the stage and access scopes A3 used,
      and `ColorAttachment → Present` with A3's. Assert against the values in
      `Build/Captures/*.xml`, and cite the capture in the test so the reference is traceable.
- [x] Write-after-write on the same resource yields one barrier; read-after-read yields none;
      read-after-write and write-after-read each yield one with the right direction.
- [x] A resource with three consecutive accesses yields two barriers, not one merged or three.
- [x] Culled passes contribute no barriers.
- [x] A barrier is never derived for a resource a pass declared and no pass used.

**Prove the derivation can fail.** Mutate the derivation — swap a layout pair, drop a stage,
merge two barriers into one — and show which assertion catches each. A3's reviews found that
six mutations went undetected in code whose comments claimed coverage; the derivation is where
that would hurt most.

### What Task 3 decided, and what it added

**The model.** A resource's accesses in execution order are a *chain of states*: an import's
declared incoming state or a transient's creation, one state per surviving pass, and for an
import its declared outgoing state. A gap in the chain becomes a barrier **unless the two
states are identical and neither side of the gap is a state a pass wrote**. The write term is
what keeps write-after-write — identical layout, identical scopes, and mandatory — from being
suppressed as a no-op, and it reads **both** sides for a reason each end supplies: a pass writing
into a state identical to its import's declared *incoming* one still gets its barrier, because
the graph cannot order against work it was only told about; and a pass whose write is followed by
an *outgoing* state that names a write of its own still gets one, because that is a
write-after-write across the graph boundary that nothing else in the frame orders.

**An earlier revision of this section read the write term on the side after the gap only, and
that was wrong at the outgoing end** — with no layout change there is nothing for a validation
layer to object to either, so the dropped barrier was silent corruption rather than a reported
error. The rule above is what `Source/Monarc.Render/Private/DeriveBarriers.cpp` implements and
argues, and Task 3's review is where the one-sided reading was found.

**Three additions, recorded as additions rather than checkboxes**, in the shape
`RefuseUnorderedOverwrites` uses, so a later reader does not preserve them for the wrong reason:

- **A transient gets an opening transition from `TextureLayout::Undefined`.** The checklist
  above names only an import's two declared ends. A transient has neither, and
  `IDevice::CreateTexture` asks for `VK_IMAGE_LAYOUT_UNDEFINED` — so a derivation that took the
  line literally would render into an image still in `Undefined`.
  `BarrierCauseKind::TransientCreation` is the cause that says so without claiming a declaration
  the resource does not have.
- **`DiagnosticKind::BarrierPoolExhausted`.** The derivation is the first thing to fill a pool
  the *graph* sizes, and it refuses rather than growing, like every other pool here. It is the
  one refusal `Compile` makes after a frame has been decided, so it leaves the order, the
  culling, the lifetimes, the groups and a partial barrier list in the report.
- **A barrier cause that is not a `PassAccess` renders its access as `none`.**
  `BarrierCauseSide::access` is filler on those sides, and the text was printing the filler's
  enumerator — an import's incoming state read as a colour attachment read.

**An imported resource no surviving pass touches is left alone.** No incoming transition, no
outgoing one. Emitting `incoming → outgoing` would be a barrier for an operation the frame did
not perform, and its cause would name two import ends with no access between them — in the field
that exists so a barrier is traceable to the accesses that asked for it. Culling never drops a
pass that *writes* an import, so the only way to reach this is a read-only import whose readers
were all culled; the report says exactly that, with the pass marked `culled` and the lifetime
empty.

**What a richer access set would want here and this one cannot use: merging a run of reads.**
The chain is walked pairwise. A graph that folded consecutive readers into one state would emit
one widened barrier in front of the first of them — but with `ResourceAccess` as it stands the
two answers cannot differ, because every read a `TextureId` can carry names a layout of its own,
so two consecutive reads are either the same access (one state, no barrier) or two layouts (a
transition no merge removes). It becomes a real question when two reads share a layout, or when
`BufferId` arrives and a read has no layout at all.

---

## Task 4: Execute, and light the window through the graph

- [ ] `Execute.cpp` records the compiled graph into an `ICommandList` through `Monarc.RHI` —
      the only file in the module that touches one. Barriers come from the derivation; passes
      contribute only their callbacks.
- [ ] Transient resources are created and destroyed around the frame, each with its own
      allocation, with the aliasing gap stated where the allocation happens.
- [ ] **Re-run `PassCommandList`'s member enumeration, because this task is the edit that
      invalidates it.** The `static_assert`s in Tests/TestPassDeclaration.cpp that keep
      ADR-0006's central promise are *name-based*: `kCanGetCommandList` and
      `kCanReachCommandListPointer` detect `Commands()` and `m_commands` being re-exposed, and
      `is_convertible_v` catches a conversion operator — but a **newly named** public accessor
      returning `RHI::ICommandList&` is invisible to all of them. Task 4 is when `Execute`
      first constructs one of these and first has a reason to forward a recording call through
      it, so adding a forwarding method is the moment to enumerate the class again rather than
      to read a green suite as coverage. Add a guard per new member that can yield a list.
- [ ] `Monarc.FirstLight` **stops writing barriers and stops calling `BeginRendering`
      directly.** It declares a pass that writes the imported swapchain image with a clear
      load-op, and the graph does the rest. Deleting that hand-written code is the point of the
      phase; leaving it beside the graph as a fallback would defeat it.
- [ ] **Build the device-free stub `ICommandList`, which Task 3 decided and deferred to here.**
      The decision is yes: it closes five uncalled things at once — `Execute`'s body,
      `PassCommandList`'s private constructor and `Commands()`, and both `Invoke` bodies — and
      turns *"the graph emitted exactly these barriers, in this order, around this rendering
      pass"* into a device-free assertion, which is strictly stronger than inspecting a
      derivation and hoping execution matches it. It is Task 4's rather than Task 3's because
      Task 3 derives and does not record, so a stub built there would have had no caller, which
      is the thing this codebase's "arrives with its first user" rule exists to prevent.
      `PassCommandList`'s own comment already names the `ForTesting` factory that arrives with
      it.

      **What it should record, from Task 3's side:** each `Barrier` it was handed, in order,
      with all six ADR-0005 fields and the texture; each `BeginRendering`/`EndRendering` pair;
      and `Begin`/`End`. That is enough to assert the two things inspection cannot — that the
      derived barriers reach the list *at all*, and that each lands on the side of
      `BeginRendering` its `emittedBeforePass` says it does.

- [ ] **The declaration `Monarc.FirstLight` makes is Task 3's to hand over, and it is this**, with
      the reasoning at `SwapchainImport` in Tests/TestDeriveBarriers.cpp: incoming
      `{Undefined, ColorAttachmentOutput, None}`, outgoing `{PresentSource, None, None}`. Five of
      those six values are forced by the swapchain contract; the sixth, `incoming.stage`, is
      `ColorAttachmentOutput` because `VulkanDeviceState::SubmitList` waits on the acquire
      semaphore at that stage and a layout transition is a write that has to be ordered after
      that wait. **Do not change it to make a capture match** — the derivation is a function of
      it, which *"the derived barriers are a function of the declared import states"* pins.

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

- ~~**What a pass declaring two accesses with disagreeing layouts should mean.**~~ **Answered by
  Task 3: refused at declaration, with `DiagnosticKind::AccessLayoutConflict`.** The derivation
  combines a pass's accesses to one resource into a single required state — one layout, the union
  of the stages, the union of the accesses — because a texture is in one layout at a time and a
  barrier cannot be recorded inside a rendering instance, so there is nowhere to put one between a
  pass's own two accesses. Two layouts have no combination, and the derivation would have had to
  pick one arbitrarily and transition into it. The refusal is at declaration rather than at
  compile because `RequirementOf` is `constexpr` and the conflict is visible in the declarations
  alone, and it forecloses nothing: a combined read-and-write depth access is one more
  `ResourceAccess` enumerator with one layout, and it stops being a conflicting pair on the day it
  arrives. Colour read plus write is untouched — both ask for `ColorAttachment` — and stays
  tested as legal.
- ~~**A device-free stub `ICommandList`, which Tasks 3 and 4 want anyway.**~~ **Decided by Task 3:
  yes, and built in Task 4**, where its first caller is. See the checkbox added to Task 4 for what
  it should record and why it is not Task 3's to build. The original question follows.

  Task 1 reports that
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
- ~~**Task 3 has no inter-pass write-after-read declaration to exercise, and its checklist line
  assumes one.**~~ **Task 3 wrote against the two shapes it can be given, and found that the
  intra-pass one is not a barrier at all.** A read-modify-write pass's two accesses combine into
  one state, so there is no gap inside it and nothing to emit — the write-after-read shows up in
  the *combined access mask* of the barrier that reaches the pass, which is what a derivation
  that dropped the read half would get wrong. The import's outgoing transition after a read is a
  genuine one and is asserted as such. Both have a case; neither is the inter-pass
  anti-dependency, and the original analysis follows unchanged.

  *"Read-after-write and write-after-read each yield one with the right
  direction"* is written as though a frame could declare a read that precedes an overwrite. It
  cannot. Task 2 found the reason: an edge runs from every writer of a resource to every
  different pass that reads it, so a writer is before a reader in **every** topological order
  the sort can produce, and a read that is meant to happen before another pass's write is not
  merely untested but unrepresentable. Task 2 turned the frame that needs it — *A renders into
  T, B samples T, C reuses T as scratch* — from a silently wrong order into a named refusal,
  `DiagnosticKind::UnorderedOverwrite`.

  So the write-after-read instances the derivation can actually be given are two, and Task 3
  must be written against those rather than against a shape it cannot get: an **intra-pass**
  one, where a read-modify-write pass reads and then writes one resource, and an imported
  resource's **declared outgoing transition**, whose last access may be a read. Both are worth
  a case; neither is the inter-pass anti-dependency the line implies.

  **What would restore the general case is resource versioning** — a write produces a new
  version, a read names one — which is a change to resource *identity* reaching `ResourceId`,
  `PassBuilder`, lifetimes and aliasing. It is deliberately not in A4: M0 needs no frame that
  reuses a resource after reading it, A4 has one pass and one imported resource, and Phase B's
  depth and forward passes consume rather than reuse. It arrives with its first user, and the
  refusal foreclosing nothing is what makes that safe — a declaration refused today becomes an
  accepted one, and no call site changes shape.
