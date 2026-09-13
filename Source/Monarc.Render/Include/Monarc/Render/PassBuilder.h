#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>
#include <Monarc/Render/Access.h>
#include <Monarc/Render/ResourceId.h>

#include <cstddef>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Monarc::Render {

class RenderGraph;

/// An imported texture: a resource the graph uses and does not own.
///
/// **Every field is required by the constructor, and that is the whole design of this type.**
/// `RHI::TextureBarrier` in Monarc/RHI/Barrier.h refuses a default constructor for a reason
/// that applies exactly here: a barrier -- or an import -- whose states were left to defaults
/// is byte-identical to one whose author meant `Undefined` and `None`, so there is nothing
/// left for a run-time check to look at. The plan's requirement is that external resources be
/// "declared, not smuggled", and an import that could omit its before-and-after would be
/// smuggling with extra steps: the two states are precisely what makes the swapchain's two
/// barriers derivable rather than guessed.
///
/// So there is no default constructor and no aggregate initialisation, and omitting an
/// argument does not produce a plausible default -- it fails to compile. The diagnostics both
/// compilers give are recorded in Tests/TestPassDeclaration.cpp.
///
/// The description is required too, and not because the graph owns the memory. A rendering
/// pass is described by its attachments' extent and format, so the graph needs both to begin
/// one -- see `RHI::RenderingDescription`. For a swapchain image those come from
/// `ISwapchain::Extent()` and `ISwapchain::ImageFormat()`, which is the caller's to read and
/// not the graph's to discover.
///
/// **The outgoing state binds the graph for a resource the graph used, and for no other.** If
/// culling drops every pass that named this import, the derivation emits no transition for it at
/// all and the resource is left in the incoming state it was handed -- and the build *succeeds*,
/// because a barrier for an operation the frame did not perform would claim something the frame
/// did not do. Private/DeriveBarriers.cpp argues that decision where it is made.
///
/// Only a read-only import can reach it: culling keeps every pass that writes an import, so an
/// import the frame produced into always gets both its transitions. An importer that must know
/// which happened reads `ResourceInspection::lifetime.IsUnused()` in the report -- there is no
/// other signal, and counting barriers is not one, because a list does not say what it omits.
class TextureImport {
public:
    constexpr TextureImport(RHI::TextureHandle texture, RHI::TextureDescription description,
                            TextureState incoming, TextureState outgoing)
        : m_texture(texture),
          m_description(description),
          m_incoming(incoming),
          m_outgoing(outgoing) {}

    [[nodiscard]] constexpr RHI::TextureHandle Texture() const { return m_texture; }
    [[nodiscard]] constexpr const RHI::TextureDescription& Description() const {
        return m_description;
    }
    [[nodiscard]] constexpr TextureState Incoming() const { return m_incoming; }
    [[nodiscard]] constexpr TextureState Outgoing() const { return m_outgoing; }

    // No `operator==`, for the reason `ResourceInspection` in GraphInspection.h records at
    // length: `RHI::TextureDescription` has none, so a defaulted comparison over a member of
    // that type is implicitly deleted -- which MSVC accepts declaring and clang-cl refuses.
    // Nothing compares two imports; what a test compares is what inspection reported.

private:
    RHI::TextureHandle      m_texture;
    RHI::TextureDescription m_description;
    TextureState            m_incoming;
    TextureState            m_outgoing;
};

/// The command list a pass's recording callback is given: `RHI::ICommandList` with no
/// `Barrier` on it.
///
/// **This is ADR-0006's central promise turned into a compile error rather than a rule.** That
/// ADR's decision is that rendering features "do not record barriers, choose queues, allocate
/// transient resources, or decide execution order", and the phase plan asks whether the first
/// of those can be arranged cheaply rather than left to review. It can, and this is it: a
/// callback handed one of these cannot name `Barrier` at all, because there is no such member
/// and no way to reach the wrapped list. Tests/TestPassDeclaration.cpp pins that with
/// `static_assert`s and records the diagnostic both compilers give.
///
/// **It offers nothing yet, and that is exactly right for Phase A4 rather than an
/// unfinished job.** `RHI::ICommandList` can do four things -- `Begin`/`End`, `Barrier`,
/// `BeginRendering`/`EndRendering` and `CopyTextureToBuffer` -- and a pass may do none of
/// them: the first pair is the frame's, the second is what this class exists to forbid, the
/// third is the graph's -- a pass declares an attachment with `PassBuilder::ColorAttachment`
/// and `RenderGraph::Execute` begins rendering with it, which is the point of declaring one --
/// and the fourth names `RHI::TextureHandle` and `RHI::BufferHandle` directly, where a pass
/// holds a `TextureId`. A4's own pass records nothing at all: it clears through an attachment
/// load-op, so its callback has no work and it declares none.
///
/// **Its member list was enumerated again in Task 4, which is the edit the enumeration in
/// Tests/TestPassDeclaration.cpp was written to be re-run by, and it grew no member.** `Execute`
/// constructs one and calls `Commands()` on it; neither is new, and no recording call was
/// forwarded through it because the graph's own attachments are what `BeginRendering` is built
/// from and A4 gives a pass nothing else to record. So the guards there cover the class as it
/// stands, and `sizeof` is pinned beside them so that a *data* member added later fails a
/// build rather than waiting for the next audit.
///
/// **What it will cost, stated rather than predicted.** Each recording call a pass is
/// eventually allowed becomes one forwarding method here -- a second line to write per call,
/// over a stored pointer, with nothing virtual in this class. That cost is real and paid on
/// purpose; no claim is made here about what an optimiser does with it, because nothing has
/// measured that. The alternative considered was splitting `RHI::ICommandList` into a
/// draw-only base and a barrier-carrying derived interface, which puts the split in
/// `Monarc.RHI` where it would be motivated entirely by `Monarc.Render`'s promise and would
/// change an interface the Vulkan backend already implements. A wrapper in the module that
/// wants the guarantee is the cheaper half of that trade.
class PassCommandList {
public:
    // Non-copyable and non-movable: a pass is handed a reference for the duration of its
    // callback and has no business keeping one. Also what closes the last way out -- a copy
    // could outlive the recording it belongs to.
    PassCommandList(const PassCommandList&)            = delete;
    PassCommandList& operator=(const PassCommandList&) = delete;
    PassCommandList(PassCommandList&&)                 = delete;
    PassCommandList& operator=(PassCommandList&&)      = delete;

private:
    friend class RenderGraph;

    /// **Private, so only the graph can hand one to a pass.** A feature that could construct
    /// its own over a raw `RHI::ICommandList` would not have gained anything it did not
    /// already have, but it would make this class look like something a caller assembles
    /// rather than something it is given.
    ///
    /// **This comment promised a `ForTesting` factory "with the first test that needs to invoke
    /// a callback without a graph, which is Task 4's". Task 4 has no such test, and the factory
    /// is therefore not here.** What closes the invocation path is `RenderGraph::Execute` itself:
    /// it constructs one of these, calls `Detail::IPassRecord::Invoke` through it, and
    /// Tests/TestExecute.cpp drives that with a stub `RHI::ICommandList`, so the constructor,
    /// `Commands()` and both `Invoke` bodies are all run rather than merely asserted about.
    ///
    /// A standalone one would buy nothing anyway, and the reason is this class's own emptiness:
    /// it offers no operation, so a callback handed one cannot do anything a test could observe
    /// -- including telling which list it wraps. So the factory would be a public way to build
    /// the thing the private constructor exists to withhold, in exchange for no assertion at
    /// all. It arrives if a test ever needs one, which is the rule everything else in this module
    /// follows; until then the narrowest confinement of a `ForTesting` hole is not to open it.
    explicit PassCommandList(RHI::ICommandList& commands) : m_commands(&commands) {}

    /// The wrapped list. **Reachable only by `RenderGraph`** -- this is the single seam, and
    /// if it were public the class would forbid nothing. Task 4's `Execute` is its first
    /// caller.
    ///
    /// Its privacy, and the pointer's below, are each pinned by their own `static_assert` --
    /// `kCanGetCommandList` and `kCanReachCommandListPointer` in
    /// Tests/TestPassDeclaration.cpp. Opening either one alone is a mutation the rest of that
    /// file does not notice, which is how it went unguarded to begin with.
    [[nodiscard]] RHI::ICommandList& Commands() const { return *m_commands; }

    RHI::ICommandList* m_commands;
};

namespace Detail {

/// Largest recording callback a pass will store inline, and its alignment.
///
/// **Inline rather than heap-allocated, for `JobSystem`'s reason and with its discipline.** A
/// graph is declared and thrown away every frame, so declaration is a path a frame runs and
/// must not touch the allocator; the storage is part of the pass pool `RenderGraph::Config`
/// sizes at construction.
///
/// 64 is the same number `JobSystem::kInlineCallableSize` uses, and what it buys here is the
/// same arithmetic: a `PassRecord<Callable>` is one vtable pointer plus the callable, so on
/// this ABI 64 bytes admits a capture of up to 56 -- seven pointers. `PassBuilder::Record`'s
/// two `static_assert`s turn an over-large capture into a compile error rather than a silent
/// allocation on a path that promises not to allocate.
/// @{
inline constexpr usize kPassRecordSize  = 64;
inline constexpr usize kPassRecordAlign = alignof(std::max_align_t);
/// @}

/// Type-erased recording callback, invoked at most once per frame and destroyed on the next
/// `RenderGraph::Reset`. Mirrors `Detail::IJobCallable` in Monarc/Jobs/JobSystem.h.
class IPassRecord {
public:
    virtual ~IPassRecord() = default;

    IPassRecord(const IPassRecord&)            = delete;
    IPassRecord& operator=(const IPassRecord&) = delete;

    virtual void Invoke(PassCommandList& commands) = 0;

protected:
    IPassRecord() = default;
};

template <typename Callable>
class PassRecord final : public IPassRecord {
public:
    // A constructor template with its own forwarding-reference parameter, distinct from the
    // class's already-fixed `Callable` -- `Detail::JobCallable`'s shape, and for its reason:
    // `Record`'s `Callable&&` is a forwarding reference and may legitimately be handed an
    // lvalue, which a plain rvalue-reference constructor would reject once `Callable` is
    // fixed by this class's own instantiation.
    template <typename Forwarded>
    explicit PassRecord(Forwarded&& callable) : m_callable(std::forward<Forwarded>(callable)) {}

    void Invoke(PassCommandList& commands) override { m_callable(commands); }

private:
    Callable m_callable;
};

}  // namespace Detail

/// What one pass declares.
///
/// **A builder over a pass the graph already holds, not a description the caller fills in and
/// hands over.** `RenderGraph::AddPass` claims the pass slot first and returns one of these
/// naming it, so every declaration below writes straight into the graph's fixed pools and
/// nothing is copied at the end. A description struct would have needed its own bounded
/// arrays of reads and writes -- a second set of capacities to size, and a second place for
/// them to be exhausted.
///
/// **Every declaration returns `[[nodiscard]] Status`, and that is the design decision worth
/// arguing for.** The alternative -- infallible-looking calls that accumulate errors for
/// `Compile` to report -- reads better at a call site and loses the one thing a declaration
/// error has and a compile error does not: the call that caused it. A refusal here names the
/// resource at the line that named it wrongly. The errors are *also* recorded structurally,
/// in `GraphInspection::diagnostics`, because `Error::message` is a string literal and cannot
/// carry which resource -- see `GraphDiagnostic`.
///
/// Trivially copyable, and a value: it holds the graph, the pass index, and the build
/// generation the pass belongs to. **A builder kept across a `RenderGraph::Reset` is refused
/// rather than acted on**, which is what the generation is for -- pass indices start again
/// from zero on every build, so a stale builder would otherwise declare into whatever pass
/// now occupies its slot.
class PassBuilder {
public:
    /// Declares a transient texture: created by the graph, alive for part of this frame, and
    /// a candidate for culling and for aliasing.
    ///
    /// **Creating a resource is not accessing it.** The creating pass gets no implicit read
    /// or write, and must declare one like any other pass. That is deliberate and the plan's
    /// open question is why: a transient that nothing wrote is a case Task 2's culling has to
    /// distinguish from an imported resource whose first access is legitimately a read, and a
    /// creation call that silently counted as a write would make the first of those
    /// unrepresentable.
    ///
    /// `name` is not copied. It must reference a string literal or otherwise long-lived
    /// storage, exactly like `Error::message` and `JobSystem`'s job names.
    ///
    /// Fails with `ErrorCode::OutOfMemory` if the resource pool is full, and with
    /// `ErrorCode::NotFound` for a builder from a previous build.
    [[nodiscard]] Result<TextureId> CreateTexture(std::string_view                name,
                                                  const RHI::TextureDescription& description);

    /// Declares an imported texture: one the graph uses and does not own, with its incoming
    /// and outgoing states stated. See `TextureImport`.
    ///
    /// `name` is not copied; `CreateTexture`'s note applies.
    ///
    /// Fails with `ErrorCode::OutOfMemory` if the resource pool is full, with
    /// `ErrorCode::AlreadyExists` if this build already imported the same
    /// `RHI::TextureHandle` -- which would give one physical resource two independent
    /// identities and two independent derivations -- and with `ErrorCode::NotFound` for a
    /// builder from a previous build.
    [[nodiscard]] Result<TextureId> ImportTexture(std::string_view     name,
                                                  const TextureImport& import);

    /// Declares that this pass reads `texture`, and how.
    ///
    /// Fails with `ErrorCode::InvalidArgument` if `access` writes -- use `Write` -- if it
    /// can only name a buffer (see `AccessRequirement::namesTexture`), or if this pass already
    /// declared an access to this resource needing a *different layout* (see
    /// `DiagnosticKind::AccessLayoutConflict`); with `ErrorCode::NotFound` if `texture` names no
    /// resource in this build, including an id from a previous build; with
    /// `ErrorCode::AlreadyExists` if this pass already declared exactly this access to this
    /// resource; and with `ErrorCode::OutOfMemory` if the access pool is full.
    [[nodiscard]] Status Read(TextureId texture, ResourceAccess access);

    /// Declares that this pass writes `texture`, and how. `Read`'s failures apply, with the
    /// direction check reversed.
    ///
    /// **Declaring the same resource as both a read and a write is legal where the two ask for
    /// the same layout**, and is what a read-modify-write colour attachment is -- a
    /// `LoadOp::Load` target, or a blend. Both accesses are recorded and both are reported
    /// through inspection (see `AccessInspection`), and the derivation combines them into one
    /// required state carrying both access bits. A pair whose layouts *disagree* -- the depth
    /// test-and-write pair -- is refused instead; see
    /// `ResourceAccess::DepthStencilAttachmentWrite`.
    [[nodiscard]] Status Write(TextureId texture, ResourceAccess access);

    /// Declares that this pass renders into `texture` as a colour attachment, and how its
    /// contents are treated at each end of the pass.
    ///
    /// **This is how a pass gets an `ICommandList::BeginRendering`, and a pass that declares
    /// none gets none at all** -- no `BeginRendering`, no `EndRendering`, and its callback runs
    /// between the barriers either side of it. A compute pass is the obvious future case; a pass
    /// that only copies is one today. `RenderGraph::Execute` records that.
    ///
    /// **Declaration order is slot order, which is to say shader output location.**
    /// `RHI::RenderingDescription::colorAttachments` is a span whose index is the location a
    /// fragment shader writes through, so the first call here is location 0 and the order these
    /// calls are made in is load-bearing rather than cosmetic. `AttachmentInspection::slot`
    /// reports it.
    ///
    /// **It declares the access itself, and there is deliberately no matching `Write` call to
    /// make beside it.** The load-op already determines the access set exactly: `LoadOp::Clear`
    /// and `LoadOp::DontCare` overwrite every pixel and are `ResourceAccess::ColorAttachmentWrite`
    /// alone, and `LoadOp::Load` keeps what is there, which is the read-modify-write pair --
    /// `ColorAttachmentRead` *and* `ColorAttachmentWrite`, the two accesses `Write`'s own comment
    /// describes as legal together. Asking a caller for both an attachment and an access would
    /// create a pair that can disagree -- an attachment cleared on load whose author also
    /// declared a read is a declaration the derivation would act on and nobody meant -- and this
    /// module's habit is to make a contradiction unrepresentable rather than diagnosable.
    ///
    /// **That cuts against `CreateTexture`'s "creating a resource is not accessing it", and the
    /// difference is real.** A created resource genuinely might never be touched, which is a
    /// case culling has to be able to see. An attachment *is* the access: there is no rendering
    /// instance that names a colour attachment and does not write it.
    ///
    /// The store-op is not part of that: `StoreOp::DontCare` discards the result rather than not
    /// producing it, so it writes exactly as `StoreOp::Store` does.
    ///
    /// **Every argument is required, including the clear value**, for the reason `TextureImport`
    /// argues at length: a defaulted clear value is byte-identical to one an author meant, and
    /// `{0, 0, 0, 1}` is a plausible-looking black that a `LoadOp::Clear` attachment would
    /// silently use. A `LoadOp::Load` attachment passes `RHI::ClearColor{}` and that is a visible
    /// statement at the call site rather than an absence.
    ///
    /// **Declaring this resource as an attachment *and* calling `Write` on it is refused**, and
    /// by `DiagnosticKind::DuplicateAccess` rather than by a rule of its own -- the attachment
    /// declares the write, so the second one is the same access twice. Which of the two calls is
    /// refused is whichever came second.
    ///
    /// Fails with `ErrorCode::NotFound` for a builder from a previous build and for an id naming
    /// no resource in this build; with `ErrorCode::InvalidArgument` if the resource's
    /// `RHI::TextureDescription::usage` lacks `RHI::TextureUsage::ColorAttachment`, if its extent
    /// has a zero dimension, if this pass already declared that resource as an attachment, if
    /// this pass already has `RHI::kMaxColorAttachments` of them, or if its extent disagrees with
    /// this pass's first attachment; and with `ErrorCode::OutOfMemory` if the attachment pool or
    /// the access pool is full. Each has its own `DiagnosticKind`.
    ///
    /// **A refusal from the access half can leave the read declared and the write not**, which
    /// is the one way this call is not all-or-nothing. It costs nothing: a build with a refused
    /// declaration does not compile at all, so what is left behind is part of a frame nobody
    /// gets to run -- the same property the accesses declared before an `AccessPoolExhausted`
    /// already have.
    [[nodiscard]] Status ColorAttachment(TextureId texture, RHI::LoadOp loadOp,
                                         RHI::StoreOp storeOp, RHI::ClearColor clearValue);

    /// Sets the callback that records this pass's commands.
    ///
    /// Optional, and a pass without one is not an error: `Monarc.FirstLight`'s pass clears
    /// through an attachment load-op and has nothing to record. `PassInspection::hasRecord`
    /// reports which.
    ///
    /// The callable is moved or copied into storage inline inside the pass slot -- never
    /// heap-allocated, because declaration is a path a frame runs -- so it must fit
    /// `Detail::kPassRecordSize` at `Detail::kPassRecordAlign`. The `static_assert`s below
    /// turn an over-large capture into a compile error rather than a silent allocation.
    ///
    /// It receives a `PassCommandList&`, which is `RHI::ICommandList` with no `Barrier` on
    /// it. See that class: a pass cannot record a barrier, and that is a compile error rather
    /// than a rule.
    ///
    /// Fails with `ErrorCode::AlreadyExists` if this pass already set one, and with
    /// `ErrorCode::NotFound` for a builder from a previous build.
    template <typename Callable>
    [[nodiscard]] Status Record(Callable&& callable) {
        using Stored = std::decay_t<Callable>;
        static_assert(sizeof(Detail::PassRecord<Stored>) <= Detail::kPassRecordSize,
                      "pass recording callback does not fit RenderGraph's inline storage -- "
                      "capture less, or capture a pointer to shared state instead of the "
                      "state itself");
        static_assert(alignof(Detail::PassRecord<Stored>) <= Detail::kPassRecordAlign,
                      "pass recording callback's alignment exceeds RenderGraph's inline "
                      "storage alignment");

        const Result<void*> storage = ClaimRecordStorage();
        if (!storage) {
            return std::unexpected(storage.error());
        }
        // The `IPassRecord*` the placement new yields is committed rather than recovered
        // later from the storage address. Recovering it would mean casting `std::byte*` to a
        // base class of an object constructed there, which is only correct because this
        // hierarchy happens to have one non-virtual base at offset zero -- a property nothing
        // enforces. Keeping the pointer costs one word per pass slot and needs no such
        // premise. It is also why the slot's occupancy *is* that pointer: if the callable's
        // move constructor threw, the slot is still empty, which is the truth.
        Detail::IPassRecord* record =
            ::new (*storage) Detail::PassRecord<Stored>(std::forward<Callable>(callable));
        CommitRecord(record);
        return {};
    }

    /// This pass's index in declaration order, which is also its index into
    /// `GraphInspection::passes`.
    [[nodiscard]] u32 Index() const { return m_pass; }

private:
    friend class RenderGraph;

    PassBuilder(RenderGraph& graph, u32 pass, u32 generation)
        : m_graph(&graph), m_pass(pass), m_generation(generation) {}

    /// Reserves this pass's inline callback storage and hands back the address to
    /// placement-construct into, or refuses. Non-template so that `Record` above needs
    /// nothing of `RenderGraph`'s definition; defined in Private/RenderGraph.cpp, where
    /// `RenderGraph` is complete.
    [[nodiscard]] Result<void*> ClaimRecordStorage() const;

    /// Records that `record` now lives in this pass's storage. Only ever called immediately
    /// after `ClaimRecordStorage` returned that storage, which is why it has nothing to
    /// refuse: the pass and generation it would re-check were checked one line ago.
    void CommitRecord(Detail::IPassRecord* record) const;

    RenderGraph* m_graph;
    u32          m_pass;
    u32          m_generation;
};

}  // namespace Monarc::Render
