#include <Monarc/Render/RenderGraph.h>

#include <Monarc/Core/Log.h>

#include <span>

namespace Monarc::Render {

namespace LogCategories {

/// Nested in a namespace of its own for one mechanical reason, the same one
/// Monarc.Host.Windowed/Private/Platform/Windows/Window.cpp records: MONARC_LOG_CATEGORY
/// declares a *variable* named after the category, and a variable called `RenderGraph` in this
/// namespace would hide the class of that name. The nesting keeps the category's spelling,
/// which is what appears in a log line.
MONARC_LOG_CATEGORY(RenderGraph, Info);

}  // namespace LogCategories

RenderGraph::RenderGraph(IAllocator& allocator, const Config& config)
    : m_config(config),
      m_passes(allocator),
      m_resources(allocator),
      m_accesses(allocator),
      m_barriers(allocator),
      m_diagnostics(allocator),
      m_records(allocator) {
    // The only allocation this class ever makes, and it happens here. Every pool is reserved
    // to its configured capacity and never grows -- `Array::Reserve` is what buys the buffer,
    // and every append below refuses rather than reaching `Emplace`'s growth path. See the
    // class comment: declaration is a path a frame runs.
    m_passes.Reserve(m_config.maxPasses);
    m_resources.Reserve(m_config.maxResources);
    m_accesses.Reserve(m_config.maxAccesses);
    m_barriers.Reserve(m_config.maxBarriers);
    m_diagnostics.Reserve(m_config.maxDiagnostics);

    // Filled to capacity rather than appended to, so that a pass's index is its index here
    // from construction. See `PassRecordSlot`.
    m_records.Reserve(m_config.maxPasses);
    for (u32 i = 0; i < m_config.maxPasses; ++i) {
        m_records.Emplace();
    }
}

RenderGraph::~RenderGraph() { DestroyRecords(); }

void RenderGraph::Reset() {
    DestroyRecords();

    // Clear, not release: `Array::Clear` destroys every element and keeps the capacity, which
    // is the whole point of resetting rather than reconstructing.
    m_passes.Clear();
    m_resources.Clear();
    m_accesses.Clear();
    m_barriers.Clear();
    m_diagnostics.Clear();

    // **The dropped count is part of the build, and clearing it is what stops one overflowed
    // frame from poisoning every frame after it.** `Compile` reads `m_diagnosticsDropped != 0`
    // as "a declaration was refused" -- see Private/Compile.cpp -- so a graph that filled its
    // diagnostics pool once and kept the count would refuse to compile for the rest of its
    // life, returning `ErrorCode::Unknown` with an empty diagnostics list. That is the least
    // debuggable failure this design can produce, and it is one deleted line away.
    m_diagnosticsDropped = 0;
    m_phase              = GraphPhase::Declaring;
    ++m_buildGeneration;
}

Result<PassBuilder> RenderGraph::AddPass(std::string_view name) {
    if (m_phase != GraphPhase::Declaring) {
        // **"no longer accepting declarations" rather than "is compiled", because this fires in
        // `CompileFailed` too.** A build whose compilation was *refused* is not compiled, and a
        // literal saying it was would send a reader looking for a frame that was never
        // produced. `DiagnosticKind::AlreadyCompiled`'s own doc has it right -- "called on a
        // graph that was not accepting declarations" -- and this is the wording that agrees
        // with it.
        return std::unexpected(Refuse(DiagnosticKind::AlreadyCompiled,
                                      ErrorCode::InvalidArgument,
                                      "RenderGraph::AddPass: this build is no longer accepting "
                                      "declarations",
                                      kNoPass, TextureId{}));
    }
    if (m_passes.Size() >= m_config.maxPasses) {
        // `kNoPass`, not `m_passes.Size()`: no pass was created, and `GraphDiagnostic::pass` is
        // "the pass involved ... or `kNoPass` where none is". The index this refusal *would*
        // have handed out names nothing, and a reader who looked it up would find the last pass
        // that did get created. The refusal ten lines above answers the same way.
        return std::unexpected(Refuse(DiagnosticKind::PassPoolExhausted, ErrorCode::OutOfMemory,
                                      "RenderGraph::AddPass: pass pool exhausted", kNoPass,
                                      TextureId{}));
    }

    const u32      index = static_cast<u32>(m_passes.Size());
    PassInspection pass{};
    pass.name  = name;
    pass.index = index;
    // No execution order until `Compile` settles one. `kNoPass` and not `index`, so that a
    // graph inspected mid-declaration cannot be read as though its order were decided.
    pass.executionOrder = kNoPass;
    pass.culled         = false;
    pass.queue          = GraphQueue::Graphics;
    pass.hasRecord      = false;
    m_passes.Push(pass);

    return PassBuilder(*this, index, m_buildGeneration);
}

GraphInspection RenderGraph::Inspect() const {
    GraphInspection inspection{};
    inspection.phase           = m_phase;
    inspection.buildGeneration = m_buildGeneration;
    inspection.passes = std::span<const PassInspection>(m_passes.Data(), m_passes.Size());
    inspection.resources =
        std::span<const ResourceInspection>(m_resources.Data(), m_resources.Size());
    inspection.accesses = std::span<const AccessInspection>(m_accesses.Data(), m_accesses.Size());
    inspection.barriers = std::span<const DerivedBarrier>(m_barriers.Data(), m_barriers.Size());
    inspection.diagnostics =
        std::span<const GraphDiagnostic>(m_diagnostics.Data(), m_diagnostics.Size());
    inspection.diagnosticsDropped = m_diagnosticsDropped;
    return inspection;
}

bool RenderGraph::IsCurrentPass(u32 pass, u32 generation) const {
    return generation == m_buildGeneration && pass < m_passes.Size();
}

Error RenderGraph::Refuse(DiagnosticKind kind, ErrorCode code, const char* message, u32 pass,
                          TextureId resource) {
    // The log line carries the composed detail and the diagnostic carries the structure, and
    // both exist for the reason `GraphDiagnostic` gives: `Error::message` is a non-owning view
    // and therefore a string literal, so "an error naming the resource" cannot be said through
    // the returned `Error` at all. The log is for a human; the diagnostic is what a test
    // asserts on.
    MONARC_LOG(LogCategories::RenderGraph, Error,
               "refused: {} ({}) pass {} resource {}:{} -- {}", ToString(kind), ToString(code),
               pass, resource.index, resource.generation, message);

    if (m_diagnostics.Size() < m_config.maxDiagnostics) {
        // `group` is deliberately left at its default rather than assigned like the five fields
        // below it: every refusal a *declaration* can produce is about one pass and one
        // resource, so every row this function writes stands alone, and a redundant write would
        // read as though something here chose a group. Task 2's cycle report is the first
        // caller with a reason to set one -- see `GraphDiagnostic::group`.
        GraphDiagnostic diagnostic{};
        diagnostic.kind     = kind;
        diagnostic.code     = code;
        diagnostic.message  = message;
        diagnostic.pass     = pass;
        diagnostic.resource = resource;
        m_diagnostics.Push(diagnostic);
    } else {
        // Counted rather than dropped silently. A truncated diagnostics list that read as a
        // complete one would hide the very refusal it was recording -- see
        // `GraphInspection::diagnosticsDropped`.
        ++m_diagnosticsDropped;
    }

    return Error{code, message};
}

Result<void*> RenderGraph::ClaimRecordStorage(u32 pass, u32 generation) {
    if (!IsCurrentPass(pass, generation)) {
        return std::unexpected(Refuse(DiagnosticKind::UnknownPass, ErrorCode::NotFound,
                                      "PassBuilder::Record: this builder names no pass in the "
                                      "current build",
                                      pass, TextureId{}));
    }
    if (m_records[pass].record != nullptr) {
        return std::unexpected(Refuse(DiagnosticKind::RecordAlreadySet, ErrorCode::AlreadyExists,
                                      "PassBuilder::Record: this pass already set a recording "
                                      "callback",
                                      pass, TextureId{}));
    }
    // **`m_records[pass]`, and the index is the whole of the correctness here.** Handing back
    // another pass's storage would placement-construct this callable over one that is already
    // live -- without destroying it, since the occupancy check above looked at a different slot
    // -- and `DestroyRecords` would then destroy the survivor twice, once through each slot's
    // pointer. `CommitRecord` below writes `m_records[pass].record` and `m_passes[pass]`, so the
    // two halves of one operation have to agree on which pass they are about.
    return static_cast<void*>(m_records[pass].storage);
}

void RenderGraph::CommitRecord(u32 pass, Detail::IPassRecord* record) {
    m_records[pass].record   = record;
    m_passes[pass].hasRecord = true;
}

void RenderGraph::DestroyRecords() {
    for (PassRecordSlot& slot : m_records) {
        if (slot.record != nullptr) {
            slot.record->~IPassRecord();
            slot.record = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------------------
// PassBuilder, whose every method is a forward into the graph that owns its pass.
//
// Defined here rather than in PassBuilder.h because each needs `RenderGraph` to be complete,
// and PassBuilder.h cannot include RenderGraph.h -- the dependency runs the other way.
// ---------------------------------------------------------------------------------------

Result<TextureId> PassBuilder::CreateTexture(std::string_view                name,
                                             const RHI::TextureDescription& description) {
    return m_graph->DeclareTransient(m_pass, m_generation, name, description);
}

Result<TextureId> PassBuilder::ImportTexture(std::string_view name, const TextureImport& import) {
    return m_graph->DeclareImport(m_pass, m_generation, name, import);
}

Status PassBuilder::Read(TextureId texture, ResourceAccess access) {
    return m_graph->DeclareAccess(m_pass, m_generation, texture, access, false);
}

Status PassBuilder::Write(TextureId texture, ResourceAccess access) {
    return m_graph->DeclareAccess(m_pass, m_generation, texture, access, true);
}

Result<void*> PassBuilder::ClaimRecordStorage() const {
    return m_graph->ClaimRecordStorage(m_pass, m_generation);
}

void PassBuilder::CommitRecord(Detail::IPassRecord* record) const {
    m_graph->CommitRecord(m_pass, record);
}

}  // namespace Monarc::Render
