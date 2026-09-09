#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>
#include <Monarc/Render/Access.h>
#include <Monarc/Render/ResourceId.h>

#include <span>
#include <string_view>

namespace Monarc::Render {

// ---------------------------------------------------------------------------------------
// What a compiled graph can say about itself.
//
// **Inspectability is a requirement here, not a debug feature, and this header is where that
// stops being a sentence in a document.** Docs/Rendering/Render-Graph.md lists what the graph
// must be able to emit as data -- the pass list in execution order with culled passes marked,
// every resource with its lifetime and aliasing group, every derived barrier with the accesses
// that caused it, and queue assignments -- and gives three reasons: a debug view, a diffable
// artifact, and tests that assert on derived barriers rather than on pixels. The third is the
// one that shapes this file.
//
// **Structured first, text second.** Every type below is a plain aggregate a test can assert
// on field by field, and `WriteInspectionText` at the foot is a *projection* of them rather
// than the source of truth. That order is deliberate: a suite that asserts by matching
// substrings of a rendered report passes for the wrong reasons, and reviews on the Phase A3
// branch deleted assertions of exactly that shape. The text exists because a debug view and a
// diffable artifact need one, and it is tested as its own function.
//
// **Fields whose data arrives in a later task are present now, and empty.** `DerivedBarrier`
// and its `BarrierCause` are Task 3's, and say so in their own comments. `ResourceLifetime`,
// `ResourceInspection::aliasGroup` and the culling and ordering `PassInspection` reports are
// Task 2's and are computed. A field with nothing computing it yet is a gap that is visible; a
// decision the type cannot express is a decision no test can reach, which is the failure mode
// this whole header exists to avoid.
// ---------------------------------------------------------------------------------------

/// No pass. An execution-order position a culled pass does not have, and the end of the graph
/// where a barrier is emitted after every pass rather than in front of one.
inline constexpr u32 kNoPass = static_cast<u32>(-1);

/// No aliasing group -- a resource that shares memory with nothing.
inline constexpr u32 kNoAliasGroup = static_cast<u32>(-1);

/// No diagnostic group -- a refusal that stands alone rather than being one row of a report
/// about several passes at once. See `GraphDiagnostic::group`.
inline constexpr u32 kNoDiagnosticGroup = static_cast<u32>(-1);

/// How far through the three phases a graph is.
enum class GraphPhase : u32 {
    /// Accepting declarations. Where a graph is after construction and after `Reset`.
    Declaring = 0,

    /// `Compile` succeeded. Execution order is settled and inspection describes a real frame.
    Compiled,

    /// `Compile` refused. **The inspection is still readable, and that is the point** -- the
    /// diagnostics list is what says which pass or resource was at fault, and a phase that
    /// made the report unavailable would hide it exactly when it is wanted.
    CompileFailed,
};

/// Whether the graph owns a resource or merely borrows it.
enum class ResourceOrigin : u32 {
    /// Created by a pass, owned by the graph, alive for part of one frame. A candidate for
    /// culling and for aliasing.
    Transient = 0,

    /// Owned by something outside the graph -- the swapchain, in Phase A4. **Never culled and
    /// never aliased**: something outside the graph consumes it, and the graph does not own
    /// its memory. Its declared incoming and outgoing states are the first and last
    /// transitions the derivation produces for it.
    Imported,
};

/// Which queue a pass runs on.
///
/// **One enumerator, and that is the honest shape of A4's queue assignment rather than an
/// omission.** Render-Graph.md lists queue assignment as one of the four things inspection
/// must emit, and ADR-0006 defers multiple queues past M0 -- so every pass is assigned to the
/// graphics queue and this field records that assignment. There are no cross-queue waits to
/// report because there is no second queue to wait on. `AsyncCompute` and `Transfer` arrive
/// with the first pass that has a reason to be on one, and the cross-queue synchronisation
/// they need arrives with them.
enum class GraphQueue : u32 {
    Graphics = 0,
};

/// The span of execution order over which a resource is live: first write to last read.
///
/// **Computed over the passes that survive culling, which is what makes it shrink.** A pass
/// that does not run cannot extend a lifetime, so a transient whose last reader was culled is
/// live for a shorter span than its declarations suggest -- and that is the point of computing
/// the two in this order rather than the other. See `CullPasses` and `ComputeLifetimes` in
/// Private/Compile.cpp.
///
/// **Both ends are positions in execution order, and every resource gets one, imported
/// included.** The alias grouping is the only consumer that ignores the imported ones, because
/// the graph does not own their memory.
struct ResourceLifetime {
    /// Execution-order position of the first surviving pass that **writes** the resource, or
    /// `kNoPass`.
    ///
    /// A read is not a first use here. For a transient that is not a distinction that can bite
    /// -- one read by a surviving pass and written by none is refused with
    /// `DiagnosticKind::TransientNeverWritten` -- but an imported resource a surviving pass
    /// only reads legitimately has `kNoPass` here and a real `lastPass`, because its first
    /// contents came from outside the graph.
    u32 firstPass = kNoPass;

    /// Execution-order position of the last surviving pass that reads or writes it, or
    /// `kNoPass`.
    u32 lastPass = kNoPass;

    /// Whether this names no *write*.
    ///
    /// **Two queries and not one, because the one they were split out of answered the wrong
    /// question for a resource class A4's headline barriers are about.** A single `IsEmpty()`
    /// reading `firstPass` reported `true` for a read-only imported resource -- which has no
    /// first write and a perfectly real `lastPass`, and is genuinely used. Nothing lied while
    /// the only caller was the alias grouping, which asks this question about transients under
    /// `ResourceOrigin::Transient` and gets the same answer either way. The next caller is
    /// Task 3's barrier derivation, which asks "does this resource have a lifetime at all?" --
    /// and for a read-only import the honest answer is yes. So the two questions have two
    /// names, and neither of them is the ambiguous one.
    ///
    /// For a transient the two coincide after culling, which is why the alias grouping is
    /// indifferent: a surviving reader of a transient implies a surviving writer -- culling
    /// keeps a pass whose reader survives -- so a transient with a last use has a first write.
    /// An imported resource is where they come apart, because its first contents came from
    /// outside the graph.
    /// @{
    [[nodiscard]] constexpr bool HasNoWrite() const { return firstPass == kNoPass; }

    /// Whether this names no surviving use of any kind -- no write and no read.
    [[nodiscard]] constexpr bool IsUnused() const {
        return firstPass == kNoPass && lastPass == kNoPass;
    }
    /// @}

    constexpr bool operator==(const ResourceLifetime&) const = default;
};

/// One declared pass.
struct PassInspection {
    /// Non-owning, and expected to reference a string literal or otherwise long-lived
    /// storage, exactly like `Error::message` and `JobSystem`'s job names.
    std::string_view name = {};

    /// This pass's position in declaration order. Also its index into
    /// `GraphInspection::passes`.
    u32 index = 0;

    /// This pass's position among the passes that will actually run, or `kNoPass` when
    /// `culled` -- and `kNoPass` also while the graph is still declaring, because nothing has
    /// settled an order yet.
    ///
    /// **Derived by a topological sort of the declared dependencies, not taken from declaration
    /// order** -- so the two numbers genuinely differ, and a pass declared third can run first.
    /// Declaration order is only the sort's tie-break, which is what keeps a frame's report
    /// stable between builds. `OrderPasses` in Private/Compile.cpp carries the argument.
    ///
    /// **Dense over the passes that run**: the survivors are numbered `0..n-1` in execution
    /// order with no gaps where a culled pass was, because this field is a position among the
    /// passes that will actually run rather than a place in the sorted list.
    ///
    /// `AddPass` writes `kNoPass` on purpose so that a graph inspected mid-declaration cannot be
    /// read as though its order were decided, and a graph whose `Compile` *failed* keeps
    /// `kNoPass` here for the same reason -- there is no order to report.
    /// Tests/TestPassDeclaration.cpp asserts both.
    ///
    /// **Defaulted to `kNoPass` and not to zero**, which is the sentinel convention every other
    /// "no pass here" field in this header follows. A default-constructed `PassInspection` that
    /// claimed to run first would be a hand-built report saying something its author did not.
    u32 executionOrder = kNoPass;

    /// Whether nothing consumes this pass's outputs, so it will not run.
    ///
    /// **"Consumes" means a declared read by a pass that itself runs, or the world outside the
    /// graph.** A pass writing an imported resource always survives; a pass writing only
    /// transients survives exactly while some surviving pass reads one of them; a pass writing
    /// nothing at all never survives. A recording callback is not an output and does not keep a
    /// pass alive -- `CullPasses` in Private/Compile.cpp says why.
    ///
    /// False for every pass while the graph is declaring, and false after a failed `Compile`:
    /// nothing was decided in either case.
    bool culled = false;

    /// The queue this pass is assigned to. See `GraphQueue`.
    GraphQueue queue = GraphQueue::Graphics;

    /// Whether the pass set a recording callback.
    ///
    /// Reported because it is a declaration fact, and because a pass in A4 legitimately has
    /// none: `Monarc.FirstLight`'s clear is an attachment load-op, so its pass declares an
    /// access and records nothing. A round-trip test that could not see this could not tell
    /// a stored callback from a dropped one.
    bool hasRecord = false;

    constexpr bool operator==(const PassInspection&) const = default;
};

/// One declared resource.
///
/// **No declaring pass, and that is a decision rather than an omission.** "Which pass created
/// this transient" is a declaration fact this report deliberately cannot emit: creating a
/// resource is not accessing it -- `PassBuilder::CreateTexture` states that rule, and
/// `AccessInspection` is where every pass-to-resource edge lives -- so a `createdBy` field
/// would be the one edge in the report that is not an access. It would also tie a resource to a
/// pass the culling may drop, leaving a live resource pointing at a pass that does not run, and
/// the derivation has no use for it: a barrier comes from two *accesses*. If a debug view ever
/// wants the answer, the declaring pass is recoverable from nothing today, which is the honest
/// cost of this and is why it is written down here.
struct ResourceInspection {
    /// Non-owning; `PassInspection::name`'s note applies.
    std::string_view name = {};

    /// The id passes name this resource by, including the build generation it belongs to.
    TextureId id = {};

    ResourceOrigin origin = ResourceOrigin::Transient;

    /// What the resource is. Supplied by the creating pass for a transient, and by the
    /// importing pass for an imported one -- the graph needs the extent and format whether or
    /// not it owns the memory, because a rendering pass is described by its attachments.
    RHI::TextureDescription description = {};

    /// The live resource an imported declaration stands for. **Invalid unless `origin` is
    /// `Imported`** -- a transient has no physical resource until execution creates one, and
    /// may never get one at all if it is culled.
    RHI::TextureHandle importedTexture = {};

    /// The state the resource is in when it reaches the graph. Meaningful only when `origin`
    /// is `Imported`; a transient's first state is whatever creating it leaves it in.
    TextureState incoming = {};

    /// The state the resource must be in when the graph is done with it. Meaningful only when
    /// `origin` is `Imported`.
    TextureState outgoing = {};

    /// The span of execution order over which the resource is live. See `ResourceLifetime`,
    /// which is where the two ends' exact meanings are -- and ask it `IsUnused()` rather than
    /// `HasNoWrite()` for "is this resource used at all", because a read-only imported
    /// resource has no first write and is used.
    ResourceLifetime lifetime = {};

    /// The group of resources this one shares memory with, or `kNoAliasGroup`.
    ///
    /// **Computed, and deliberately not honoured.** The graph groups transients whose
    /// lifetimes do not overlap and whose descriptions are compatible, and emits the grouping
    /// here; the RHI still backs every transient with its own allocation, because sharing one
    /// needs sub-allocation and Monarc's memory is one allocation per resource. **So a
    /// populated alias group is a decision that was computed and reported, not memory that was
    /// saved, and a green aliasing test says nothing about bytes.** `GroupAliases` in
    /// Private/Compile.cpp repeats this where the decision is made.
    ///
    /// `kNoAliasGroup` means this resource shares memory with nothing -- an imported resource,
    /// one no surviving pass writes, or one whose lifetime overlaps or whose description
    /// disagrees with every other candidate's. A group always has at least two members.
    u32 aliasGroup = kNoAliasGroup;

    // **No `operator==`, unlike every other type in this header, and clang is what said so.**
    // `RHI::TextureDescription` has none -- `Format` and `TextureUsage` are plain enums and
    // `Extent2D` has one, but the struct itself was never given one, because nothing in
    // Phase A3 compared two descriptions. A defaulted comparison here is therefore implicitly
    // deleted, which MSVC accepts declaring (it only refuses a *use*) and clang-cl rejects
    // outright: `error: explicitly defaulted equality comparison operator is implicitly
    // deleted [-Werror,-Wdefaulted-function-deleted]`, with `note: ... because there is no
    // viable 'operator==' for member 'description'`. Found by the clang-debug preset with all
    // twenty msvc-debug tests green, which is the divergence ADR-0003 keeps that build for.
    //
    // Absent rather than hand-written, and the first real caller settled it: the alias
    // grouping wanted *compatibility* -- same format, same extent -- rather than equality, and
    // `DescriptionsAreCompatible` in Private/Compile.cpp is where that lives. Guessing at the
    // rule before it had a caller would have produced the wrong function; comparing the two
    // fields a test cares about, which is what Tests/TestPassDeclaration.cpp does, cost
    // nothing in the meantime.
};

/// One pass's declared access to one resource.
///
/// **A flat list rather than a per-pass one, and that is what makes the read-modify-write case
/// expressible.** A pass declaring the same resource as both a read and a write -- legal, and
/// what a blended or `LoadOp::Load` colour attachment is -- appears here as two entries with
/// the same `pass` and `resource` and different `access`. A model that stored one usage per
/// (pass, resource) pair would have had to collapse them.
struct AccessInspection {
    /// Index into `GraphInspection::passes` -- declaration order, not execution order.
    u32 pass = 0;

    TextureId resource = {};

    ResourceAccess access = ResourceAccess::ColorAttachmentRead;

    constexpr bool operator==(const AccessInspection&) const = default;
};

/// What put one side of a derived barrier where it is.
enum class BarrierCauseKind : u32 {
    /// An imported resource's declared incoming state -- the before side of its first
    /// transition.
    ImportIncoming = 0,

    /// A pass's declared access. `BarrierCauseSide::pass` and `::access` name which.
    PassAccess,

    /// An imported resource's declared outgoing state -- the after side of its last
    /// transition.
    ImportOutgoing,
};

/// One side of the reason a barrier exists.
struct BarrierCauseSide {
    BarrierCauseKind kind = BarrierCauseKind::PassAccess;

    /// Execution-order position of the pass whose access this is, or `kNoPass` when `kind` is
    /// not `PassAccess`.
    u32 pass = kNoPass;

    /// The access. **Read only when `kind` is `PassAccess`** -- `ResourceAccess` has no
    /// not-an-access value, so `kind` is the discriminator and this field is filler
    /// otherwise, in the shape `AccessRequirement::namesTexture` explains.
    ResourceAccess access = ResourceAccess::ColorAttachmentRead;

    constexpr bool operator==(const BarrierCauseSide&) const = default;
};

/// Why a barrier was derived: the two accesses whose disagreement produced it.
///
/// **Render-Graph.md asks for "every derived barrier, with the accesses that caused it", and
/// this is that clause.** ADR-0006 lists the cost of a derived barrier plainly: debugging a
/// wrong one means debugging a derivation rather than reading a line somebody wrote. Carrying
/// the cause is what keeps that from being archaeology -- a wrong barrier is traceable to the
/// pair of declarations that asked for it.
struct BarrierCause {
    BarrierCauseSide before = {};
    BarrierCauseSide after  = {};

    constexpr bool operator==(const BarrierCause&) const = default;
};

/// One barrier the graph derived.
///
/// **Task 3 fills this list; it is empty until then, and the type is here now on purpose.** A
/// barrier the inspection could not express would be a barrier no test could assert on, which
/// is the whole thing A4 exists to change.
///
/// **ADR-0005's six fields plus a `TextureId`, and deliberately not an `RHI::TextureBarrier`.**
/// A compiled graph has no `RHI::TextureHandle` for a transient -- execution is what creates
/// one, and a culled resource never gets one -- so a derived barrier that had to name a live
/// texture could not be produced by compilation at all, and every derivation test would need
/// a device. Task 4's `Execute` is what turns one of these plus a resolved handle into an
/// `RHI::TextureBarrier`.
struct DerivedBarrier {
    TextureId resource = {};

    /// Execution-order position of the pass this barrier is recorded in front of, or
    /// `kNoPass` for one recorded after every pass -- which is what an imported resource's
    /// outgoing transition is.
    u32 emittedBeforePass = kNoPass;

    RHI::TextureLayout layoutBefore = RHI::TextureLayout::Undefined;
    RHI::TextureLayout layoutAfter  = RHI::TextureLayout::Undefined;
    RHI::PipelineStage syncBefore   = RHI::PipelineStage::None;
    RHI::PipelineStage syncAfter    = RHI::PipelineStage::None;
    RHI::Access        accessBefore = RHI::Access::None;
    RHI::Access        accessAfter  = RHI::Access::None;

    BarrierCause cause = {};

    constexpr bool operator==(const DerivedBarrier&) const = default;
};

/// What kind of thing the graph refused.
///
/// **One enumerator per refusal the graph can produce, and no placeholders for later tasks.**
/// A diagnostic kind is what a test asserts on instead of matching a message, so an
/// enumerator nothing emits would be an assertion nobody could write. The first twelve are
/// declaration refusals; the last three are Task 2's, and are the only three `Compile` itself
/// records about the declarations it was given.
enum class DiagnosticKind : u32 {
    /// `AddPass` was called with every pass slot occupied.
    PassPoolExhausted = 0,

    /// A resource was declared with every resource slot occupied.
    ResourcePoolExhausted,

    /// An access was declared with every access slot occupied.
    AccessPoolExhausted,

    /// An id named no resource in this build -- out of range, or from a previous build. See
    /// `ResourceId`'s note on the build generation.
    UnknownResource,

    /// A `PassBuilder` named no pass in this build -- one kept across a `RenderGraph::Reset`.
    /// The same recycling hazard `UnknownResource` covers for ids, for passes.
    UnknownPass,

    /// `Read` was given a write access, or `Write` a read access.
    AccessDirectionMismatch,

    /// A texture was declared with an access that can only name a buffer -- see
    /// `AccessRequirement::namesTexture`.
    AccessNamesNoTexture,

    /// The same pass declared the same access to the same resource twice.
    DuplicateAccess,

    /// Two declarations imported the same `RHI::TextureHandle`, which would give one physical
    /// resource two independent identities in one build.
    DuplicateImport,

    /// An import named an invalid `RHI::TextureHandle` -- what an unchecked
    /// `ISwapchain::Acquire` or `IDevice::CreateTexture` leaves behind. Refused at declaration
    /// because the alternative is finding out at execution, where `ICommandList::Barrier`'s
    /// answer to a handle it cannot resolve is to end the process.
    InvalidImport,

    /// A pass set a second recording callback.
    RecordAlreadySet,

    /// `Compile` was called on a graph that was not accepting declarations.
    AlreadyCompiled,

    /// This pass is one of a set among which no execution order exists: each of them depends,
    /// directly or through the others, on a resource one of the others writes.
    ///
    /// **One row per pass in the set, all carrying the same `group`** -- see that field. The
    /// set is the passes the cycle runs through, not every pass that cannot be ordered: a pass
    /// that merely *reads* the output of a cycle has no order either, and gets no row, because
    /// it is not what has to change.
    DependencyCycle,

    /// A transient resource that some pass reads and no pass writes.
    ///
    /// **Distinct from `DependencyCycle`, and the two must stay distinguishable**: a cycle is a
    /// mutual dependency and this is an absent one. A transient has no contents until something
    /// writes it, so a pass reading one that nothing wrote reads undefined memory. An *imported*
    /// resource read first is not this case at all -- it arrives with a declared
    /// `ResourceInspection::incoming` state, so reading it first is meaningful.
    ///
    /// One row per resource rather than per reading pass: what is wrong is that nothing writes
    /// the resource, which is one mistake however many passes read it. `pass` names the pass
    /// that declared the earliest of those reads.
    TransientNeverWritten,

    /// A resource some pass reads, and that two or more passes write without reading -- so
    /// which of those writes the read sees is not something the declarations say.
    ///
    /// **This is a stated limit of a graph whose resources have no versions, not a rule about
    /// what a frame may do.** The standard answer is versioning: a write produces a new
    /// version of a resource and a read names one, at which point "B reads what A wrote and C
    /// overwrites it afterwards" is a declaration -- A writes version 1, B reads version 1, C
    /// writes version 2, and C's write is ordered after B's read by a write-after-read edge
    /// that is now well-defined because the two accesses name different versions. Monarc's
    /// resources have one identity and no version, so that frame cannot be declared here, and
    /// this refusal is what says so instead of ordering it wrong. Every declaration refused by
    /// this kind becomes a legal declaration the day versioning arrives; no call site changes
    /// shape.
    ///
    /// **Why exactly this shape, from the edge rule.** An edge runs from a writer of a
    /// resource to every *different* pass that reads it and nothing else is an edge, so every
    /// foreign write of a resource precedes every read of it in every order the sort can
    /// produce -- and a reader's own write follows its own read, which is what a
    /// read-modify-write pass means. What a read sees is therefore the last of the foreign
    /// writes, and that is determined only when the foreign writers have a last one. A writer
    /// that *also* reads the resource has every other writer of it before it, by that same
    /// edge, and there can be at most one such pass (two are a two-pass cycle on that resource
    /// alone, and are refused as one). Two writers that do *not* read it have no edge between
    /// them -- there is deliberately no write-after-write edge -- so nothing the declarations
    /// say orders them, and the sort's declaration-order tie-break picks which one the read
    /// sees. Hence: a read, plus two passes that write and do not read.
    ///
    /// One row per resource rather than per reading or writing pass, for
    /// `TransientNeverWritten`'s reason: it is one mistake about one resource however many
    /// passes are caught in it. `pass` names the pass that declared the earliest read, which
    /// is the access that would have been handed the wrong bytes. **Imported resources are
    /// refused too**, unlike `TransientNeverWritten`: an import declares what state it arrives
    /// in, which makes reading it first meaningful, and says nothing at all about which of two
    /// passes overwrote it first.
    UnorderedOverwrite,
};

/// One refusal, with what it was about.
///
/// **This list exists because `Error::message` is a non-owning view and therefore a string
/// literal, and "an error naming the resource" cannot be expressed that way.** The house rule
/// sends the composed detail to `MONARC_LOG` at the failure site, and that line is still
/// emitted for the human.
///
/// **What it is not is "because a test cannot read a log line".** It can:
/// `TestSupport::LogCapture` installs a sink for the whole run and Tests/TestPassDeclaration.cpp
/// asserts on a captured line. The reason is the one the phase plan states -- inspection data
/// "structured enough to assert on field by field rather than by string matching" -- and a
/// substring match on a rendered line is precisely the assertion it forbids. Reviews on the
/// Phase A3 branch deleted assertions of that shape. A field says which kind of refusal, which
/// pass and which resource, and a wrong value fails; a `find()` on a log line says only that
/// some line contained some characters, and keeps passing while the thing it describes changes
/// underneath it. The sink is the weaker vehicle in its own right too -- process-global,
/// unsynchronised, shared by every case in the run, and bounded in both line length and line
/// count -- which is why the one case that reads it is the case about the log line itself.
///
/// So every refusal is *also* recorded here, structurally: the kind, the code and literal the
/// caller was handed, and the pass and resource involved.
///
/// **Task 2's requirement that a dependency cycle be "reported naming the passes" is the same
/// problem and wants the same vehicle, and `group` below is what makes this struct able to
/// carry it.** One `pass` field cannot. A cycle's identity is the *set* of passes in it, so one
/// diagnostic per member loses the grouping -- two overlapping cycles arrive as six unrelated
/// rows -- and a cycle longer than the room `maxDiagnostics` has left is cut in half, at which
/// point `diagnosticsDropped` says a number and nothing says which cycle the missing rows
/// belonged to.
///
/// **A group id rather than a second `pass` field or an inline pass list, because a cycle's
/// length is not bounded by anything this type is in a position to pick.** A second field caps
/// the expressible cycle at two passes. An inline list caps it at whatever N is chosen and then
/// needs its own truncation accounting *inside* one row -- a second copy of the thing
/// `diagnosticsDropped` already does once for the list -- and pays for N passes in all
/// thirty-two rows of a pool where one kind of refusal uses them. A `u32` costs neither, keeps
/// this type trivially copyable and defaulted-comparable -- which both suites that assert on it
/// rely on -- and composes with the truncation the list already reports: a reader of a clipped
/// report still sees that the surviving rows of group 0 name passes 3 and 7, and that something
/// was dropped.
///
/// What it does **not** express is the order round the cycle -- only membership, which is what
/// "naming the passes" asks for. Rows render in the order they were recorded, so a detector
/// that pushes a cycle's members in cycle order gets the order out of the list's own order for
/// nothing; one that does not has lost nothing this field promised.
struct GraphDiagnostic {
    DiagnosticKind kind = DiagnosticKind::PassPoolExhausted;

    /// The code the refusal returned to its caller.
    ErrorCode code = ErrorCode::Unknown;

    /// The literal the refusal returned. Non-owning, and always a string literal.
    std::string_view message = {};

    /// The pass involved, in declaration order, or `kNoPass` where none is.
    u32 pass = kNoPass;

    /// The resource involved, or an invalid id where none is. For `UnknownResource` this is
    /// the id that failed to resolve, which is the "naming it" half of the report.
    TextureId resource = {};

    /// Which multi-row report this row is one of, or `kNoDiagnosticGroup` for a refusal that
    /// stands alone.
    ///
    /// **`kNoDiagnosticGroup` on every refusal a *declaration* produces**, because every one of
    /// those is about one pass and one resource and stands alone. `DiagnosticKind::DependencyCycle`
    /// is the one kind that sets it: one row per pass in the cycle, all carrying the same id,
    /// and a second cycle in the same build gets the next id. See the note above for why this
    /// shape and not the two others.
    ///
    /// **What one group is, exactly, is a set of passes among which no order exists** -- which
    /// merges two cycles that share a pass into one group of three rather than reporting two
    /// groups of two. That is a deliberate narrowing of what this field could express, and
    /// Private/Compile.cpp argues it where the grouping happens: enumerating every elementary
    /// cycle is exponential in the pass count, and the mutually-dependent set is both
    /// computable in linear time and the minimal set of declarations that has to change.
    u32 group = kNoDiagnosticGroup;

    constexpr bool operator==(const GraphDiagnostic&) const = default;
};

/// Everything a graph can say about the build it is holding.
///
/// **A view, not a copy**: every span points into the graph's own arrays and is valid until
/// the next `Reset` or declaration. That is what keeps inspection free of allocation and
/// keeps the graph's decisions and its report from being two things that can disagree.
struct GraphInspection {
    GraphPhase phase = GraphPhase::Declaring;

    /// Which build this is. Bumped by `RenderGraph::Reset`; the generation every `TextureId`
    /// in this build carries.
    u32 buildGeneration = 0;

    /// Declaration order. `PassInspection::executionOrder` is the other order, and
    /// `WriteInspectionText` is what renders the list in it -- the array stays in declaration
    /// order so that an index into it is stable and a culled pass still has a row.
    std::span<const PassInspection> passes = {};

    std::span<const ResourceInspection> resources = {};

    /// Declaration order, flat across passes. See `AccessInspection`.
    std::span<const AccessInspection> accesses = {};

    /// Empty until Task 3.
    std::span<const DerivedBarrier> barriers = {};

    std::span<const GraphDiagnostic> diagnostics = {};

    /// How many diagnostics were dropped because the diagnostic pool was full.
    ///
    /// **Non-zero means `diagnostics` is not the whole story, and a reader that ignored this
    /// would read a truncated list as a complete one** -- which is the shape of failure this
    /// codebase keeps finding, a report that can be silently incomplete without saying so.
    u32 diagnosticsDropped = 0;
};

/// The enumerator's own spelling. Never nullptr; a value outside the enumerator set gets a
/// name of its own rather than any real enumerator's.
/// @{
[[nodiscard]] const char* ToString(GraphPhase phase);
[[nodiscard]] const char* ToString(ResourceOrigin origin);
[[nodiscard]] const char* ToString(GraphQueue queue);
[[nodiscard]] const char* ToString(BarrierCauseKind kind);
[[nodiscard]] const char* ToString(DiagnosticKind kind);
/// @}

/// How much of `WriteInspectionText`'s output fitted, and how much there was.
struct InspectionText {
    /// Characters actually placed in the caller's buffer.
    usize written = 0;

    /// Characters the whole report would have taken. Larger than `written` exactly when the
    /// buffer was too small.
    usize needed = 0;

    [[nodiscard]] constexpr bool Truncated() const { return needed > written; }
};

/// Renders `inspection` as stable, diffable text into `out`, and reports how much there was.
///
/// **A projection of the structure above, not the thing tests assert on.** Its callers are a
/// debug view, and a diffable artifact for when a frame changes unexpectedly -- both of which
/// Render-Graph.md names. Tests/TestGraphInspection.cpp tests *this function* against exact
/// expected text, which is the one place a string comparison is the right assertion; every
/// other suite asserts on the fields.
///
/// Stable means: one entity per line, `key=value` throughout, a fixed field order, and nothing
/// that varies between runs of the same declaration -- no addresses, no timings, no iteration
/// order that depends on a hash. Two builds that declared the same thing render identically,
/// which is what makes a diff meaningful.
///
/// **The pass lines come out in execution order, which is not the order `passes` holds them
/// in.** That array is declaration order on purpose -- see the field -- and the text is the
/// artifact a human diffs, so it is worth reading in the order the frame runs. Culled passes
/// sort to the end and stay marked in place. Everything else -- resources, accesses, barriers,
/// diagnostics -- renders in the order its own array holds, and the leading number on each line
/// is that array index. `WritePassLines` in Private/GraphInspection.cpp argues it at length.
///
/// **Two index spaces name passes, and every number that is in one of them says which.** A
/// pass has a declaration index (`PassInspection::index`, and its position in `passes`) and an
/// execution position (`PassInspection::executionOrder`), and they are **different numbers in
/// any frame whose passes were reordered or culled**, which is most of them. `decl-pass=` is
/// always the first; `order=` -- on the pass line, on the barrier line's `before-order=`, and
/// inside a barrier cause -- is always the second. Nothing renders a bare number for a pass,
/// and the leading number on every line is a row in that line's own array rather than either
/// space.
///
/// The distinction was spelled out here before anything could produce it, while a compiled
/// graph's two spaces still coincided -- so that the first report where the same printed
/// number in two lines meant two different passes was not a report somebody had already
/// learned to read the other way.
///
/// Written through `std::format_to_n`, which is ADR-0003's condition on `<format>` in runtime
/// code, and through `std::formatted_size` for the counting-only call -- see
/// Private/GraphInspection.cpp, which argues that the second meets the same condition and
/// records what each was *measured* to allocate. The short version: nothing at all under a
/// release standard library, and one transient 16-byte checked-iterator proxy per `{:x}` field
/// under a debug one. The output is **not** NUL-terminated; `written` is the length,
/// and `std::string_view(buffer, result.written)` is how to read it back. Truncation is
/// reported rather than silent -- a report that could be clipped without saying so would be
/// worse than no report, since the clipping falls at the end where the barriers are.
[[nodiscard]] InspectionText WriteInspectionText(const GraphInspection& inspection,
                                                 std::span<char>        out);

}  // namespace Monarc::Render
