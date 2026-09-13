#include <TestSupport/RecordingRhi.h>

// The device-free half of Phase A4, as a pair of objects.
//
// **Nothing in here is shipped, and nothing in the module links it**: `monarc_module` drops
// every file under a `TestSupport/` directory and `_monarc_add_test_binary` compiles them into
// the test binaries instead -- see `MONARC_TEST_SUPPORT_DIR` in CMake/MonarcModule.cmake, which
// holds the convention and the measurement that motivated it.
//
// **Why a stub rather than a device test.** `RenderGraph::Execute`'s whole output is a sequence
// of calls on an `RHI::ICommandList`, and on a real device that sequence is observable only by
// its effects -- a pixel, or a validation message, or a RenderDoc capture. Through this it is
// observable directly, in order, field by field, on a machine with no GPU. That inversion is
// what Phase A4 exists for, and the phase plan says so: "a barrier regression should fail a unit
// test, not a screenshot comparison".
//
// **The refusals are the half that is easy to leave out.** A stub that recorded every call it
// was handed and refused none would let `Execute` record `BeginRendering` inside another
// `BeginRendering`, or barrier a list it never began, and the suite would stay green -- so the
// stub would be evidence that the graph produced *some* sequence rather than a legal one. Every
// precondition `RHI::ICommandList` and `RHI::IDevice` state is therefore checked here, and
// Tests/TestExecute.cpp drives each of them wrongly on purpose.

namespace Monarc::Render::TestSupport {

namespace {

/// The entry every refusal records. Its `kind` is the whole of it: what was refused matters far
/// less than that something was, because a case asserting an exact command sequence fails on the
/// extra entry whatever it holds.
[[nodiscard]] RecordedCommand RefusalEntry() {
    RecordedCommand command{};
    command.kind = RecordedKind::Refused;
    return command;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// RecordingCommandList
// ---------------------------------------------------------------------------------------

void RecordingCommandList::Push(const RecordedCommand& command) {
    if (m_count == kMaxCommands) {
        // The *newest* is what goes, so an overflowed log holds the head of the frame and
        // `Dropped()` counts what never got in. `LogCapture` makes the same choice, and the
        // reason is the same: a case that read a truncated log as a complete one would assert
        // "these are all the commands" about a prefix.
        ++m_dropped;
        return;
    }
    m_commands[m_count] = command;
    ++m_count;
}

Error RecordingCommandList::Refused(ErrorCode code, const char* message) {
    ++m_violations;
    Push(RefusalEntry());
    return Error{code, message};
}

void RecordingCommandList::RefuseVoid() {
    ++m_violations;
    Push(RefusalEntry());
}

Status RecordingCommandList::Begin() {
    // **Both halves of `ICommandList::Begin`'s refusal, and the second is not a restatement of
    // the first.** A list `End` closed is not recording, so a "not already recording" check
    // alone would accept it -- which is the mistake a frame loop that submits and comes round
    // without a fresh `BeginFrame` makes.
    if (m_state == State::Recording) {
        return std::unexpected(
            Refused(ErrorCode::InvalidArgument, "RecordingCommandList::Begin: already recording"));
    }
    if (m_state == State::Recorded) {
        return std::unexpected(Refused(ErrorCode::InvalidArgument,
                                       "RecordingCommandList::Begin: already recorded -- only "
                                       "BeginFrame makes a list recordable again"));
    }

    m_state = State::Recording;
    RecordedCommand command{};
    command.kind = RecordedKind::Begin;
    Push(command);
    return {};
}

Status RecordingCommandList::End() {
    if (m_state != State::Recording) {
        return std::unexpected(
            Refused(ErrorCode::InvalidArgument, "RecordingCommandList::End: not recording"));
    }
    if (m_renderingOpen) {
        return std::unexpected(Refused(ErrorCode::InvalidArgument,
                                       "RecordingCommandList::End: a rendering pass is still "
                                       "open"));
    }

    m_state = State::Recorded;
    RecordedCommand command{};
    command.kind = RecordedKind::End;
    Push(command);
    return {};
}

void RecordingCommandList::Barrier(const RHI::GlobalBarrier& barrier) {
    if (m_state != State::Recording || m_renderingOpen) {
        RefuseVoid();
        return;
    }
    RecordedCommand command{};
    command.kind         = RecordedKind::GlobalBarrier;
    command.syncBefore   = barrier.syncBefore;
    command.syncAfter    = barrier.syncAfter;
    command.accessBefore = barrier.accessBefore;
    command.accessAfter  = barrier.accessAfter;
    Push(command);
}

void RecordingCommandList::Barrier(const RHI::BufferBarrier& barrier) {
    if (m_state != State::Recording || m_renderingOpen) {
        RefuseVoid();
        return;
    }
    RecordedCommand command{};
    command.kind         = RecordedKind::BufferBarrier;
    command.buffer       = barrier.buffer;
    command.syncBefore   = barrier.syncBefore;
    command.syncAfter    = barrier.syncAfter;
    command.accessBefore = barrier.accessBefore;
    command.accessAfter  = barrier.accessAfter;
    Push(command);
}

void RecordingCommandList::Barrier(const RHI::TextureBarrier& barrier) {
    // **The stale-handle case the real one ends the process over.** `ICommandList::Barrier`
    // returns void, so it cannot report a handle it cannot resolve and refuses out of band
    // instead; here it is a violation, for the reason the class comment gives.
    if (m_state != State::Recording || m_renderingOpen || !m_device->Resolves(barrier.Texture())) {
        RefuseVoid();
        return;
    }

    RecordedCommand command{};
    command.kind         = RecordedKind::TextureBarrier;
    command.texture      = barrier.Texture();
    command.layoutBefore = barrier.LayoutBefore();
    command.layoutAfter  = barrier.LayoutAfter();
    command.syncBefore   = barrier.SyncBefore();
    command.syncAfter    = barrier.SyncAfter();
    command.accessBefore = barrier.AccessBefore();
    command.accessAfter  = barrier.AccessAfter();
    Push(command);
}

Status RecordingCommandList::BeginRendering(const RHI::RenderingDescription& description) {
    if (m_state != State::Recording) {
        return std::unexpected(Refused(ErrorCode::InvalidArgument,
                                       "RecordingCommandList::BeginRendering: not recording"));
    }
    if (m_renderingOpen) {
        return std::unexpected(Refused(ErrorCode::InvalidArgument,
                                       "RecordingCommandList::BeginRendering: a rendering pass "
                                       "is already open"));
    }
    if (description.extent.IsEmpty()) {
        return std::unexpected(Refused(ErrorCode::InvalidArgument,
                                       "RecordingCommandList::BeginRendering: empty extent"));
    }
    if (description.colorAttachments.size() > RHI::kMaxColorAttachments) {
        return std::unexpected(Refused(ErrorCode::InvalidArgument,
                                       "RecordingCommandList::BeginRendering: too many colour "
                                       "attachments"));
    }
    // **One question and one refusal, which it was not until a mutation test said so.** The two
    // conditions `ICommandList::BeginRendering` names -- a texture the device does not have, and
    // one without `TextureUsage::ColorAttachment` -- were two checks here, and removing the first
    // left the suite green: a handle the device does not have has no usage either, so the second
    // refused the same call under a different stated reason. Both give
    // `ErrorCode::InvalidArgument`, so there was never a second signal for a test to read. Asking
    // `IsRenderable` once is the shape that has one answer and one mutation that kills it.
    for (const RHI::ColorAttachment& attachment : description.colorAttachments) {
        if (!m_device->IsRenderable(attachment.texture)) {
            return std::unexpected(Refused(ErrorCode::InvalidArgument,
                                           "RecordingCommandList::BeginRendering: an attachment "
                                           "names a texture this device cannot render into"));
        }
    }

    RecordedCommand command{};
    command.kind            = RecordedKind::BeginRendering;
    command.extent          = description.extent;
    command.attachmentCount = description.colorAttachments.size();
    for (usize i = 0; i < command.attachmentCount; ++i) {
        command.attachments[i] = description.colorAttachments[i];
    }
    Push(command);

    m_renderingOpen = true;
    return {};
}

void RecordingCommandList::EndRendering() {
    if (m_state != State::Recording || !m_renderingOpen) {
        RefuseVoid();
        return;
    }
    m_renderingOpen = false;
    RecordedCommand command{};
    command.kind = RecordedKind::EndRendering;
    Push(command);
}

Status RecordingCommandList::CopyTextureToBuffer(RHI::TextureHandle, RHI::BufferHandle) {
    // **No caller in `Monarc.Render`, so it refuses rather than succeeding silently.** A pass
    // holds a `TextureId` and cannot name an `RHI::BufferHandle` at all -- `PassCommandList`'s
    // own comment enumerates why each of `ICommandList`'s calls belongs to somebody else -- so
    // an implementation here would be answering a question nothing asks.
    return std::unexpected(Refused(ErrorCode::Unsupported,
                                   "RecordingCommandList::CopyTextureToBuffer: not implemented "
                                   "-- nothing in Monarc.Render records a copy"));
}

void RecordingCommandList::MakeRecordable() {
    m_state         = State::Initial;
    m_renderingOpen = false;
}

void RecordingCommandList::Clear() {
    m_count      = 0;
    m_dropped    = 0;
    m_violations = 0;
}

const RecordedCommand& RecordingCommandList::At(usize index) const {
    static const RecordedCommand kNone{};
    return index < m_count ? m_commands[index] : kNone;
}

// ---------------------------------------------------------------------------------------
// RecordingDevice
// ---------------------------------------------------------------------------------------

Result<RHI::TextureHandle> RecordingDevice::CreateTexture(
    const RHI::TextureDescription& description) {
    // `IDevice::CreateTexture`'s three refusals, kept because `RenderGraph::Execute` hands this
    // a transient's declared description unchanged -- so a graph that let a nonsense description
    // through would be caught here rather than on a driver.
    if (description.extent.IsEmpty() || description.format == RHI::Format::Unknown ||
        description.usage == RHI::TextureUsage::None) {
        ++m_violations;
        return Err(ErrorCode::InvalidArgument,
                   "RecordingDevice::CreateTexture: empty extent, unknown format, or no usage");
    }

    for (usize index = 0; index < kMaxTextures; ++index) {
        if (m_textures[index].occupied) {
            continue;
        }
        // **Bumped on the claim as well as on the destroy, which is ADR-0002's rule and
        // `VulkanDevice`'s measured behaviour -- and in *this* stub either bump alone would
        // do.** `Resolves` reads `occupied` as well as the generation, so a destroyed slot
        // refuses an old handle whether or not its generation moved, and a reclaimed slot
        // refuses it as long as *one* of the two bumps happened. Mutation-tested: removing
        // either bump on its own leaves the suite green, and removing both is what
        // *"a destroyed texture's handle is stale from the destroy onwards"* catches.
        //
        // Both are kept anyway, because the point of this stub is to behave like the device it
        // stands in for, and a graph written against a looser one would meet the real rule on a
        // driver. What is not claimed here is the window `VulkanDevice`'s own comment measured:
        // that argument is about a pool whose occupancy flag is the only other signal, and this
        // one reads that flag too.
        ++m_textures[index].generation;
        m_textures[index].occupied    = true;
        m_textures[index].description = description;
        ++m_created;
        return RHI::TextureHandle::ForTesting(static_cast<u32>(index),
                                              m_textures[index].generation);
    }

    return Err(ErrorCode::OutOfMemory, "RecordingDevice::CreateTexture: texture pool exhausted");
}

void RecordingDevice::DestroyTexture(RHI::TextureHandle texture) {
    // Safe on an invalid handle and on one already destroyed, which `IDevice::DestroyTexture`
    // requires because "destroy what may or may not still exist" is what a teardown path has.
    // Not a violation for the same reason.
    if (!Resolves(texture)) {
        return;
    }
    ++m_textures[texture.index].generation;
    m_textures[texture.index].occupied = false;
}

Result<RHI::BufferHandle> RecordingDevice::CreateBuffer(const RHI::BufferDescription&) {
    ++m_violations;
    return Err(ErrorCode::Unsupported,
               "RecordingDevice::CreateBuffer: not implemented -- Monarc.Render declares no "
               "buffers, because there is no BufferId");
}

void RecordingDevice::DestroyBuffer(RHI::BufferHandle) { ++m_violations; }

Result<std::span<const u8>> RecordingDevice::MapBufferForRead(RHI::BufferHandle) {
    ++m_violations;
    return Err(ErrorCode::Unsupported, "RecordingDevice::MapBufferForRead: not implemented");
}

void RecordingDevice::UnmapBuffer(RHI::BufferHandle) { ++m_violations; }

Result<RHI::ICommandList*> RecordingDevice::BeginFrame() {
    m_commands.MakeRecordable();
    return &m_commands;
}

Status RecordingDevice::WaitIdle() {
    // **Refused rather than answered with a trivial success.** There is no GPU here, so "the
    // device is idle" is true in a sense that would tell a caller nothing -- and a stub that
    // answered it would let a future `Execute` grow a dependency on a wait this suite never
    // checked.
    ++m_violations;
    return Err(ErrorCode::Unsupported, "RecordingDevice::WaitIdle: not implemented");
}

bool RecordingDevice::Resolves(RHI::TextureHandle texture) const {
    if (!texture.IsValid() || texture.index >= kMaxTextures) {
        return false;
    }
    const TextureSlot& slot = m_textures[texture.index];
    return slot.occupied && slot.generation == texture.generation;
}

bool RecordingDevice::IsRenderable(RHI::TextureHandle texture) const {
    return Resolves(texture) &&
           RHI::HasAny(m_textures[texture.index].description.usage,
                       RHI::TextureUsage::ColorAttachment);
}

usize RecordingDevice::LiveTextures() const {
    usize live = 0;
    for (const TextureSlot& slot : m_textures) {
        live += slot.occupied ? 1u : 0u;
    }
    return live;
}

Result<u64> RecordingDevice::RefusingQueue::Submit(RHI::ICommandList&) {
    return Err(ErrorCode::Unsupported, "RecordingDevice: this device has no queue to submit to");
}

Status RecordingDevice::RefusingQueue::Wait(u64, u64) {
    return Err(ErrorCode::Unsupported, "RecordingDevice: this device has no timeline to wait on");
}

Result<u64> RecordingDevice::RefusingQueue::CompletedValue() const {
    return Err(ErrorCode::Unsupported, "RecordingDevice: this device has no timeline");
}

}  // namespace Monarc::Render::TestSupport
