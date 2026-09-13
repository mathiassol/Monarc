#pragma once

#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Device.h>

#include <span>

namespace Monarc::Render::TestSupport {

class RecordingDevice;

/// What kind of call one entry of a `RecordingCommandList`'s log is.
enum class RecordedKind : u32 {
    Begin = 0,
    End,
    GlobalBarrier,
    BufferBarrier,
    TextureBarrier,
    BeginRendering,
    EndRendering,

    /// A call the list refused, or one it has no implementation for.
    ///
    /// **The entry that exists because a stub which accepted anything would forbid nothing.**
    /// `RHI::ICommandList` states preconditions on every one of its calls, and a test that
    /// drives the graph through a stub is only evidence about the recorded frame if the stub
    /// holds the graph to them. A refused call records this instead of the call it was, so
    /// "these are exactly the commands the frame recorded" and "nothing illegal was attempted"
    /// are one assertion rather than two.
    Refused,
};

/// One call a `RecordingCommandList` was handed.
///
/// **A flat struct with a `kind` discriminator rather than a variant**, because a test reads it
/// field by field and `std::variant` would put every read behind a `get_if` that can fail. The
/// fields not belonging to `kind` keep their defaults, which `RecordedKind` is the discriminator
/// for -- the same shape `BarrierCauseSide::access` uses in GraphInspection.h and for the same
/// reason: a default is not a value that was recorded.
struct RecordedCommand {
    RecordedKind kind = RecordedKind::Begin;

    /// The texture a `TextureBarrier` named, or an attachment's in a `BeginRendering` -- see
    /// `attachments` for the latter, which is where a rendering pass's are.
    RHI::TextureHandle texture = {};

    /// The buffer a `BufferBarrier` named.
    RHI::BufferHandle buffer = {};

    /// **ADR-0005's six fields, all of them, for every barrier kind that has them.** A count
    /// would have made this stub able to say that two barriers were recorded and nothing about
    /// whether either was the one the derivation produced, which is the whole assertion Task 4
    /// exists to make.
    /// @{
    RHI::TextureLayout layoutBefore = RHI::TextureLayout::Undefined;
    RHI::TextureLayout layoutAfter  = RHI::TextureLayout::Undefined;
    RHI::PipelineStage syncBefore   = RHI::PipelineStage::None;
    RHI::PipelineStage syncAfter    = RHI::PipelineStage::None;
    RHI::Access        accessBefore = RHI::Access::None;
    RHI::Access        accessAfter  = RHI::Access::None;
    /// @}

    /// A `BeginRendering`'s render area, and its attachments in slot order.
    /// @{
    RHI::Extent2D        extent                                  = {};
    usize                attachmentCount                         = 0;
    RHI::ColorAttachment attachments[RHI::kMaxColorAttachments] = {};
    /// @}
};

/// An `RHI::ICommandList` that records what it was asked to do instead of doing it.
///
/// **This is what makes "the graph emitted exactly these barriers, in this order, around this
/// rendering pass" a device-free assertion**, which is strictly stronger than inspecting a
/// derivation and hoping execution matches it. The phase plan decided it in Task 3 and built it
/// here, where its first caller is.
///
/// **It honours `RHI::ICommandList`'s stated contracts and not merely its signatures**, because
/// a stub that accepted anything would let `RenderGraph::Execute` record an illegal sequence and
/// still pass -- the exact failure mode this codebase keeps finding. `Begin` refuses a list
/// already recording or already recorded; `End` refuses one not recording or with a rendering
/// pass still open; `BeginRendering` refuses a pass inside another, an empty extent, more than
/// `RHI::kMaxColorAttachments` attachments, and -- through the device that owns this list -- an
/// attachment whose texture the device does not have or was not created with
/// `RHI::TextureUsage::ColorAttachment`. Tests/TestRecordingRhi.cpp drives every one of those
/// wrongly on purpose, and each was mutation-tested by removing the guard and watching the case
/// go red.
///
/// The last two are **one** refusal with one message, which they were not until that mutation
/// test ran: a test could not tell them apart, so two of them was a distinction the code claimed
/// and could not keep. Private/TestSupport/RecordingRhi.cpp says so where they are checked.
///
/// **Where it deliberately diverges from the real one, and it is one thing.** `ICommandList`'s
/// four void calls report through the assertion handler and, for `Barrier`, end the process --
/// `MONARC_CHECK` followed by an unconditional break and `std::abort()`, argued at length on
/// `ICommandList::Barrier`. A stub cannot do that: the assertion under test is that the call was
/// refused, and no in-process test survives the abort to make it. So a void call in the wrong
/// state records a `RecordedKind::Refused` entry and returns, and `Violations()` counts them --
/// which is a *report* where the real one is a *stop*, and is why every case that asserts a
/// legal frame asserts `Violations() == 0` as well as the command sequence.
///
/// Owned by a `RecordingDevice`, whose `BeginFrame` hands it out and is what makes a list
/// recordable again after `End` -- the same relationship the real pair has.
class RecordingCommandList final : public RHI::ICommandList {
public:
    /// How many calls one list keeps. A frame in this suite records under a dozen; 64 is room
    /// for every case with no arithmetic to get wrong. **Beyond it the newest is dropped and
    /// `Dropped()` counts what never got in** -- `TestSupport::LogCapture` makes the same
    /// choice for the same reason: a case that read a truncated log as a complete one would
    /// assert "these are all the commands" about a prefix.
    static constexpr usize kMaxCommands = 64;

    explicit RecordingCommandList(RecordingDevice& device) : m_device(&device) {}

    [[nodiscard]] Status Begin() override;
    [[nodiscard]] Status End() override;

    void Barrier(const RHI::GlobalBarrier& barrier) override;
    void Barrier(const RHI::BufferBarrier& barrier) override;
    void Barrier(const RHI::TextureBarrier& barrier) override;

    [[nodiscard]] Status BeginRendering(const RHI::RenderingDescription& description) override;
    void                 EndRendering() override;

    [[nodiscard]] Status CopyTextureToBuffer(RHI::TextureHandle source,
                                             RHI::BufferHandle  destination) override;

    /// Makes this list recordable again, as `IDevice::BeginFrame` does for a real one.
    ///
    /// **It does not clear the log**, so a test that executes two frames reads both. `Clear`
    /// is how a test forgets what it has read.
    void MakeRecordable();

    /// Forgets every recorded call, the dropped count and the violation count.
    void Clear();

    [[nodiscard]] usize Count() const { return m_count; }
    [[nodiscard]] usize Dropped() const { return m_dropped; }

    /// How many calls were refused -- a `Status` refusal or a void call that the real interface
    /// would have ended the process over. See the class comment on the divergence.
    [[nodiscard]] usize Violations() const { return m_violations; }

    /// Call `index`, or a default-constructed entry if there is no such call. A test that asks
    /// for one past the end gets a `RecordedKind::Begin` with every other field defaulted, which
    /// is why `Count()` is asserted first everywhere.
    [[nodiscard]] const RecordedCommand& At(usize index) const;

    [[nodiscard]] bool IsRecording() const { return m_state == State::Recording; }

private:
    enum class State : u32 {
        /// Never begun, or made recordable again by `MakeRecordable`.
        Initial = 0,
        Recording,

        /// `End` has run. **Distinct from `Initial`, which is the distinction
        /// `ICommandList::Begin` says a frame loop gets wrong**: a list `End` closed is not
        /// recording, so a "not already recording" check alone would let it be begun again.
        Recorded,
    };

    /// Appends `command`, or counts it as dropped.
    void Push(const RecordedCommand& command);

    /// Records a refusal and returns it, so a call can `return Refused(...)`.
    [[nodiscard]] Error Refused(ErrorCode code, const char* message);

    /// The same for a void call, which has no `Error` to return.
    void RefuseVoid();

    RecordingDevice* m_device;

    State m_state           = State::Initial;
    bool  m_renderingOpen   = false;
    usize m_count           = 0;
    usize m_dropped         = 0;
    usize m_violations      = 0;

    RecordedCommand m_commands[kMaxCommands] = {};
};

/// An `RHI::IDevice` that creates handles and nothing else.
///
/// **Built because stubbing it makes the transient path device-free too, which is a large
/// win**: `RenderGraph::Execute` takes an `RHI::IDevice&` and creates a texture per transient,
/// so a suite without one could only ever test frames whose resources are all imported -- and
/// the creation, the ownership and the destruction in `Reset` would have had no test at all.
///
/// **It keeps ADR-0002's generation discipline, because the graph depends on it.** A slot's
/// generation is bumped on destroy *and* on the next claim, exactly as
/// `IDevice::DestroyTexture` requires and `VulkanDevice` implements, so a handle to a destroyed
/// transient is stale from the destroy onwards -- which is what makes
/// *"Reset destroys the transients Execute created"* assertable rather than merely stated.
///
/// **What it does not implement, it refuses.** Buffers, mapping and `WaitIdle` return
/// `ErrorCode::Unsupported` and the void ones count a violation; `GraphicsQueue` hands back a
/// queue whose every call refuses. Nothing in `Monarc.Render` calls any of them, and a stub that
/// answered them with a plausible success would let a future `Execute` grow a dependency on a
/// device this suite never checked.
class RecordingDevice final : public RHI::IDevice {
public:
    /// Texture slots. Small: the frames in this suite declare a handful of transients.
    static constexpr usize kMaxTextures = 32;

    RecordingDevice() : m_commands(*this) {}

    [[nodiscard]] const RHI::AdapterInfo& Adapter() const override { return m_adapter; }
    [[nodiscard]] RHI::IQueue&            GraphicsQueue() override { return m_queue; }

    [[nodiscard]] Result<RHI::TextureHandle> CreateTexture(
        const RHI::TextureDescription& description) override;
    void DestroyTexture(RHI::TextureHandle texture) override;

    [[nodiscard]] Result<RHI::BufferHandle> CreateBuffer(
        const RHI::BufferDescription& description) override;
    void DestroyBuffer(RHI::BufferHandle buffer) override;

    [[nodiscard]] Result<std::span<const u8>> MapBufferForRead(RHI::BufferHandle buffer) override;
    void                                      UnmapBuffer(RHI::BufferHandle buffer) override;

    [[nodiscard]] Result<RHI::ICommandList*> BeginFrame() override;
    [[nodiscard]] Status                     WaitIdle() override;

    /// The list `BeginFrame` hands out, for a test that wants to read it back without going
    /// through `Result`.
    [[nodiscard]] RecordingCommandList& Commands() { return m_commands; }

    /// Whether `texture` names a live slot of this device -- what `RecordingCommandList::Barrier`
    /// asks before recording, and what a test asks to see that a destroyed transient's handle
    /// went stale.
    [[nodiscard]] bool Resolves(RHI::TextureHandle texture) const;

    /// Whether this device could render into `texture`: a live slot, created with
    /// `RHI::TextureUsage::ColorAttachment`.
    ///
    /// **One question rather than the two `ICommandList::BeginRendering` names, because the two
    /// cannot be told apart from outside** -- a handle the device does not have has no usage
    /// either, and both refuse with `ErrorCode::InvalidArgument`. It was two checks until a
    /// mutation test showed that removing the first left the suite green; see
    /// `RecordingCommandList::BeginRendering`, which is the only caller.
    [[nodiscard]] bool IsRenderable(RHI::TextureHandle texture) const;

    /// How many textures are alive, and how many have ever been created. **Two numbers, because
    /// one cannot say whether a create-and-destroy pair happened at all** -- which is exactly
    /// what a test of `Execute` creating transients and `Reset` destroying them has to see.
    /// @{
    [[nodiscard]] usize LiveTextures() const;
    [[nodiscard]] usize TexturesCreated() const { return m_created; }
    /// @}

    /// How many calls this device refused, or had no implementation for.
    [[nodiscard]] usize Violations() const { return m_violations; }

private:
    /// A queue that refuses everything. Present because `IDevice::GraphicsQueue` returns a
    /// reference and therefore cannot refuse; nothing in `Monarc.Render` asks for one.
    class RefusingQueue final : public RHI::IQueue {
    public:
        [[nodiscard]] Result<u64> Submit(RHI::ICommandList& commands) override;
        [[nodiscard]] Status      Wait(u64 value, u64 timeoutNanoseconds) override;
        [[nodiscard]] Result<u64> CompletedValue() const override;
        [[nodiscard]] u64         LastSubmittedValue() const override { return 0; }
    };

    struct TextureSlot {
        bool                    occupied    = false;
        u32                     generation  = 0;
        RHI::TextureDescription description = {};
    };

    RHI::AdapterInfo     m_adapter = {};
    RefusingQueue        m_queue   = {};
    RecordingCommandList m_commands;

    TextureSlot m_textures[kMaxTextures] = {};
    usize       m_created                = 0;
    usize       m_violations             = 0;
};

}  // namespace Monarc::Render::TestSupport
