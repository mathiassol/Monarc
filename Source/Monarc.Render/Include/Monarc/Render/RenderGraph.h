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
#include <span>
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
    /// Two of the six are not the caller's to choose in any case: `maxBarriers` and
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

        /// Total colour attachments across the whole build. `maxAccesses`' argument for a flat
        /// budget applies unchanged, with a second reason of its own: a pass is capped at
        /// `RHI::kMaxColorAttachments` by `DiagnosticKind::TooManyAttachments` whatever this
        /// number is, so a per-pass field would be a second, lower cap for a caller to get
        /// wrong.
        ///
        /// Default 64 rather than `maxAccesses`' 512, because an attachment is far rarer than an
        /// access -- every attachment declares one or two accesses, and most accesses are not
        /// attachments at all. A build that overflows it is refused with
        /// `DiagnosticKind::AttachmentPoolExhausted` at the declaration that overflowed.
        u32 maxAttachments = 64;

        /// Capacity of the derived-barrier list, filled by the derivation in
        /// Private/DeriveBarriers.cpp. Allocated at construction rather than when the first
        /// barrier is derived, because compilation is a path a frame runs.
        ///
        /// A build that overflows it is refused with `DiagnosticKind::BarrierPoolExhausted`
        /// rather than growing the pool.
        ///
        /// **Bounded above by the declarations, and the tight bound is `maxAccesses +
        /// maxResources`.** A resource's chain is one state per surviving pass that touches it,
        /// framed by its first state and -- for an import -- its declared outgoing one; the gaps
        /// number the pass states plus that closing one. The *opening* end contributes no gap of
        /// its own, because the gap in front of the first pass is already the first pass's. So a
        /// resource contributes at most one barrier per (pass, resource) pair plus one, the pairs
        /// across the whole build are at most `maxAccesses`, and the plus-ones are at most
        /// `maxResources`. The bound is reached rather than merely respected --
        /// Tests/TestDeriveBarriers.cpp declares a frame that sits on it.
        ///
        /// This comment read `maxAccesses + 2 * maxResources` until Task 3's review recomputed
        /// it; that number is conservative rather than wrong, and counted the opening end twice.
        u32 maxBarriers = 256;

        /// Capacity of the diagnostics list. Small: a build with more than this many
        /// refusals has one bug repeated, not thirty-two problems. Overflow is *counted*
        /// rather than dropped silently -- see `GraphInspection::diagnosticsDropped`.
        u32 maxDiagnostics = 32;
    };

    RenderGraph(IAllocator& allocator, const Config& config);

    /// Destroys the stored recording callbacks and the transient textures `Execute` created.
    /// `Reset`'s precondition about in-flight work and about the device's lifetime applies here
    /// unchanged -- it is the same destruction.
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
    ///
    /// **And destroys the transient textures `Execute` created, which is where the graph's
    /// ownership of them ends.** The precondition is `IDevice::DestroyTexture`'s own, pushed up
    /// one level and stated rather than inherited silently: **the caller must have ensured the
    /// GPU is finished with the previous build's work before calling this.** Nothing here tracks
    /// in-flight use, because nothing in the RHI does -- `IQueue::Wait` is how a caller knows.
    ///
    /// **Why here and not at the end of `Execute`, which is what the phase plan's checklist
    /// says.** `Execute` returns *before* the command list is submitted, so a texture destroyed
    /// there is freed while a recorded command buffer still references it, and
    /// `IDevice::DestroyTexture` bumps the slot generation immediately and by design. Destroying
    /// at the *next* `Execute` does not fix it either: `IDevice::BeginFrame` waits on its own
    /// frame slot's timeline value, so with `RHI::kFramesInFlight` at two, frame N+1 has waited
    /// on frame N-1 and not on frame N.
    ///
    /// **The gap that leaves, stated because a reader will otherwise assume it closed.** A frame
    /// loop that resets one graph per frame with frames in flight needs either deferred
    /// destruction -- a queue of textures to free once a timeline value is reached -- or one
    /// graph per frame slot. Phase A4 builds neither, and nothing A4 ships reaches the hazard:
    /// its frame declares no transients at all, so there is nothing for a `Reset` to destroy.
    ///
    /// **The other half of the precondition is the device's own lifetime.** The graph keeps the
    /// `RHI::IDevice&` its last `Execute` was given, for exactly as long as it holds a texture
    /// that device made, so a graph holding transients must be `Reset` or destroyed before that
    /// device is. A graph that never executed, or whose builds declared no transient, holds no
    /// device pointer at all.
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
    /// **What it does, in the order it does it, because two of the orderings are
    /// load-bearing.** It builds the dependency graph from the declared reads and writes;
    /// refuses a graph with a dependency cycle, with a transient some pass reads and none
    /// writes, or with a resource some pass reads that two passes write without reading --
    /// see `DiagnosticKind::UnorderedOverwrite`, which is a stated limit of resources that
    /// have no versions; derives execution order by topological sort; culls the passes nothing
    /// consumes;
    /// computes each resource's lifetime over the passes that survived; groups transients
    /// whose lifetimes do not overlap and whose descriptions agree; and derives the barriers the
    /// declared accesses imply. Culling comes before lifetimes so that a culled reader shortens a
    /// lifetime rather than extending it, and ordering comes before both because a lifetime is a
    /// pair of execution positions. Derivation comes last because it walks the surviving passes
    /// in execution order.
    ///
    /// Fails with `ErrorCode::InvalidArgument` if the graph is not accepting declarations, with
    /// the code of the first recorded diagnostic if any declaration was refused -- a graph with
    /// a rejected declaration does not compile, whether or not the caller checked the `Status`
    /// that refusal returned -- with `ErrorCode::InvalidArgument` for a cycle, an unwritten
    /// transient or an unordered overwrite, and with `ErrorCode::OutOfMemory` if the derivation
    /// produced more barriers than the barrier pool holds. Either way the phase becomes
    /// `GraphPhase::CompileFailed` and inspection stays readable. For the first four, no
    /// execution order, culling decision, lifetime or alias group is reported, because none was
    /// settled; the barrier-pool refusal is the one that happens *after* those were settled, and
    /// it leaves them and a partial barrier list in the report -- see
    /// `DiagnosticKind::BarrierPoolExhausted`.
    [[nodiscard]] Status Compile();

    /// Records the compiled frame into `commands`, creating on `device` the transient resources
    /// it needs.
    ///
    /// In Private/Execute.cpp -- **the only file in this module that touches an
    /// `RHI::ICommandList`**. The signature is what keeps `Compile` device-free: everything
    /// execution needs is a parameter here rather than a constructor argument.
    ///
    /// **That "only file" is kept by the type system and not by a rule, which is worth stating
    /// because it is not obvious and is not total.** This is the one declaration in the module
    /// that takes an `RHI::ICommandList&`, and the only other way to hold one is
    /// `PassCommandList::Commands()`, which is private to `RenderGraph` -- so no other
    /// translation unit here can obtain a list to record into. What the compiler does not stop
    /// is `RenderGraph` growing a *second* member that takes one; there is no architecture gate
    /// for that, because the textual check a gate could make -- "no file but Execute.cpp names
    /// `ICommandList`" -- is already false: PassBuilder.h names it, holds one, and must.
    ///
    /// **What it records, in order, because that order is the whole deliverable:**
    /// `ICommandList::Begin`; then, for each surviving pass in execution order, every
    /// `DerivedBarrier` whose `emittedBeforePass` is that pass's position, then
    /// `BeginRendering` if the pass declared attachments, then the pass's recording callback
    /// through a `PassCommandList`, then `EndRendering`; then every `DerivedBarrier` with
    /// `emittedBeforePass == kNoPass`, which is the imports' outgoing transitions; then
    /// `ICommandList::End`. The barriers keep the order `GraphInspection::barriers` is in --
    /// this reads that order and does not re-derive it.
    ///
    /// **A pass with no attachments gets no `BeginRendering` and no `EndRendering` at all.** Its
    /// callback is still invoked, between the barriers in front of it and the next pass's. That
    /// is what a compute pass will be, and it is what a pass that only copies is today.
    ///
    /// **It does not destroy the transients it creates, and that is the decision this call makes
    /// rather than an omission** -- see `Reset`, which does. `IDevice::DestroyTexture` requires
    /// its caller to have ensured the GPU is finished, and this call returns *before* the list is
    /// submitted, so a texture destroyed here would be freed while a recorded command buffer
    /// still referenced it.
    ///
    /// **Calling it twice on one compiled build is legal and creates nothing twice**: a
    /// transient already created is reused, so the second recording names the same textures. It
    /// is not something a frame loop does -- one build, one recording -- and it is what makes a
    /// retry after a refused `Execute` leak nothing.
    ///
    /// Fails with `ErrorCode::InvalidArgument` for a graph that has not compiled and for one
    /// whose compile failed -- **a partial frame is not what a caller of either wanted** -- and
    /// with whatever `device` or `commands` returned otherwise. **A refusal made before
    /// `Begin` leaves `commands` untouched; one made after it leaves the list recording and
    /// abandoned mid-frame**, which is a real outcome rather than a hypothetical because
    /// `BeginRendering` returns a `Status`. Private/Execute.cpp says which refusals are on which
    /// side of that line, and the caller's answer to the second is to drop the recording rather
    /// than to submit it.
    ///
    /// Nothing it does reaches `GraphInspection`: the report describes the build, and execution
    /// is not part of one. See the note on `DiagnosticKind`.
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
    [[nodiscard]] Status DeclareColorAttachment(u32 pass, u32 generation, TextureId texture,
                                                RHI::LoadOp loadOp, RHI::StoreOp storeOp,
                                                const RHI::ClearColor& clearValue);
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
    /// full -- saying that it did. Two kinds of refusal deliberately do not: `Compile`'s report
    /// that an earlier declaration was refused, which would add a row naming nothing on top of
    /// the row that already says what happened, and **every refusal `Execute` makes**, including
    /// the ones that do name a resource -- a texture the device would not create, a rendering
    /// pass the list would not begin. Those are not facts about the declarations this report
    /// describes, and a row recorded during execution would land in a report whose phase reads
    /// `Compiled`: a report saying the build succeeded and carrying a refusal at once. See the
    /// note at the head of `DiagnosticKind`.
    ///
    /// `message` must be a string literal -- `Error::message` is a non-owning view.
    ///
    /// `group` is defaulted because exactly one caller passes one: the cycle report, which is
    /// the only refusal in the module that is about several passes at once. Defaulted rather
    /// than required so that the twelve declaration refusals keep saying nothing about a
    /// grouping they do not have -- see `GraphDiagnostic::group`.
    [[nodiscard]] Error Refuse(DiagnosticKind kind, ErrorCode code, const char* message,
                               u32 pass, TextureId resource,
                               u32 group = kNoDiagnosticGroup);

    /// Destroys every stored recording callback. Called by `Reset` and by the destructor.
    void DestroyRecords();

    /// Destroys every transient texture `Execute` created and forgets the device it made them
    /// on. Called by `Reset` and by the destructor, which is where this graph's ownership of
    /// them ends -- `Reset`'s comment carries the argument and the precondition.
    ///
    /// A no-op on a graph that never executed, and on one whose builds declared no transient:
    /// `m_transientDevice` is null and every handle in `m_transientTextures` is invalid.
    void DestroyTransients();

    // ---------------------------------------------------------------------------------
    // Execution's stages, in Private/Execute.cpp and called only by `Execute`. **The only
    // functions in this class below which there is a device**, which is the line the class
    // comment draws and the reason `Compile`'s stages are all above it.
    // ---------------------------------------------------------------------------------

    /// Creates a texture on `device` for every transient some surviving pass touches, and
    /// records it in `m_transientTextures`. A transient already created is left alone, which is
    /// what makes a second `Execute` of one build -- or a retry after a refused one -- create
    /// nothing twice.
    [[nodiscard]] Status CreateTransients(RHI::IDevice& device);

    /// The live texture `resource` stands for: an import's declared handle, or the transient
    /// one `CreateTransients` made. Invalid for a resource that has neither, which is a state no
    /// barrier can name -- see the caller.
    [[nodiscard]] RHI::TextureHandle TextureOf(u32 resource) const;

    /// Records every barrier whose `emittedBeforePass` is `position`, in the order
    /// `GraphInspection::barriers` holds them. `kNoPass` is the end-of-frame position, which is
    /// how the imports' outgoing transitions are recorded.
    [[nodiscard]] Status RecordBarriersAt(u32 position, RHI::ICommandList& commands);

    /// Begins `pass`'s rendering instance, or does nothing at all if it declared no colour
    /// attachment. `began` says which, so that the caller ends only what was begun.
    [[nodiscard]] Status BeginPassRendering(u32 pass, RHI::ICommandList& commands, bool& began);

    // ---------------------------------------------------------------------------------
    // Compilation's stages, in Private/Compile.cpp and called only by `Compile` in the
    // order they are declared. Each is a pure function of the declarations plus what the
    // stages before it wrote -- there is no device anywhere below this line.
    // ---------------------------------------------------------------------------------

    /// Buckets the flat access list by resource and by pass, so that every later stage can
    /// enumerate one pass's accesses or one resource's accesses without scanning all of them.
    void BuildAccessBuckets();

    /// The accesses naming one resource, or declared by one pass, as indices into
    /// `m_accesses` and in declaration order. Valid only after `BuildAccessBuckets`.
    /// @{
    [[nodiscard]] std::span<const u32> AccessesOfResource(u32 resource) const;
    [[nodiscard]] std::span<const u32> AccessesOfPass(u32 pass) const;
    /// @}

    /// Visits the dependency edges leaving `pass` -- `visit(reader)` once per other pass that
    /// reads a resource `pass` writes -- or those entering it. The two walk the same edge set
    /// with the same multiplicity, in the two directions.
    ///
    /// **What is and is not an edge is argued at length at the head of Private/Compile.cpp**,
    /// which is also the only translation unit that instantiates these: they are templates so
    /// that a visitor costs no indirect call and no type erasure on a path a frame runs, and
    /// private because the edge rule is compilation's business and nothing else's.
    /// @{
    template <typename Visit>
    void ForEachSuccessor(u32 pass, Visit visit) const;

    template <typename Visit>
    void ForEachPredecessor(u32 pass, Visit visit) const;
    /// @}

    /// Fills `m_passOrder` with a topological order of the declared dependencies, refusing a
    /// graph that has none.
    ///
    /// A dependency cycle is the only way this fails, and it reports one
    /// `DiagnosticKind::DependencyCycle` per pass in each cycle, grouped.
    [[nodiscard]] Status OrderPasses();

    /// Records one `DiagnosticKind::DependencyCycle` row per pass in each cycle, grouped, and
    /// returns the refusal `Compile` hands its caller. Called only from `OrderPasses`, and only
    /// on the path where it could not place every pass.
    [[nodiscard]] Error ReportDependencyCycles();

    /// Marks every pass `OrderPasses` could not place that is reachable from `from`, following
    /// dependency edges forwards or backwards. The two sweeps together are what identify one
    /// mutually-dependent set; see `ReportDependencyCycles`.
    void MarkReachable(u32 from, bool forward);

    /// Refuses every transient that some pass reads and no pass writes. Independent of
    /// execution order, so it runs whether or not `OrderPasses` succeeded.
    [[nodiscard]] Status RefuseUnwrittenTransients();

    /// Refuses every resource some pass reads that two or more passes write without reading --
    /// the declaration whose meaning the edge rule cannot fix, and whose order the sort would
    /// otherwise settle by a tie-break. See `DiagnosticKind::UnorderedOverwrite`. Independent
    /// of execution order for `RefuseUnwrittenTransients`' reason.
    [[nodiscard]] Status RefuseUnorderedOverwrites();

    /// Whether `pass` declares a read of `resource`. Valid only after `BuildAccessBuckets`.
    [[nodiscard]] bool PassReadsResource(u32 pass, u32 resource) const;

    /// Marks every pass nothing consumes, transitively. Needs `m_passOrder`.
    void CullPasses();

    /// Numbers the surviving passes `0..n-1` in execution order, and writes `kNoPass` for the
    /// culled ones.
    void NumberSurvivingPasses();

    /// Computes every resource's lifetime over the surviving passes.
    void ComputeLifetimes();

    /// Groups transients whose lifetimes do not overlap and whose descriptions are compatible.
    ///
    /// **The grouping is a decision, not a saving** -- every transient still gets its own
    /// allocation. The function's own comment says so where the decision is made.
    void GroupAliases();

    /// One point in a resource's chain of states: the state it is in, and what put it there.
    ///
    /// **The chain is what a barrier is derived between**, and its ends are not pass accesses:
    /// an imported resource's declared incoming and outgoing states, or a transient's creation
    /// and nothing. Private/DeriveBarriers.cpp is where the chain and the emission rule are
    /// argued.
    struct ResourceStep {
        RHI::TextureState state = {};
        BarrierCauseSide  cause = {};

        /// Whether a **pass** wrote the resource at this step.
        ///
        /// Read by `IsTransition` on **both** sides of a gap: that is what makes a
        /// write-after-write between two identical states a barrier, whether the second write is
        /// another pass or the external work an import's outgoing state declares. Between two
        /// passes and at a chain's incoming end the two sides cannot disagree, so the `to` side
        /// alone would answer; at the outgoing end it would not, and the difference there is a
        /// dropped write-after-write across the graph boundary.
        ///
        /// **False at both ends of every chain, and that is a decision rather than an
        /// omission** -- see Private/DeriveBarriers.cpp on why an import's declared states and a
        /// created texture's initial one are facts about a resource rather than operations to be
        /// ordered against. Stored rather than recovered from `cause.access`, which would make
        /// the emission rule depend on which access the cause happens to blame.
        bool passWrites = false;
    };

    /// Fills `m_barriers` with the transitions the declared accesses imply, in the order a
    /// frame records them. In Private/DeriveBarriers.cpp, which argues the rule.
    ///
    /// Refuses with `ErrorCode::OutOfMemory` if the barrier pool fills, and only then; every
    /// other way a build can be wrong was refused by an earlier stage or by the declaration.
    [[nodiscard]] Status DeriveBarriers();

    /// Whether moving a resource from `from` to `to` is a transition that has to be recorded.
    ///
    /// **The rule the derivation turns on, as one function with one expression**, so that a
    /// mutation to it is a mutation to the rule rather than to one case of it.
    /// Private/DeriveBarriers.cpp argues all three of the cases it decides.
    [[nodiscard]] static bool IsTransition(const ResourceStep& from, const ResourceStep& to);

    /// Writes `resource`'s first step -- an import's declared incoming state, or a transient's
    /// creation -- into `m_resourceStep`.
    void StartResourceStep(u32 resource);

    /// Combines every access `pass` declares to `resource` into the one step that pass is.
    /// Valid only after `BuildAccessBuckets` and `NumberSurvivingPasses`.
    [[nodiscard]] ResourceStep CombinePassStep(u32 pass, u32 resource) const;

    /// Emits the barrier from `resource`'s current step to `next`, unless that is not a
    /// transition, and advances the current step. `declaringPass` is the declaration index of
    /// the pass `next` belongs to, for the refusal to name, or `kNoPass`.
    [[nodiscard]] Status StepResource(u32 resource, u32 declaringPass, const ResourceStep& next);

    // No allocator member. Every pool below holds its own reference (`Array` takes one at
    // construction), and this class allocates nowhere except in its constructor -- so a second
    // copy of the reference would be a field with no reader.
    Config m_config;

    GraphPhase m_phase              = GraphPhase::Declaring;
    u32        m_buildGeneration    = 0;
    u32        m_diagnosticsDropped = 0;

    Array<PassInspection>       m_passes;
    Array<ResourceInspection>   m_resources;
    Array<AccessInspection>     m_accesses;
    Array<AttachmentInspection> m_attachments;
    Array<DerivedBarrier>       m_barriers;
    Array<GraphDiagnostic>      m_diagnostics;
    Array<PassRecordSlot>       m_records;

    /// The live texture each transient was given by `Execute`, indexed by resource index, or an
    /// invalid handle for a transient that has none -- one no surviving pass touches, one whose
    /// build has not executed, and every imported resource, whose handle is its own.
    /// `maxResources` entries, filled at construction like the compile scratch below.
    ///
    /// **Cleared by `Reset` rather than left, because a handle here is an ownership claim** --
    /// `DestroyTransients` destroys exactly what this array names, and a stale entry would name
    /// a slot some later build's texture now occupies.
    Array<RHI::TextureHandle> m_transientTextures;

    /// The device `Execute` last created a transient on, or nullptr.
    ///
    /// **The one device this class holds, and the class comment's claim survives it:** the
    /// *constructor* takes an allocator and a `Config` and nothing else, and declaration and
    /// compilation never read this. It is written by `CreateTransients` and read only by
    /// `DestroyTransients`, because destroying a texture needs the device that made it and
    /// `Reset` has no parameter to be handed one through. `Reset`'s comment states the lifetime
    /// precondition that buys.
    RHI::IDevice* m_transientDevice = nullptr;

    // ---------------------------------------------------------------------------------
    // Compilation's scratch state.
    //
    // **Every array below is filled to its capacity in the constructor and indexed from
    // there on, exactly as `m_records` is, so that `Compile` appends to nothing and
    // allocates nothing.** A frame compiles the graph, so compilation is a path a frame
    // runs and is held to the same rule declaration is -- see the class comment.
    //
    // **No `Config` field was added for any of them, and that is the point of listing the
    // sizes here.** Each is a function of a capacity the caller already chose: an entry per
    // pass, an entry per resource, or an entry per access. A `Config` field for a scratch
    // array would be asking a caller to size a buffer whose existence is an implementation
    // detail of a stage they cannot see, which is the same objection `maxBarriers` and
    // `maxDiagnostics` answer by being defaulted.
    //
    // They are *not* cleared by `Reset`, and do not need to be: every stage writes the slots
    // it will read at the start of its own pass over them, so no compile can read a value the
    // previous one left. The one array whose *valid range* depends on an earlier stage having
    // succeeded says so at its own field. Tests/TestDependencyGraph.cpp compiles a graph,
    // resets it and compiles a second, opposite build for exactly this reason.
    // ---------------------------------------------------------------------------------

    /// Access indices grouped by the resource they name: `maxAccesses` entries, with
    /// `m_resourceBucketStart` holding the `maxResources + 1` bucket boundaries. Declaration
    /// order is preserved inside each bucket, which is what makes every stage's answer
    /// deterministic.
    /// @{
    Array<u32> m_accessesByResource;
    Array<u32> m_resourceBucketStart;
    /// @}

    /// The same, grouped by the pass that declared them: `maxAccesses` entries and
    /// `maxPasses + 1` boundaries.
    /// @{
    Array<u32> m_accessesByPass;
    Array<u32> m_passBucketStart;
    /// @}

    /// Execution position to declaration index -- the topological order `OrderPasses`
    /// produces, and the array every stage after it walks. `maxPasses` entries; only the
    /// first `m_passes.Size()` are meaningful, and only after a successful `OrderPasses`.
    Array<u32> m_passOrder;

    /// Kahn's unsatisfied-predecessor count per pass, and afterwards the record of which
    /// passes got ordered at all -- which is what `ReportDependencyCycles` reads it for.
    /// `maxPasses` entries.
    Array<u32> m_passIndegree;

    /// Traversal flags per pass, used only by `ReportDependencyCycles`. `maxPasses` entries.
    Array<u32> m_passMark;

    /// The traversal work stack `ReportDependencyCycles` pushes onto. `maxPasses` entries,
    /// which is enough because nothing is pushed twice.
    Array<u32> m_passStack;

    /// The first-fit bin `GroupAliases` put each resource in, before bins of one member are
    /// discarded and the rest become alias groups. `maxResources` entries.
    Array<u32> m_resourceBin;

    /// The step each resource's chain has reached. `maxResources` entries.
    ///
    /// **A running step per resource rather than a chain rebuilt per gap**, which is what makes
    /// the derivation one sweep of the access list rather than one sweep per execution position.
    /// Written by `StartResourceStep` before the walk and advanced by `StepResource`; meaningful
    /// only for a resource some surviving pass touches, and only during `DeriveBarriers`.
    Array<ResourceStep> m_resourceStep;
};

}  // namespace Monarc::Render
