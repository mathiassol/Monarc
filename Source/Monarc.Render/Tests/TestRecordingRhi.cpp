#include <doctest/doctest.h>

#include <TestSupport/RecordingRhi.h>

#include <span>

// The instrument, tested before anything is measured with it.
//
// **`TestExecute.cpp` asserts that the graph recorded exactly a given sequence of commands, and
// that assertion is only worth as much as the stub underneath it.** A stub that accepted
// anything would let `RenderGraph::Execute` record an illegal frame -- a barrier inside a
// rendering instance, a `Begin` on a list already recorded -- and the suite would stay green
// having measured nothing. So every precondition `RHI::ICommandList` and `RHI::IDevice` state is
// driven wrongly here, on purpose, and asserted to refuse.
//
// This is the same discipline `TestPassDeclaration.cpp`'s positive probes follow: an assertion
// that something is forbidden is worth making only beside evidence that the question can be
// answered the other way.

using Monarc::ErrorCode;
using Monarc::Result;
using Monarc::Status;
using Monarc::usize;
using Monarc::Render::TestSupport::RecordedKind;
using Monarc::Render::TestSupport::RecordingCommandList;
using Monarc::Render::TestSupport::RecordingDevice;
using Monarc::RHI::Access;
using Monarc::RHI::BufferHandle;
using Monarc::RHI::ClearColor;
using Monarc::RHI::ColorAttachment;
using Monarc::RHI::Extent2D;
using Monarc::RHI::Format;
using Monarc::RHI::GlobalBarrier;
using Monarc::RHI::kMaxColorAttachments;
using Monarc::RHI::LoadOp;
using Monarc::RHI::PipelineStage;
using Monarc::RHI::RenderingDescription;
using Monarc::RHI::StoreOp;
using Monarc::RHI::TextureBarrier;
using Monarc::RHI::TextureDescription;
using Monarc::RHI::TextureHandle;
using Monarc::RHI::TextureLayout;
using Monarc::RHI::TextureUsage;

namespace {

constexpr TextureDescription kTarget{Extent2D{1280, 720}, Format::B8G8R8A8_UNORM,
                                     TextureUsage::ColorAttachment};

/// A texture the stub device really made, so that the list's handle resolution has something to
/// resolve. `REQUIRE` rather than a returned `Result`, because every case below needs one and a
/// device that refused to make it has broken the case rather than answered it.
[[nodiscard]] TextureHandle MakeTarget(RecordingDevice& device,
                                       const TextureDescription& description = kTarget) {
    const Result<TextureHandle> texture = device.CreateTexture(description);
    REQUIRE(texture.has_value());
    return *texture;
}

/// The one-attachment rendering description the cases below begin a pass with.
[[nodiscard]] RenderingDescription OneAttachment(std::span<const ColorAttachment> attachments) {
    RenderingDescription description{};
    description.extent           = kTarget.extent;
    description.colorAttachments = attachments;
    return description;
}

}  // namespace

TEST_CASE("a legal frame records every call in order and violates nothing") {
    // The positive probe the refusals below need: without it, a stub that refused *everything*
    // would pass every case in this file and forbid the legal frame too.
    RecordingDevice      device;
    RecordingCommandList list(device);

    const TextureHandle target = MakeTarget(device);

    REQUIRE(list.Begin());
    list.Barrier(TextureBarrier(target, TextureLayout::Undefined, TextureLayout::ColorAttachment,
                                PipelineStage::ColorAttachmentOutput,
                                PipelineStage::ColorAttachmentOutput, Access::None,
                                Access::ColorAttachmentWrite));
    const ColorAttachment attachments[] = {
        ColorAttachment{target, LoadOp::Clear, StoreOp::Store, ClearColor{0.25F, 0.5F, 0.75F, 1.0F}}};
    REQUIRE(list.BeginRendering(OneAttachment(attachments)));
    list.EndRendering();
    REQUIRE(list.End());

    REQUIRE(list.Count() == 5u);
    CHECK(list.At(0).kind == RecordedKind::Begin);

    CHECK(list.At(1).kind == RecordedKind::TextureBarrier);
    CHECK(list.At(1).texture == target);
    CHECK(list.At(1).layoutBefore == TextureLayout::Undefined);
    CHECK(list.At(1).layoutAfter == TextureLayout::ColorAttachment);
    CHECK(list.At(1).syncBefore == PipelineStage::ColorAttachmentOutput);
    CHECK(list.At(1).syncAfter == PipelineStage::ColorAttachmentOutput);
    CHECK(list.At(1).accessBefore == Access::None);
    CHECK(list.At(1).accessAfter == Access::ColorAttachmentWrite);

    CHECK(list.At(2).kind == RecordedKind::BeginRendering);
    CHECK(list.At(2).extent.width == 1280u);
    CHECK(list.At(2).extent.height == 720u);
    REQUIRE(list.At(2).attachmentCount == 1u);
    CHECK(list.At(2).attachments[0].texture == target);
    CHECK(list.At(2).attachments[0].loadOp == LoadOp::Clear);
    CHECK(list.At(2).attachments[0].storeOp == StoreOp::Store);
    CHECK(list.At(2).attachments[0].clearValue == ClearColor{0.25F, 0.5F, 0.75F, 1.0F});

    CHECK(list.At(3).kind == RecordedKind::EndRendering);
    CHECK(list.At(4).kind == RecordedKind::End);

    CHECK(list.Violations() == 0u);
    CHECK(list.Dropped() == 0u);
    CHECK(device.Violations() == 0u);
}

TEST_CASE("Begin refuses a list already recording, and one already recorded") {
    // **Two refusals and not one**, which is the distinction `ICommandList::Begin` says a frame
    // loop gets wrong: a list `End` closed is not recording, so a check for "already recording"
    // alone would let it be begun again without a fresh `BeginFrame`.
    RecordingDevice      device;
    RecordingCommandList list(device);

    REQUIRE(list.Begin());
    const Status again = list.Begin();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == ErrorCode::InvalidArgument);

    REQUIRE(list.End());
    const Status afterEnd = list.Begin();
    REQUIRE_FALSE(afterEnd.has_value());
    CHECK(afterEnd.error().code == ErrorCode::InvalidArgument);

    CHECK(list.Violations() == 2u);
}

TEST_CASE("BeginFrame is what makes a recorded list recordable again") {
    // The other half of the case above, and the reason `MakeRecordable` exists: a real
    // `IDevice::BeginFrame` waits on the frame's timeline and resets the pool, and is the only
    // thing that legitimises a second `Begin`.
    RecordingDevice device;

    const Result<Monarc::RHI::ICommandList*> first = device.BeginFrame();
    REQUIRE(first.has_value());
    REQUIRE(*first == &device.Commands());
    REQUIRE((*first)->Begin());
    REQUIRE((*first)->End());

    const Result<Monarc::RHI::ICommandList*> second = device.BeginFrame();
    REQUIRE(second.has_value());
    CHECK((*second)->Begin());

    // The log is cumulative across the two frames, which is what lets a case read both.
    CHECK(device.Commands().Count() == 3u);
    CHECK(device.Commands().Violations() == 0u);
}

TEST_CASE("End refuses a list that is not recording, and one with a pass still open") {
    RecordingDevice      device;
    RecordingCommandList list(device);

    const Status beforeBegin = list.End();
    REQUIRE_FALSE(beforeBegin.has_value());
    CHECK(beforeBegin.error().code == ErrorCode::InvalidArgument);

    const TextureHandle   target         = MakeTarget(device);
    const ColorAttachment attachments[]  = {ColorAttachment{target, LoadOp::Clear, StoreOp::Store,
                                                            ClearColor{}}};
    REQUIRE(list.Begin());
    REQUIRE(list.BeginRendering(OneAttachment(attachments)));

    const Status withPassOpen = list.End();
    REQUIRE_FALSE(withPassOpen.has_value());
    CHECK(withPassOpen.error().code == ErrorCode::InvalidArgument);

    // And it succeeds once the pass is closed, so the refusal above is about the open pass and
    // not about the list.
    list.EndRendering();
    CHECK(list.End());
    CHECK(list.Violations() == 2u);
}

TEST_CASE("BeginRendering refuses a pass inside another, and an empty extent") {
    RecordingDevice      device;
    RecordingCommandList list(device);

    const TextureHandle   target        = MakeTarget(device);
    const ColorAttachment attachments[] = {
        ColorAttachment{target, LoadOp::Clear, StoreOp::Store, ClearColor{}}};

    REQUIRE(list.Begin());
    REQUIRE(list.BeginRendering(OneAttachment(attachments)));

    const Status nested = list.BeginRendering(OneAttachment(attachments));
    REQUIRE_FALSE(nested.has_value());
    CHECK(nested.error().code == ErrorCode::InvalidArgument);
    list.EndRendering();

    RenderingDescription empty{};
    empty.extent           = Extent2D{0, 720};
    empty.colorAttachments = attachments;
    const Status refused   = list.BeginRendering(empty);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);

    CHECK(list.Violations() == 2u);
}

TEST_CASE("BeginRendering refuses more than kMaxColorAttachments") {
    RecordingDevice      device;
    RecordingCommandList list(device);

    ColorAttachment attachments[kMaxColorAttachments + 1] = {};
    for (ColorAttachment& attachment : attachments) {
        attachment = ColorAttachment{MakeTarget(device), LoadOp::Clear, StoreOp::Store,
                                     ClearColor{}};
    }

    REQUIRE(list.Begin());
    RenderingDescription description{};
    description.extent           = kTarget.extent;
    description.colorAttachments = attachments;

    const Status refused = list.BeginRendering(description);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);

    // And exactly `kMaxColorAttachments` is accepted, so the bound is the bound rather than one
    // either side of it.
    description.colorAttachments =
        std::span<const ColorAttachment>(attachments, kMaxColorAttachments);
    CHECK(list.BeginRendering(description));
    CHECK(list.Violations() == 1u);
}

TEST_CASE("BeginRendering refuses an attachment the device does not have") {
    // **The generation check ADR-0002 exists for, reaching the render graph.** A handle to a
    // destroyed texture is stale from the destroy onwards, and a rendering pass that named one
    // would be rendering into whatever occupies the slot next.
    RecordingDevice      device;
    RecordingCommandList list(device);

    const TextureHandle target = MakeTarget(device);
    device.DestroyTexture(target);
    CHECK_FALSE(device.Resolves(target));

    const ColorAttachment attachments[] = {
        ColorAttachment{target, LoadOp::Clear, StoreOp::Store, ClearColor{}}};

    REQUIRE(list.Begin());
    const Status refused = list.BeginRendering(OneAttachment(attachments));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK(list.Violations() == 1u);
}

TEST_CASE("BeginRendering refuses an attachment without the colour-attachment usage") {
    RecordingDevice      device;
    RecordingCommandList list(device);

    const TextureHandle readback = MakeTarget(
        device, TextureDescription{Extent2D{1280, 720}, Format::B8G8R8A8_UNORM,
                                   TextureUsage::TransferSource});
    const ColorAttachment attachments[] = {
        ColorAttachment{readback, LoadOp::Clear, StoreOp::Store, ClearColor{}}};

    REQUIRE(list.Begin());
    const Status refused = list.BeginRendering(OneAttachment(attachments));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK(list.Violations() == 1u);
}

TEST_CASE("the void calls record a violation where the real ones end the process") {
    // **The stub's one deliberate divergence from `RHI::ICommandList`, and this is the case that
    // says what it is.** `Barrier` and `EndRendering` return void, so the real interface reports
    // through the assertion handler and, for `Barrier`, follows it with an unconditional break
    // and `std::abort()`. No in-process test survives that, and the assertion worth making is
    // that the call was refused -- so the stub counts it instead.
    RecordingDevice      device;
    RecordingCommandList list(device);

    const TextureHandle target = MakeTarget(device);
    const TextureBarrier barrier(target, TextureLayout::Undefined, TextureLayout::ColorAttachment,
                                 PipelineStage::ColorAttachmentOutput,
                                 PipelineStage::ColorAttachmentOutput, Access::None,
                                 Access::ColorAttachmentWrite);

    // Not recording.
    list.Barrier(barrier);
    CHECK(list.Violations() == 1u);

    // Ending a rendering pass that was never begun.
    REQUIRE(list.Begin());
    list.EndRendering();
    CHECK(list.Violations() == 2u);

    // A barrier inside a rendering instance, which is illegal in Vulkan whatever it contains.
    const ColorAttachment attachments[] = {
        ColorAttachment{target, LoadOp::Clear, StoreOp::Store, ClearColor{}}};
    REQUIRE(list.BeginRendering(OneAttachment(attachments)));
    list.Barrier(barrier);
    list.Barrier(GlobalBarrier{});
    CHECK(list.Violations() == 4u);
    list.EndRendering();

    // And a barrier naming a texture the device no longer has.
    device.DestroyTexture(target);
    list.Barrier(barrier);
    CHECK(list.Violations() == 5u);
}

TEST_CASE("a global barrier is recorded with its four fields") {
    // Nothing in `Monarc.Render` derives one -- every `DerivedBarrier` names a `TextureId` --
    // so what this buys is that a graph which started emitting them could not do it unnoticed:
    // the entry appears in the log and a case asserting an exact sequence fails on it.
    RecordingDevice      device;
    RecordingCommandList list(device);

    REQUIRE(list.Begin());
    list.Barrier(GlobalBarrier{PipelineStage::Copy, PipelineStage::ColorAttachmentOutput,
                               Access::TransferWrite, Access::ColorAttachmentRead});

    REQUIRE(list.Count() == 2u);
    CHECK(list.At(1).kind == RecordedKind::GlobalBarrier);
    CHECK(list.At(1).syncBefore == PipelineStage::Copy);
    CHECK(list.At(1).syncAfter == PipelineStage::ColorAttachmentOutput);
    CHECK(list.At(1).accessBefore == Access::TransferWrite);
    CHECK(list.At(1).accessAfter == Access::ColorAttachmentRead);
    CHECK(list.Violations() == 0u);
}

TEST_CASE("a buffer barrier is recorded with its buffer and its four fields") {
    RecordingDevice      device;
    RecordingCommandList list(device);

    const BufferHandle buffer = BufferHandle::ForTesting(3, 1);
    REQUIRE(list.Begin());
    list.Barrier(Monarc::RHI::BufferBarrier{buffer, PipelineStage::Copy,
                                            PipelineStage::ColorAttachmentOutput,
                                            Access::TransferWrite, Access::ColorAttachmentRead});

    REQUIRE(list.Count() == 2u);
    CHECK(list.At(1).kind == RecordedKind::BufferBarrier);
    CHECK(list.At(1).buffer == buffer);
    CHECK(list.At(1).syncBefore == PipelineStage::Copy);
    CHECK(list.At(1).accessAfter == Access::ColorAttachmentRead);
    CHECK(list.Violations() == 0u);
}

TEST_CASE("CopyTextureToBuffer refuses rather than succeeding silently") {
    RecordingDevice      device;
    RecordingCommandList list(device);

    REQUIRE(list.Begin());
    const Status refused =
        list.CopyTextureToBuffer(MakeTarget(device), BufferHandle::ForTesting(0, 1));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::Unsupported);
    CHECK(list.Violations() == 1u);
}

TEST_CASE("the command log drops the newest beyond its capacity and counts it") {
    // `LogCapture`'s rule, and it is here for the same reason: a case that read a truncated log
    // as a complete one would assert "these are all the commands" about a prefix.
    RecordingDevice      device;
    RecordingCommandList list(device);

    const TextureHandle target = MakeTarget(device);
    const TextureBarrier barrier(target, TextureLayout::ColorAttachment,
                                 TextureLayout::ColorAttachment,
                                 PipelineStage::ColorAttachmentOutput,
                                 PipelineStage::ColorAttachmentOutput,
                                 Access::ColorAttachmentWrite, Access::ColorAttachmentWrite);

    REQUIRE(list.Begin());
    for (usize i = 0; i < RecordingCommandList::kMaxCommands + 5; ++i) {
        list.Barrier(barrier);
    }

    CHECK(list.Count() == RecordingCommandList::kMaxCommands);
    CHECK(list.Dropped() == 6u);
    // The head is kept, so the first entry is still the `Begin`.
    CHECK(list.At(0).kind == RecordedKind::Begin);
    CHECK(list.Violations() == 0u);
}

TEST_CASE("Clear forgets the log and MakeRecordable does not") {
    RecordingDevice      device;
    RecordingCommandList list(device);

    REQUIRE(list.Begin());
    REQUIRE(list.End());
    CHECK(list.Count() == 2u);

    list.MakeRecordable();
    CHECK(list.Count() == 2u);

    list.Clear();
    CHECK(list.Count() == 0u);
    CHECK(list.Violations() == 0u);
    CHECK(list.Dropped() == 0u);
}

TEST_CASE("the device refuses a texture description it cannot make") {
    // `IDevice::CreateTexture`'s three refusals. `RenderGraph::Execute` hands this a transient's
    // declared description unchanged, so a graph that let a nonsense one through is caught here.
    RecordingDevice device;

    CHECK_FALSE(device
                    .CreateTexture(TextureDescription{Extent2D{0, 720}, Format::B8G8R8A8_UNORM,
                                                      TextureUsage::ColorAttachment})
                    .has_value());
    CHECK_FALSE(device
                    .CreateTexture(TextureDescription{Extent2D{1280, 720}, Format::Unknown,
                                                      TextureUsage::ColorAttachment})
                    .has_value());
    CHECK_FALSE(device
                    .CreateTexture(TextureDescription{Extent2D{1280, 720},
                                                      Format::B8G8R8A8_UNORM,
                                                      TextureUsage::None})
                    .has_value());
    CHECK(device.TexturesCreated() == 0u);
    CHECK(device.LiveTextures() == 0u);
}

TEST_CASE("the device's texture pool is fixed and refuses rather than growing") {
    RecordingDevice device;
    for (usize i = 0; i < RecordingDevice::kMaxTextures; ++i) {
        CHECK(device.CreateTexture(kTarget).has_value());
    }
    const Result<TextureHandle> extra = device.CreateTexture(kTarget);
    REQUIRE_FALSE(extra.has_value());
    CHECK(extra.error().code == ErrorCode::OutOfMemory);
    CHECK(device.LiveTextures() == RecordingDevice::kMaxTextures);
}

TEST_CASE("a destroyed texture's handle is stale from the destroy onwards") {
    // **ADR-0002's timing, kept because the render graph depends on it.** A slot whose
    // generation moved only when it was next claimed would leave a window -- destroyed, not yet
    // reclaimed -- in which a stale handle still matched, and *"Reset destroys the transients
    // Execute created"* would be unassertable.
    RecordingDevice device;

    const TextureHandle first = MakeTarget(device);
    CHECK(device.Resolves(first));
    CHECK(device.LiveTextures() == 1u);

    device.DestroyTexture(first);
    CHECK_FALSE(device.Resolves(first));
    CHECK(device.LiveTextures() == 0u);
    CHECK(device.TexturesCreated() == 1u);

    // The slot is reused, and the handle it hands out is not the old one.
    const TextureHandle second = MakeTarget(device);
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    CHECK(device.Resolves(second));
    CHECK_FALSE(device.Resolves(first));
    CHECK(device.TexturesCreated() == 2u);

    // Destroying twice, and destroying an invalid handle, are both no-ops -- which is what
    // `IDevice::DestroyTexture` requires, because "destroy what may or may not still exist" is
    // what a teardown path has.
    device.DestroyTexture(first);
    device.DestroyTexture(TextureHandle{});
    CHECK(device.Resolves(second));
    CHECK(device.Violations() == 0u);
}

TEST_CASE("the device refuses everything it does not implement") {
    // **Refusing rather than succeeding is what stops a future `Execute` from growing a
    // dependency this suite never checked.** Nothing in `Monarc.Render` calls any of these:
    // there is no `BufferId`, so no pass can name a buffer, and the graph neither submits nor
    // waits.
    RecordingDevice device;

    CHECK_FALSE(device.CreateBuffer(Monarc::RHI::BufferDescription{}).has_value());
    CHECK_FALSE(device.MapBufferForRead(BufferHandle::ForTesting(0, 1)).has_value());
    CHECK_FALSE(device.WaitIdle().has_value());
    device.DestroyBuffer(BufferHandle::ForTesting(0, 1));
    device.UnmapBuffer(BufferHandle::ForTesting(0, 1));

    CHECK(device.Violations() == 5u);

    RecordingCommandList list(device);
    CHECK_FALSE(device.GraphicsQueue().Submit(list).has_value());
    CHECK_FALSE(device.GraphicsQueue().Wait(1, 0).has_value());
    CHECK_FALSE(device.GraphicsQueue().CompletedValue().has_value());
    CHECK(device.GraphicsQueue().LastSubmittedValue() == 0u);
}
