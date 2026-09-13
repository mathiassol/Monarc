#include <Monarc/Render/RenderGraph.h>

#include <Monarc/Core/Assert.h>

#include <span>

// Execution: the only file in this module that touches an `RHI::ICommandList`, and the only one
// that touches an `RHI::IDevice`.
//
// **Everything above this file is a pure function of the declarations; everything in it is the
// frame.** `Compile` -- the dependency graph, the culling, the lifetimes, the alias groups and
// the derivation -- runs on a machine with no GPU, which is where CI runs, and that is what lets
// Tests/TestExecute.cpp assert on what *this* file records without one too: the stub
// `RHI::ICommandList` in Private/TestSupport/ is handed the same calls a driver would be, and a
// test reads them back in order. The phase plan's inversion of A3 is exactly that -- "the graph
// emitted exactly these barriers, in this order, around this rendering pass" is an assertion
// rather than a screenshot.
//
// ---------------------------------------------------------------------------------------
// The recording order, which is the deliverable.
//
//   1. `Begin`
//   2. for each surviving pass in execution order:
//        every `DerivedBarrier` whose `emittedBeforePass` is that pass's position,
//        `BeginRendering` if the pass declared colour attachments,
//        the pass's recording callback through a `PassCommandList`,
//        `EndRendering` if one was begun
//   3. every `DerivedBarrier` with `emittedBeforePass == kNoPass` -- the imports' outgoing
//      transitions, which belong to no pass and are recorded after all of them
//   4. `End`
//
// **The barrier order inside one step is `GraphInspection::barriers`' own, read and not
// re-derived.** That field documents both of its tie-breaks -- within a pass the barriers follow
// that pass's access list, and the end-of-frame ones follow the resource list -- and the
// derivation produced the whole list in exactly the order a frame records it. So the loops below
// select from that list by `emittedBeforePass` and keep the order they find, which is a
// selection rather than a sort: a sort here would be a second opinion about an order that is
// already settled, free to disagree with the report.
//
// **A pass with no attachments gets no `BeginRendering` at all**, and its callback still runs.
// `PassBuilder::ColorAttachment` states it from the declaration side; a compute pass is the
// obvious future case and a pass that only copies is one today.
//
// ---------------------------------------------------------------------------------------
// What happens to the command list when this refuses.
//
// **Two classes, and a caller has to be able to tell them apart.** The phase refusals happen
// before `Begin` and leave `commands` untouched; every other refusal -- a texture the device
// would not create, a rendering pass the list would not begin -- happens after it, and leaves
// the list **recording and abandoned mid-frame**. That is not a hypothetical: `BeginRendering`
// returns a `Status` precisely so that a caller finds out, and the only honest thing to do with
// a half-recorded frame is to drop it rather than submit it. There is no attempt to unwind:
// `ICommandList` has no way to discard what was recorded, and `End`-ing a list whose rendering
// instance is still open is itself refused.
//
// The one ordering that keeps the untouched class as wide as it can be is transient creation,
// which happens **before** `Begin` for exactly that reason -- a device that refuses a texture
// then costs the caller nothing but the textures it did create, which the graph owns and `Reset`
// destroys.
//
// **Nothing here records a `GraphDiagnostic`.** `RenderGraph::Refuse` is for refusals about the
// declarations, and a row added during execution would land in a report whose `GraphPhase` reads
// `Compiled` -- saying the build succeeded and carrying a refusal at once. The `Status` is the
// report execution gets, and it carries the device's or the list's own message unchanged where
// there is one.
// ---------------------------------------------------------------------------------------

namespace Monarc::Render {

void RenderGraph::DestroyTransients() {
    if (m_transientDevice == nullptr) {
        return;
    }
    for (RHI::TextureHandle& texture : m_transientTextures) {
        // Safe on an invalid handle -- `IDevice::DestroyTexture` says so, because "destroy what
        // may or may not still exist" is what a teardown path actually has -- so this needs no
        // guard per slot. The handle is cleared either way, since a slot that kept a destroyed
        // handle would name whatever texture the device puts in that slot next.
        m_transientDevice->DestroyTexture(texture);
        texture = RHI::TextureHandle{};
    }
    m_transientDevice = nullptr;
}

Status RenderGraph::CreateTransients(RHI::IDevice& device) {
    for (u32 resource = 0; resource < static_cast<u32>(m_resources.Size()); ++resource) {
        const ResourceInspection& declared = m_resources[resource];
        if (declared.origin != ResourceOrigin::Transient) {
            // An import's texture is the importer's and was named in the declaration. The graph
            // does not own it, does not create it and must not destroy it.
            continue;
        }
        if (declared.lifetime.IsUnused()) {
            // **No surviving pass touches it, so nothing is allocated for it at all.** That is
            // what culling buys in memory rather than only in recorded commands, and it is the
            // same predicate the derivation uses to decide that a resource has no chain --
            // which is why every barrier below resolves to a texture that exists.
            continue;
        }
        if (m_transientTextures[resource].IsValid()) {
            // Already created by an earlier `Execute` of this same build. Creating a second one
            // would leak the first, since only this array is destroyed.
            continue;
        }

        // ---------------------------------------------------------------------------------
        // **This is where the aliasing gap is, and it is one call wide.** `GroupAliases` has
        // already decided which transients *could* share memory -- their lifetimes do not
        // overlap and their descriptions agree -- and reported it in
        // `ResourceInspection::aliasGroup`. **Nothing below honours it.** Every transient gets
        // its own `CreateTexture`, and therefore its own allocation, because Monarc's memory is
        // one allocation per resource and sharing one needs sub-allocation from an allocator
        // that does not exist. So a populated alias group is a decision that was computed and
        // reported, never memory that was saved, and a green aliasing test says nothing about
        // bytes. Honouring it is the memory-allocator work A3 deferred; what would change here
        // is this call and, as Private/DeriveBarriers.cpp records, the derivation's pairing
        // between two transients that share a group.
        // ---------------------------------------------------------------------------------
        const Result<RHI::TextureHandle> created = device.CreateTexture(declared.description);
        if (!created) {
            return std::unexpected(created.error());
        }
        m_transientTextures[resource] = *created;
        m_transientDevice             = &device;
    }
    return {};
}

RHI::TextureHandle RenderGraph::TextureOf(u32 resource) const {
    return m_resources[resource].origin == ResourceOrigin::Imported
               ? m_resources[resource].importedTexture
               : m_transientTextures[resource];
}

Status RenderGraph::RecordBarriersAt(u32 position, RHI::ICommandList& commands) {
    for (const DerivedBarrier& barrier : m_barriers) {
        if (barrier.emittedBeforePass != position) {
            continue;
        }

        const RHI::TextureHandle texture = TextureOf(barrier.resource.index);

        // **An invariant between two loops in this file, which is what a check is for.** A
        // barrier exists only for a resource some surviving pass touched, and `CreateTransients`
        // creates a texture for exactly that set -- so this handle is valid for every barrier a
        // declaration can produce, and no test reaches the refusal below. It is here anyway
        // because the alternative is not a wrong frame: `ICommandList::Barrier` returns void and
        // answers a handle it cannot resolve by ending the process, so a broken invariant would
        // take the program with it rather than reporting.
        MONARC_CHECK(texture.IsValid(),
                     "a derived barrier names a resource execution has no texture for");
        if (!texture.IsValid()) {
            return Err(ErrorCode::InvalidArgument,
                       "RenderGraph::Execute: a derived barrier names a resource with no "
                       "texture");
        }

        commands.Barrier(RHI::TextureBarrier(texture, barrier.layoutBefore, barrier.layoutAfter,
                                             barrier.syncBefore, barrier.syncAfter,
                                             barrier.accessBefore, barrier.accessAfter));
    }
    return {};
}

Status RenderGraph::BeginPassRendering(u32 pass, RHI::ICommandList& commands, bool& began) {
    began = false;

    // A fixed array on the stack, sized by the bound `DiagnosticKind::TooManyAttachments`
    // enforces at declaration -- so this cannot overflow, and execution allocates nothing.
    RHI::ColorAttachment attachments[RHI::kMaxColorAttachments] = {};
    usize                count                                  = 0;
    RHI::Extent2D        extent                                 = {};

    for (const AttachmentInspection& declared : m_attachments) {
        if (declared.pass != pass) {
            continue;
        }
        // **The declaration list's own order, which `AttachmentInspection::slot` says is the
        // shader output location.** Writing to `attachments[declared.slot]` instead would be a
        // second opinion about the same order; the two agree because `DeclareColorAttachment`
        // assigns `slot` by counting this pass's earlier attachments, and
        // `GraphInspection::attachments` promises a reader that they do.
        MONARC_CHECK(declared.slot == static_cast<u32>(count),
                     "a pass's attachments are not in slot order");

        attachments[count].texture    = TextureOf(declared.resource.index);
        attachments[count].loadOp     = declared.loadOp;
        attachments[count].storeOp    = declared.storeOp;
        attachments[count].clearValue = declared.clearValue;

        // Every attachment of one pass agrees on extent -- `DiagnosticKind::AttachmentExtentConflict`
        // is what makes that true -- so the first one's is the pass's, and the rest confirm it.
        extent = m_resources[declared.resource.index].description.extent;
        ++count;
    }

    if (count == 0) {
        // No attachments, no rendering instance. See `PassBuilder::ColorAttachment`.
        return {};
    }

    RHI::RenderingDescription description{};
    description.extent           = extent;
    description.colorAttachments = std::span<const RHI::ColorAttachment>(attachments, count);
    if (Status begun = commands.BeginRendering(description); !begun) {
        return begun;
    }
    began = true;
    return {};
}

Status RenderGraph::Execute(RHI::IDevice& device, RHI::ICommandList& commands) {
    // **A `default`-less switch over the phase, so a fourth one is a build error here.** The two
    // refusals are separate because the conditions are: a graph that never compiled has no frame
    // at all, and one whose compile *failed* has a report full of diagnostics and a partial
    // barrier list -- recording either would put part of a frame nobody asked for into the
    // caller's command list. Both happen before `Begin`, so `commands` is untouched.
    switch (m_phase) {
        case GraphPhase::Declaring:
            return Err(ErrorCode::InvalidArgument,
                       "RenderGraph::Execute: this build has not been compiled");
        case GraphPhase::CompileFailed:
            return Err(ErrorCode::InvalidArgument,
                       "RenderGraph::Execute: this build's compilation was refused");
        case GraphPhase::Compiled: break;
    }

    // Before `Begin`, so that a device which refuses a texture costs the caller an untouched
    // command list -- see the note on the two refusal classes at the head of this file.
    if (Status created = CreateTransients(device); !created) {
        return created;
    }

    if (Status begun = commands.Begin(); !begun) {
        return begun;
    }

    for (u32 sorted = 0; sorted < static_cast<u32>(m_passes.Size()); ++sorted) {
        const u32             pass     = m_passOrder[sorted];
        const PassInspection& declared = m_passes[pass];
        if (declared.culled) {
            // **A culled pass records nothing, and its callback does not run.** It contributes
            // no barrier either, which is the derivation's doing rather than this loop's.
            continue;
        }

        if (Status barriers = RecordBarriersAt(declared.executionOrder, commands); !barriers) {
            return barriers;
        }

        bool began = false;
        if (Status rendering = BeginPassRendering(pass, commands, began); !rendering) {
            return rendering;
        }

        // **The callback runs inside the rendering instance when there is one**, which is what a
        // pass that draws needs and is why `PassCommandList` forbids `Barrier`: a barrier is not
        // permitted inside a dynamic-rendering instance at all, so the one call a pass must not
        // make is the one it structurally cannot. `PassCommandList`'s private constructor is
        // reached from here and nowhere else.
        if (Detail::IPassRecord* record = m_records[pass].record; record != nullptr) {
            PassCommandList passCommands(commands);
            record->Invoke(passCommands);
        }

        if (began) {
            commands.EndRendering();
        }
    }

    // The imports' outgoing transitions, after every pass. `kNoPass` is the position
    // `DerivedBarrier::emittedBeforePass` uses for them, and it is the same selection the loop
    // above makes, with the end-of-frame key.
    if (Status closing = RecordBarriersAt(kNoPass, commands); !closing) {
        return closing;
    }

    return commands.End();
}

}  // namespace Monarc::Render
