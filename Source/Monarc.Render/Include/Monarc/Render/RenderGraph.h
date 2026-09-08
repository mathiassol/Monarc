#pragma once

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/Render/Access.h>
#include <Monarc/Render/GraphInspection.h>
#include <Monarc/Render/PassBuilder.h>
#include <Monarc/Render/ResourceId.h>

#include <cstddef>
#include <string_view>

namespace Monarc::Render {

/// The render graph: passes declare what they read and write, and the graph decides how it
/// happens.
///
/// Design decision: [ADR-0006](Docs/Architecture/Decisions/ADR-0006-render-graph.md).
/// The contract, and the seven steps a frame goes through, are in
/// Docs/Rendering/Render-Graph.md.
///
/// **Three phases, and compile is separable from execute on purpose.**
///
/// 1. **Declare.** `AddPass` claims a pass and returns a `PassBuilder`; the builder declares
///    reads, writes, transient creation, resource import and a recording callback.
/// 2. **Compile.** Everything the graph decides: which passes run and in what order, what
///    each resource's lifetime is, which transients may share memory, and what barriers the
///    declared accesses imply.
/// 3. **Execute.** Records the compiled frame into an `RHI::ICommandList`.
///
/// **Nothing about the first two phases needs a device, and that is the whole design of this
/// class rather than a happy accident.** Declaration is bookkeeping and compilation is a pure
/// function over it -- dependency building, culling, lifetimes, alias grouping and barrier
/// derivation are computation over declarations with no GPU anywhere in them. So the
/// constructor takes an allocator and a `Config` and nothing else, and everything execution
/// needs -- a device, a command list -- is a parameter of `Execute`. The consequence is the
/// one the phase plan asks for: a test can declare, compile and inspect a whole frame's
/// synchronisation on a machine with no GPU, no Vulkan driver and no display, which is the
/// machine CI runs on. A constructor that took an `RHI::IDevice&` would have moved the most
/// important code in the rendering layer behind a `Skipped` test.
///
/// **Fixed-capacity pools, sized from `Config` at construction and never grown.** A graph is
/// declared and thrown away every frame, so declaration is a path a frame runs and must not
/// touch the allocator; `Reset` clears the pools and keeps their capacity. A full pool reports
/// `ErrorCode::OutOfMemory` rather than reallocating. That is `JobSystem::Config`'s discipline
/// and `RHI::DeviceConfig`'s, and it is here for their reason.
///
/// Non-copyable and non-movable: a `PassBuilder` names its graph by pointer, so a graph cannot
/// be relocated out from under one.
class RenderGraph {
public:
    /// How many of each thing one build may hold.
    ///
    /// **Fixed at construction and never grown** -- see the class comment. The defaults are
    /// small because Phase A4 declares one pass, one resource and one access: the numbers that
    /// will matter are Phase B's, once there is a renderer to size them from. A build that
    /// exceeds one of these is refused with `ErrorCode::OutOfMemory` at the declaration that
    /// exceeded it, and the refusal is recorded in `GraphInspection::diagnostics`.
    ///
    /// **Every field here is defaulted, where `JobSystem::Config` deliberately leaves
    /// `workerCount` and `maxJobs` undefaulted so that a caller has to choose. The divergence
    /// is deliberate, and it is about what a wrong number costs.** `JobSystem::workerCount`
    /// has no defensible default at all -- the right value is a property of the host machine,
    /// and zero does not refuse, it silently never runs a job. `maxJobs` bounds *concurrently
    /// outstanding* work, so whether it is exceeded depends on submission rate and dependency
    /// depth under load: an intermittent `OutOfMemory` on a hot path, found in production
    /// rather than by whoever picked the number. A graph capacity fails the other way round. A
    /// frame's declarations are written in source and are the same every frame, so a capacity
    /// that is too small is exceeded on the *first* frame, deterministically, at the
    /// declaration that exceeded it -- with the code returned to that call and a
    /// `GraphDiagnostic` naming the pass. It cannot lurk, and the person who accepted the
    /// default is the person who finds it.
    ///
    /// Two of the five are not the caller's to choose in any case: `maxBarriers` and
    /// `maxDiagnostics` size pools the *graph* fills, so a caller asked to pick one would be
    /// predicting the derivation's own output. And the nearer precedent in this tree already
    /// defaults everything -- `RHI::DeviceConfig`'s `maxTextures` and `maxBuffers`, which is
    /// the same shape: a fixed-capacity pool sized at construction that refuses with
    /// `OutOfMemory`. What this class takes from `JobSystem::Config` is the discipline the
    /// class comment names -- never grown, refusing rather than reallocating -- and that part
    /// is unchanged.
    struct Config {
        u32 maxPasses    = 64;
        u32 maxResources = 128;

        /// Total (pass, resource, access) triples across the whole build, not per pass. A
        /// flat budget because passes differ wildly in how many resources they touch, and a
        /// per-pass cap would have to be sized for the widest one and paid for by every
        /// other.
        u32 maxAccesses = 512;

        /// Capacity of the derived-barrier list. **Nothing fills it in Task 1** -- derivation
        /// is Task 3's -- so this sizes a pool that is currently always empty, and is here
        /// because the pool has to be allocated at construction rather than when the first
        /// barrier is derived.
        u32 maxBarriers = 256;

        /// Capacity of the diagnostics list. Small: a build with more than this many
        /// refusals has one bug repeated, not thirty-two problems. Overflow is *counted*
        /// rather than dropped silently -- see `GraphInspection::diagnosticsDropped`.
        u32 maxDiagnostics = 32;
    };

    RenderGraph(IAllocator& allocator, const Config& config);
    ~RenderGraph();

    RenderGraph(const RenderGraph&)            = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&)                 = delete;
    RenderGraph& operator=(RenderGraph&&)      = delete;

    /// Discards every declaration and begins a new build, keeping every pool's capacity.
    ///
    /// **Bumps the build generation, and that is what makes a stale id fail loudly.** Pass and
    /// resource indices start again from zero, so a `TextureId` or a `PassBuilder` held across
    /// this call would name a *different* declaration rather than a missing one -- silently,
    /// with no bad pointer for a sanitizer to find. Every id and builder the graph resolves is
    /// checked against the current generation, so the stale one is refused instead. See
    /// `ResourceId`'s class comment.
    ///
    /// Destroys any recording callbacks the previous build stored.
    void Reset();

    /// Claims a pass and returns the builder that declares it.
    ///
    /// `name` is not copied. It must reference a string literal or otherwise long-lived
    /// storage, exactly like `Error::message` and `JobSystem`'s job names -- inspection hands
    /// it back as a `std::string_view`.
    ///
    /// Fails with `ErrorCode::OutOfMemory` if the pass pool is full, and with
    /// `ErrorCode::InvalidArgument` if this graph is no longer accepting declarations --
    /// `Compile` has run and no `Reset` has run since.
    [[nodiscard]] Result<PassBuilder> AddPass(std::string_view name);

    /// Turns the declarations into a frame: execution order, lifetimes, alias groups and
    /// derived barriers.
    ///
    /// **Callable with no device, deliberately and permanently -- see the class comment.**
    ///
    /// **What it does in Phase A4 Task 1, stated exactly, because the rest of the phase is
    /// what fills it in.** It validates the declarations and settles execution order as
    /// declaration order, with nothing culled. It computes no lifetimes, groups no aliases and
    /// derives no barriers: Task 2 is the first two and Task 3 the third, and until then
    /// inspection reports every lifetime empty, every alias group absent and the barrier list
    /// empty. A green suite here is not evidence that any of those work.
    ///
    /// Fails with `ErrorCode::InvalidArgument` if the graph is not accepting declarations, and
    /// with the code of the first recorded diagnostic if any declaration was refused -- a
    /// graph with a rejected declaration does not compile, whether or not the caller checked
    /// the `Status` that refusal returned. Either way the phase becomes
    /// `GraphPhase::CompileFailed` and inspection stays readable.
    [[nodiscard]] Status Compile();

    /// Records the compiled frame into `commands`, creating and destroying the transient
    /// resources it needs on `device`.
    ///
    /// **Not implemented in Task 1, and it refuses rather than doing nothing.** Task 4 of the
    /// phase plan is what writes it, in Private/Execute.cpp -- the only file in this module
    /// that will touch an `RHI::ICommandList`. Until then this returns
    /// `ErrorCode::Unsupported`, because a call that silently succeeded having recorded
    /// nothing is the shape of green that this codebase keeps finding and deleting.
    ///
    /// The signature is the point of declaring it now: **everything execution needs is a
    /// parameter here rather than a constructor argument**, which is what keeps `Compile`
    /// device-free.
    [[nodiscard]] Status Execute(RHI::IDevice& device, RHI::ICommandList& commands);

    /// Everything the graph can say about the build it is holding. See `GraphInspection`.
    ///
    /// **A view over the graph's own arrays, and what goes stale about it is the length rather
    /// than the storage.** The pools are fixed at construction and never grow, so the pointers
    /// a returned `GraphInspection` holds stay valid for the graph's whole life -- but a
    /// declaration made after this call is not in the value it returned, and `Reset` destroys
    /// what is. Call it again rather than keeping one across either.
    ///
    /// Legal in every phase: a failed compile is exactly when the diagnostics matter.
    [[nodiscard]] GraphInspection Inspect() const;

    /// Which build this is. Bumped by `Reset`; the generation every `TextureId` this build
    /// hands out carries.
    [[nodiscard]] u32 BuildGeneration() const { return m_buildGeneration; }

    // There is deliberately no accessor for the `Config`. Nothing outside this class reads
    // one back -- a caller that chose the sizes has them -- and the rule this module applies
    // to its own enumerators applies to its accessors too.

private:
    friend class PassBuilder;

    /// One pass's recording callback storage. Indexed by pass index.
    ///
    /// **Separate from `PassInspection`, and pre-sized to `Config::maxPasses` at construction
    /// rather than appended to.** Two reasons, and the second is the one that decided it.
    /// Storing 64 bytes of type-erased callable inside the inspection type would put the
    /// callback's bytes in the graph's report, where nothing can assert on them. And a second
    /// array appended in step with the first would be a lockstep invariant to keep true;
    /// filling this one to capacity up front removes the invariant entirely -- a pass's index
    /// *is* its index here, from construction, whether or not the pass exists yet.
    struct PassRecordSlot {
        /// The live callback in `storage`, or nullptr when this pass has none. **The
        /// occupancy flag and the pointer are the same field on purpose** -- see the note in
        /// `PassBuilder::Record` for why the pointer is kept rather than recovered from the
        /// storage address, and why "the object was constructed" and "the slot is occupied"
        /// being one fact is the honest answer if the callable's move constructor throws.
        Detail::IPassRecord* record = nullptr;

        alignas(Detail::kPassRecordAlign) std::byte storage[Detail::kPassRecordSize] = {};
    };

    // Declaration, in Private/ResourceRegistry.cpp and Private/RenderGraph.cpp. PassBuilder
    // forwards to these; they are private because a pass declares through its builder and
    // there is one way to do it.
    [[nodiscard]] Result<TextureId> DeclareTransient(u32 pass, u32 generation,
                                                     std::string_view                name,
                                                     const RHI::TextureDescription& description);
    [[nodiscard]] Result<TextureId> DeclareImport(u32 pass, u32 generation,
                                                  std::string_view     name,
                                                  const TextureImport& import);
    [[nodiscard]] Status DeclareAccess(u32 pass, u32 generation, TextureId texture,
                                       ResourceAccess access, bool writing);
    [[nodiscard]] Result<void*> ClaimRecordStorage(u32 pass, u32 generation);

    void CommitRecord(u32 pass, Detail::IPassRecord* record);

    /// Whether `pass` names a pass of build `generation`. The stale-`PassBuilder` check every
    /// declaration above starts with.
    [[nodiscard]] bool IsCurrentPass(u32 pass, u32 generation) const;

    /// The index into `m_resources` that `texture` names, or `m_resources.Size()` if it names
    /// none -- out of range, invalid, or from a previous build.
    [[nodiscard]] usize FindResource(TextureId texture) const;

    /// Records a refusal and returns it, so a declaration can `return Refuse(...)`.
    ///
    /// **Every refusal that names a pass or a resource goes through here**, so no such refusal
    /// can happen without the diagnostics list -- or `m_diagnosticsDropped`, when the list is
    /// full -- saying that it did. Two refusals deliberately do not: `Compile`'s report that
    /// an earlier declaration was refused, which would add a row naming nothing on top of the
    /// row that already says what happened, and `Execute`'s not-implemented-yet, which is
    /// about the build being unfinished rather than about the declarations in it.
    ///
    /// `message` must be a string literal -- `Error::message` is a non-owning view.
    [[nodiscard]] Error Refuse(DiagnosticKind kind, ErrorCode code, const char* message,
                               u32 pass, TextureId resource);

    /// Destroys every stored recording callback. Called by `Reset` and by the destructor.
    void DestroyRecords();

    // No allocator member. Every pool below holds its own reference (`Array` takes one at
    // construction), and this class allocates nowhere except in its constructor -- so a second
    // copy of the reference would be a field with no reader.
    Config m_config;

    GraphPhase m_phase              = GraphPhase::Declaring;
    u32        m_buildGeneration    = 0;
    u32        m_diagnosticsDropped = 0;

    Array<PassInspection>     m_passes;
    Array<ResourceInspection> m_resources;
    Array<AccessInspection>   m_accesses;
    Array<DerivedBarrier>     m_barriers;
    Array<GraphDiagnostic>    m_diagnostics;
    Array<PassRecordSlot>     m_records;
};

}  // namespace Monarc::Render
