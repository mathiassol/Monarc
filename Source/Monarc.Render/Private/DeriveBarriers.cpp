#include <Monarc/Render/RenderGraph.h>

#include <Monarc/Core/Assert.h>

#include <span>

// Deriving the barriers: the transitions the declared reads and writes imply, in ADR-0005's
// model.
//
// **A pure function over the declarations and the stages before it, with no device anywhere in
// it** -- Private/Compile.cpp's opening argues that for the whole of compilation and this file
// keeps to it. The only RHI types named here are `PipelineStage`, `Access` and `TextureLayout`,
// which are the three parts of ADR-0005's barrier and are exactly what is being derived *into*.
// There is no `RHI::TextureHandle` on any path: a `DerivedBarrier` names a `TextureId`, because
// a compiled graph has no live texture for a transient and a derivation that needed one could
// not run without a GPU. Task 4's `Execute` is what turns one of these plus a resolved handle
// into an `RHI::TextureBarrier`.
//
// ---------------------------------------------------------------------------------------
// The model: a chain of states per resource, and one barrier per gap that is a transition.
//
// **A resource passes through a sequence of states, and a barrier is what moves it from one to
// the next.** The sequence for one resource is:
//
//   1. **Its first state**, which nothing in the graph performed:
//      - an **imported** resource's declared `ResourceInspection::incoming`, which is what the
//        importer said about a resource that reached the graph from outside it;
//      - a **transient**'s creation, which is `TextureLayout::Undefined` with no stage and no
//        access, because `IDevice::CreateTexture` asks for `VK_IMAGE_LAYOUT_UNDEFINED` and a
//        freshly created image has nothing to wait for and nothing to make available.
//   2. **One state per surviving pass that touches it**, in execution order.
//   3. **Its last state**, for an imported resource only: the declared
//      `ResourceInspection::outgoing`, which is what the importer requires the graph to leave it
//      in. A transient has no last state -- the graph destroys it, and a transition into a
//      layout nothing will read is work for nobody.
//
//      **That is correct today for two reasons, and one of them expires.** `vkDestroyImage`
//      imposes no layout requirement, so nothing on the far side of the destruction has a state
//      to be moved into; and `GroupAliases` computes alias groups without honouring them --
//      every transient still gets its own allocation, which that function says where it says it
//      -- so no second resource ever reuses the memory. **When aliasing is honoured, the second
//      reason goes and a barrier between two aliased transients becomes necessary**: the later
//      one's first write is a hazard against the earlier one's last access even though the two
//      are different resources, and the later one's transition out of `Undefined` is what
//      discards the contents it is inheriting. That barrier belongs to the gap *between* two
//      transients in one group, which is not a gap this chain has -- so what sub-allocation
//      needs is a new pairing in the walk below, not a closing state added to this list. Left
//      here rather than for whoever implements it to rediscover.
//
// That is the phase plan's sentence -- "an imported resource's declared incoming and outgoing
// states are the first and last transitions" -- with the transient's own two ends made explicit,
// because a transient that got no opening transition would be rendered into while still in
// `Undefined`.
//
// **One state per pass and not one per access, which is what makes a read-modify-write
// attachment work.** A pass declaring both `ColorAttachmentRead` and `ColorAttachmentWrite` on
// one resource needs it in one layout for the whole pass: a barrier cannot be recorded inside a
// rendering instance at all (see `ICommandList::Barrier`), so there is no place to put one
// between a pass's own two accesses even if one were wanted. So a pass's accesses to one
// resource are *combined* -- one layout, the union of the stages, the union of the accesses --
// by `CombinePassStep` below. The combination is total because `PassBuilder::Read`/`Write`
// refuse a pass that declares two accesses to one resource whose layouts disagree; see
// `DiagnosticKind::AccessLayoutConflict`, which is the open question this file's existence
// answered.
//
// **Culled passes contribute no state**, so they contribute no barrier, which is the plan's
// checkbox and falls out of walking the surviving passes rather than all of them. A resource no
// surviving pass touches gets no chain at all -- not even its import's two ends. See
// `DeriveBarriers` for that decision and its argument.
//
// ---------------------------------------------------------------------------------------
// When a gap is a transition, which is the rule the whole file turns on.
//
//     A gap emits a barrier unless the two states are identical **and** neither side of it is
//     a state a pass wrote.
//
// Three cases, and each of them is a checkbox:
//
// - **The states differ.** A layout change, a different stage to synchronise, or a different
//   access to make available or visible: all of them need the barrier. Read-after-write and
//   write-after-read are here, and so is every transition an import's two ends produce.
// - **The states are identical and a pass wrote one of the two sides.** Write-after-write: two
//   passes writing a colour attachment with the same access need the same layout and the same
//   scopes, so their states are equal -- and the second write still has to be ordered after the
//   first and see its result. **This is the one place where "same stage, same access, same
//   layout" and "not a transition" come apart, and correctness is what decides it**: a barrier
//   between two writes with identical scopes is not a no-op, it is exactly what a
//   write-after-write hazard needs. A rule written on equality alone would drop it and would
//   still pass every read-after-read case there is.
// - **The states are identical and no pass wrote either side.** Read-after-read, and the
//   "transition that is not a transition" the plan requires be representable and not emitted.
//   Two passes sampling one texture at the same stages have no hazard between them and no
//   layout to change; a barrier here would be a performance bug.
//
// **Why the write term reads both sides, and what a rule that read only the side after the gap
// got wrong.** Between two *passes* the two sides cannot disagree about writing while their
// states are equal: a pass step's access mask is the union of `RequirementOf`'s access bits, the
// write bits come only from accesses that write, so two pass steps with equal masks either both
// wrote or neither did. Tests/TestDeriveBarriers.cpp pins that premise with a `static_assert`
// over every `ResourceAccess`. At a chain's *incoming* end the two forms agree as well, because
// the step before the gap is an import's declared state or a creation and `passWrites` is false
// there by construction. So the `from` term changes the answer in exactly one place -- the gap
// between the last pass and an import's declared **outgoing** state -- and that is the place a
// one-sided rule got wrong:
//
// - **An import's outgoing state is a dst scope, not a layout wish.** `StepResource` puts
//   `outgoing.stage` and `outgoing.access` into `syncAfter` and `accessAfter` -- the half of a
//   barrier that names the work it is made visible *to* -- and `TextureImport`'s own
//   documentation reads them the same way: a swapchain's `outgoing.stage` is `None` because
//   there is no *command* after the transition. So an outgoing state that names a **write**
//   access is a statement that external commands will write the resource, and if the last pass
//   wrote it too, that gap is a write-after-write across the graph boundary. Nothing else in
//   the frame orders it -- two submissions are not ordered by being submitted in order -- and
//   with no layout change there is nothing for a validation layer to object to either, so
//   dropping it is silent corruption rather than a reported error. The graph owes the barrier.
// - **What stays suppressed at that end is a requirement a *read* already met.** If the last
//   pass only read, `passWrites` is false on both sides, and equal states then say the importer
//   asked for the scope the resource is already visible in: read-after-read across the
//   boundary, with nothing to order and nothing to make available.
// - **An import's incoming state is a fact about work the graph cannot see**, so a pass writing
//   into a state identical to it *does* get its barrier -- the side after the gap is a writing
//   pass. That is the conservative answer and the right one: the only way for the two to be
//   identical is for the importer to have declared a write of its own, and the graph has no way
//   to order against work it was only told about.
//
// **The two ends read alike now, and the reason the one-sided rule looked right is worth
// keeping.** Its argument was that an outgoing state is "a requirement already met" -- which
// conflates *"is in layout L"* with *"is available and visible to scope S"*. Those are different
// claims, and the incoming end above refuses to conflate them four lines up. The rule used to
// make that conflation at one end while refusing it at the other.
//
// **The phase plan's own two lines conflict here, and this is where the file deviates from one
// of them twice.** "same stage, same access, same layout must not be emitted" and
// "write-after-write yields one barrier" cannot both hold for a gap between two equal states
// that a pass wrote. The second wins: once between two passes, which the plan expected, and
// once at an import's outgoing end, which it did not.
//
// ---------------------------------------------------------------------------------------
// What a graph with a richer access set would do here and this one does not: merge a run of
// reads.
//
// A render graph that expects several consecutive readers with *different* scopes usually folds
// them into one read state, so that one widened barrier in front of the first reader serves all
// of them instead of one barrier per change of scope. This file does not, and walks the chain
// pairwise.
//
// **With `ResourceAccess` as it stands the two answers cannot differ, which is why it is left
// out rather than deferred with a promise.** Every read a `TextureId` can carry names a layout of
// its own -- `ColorAttachmentRead` wants `ColorAttachment`, `DepthStencilAttachmentRead` wants
// `DepthStencilReadOnly`, `SampledRead` wants `ShaderReadOnly`, `StorageRead` wants `General` --
// so two consecutive reads of a texture are either the *same* access, which is one state and no
// barrier at all, or two accesses whose layouts disagree, which needs a real layout transition
// that no merge could remove. There is no run to merge.
//
// So the merge becomes a real question only when something makes two reads share a layout: a
// second read access mapping onto an existing layout, or `BufferId`, where there is no layout and
// scope is the only thing a read has. It arrives with that, which is the rule this codebase
// applies to every enumerator and field.
//
// **What does differ pairwise today is a gap whose layout is unchanged and whose scope is not**,
// which an import's declared incoming or outgoing state produces: an importer that says
// "visible to the fragment shader" for a texture a pass samples in all three shader stages needs
// the scope widened, with no layout to change. That barrier is emitted, and
// *"a gap that changes only the synchronisation scope is still a transition"* in
// Tests/TestDeriveBarriers.cpp is what pins it -- a rule that compared layouts instead of states
// would drop it, which is how the case came to be written.
//
// **`TextureState` has three halves and each of them now has a case that moves it alone**, which
// the paragraph above found the first of and stopped at. A gap that moves only the **access** is
// a read-after-write in one layout -- a storage image written and then read -- and one that
// moves only the **layout** is an image handed over in `General` for one a pass wants in
// `ShaderReadOnly`. Both are reachable only across an import's declared state, for the same
// reason the scope gap is: a pass step's three halves all come from `RequirementOf`, whose
// access bits are one per row, so two pass steps with equal access masks have equal layouts and
// equal stages -- no gap inside the graph moves the layout alone or the stage alone. Two of them
// *can* move the access alone, at the two layouts a read and a write share (`General` and
// `ColorAttachment`), and in every such pair at least one side writes -- so the write term emits
// that gap before the comparison decides anything. Each half has its own case, because a rule
// that dropped any one of the three passed every case in the file that did not.
// ---------------------------------------------------------------------------------------

namespace Monarc::Render {

bool RenderGraph::IsTransition(const ResourceStep& from, const ResourceStep& to) {
    return !(from.state == to.state) || from.passWrites || to.passWrites;
}

void RenderGraph::StartResourceStep(u32 resource) {
    const ResourceInspection& declared = m_resources[resource];

    ResourceStep step{};
    // `passWrites` stays false at both ends of every chain -- see the head of this file. The
    // `access` field of each cause stays at its default too, because it is filler for a side
    // that is not a `PassAccess`; `BarrierCauseSide::access` says so.
    if (declared.origin == ResourceOrigin::Imported) {
        step.state      = declared.incoming;
        step.cause.kind = BarrierCauseKind::ImportIncoming;
    } else {
        // **Spelled out rather than left to `TextureState{}`'s defaults**, which happen to be
        // these three values: what is being stated is what `IDevice::CreateTexture` leaves
        // behind, and a reader of this line should see it rather than have to look the defaults
        // up. A change to those defaults must not quietly change the derivation.
        step.state = TextureState{RHI::TextureLayout::Undefined, RHI::PipelineStage::None,
                                  RHI::Access::None};
        step.cause.kind = BarrierCauseKind::TransientCreation;
    }
    m_resourceStep[resource] = step;
}

RenderGraph::ResourceStep RenderGraph::CombinePassStep(u32 pass, u32 resource) const {
    ResourceStep step{};
    step.cause.kind = BarrierCauseKind::PassAccess;
    step.cause.pass = m_passes[pass].executionOrder;

    bool first = true;
    for (const u32 index : AccessesOfPass(pass)) {
        const AccessInspection& access = m_accesses[index];
        if (access.resource.index != resource) {
            continue;
        }

        const AccessRequirement requirement = RequirementOf(access.access);
        if (first) {
            step.state.layout = requirement.layout;
            // The access the report blames, until a write is found below. For a pass that only
            // reads, the first declared read is the answer.
            step.cause.access = access.access;
            first             = false;
        } else {
            // **The invariant `DiagnosticKind::AccessLayoutConflict` exists to hold**, checked
            // where it is relied on. `MONARC_CHECK` reports and never alters control flow, so a
            // build that somehow reached here with two layouts still derives a barrier into the
            // first one -- the check is what says the derivation was handed something it
            // refuses to be handed, not a second refusal.
            MONARC_CHECK(requirement.layout == step.state.layout,
                         "a pass's accesses to one resource must agree on a layout");
        }

        // **The stage union has no case, and cannot be given one with today's `ResourceAccess`.**
        // Two accesses reach this loop together only if they agree on a layout --
        // `DiagnosticKind::AccessLayoutConflict` refuses the rest, and `IndirectRead` never names
        // a texture at all -- and the only pairs that agree are `ColorAttachment`'s read and
        // write and `General`'s. Each of those pairs shares a stage set as well, so `|=` and a
        // plain `=` give the same answer for every frame that can be declared: a mutation
        // dropping this union survives the suite, and a case written to catch it could not fail.
        // That is expected rather than a gap, and is recorded here so the next reader does not
        // spend the hunt. The access union below *is* observable, because those same two pairs
        // differ in exactly the access. Both are written as unions because the combination is a
        // union, and the day a second access maps onto an existing layout with a stage set of
        // its own this line is already right.
        step.state.stage |= requirement.scope.stage;
        step.state.access |= requirement.scope.access;

        if (requirement.writes) {
            step.passWrites = true;
            // **The write is what the report blames when the pass writes**, in preference to the
            // first access. A read-modify-write pass's read and its write are both true of the
            // pass, and the write is the half a barrier beside it is about: it is what has to be
            // made available on the before side and what has to be ordered on the after side. The
            // pass is named too, so a reader who wants the other access finds it in
            // `GraphInspection::accesses`.
            //
            // **No tie-break between two writes, because a pass cannot declare two.** Every
            // layout `RequirementOf` names is required by at most one access that writes, so two
            // writes of one resource in one pass either repeat an access --
            // `DiagnosticKind::DuplicateAccess` -- or disagree about a layout --
            // `DiagnosticKind::AccessLayoutConflict`. Tests/TestDeriveBarriers.cpp asserts the
            // premise over the whole access set rather than leaving it to this comment. A guard
            // here reading `!step.passWrites` would state a preference for the *first* write that
            // no declaration can exercise, and which write to blame is a question for whoever
            // makes two of them declarable.
            step.cause.access = access.access;
        }
    }

    return step;
}

Status RenderGraph::StepResource(u32 resource, u32 declaringPass, const ResourceStep& next) {
    const ResourceStep current = m_resourceStep[resource];
    m_resourceStep[resource]   = next;

    if (!IsTransition(current, next)) {
        return {};
    }

    DerivedBarrier barrier{};
    barrier.resource = m_resources[resource].id;
    // **`next.cause.pass`, which is `kNoPass` for an import's outgoing state** -- the one kind
    // of barrier recorded after every pass rather than in front of one, which is what
    // `DerivedBarrier::emittedBeforePass` documents. A `PassAccess` side always carries a real
    // execution position, because `CombinePassStep` is the only thing that builds one and it
    // reads `PassInspection::executionOrder` of a surviving pass.
    barrier.emittedBeforePass = next.cause.pass;

    barrier.layoutBefore = current.state.layout;
    barrier.layoutAfter  = next.state.layout;
    barrier.syncBefore   = current.state.stage;
    barrier.syncAfter    = next.state.stage;
    barrier.accessBefore = current.state.access;
    barrier.accessAfter  = next.state.access;

    barrier.cause.before = current.cause;
    barrier.cause.after  = next.cause;

    if (m_barriers.Size() >= m_config.maxBarriers) {
        // **The barriers derived so far are kept**, exactly as the accesses declared before an
        // `AccessPoolExhausted` are kept: the phase becomes `GraphPhase::CompileFailed`, so the
        // list is a partial record of a refused build rather than a frame. Discarding it would
        // remove the only evidence of how far the derivation got.
        //
        // `declaringPass` and not `barrier.emittedBeforePass`: `GraphDiagnostic::pass` is a
        // declaration index and `emittedBeforePass` is an execution position, and the two are
        // different numbers in any frame whose passes were reordered or culled. `declaringPass`
        // is also `kNoPass` for an import's outgoing transition, which belongs to no pass at all
        // -- reporting `0` there would name the first pass declared, which had nothing to do with
        // it. Both halves are pinned: Tests/TestDeriveBarriers.cpp overflows the pool in a
        // reordered frame, where the two numbers differ, and again at an end-of-frame barrier.
        return std::unexpected(Refuse(DiagnosticKind::BarrierPoolExhausted, ErrorCode::OutOfMemory,
                                      "RenderGraph::Compile: barrier pool exhausted",
                                      declaringPass, barrier.resource));
    }
    m_barriers.Push(barrier);
    return {};
}

Status RenderGraph::DeriveBarriers() {
    const u32 resourceCount = static_cast<u32>(m_resources.Size());
    const u32 passCount     = static_cast<u32>(m_passes.Size());

    for (u32 resource = 0; resource < resourceCount; ++resource) {
        StartResourceStep(resource);
    }

    // **The surviving passes in execution order, which is the order a frame records them and
    // therefore the order `GraphInspection::barriers` comes out in.** Within one pass the
    // resources come out in the order that pass declared its accesses, which is stable for the
    // same declarations -- the property `WriteInspectionText` promises about the whole report.
    for (u32 position = 0; position < passCount; ++position) {
        const u32 pass = m_passOrder[position];
        if (m_passes[pass].culled) {
            continue;
        }

        const std::span<const u32> declared = AccessesOfPass(pass);
        for (usize i = 0; i < declared.size(); ++i) {
            const u32 resource = m_accesses[declared[i]].resource.index;

            // **One step per (pass, resource) pair and not one per access.** A pass that
            // declares a read and a write of one resource reaches this line twice and must
            // produce one state, so the second arrival is skipped -- `CombinePassStep` already
            // folded it in. A scan of the accesses this pass declared *before* this one is what
            // decides which arrival is the first, over a list bounded by what one pass declares.
            bool firstForThisPass = true;
            for (usize earlier = 0; earlier < i; ++earlier) {
                if (m_accesses[declared[earlier]].resource.index == resource) {
                    firstForThisPass = false;
                    break;
                }
            }
            if (!firstForThisPass) {
                continue;
            }

            if (Status stepped = StepResource(resource, pass, CombinePassStep(pass, resource));
                !stepped) {
                return stepped;
            }
        }
    }

    // **The imported resources' outgoing transitions, last and in declaration order**, because
    // they are recorded after every pass -- there is no execution position that orders them
    // against each other, so the resource list's own order is what makes the report stable.
    for (u32 resource = 0; resource < resourceCount; ++resource) {
        const ResourceInspection& declared = m_resources[resource];
        if (declared.origin != ResourceOrigin::Imported) {
            continue;
        }
        // **`IsUnused()` and not `HasNoWrite()`, and the difference is the whole reason
        // `ResourceLifetime` has two queries.** A read-only imported resource has no first
        // *write* and is genuinely used, with a real `lastPass`; asking `HasNoWrite()` here would
        // skip exactly the resource class whose outgoing transition matters most -- a texture the
        // frame sampled and must hand back in a stated layout. `IsUnused()` is the question being
        // asked: did any surviving pass touch this at all?
        //
        // **An import no surviving pass touches is left alone, and that is a decision.** The
        // alternative is to emit `incoming -> outgoing` for it, and that would be a barrier for
        // an operation the frame did not perform: the graph never used the resource, so claiming
        // to have moved it says something the frame did not do, and the barrier's own cause would
        // name no access at all -- two import ends with nothing between them, in a field that
        // exists precisely so that a barrier is traceable to the accesses that asked for it.
        // What the report says instead is the truth: every pass that named the resource is marked
        // `culled`, and the resource's lifetime is empty. Culling keeps every pass that *writes*
        // an import, so the only way to reach this is a read-only import whose readers were all
        // culled.
        if (declared.lifetime.IsUnused()) {
            continue;
        }

        ResourceStep outgoing{};
        outgoing.state      = declared.outgoing;
        outgoing.cause.kind = BarrierCauseKind::ImportOutgoing;
        if (Status stepped = StepResource(resource, kNoPass, outgoing); !stepped) {
            return stepped;
        }
    }

    return {};
}

}  // namespace Monarc::Render
