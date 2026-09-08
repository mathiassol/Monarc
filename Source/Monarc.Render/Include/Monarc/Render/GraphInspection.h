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
// **Fields whose data arrives in a later task are present now, and empty.** `ResourceLifetime`
// and `ResourceInspection::aliasGroup` are Task 2's; `DerivedBarrier` and its `BarrierCause`
// are Task 3's. Each says so in its own comment, and each says what its value is until then.
// A field with nothing computing it yet is a gap that is visible; a decision the type cannot
// express is a decision no test can reach, which is the failure mode this whole header exists
// to avoid.
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
/// **Task 2 computes this; until then every lifetime is empty.** A compiled graph in Phase A4
/// Task 1 reports `kNoPass` for both ends of every resource, because nothing walks the access
/// list yet. Tests/TestPassDeclaration.cpp asserts exactly that, and names Task 2 as what
/// turns it into a real answer -- so a green suite here cannot be read as "lifetimes work".
struct ResourceLifetime {
    /// Execution-order position of the first pass that writes the resource, or `kNoPass`.
    u32 firstPass = kNoPass;

    /// Execution-order position of the last pass that reads or writes it, or `kNoPass`.
    u32 lastPass = kNoPass;

    /// Whether this names no passes at all -- either because nothing uses the resource, or
    /// because nothing has computed it yet. **Those two are not distinguished, and cannot be
    /// from this struct alone**: see the note above.
    [[nodiscard]] constexpr bool IsEmpty() const { return firstPass == kNoPass; }

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
    /// **Equal to `index` for every pass of a *compiled* graph in Phase A4 Task 1**, because
    /// nothing reorders and nothing culls yet: execution order is declaration order. Task 2 is
    /// what makes the two diverge. The qualification matters -- `AddPass` writes `kNoPass` on
    /// purpose so that a graph inspected mid-declaration cannot be read as though its order
    /// were decided, and Tests/TestPassDeclaration.cpp asserts exactly that before it compiles.
    ///
    /// **Defaulted to `kNoPass` and not to zero**, which is the sentinel convention every other
    /// "no pass here" field in this header follows. A default-constructed `PassInspection` that
    /// claimed to run first would be a hand-built report saying something its author did not.
    u32 executionOrder = kNoPass;

    /// Whether nothing consumes this pass's outputs, so it will not run.
    ///
    /// **Always false in Task 1.** Culling is Task 2's.
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
/// pass that Task 2 may cull, leaving a live resource pointing at a pass that does not run, and
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

    /// See `ResourceLifetime`: Task 2's, and empty until then.
    ResourceLifetime lifetime = {};

    /// The group of resources this one shares memory with, or `kNoAliasGroup`.
    ///
    /// **Task 2 computes this, and Phase A4 deliberately does not honour it.** The graph
    /// groups transients whose lifetimes do not overlap and whose descriptions are
    /// compatible, and emits the grouping here; the RHI still backs every transient with its
    /// own allocation, because sharing one needs sub-allocation and Monarc's memory is one
    /// allocation per resource. **So a populated alias group is a decision that was computed
    /// and reported, not memory that was saved.** Until Task 2 this is `kNoAliasGroup` for
    /// every resource.
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
    // Absent rather than hand-written, because the first real caller is Task 2's alias
    // grouping and what it needs is *compatibility* -- same format, same extent -- rather than
    // equality. Guessing at that rule now and then finding it wrong is worse than comparing
    // the two fields a test cares about, which is what Tests/TestPassDeclaration.cpp does.
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
/// **Task 3 fills this list; it is empty in Task 1, and the type is here now on purpose.** A
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
/// enumerator nothing emits would be an assertion nobody could write. Task 2's cycle report
/// adds its own.
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
    /// **`kNoDiagnosticGroup` on every diagnostic Task 1 records, and that is not a stub.**
    /// Every refusal a declaration can produce is about one pass and one resource, so every one
    /// of them stands alone; the field is here because the first refusal that is *not* -- Task
    /// 2's cycle -- would otherwise need this type changed underneath the two suites that
    /// already assert on it field by field. See the note above for why this shape and not the
    /// two others.
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
/// execution position (`PassInspection::executionOrder`), and they are different numbers the
/// moment Task 2 reorders or culls anything. `decl-pass=` is always the first;
/// `order=` -- on the pass line, on the barrier line's `before-order=`, and inside a barrier
/// cause -- is always the second. Nothing renders a bare number for a pass, and the leading
/// number on every line is a row in that line's own array rather than either space.
///
/// The distinction is *invisible* in Task 1, because a compiled graph's two spaces coincide.
/// Which is the reason to spell it now: the first report where the same printed number in two
/// lines meant two different passes would be a report somebody had already learned to read
/// the other way.
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
