#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Render/RenderGraph.h>

#include <TestSupport/CapturedFrame.h>
#include <TestSupport/RecordingRhi.h>

// Execution: what the graph actually records, in order.
//
// **Device-free, and that inversion is the point of Phase A4 rather than a convenience.** Every
// case below declares a frame, compiles it, executes it into the stub `RHI::ICommandList` in
// Private/TestSupport/, and reads back the calls the graph made. There is no GPU, no Vulkan
// driver and no display anywhere in this file -- and what is asserted is not "a derivation
// produced these barriers" but "these barriers reached a command list, in this order, on this
// side of `BeginRendering`", which is the half `TestDeriveBarriers.cpp` cannot reach.
//
// **The stub is tested separately, in TestRecordingRhi.cpp**, because an instrument that refused
// nothing would let these cases pass having measured an illegal frame. Every case here asserts
// `Violations() == 0` alongside the sequence, which is how the two files join up: the graph
// recorded exactly these commands, *and* nothing it did was refused.
//
// **The twelve barrier values come from Private/TestSupport/CapturedFrame.h**, which transcribes
// A3's two RenderDoc captures and is shared with TestDeriveBarriers.cpp. They are not repeated
// here: two transcriptions of one measurement is one more than can be kept in step.

using Monarc::ErrorCode;
using Monarc::Result;
using Monarc::Status;
using Monarc::SystemAllocator;
using Monarc::usize;
using Monarc::Render::DerivedBarrier;
using Monarc::Render::GraphInspection;
using Monarc::Render::GraphPhase;
using Monarc::Render::kNoPass;
using Monarc::Render::PassBuilder;
using Monarc::Render::PassCommandList;
using Monarc::Render::RenderGraph;
using Monarc::Render::ResourceAccess;
using Monarc::Render::TextureId;
using Monarc::Render::TestSupport::kCapturedClearValue;
using Monarc::Render::TestSupport::kCapturedFirstDstAccess;
using Monarc::Render::TestSupport::kCapturedFirstDstStage;
using Monarc::Render::TestSupport::kCapturedFirstNewLayout;
using Monarc::Render::TestSupport::kCapturedFirstOldLayout;
using Monarc::Render::TestSupport::kCapturedFirstSrcAccess;
using Monarc::Render::TestSupport::kCapturedFirstSrcStage;
using Monarc::Render::TestSupport::kCapturedSecondDstAccess;
using Monarc::Render::TestSupport::kCapturedSecondDstStage;
using Monarc::Render::TestSupport::kCapturedSecondNewLayout;
using Monarc::Render::TestSupport::kCapturedSecondOldLayout;
using Monarc::Render::TestSupport::kCapturedSecondSrcAccess;
using Monarc::Render::TestSupport::kCapturedSecondSrcStage;
using Monarc::Render::TestSupport::kSwapchainDescription;
using Monarc::Render::TestSupport::RecordedCommand;
using Monarc::Render::TestSupport::RecordedKind;
using Monarc::Render::TestSupport::RecordingCommandList;
using Monarc::Render::TestSupport::RecordingDevice;
using Monarc::Render::TestSupport::SwapchainImport;
using Monarc::RHI::ClearColor;
using Monarc::RHI::Extent2D;
using Monarc::RHI::Format;
using Monarc::RHI::ICommandList;
using Monarc::RHI::LoadOp;
using Monarc::RHI::StoreOp;
using Monarc::RHI::TextureDescription;
using Monarc::RHI::TextureHandle;
using Monarc::RHI::TextureUsage;

namespace {

/// A transient the size and format of the swapchain image, renderable and sampleable as far as
/// the graph is concerned.
constexpr TextureDescription kTransientDescription{
    Extent2D{1280, 720}, Format::B8G8R8A8_UNORM, TextureUsage::ColorAttachment};

/// The list a frame records into, taken the way a frame loop takes it.
///
/// `IDevice::BeginFrame` is what makes a list recordable, so going through it rather than
/// reaching for `RecordingDevice::Commands()` is what lets a case execute twice.
[[nodiscard]] ICommandList& BeginFrame(RecordingDevice& device) {
    const Result<ICommandList*> list = device.BeginFrame();
    REQUIRE(list.has_value());
    REQUIRE(*list != nullptr);
    return **list;
}

/// A texture standing in for the acquired swapchain image, made by the device.
///
/// **Made rather than invented, because that is what a swapchain image is.**
/// `RHI::AcquiredImage` registers every swapchain image in the device's own texture pool, so a
/// handle the graph barriers and renders into resolves there like any other. A
/// `TextureHandle::ForTesting` would resolve nowhere, and the stub would refuse the frame --
/// which is the stub doing its job rather than a reason to weaken it.
[[nodiscard]] TextureHandle AcquireImage(RecordingDevice& device) {
    const Result<TextureHandle> image = device.CreateTexture(kSwapchainDescription);
    REQUIRE(image.has_value());
    return *image;
}

/// The index of the first recorded call of `kind`, or `list.Count()`.
[[nodiscard]] usize IndexOf(const RecordingCommandList& list, RecordedKind kind) {
    for (usize i = 0; i < list.Count(); ++i) {
        if (list.At(i).kind == kind) {
            return i;
        }
    }
    return list.Count();
}

/// Whether `command` carries the six ADR-0005 fields `barrier` was derived with.
[[nodiscard]] bool Matches(const RecordedCommand& command, const DerivedBarrier& barrier) {
    return command.kind == RecordedKind::TextureBarrier &&
           command.layoutBefore == barrier.layoutBefore &&
           command.layoutAfter == barrier.layoutAfter &&
           command.syncBefore == barrier.syncBefore && command.syncAfter == barrier.syncAfter &&
           command.accessBefore == barrier.accessBefore &&
           command.accessAfter == barrier.accessAfter;
}

/// Declares the frame `Monarc.FirstLight` makes -- one pass, one imported swapchain image,
/// cleared on load -- and compiles it.
void DeclareFirstLightFrame(RenderGraph& graph, TextureHandle image) {
    Result<PassBuilder> pass = graph.AddPass("Clear");
    REQUIRE(pass.has_value());
    const Result<TextureId> swapchain = pass->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(pass->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store, kCapturedClearValue));
    REQUIRE(graph.Compile());
}

}  // namespace

TEST_CASE("the FirstLight frame records A3's two barriers around one rendering pass") {
    // **The phase's headline, one level below TestDeriveBarriers.cpp's.** That file asserts the
    // derivation produced A3's twelve measured values; this one asserts they reach a command
    // list -- in the recording order, with the rendering instance between them, and with nothing
    // else recorded at all.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);
    DeclareFirstLightFrame(graph, image);

    ICommandList& commands = BeginFrame(device);
    REQUIRE(graph.Execute(device, commands));

    const RecordingCommandList& list = device.Commands();
    REQUIRE(list.Count() == 6u);
    CHECK(list.Dropped() == 0u);
    CHECK(list.Violations() == 0u);
    CHECK(device.Violations() == 0u);

    CHECK(list.At(0).kind == RecordedKind::Begin);

    CHECK(list.At(1).kind == RecordedKind::TextureBarrier);
    CHECK(list.At(1).texture == image);
    CHECK(list.At(1).layoutBefore == kCapturedFirstOldLayout);
    CHECK(list.At(1).layoutAfter == kCapturedFirstNewLayout);
    CHECK(list.At(1).syncBefore == kCapturedFirstSrcStage);
    CHECK(list.At(1).syncAfter == kCapturedFirstDstStage);
    CHECK(list.At(1).accessBefore == kCapturedFirstSrcAccess);
    CHECK(list.At(1).accessAfter == kCapturedFirstDstAccess);

    CHECK(list.At(2).kind == RecordedKind::BeginRendering);
    CHECK(list.At(2).extent == kSwapchainDescription.extent);
    REQUIRE(list.At(2).attachmentCount == 1u);
    CHECK(list.At(2).attachments[0].texture == image);
    CHECK(list.At(2).attachments[0].loadOp == LoadOp::Clear);
    CHECK(list.At(2).attachments[0].storeOp == StoreOp::Store);
    CHECK(list.At(2).attachments[0].clearValue == kCapturedClearValue);

    CHECK(list.At(3).kind == RecordedKind::EndRendering);

    CHECK(list.At(4).kind == RecordedKind::TextureBarrier);
    CHECK(list.At(4).texture == image);
    CHECK(list.At(4).layoutBefore == kCapturedSecondOldLayout);
    CHECK(list.At(4).layoutAfter == kCapturedSecondNewLayout);
    CHECK(list.At(4).syncBefore == kCapturedSecondSrcStage);
    CHECK(list.At(4).syncAfter == kCapturedSecondDstStage);
    CHECK(list.At(4).accessBefore == kCapturedSecondSrcAccess);
    CHECK(list.At(4).accessAfter == kCapturedSecondDstAccess);

    CHECK(list.At(5).kind == RecordedKind::End);
}

TEST_CASE("each barrier lands on the side of BeginRendering its emittedBeforePass says") {
    // **The assertion inspection cannot make, and the reason the stub exists.** A derivation
    // test can say a barrier carries `emittedBeforePass == kNoPass`; only a recording can say
    // that the barrier was therefore recorded *after* the rendering instance. This case joins
    // the two: it reads the field off the report and checks the recorded position against it,
    // rather than hardcoding either.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);
    DeclareFirstLightFrame(graph, image);
    REQUIRE(graph.Execute(device, BeginFrame(device)));

    const RecordingCommandList& list       = device.Commands();
    const GraphInspection       inspection = graph.Inspect();
    const usize                 beginPass  = IndexOf(list, RecordedKind::BeginRendering);
    const usize                 endPass    = IndexOf(list, RecordedKind::EndRendering);
    REQUIRE(beginPass < list.Count());
    REQUIRE(endPass < list.Count());

    REQUIRE(inspection.barriers.size() == 2u);
    for (const DerivedBarrier& barrier : inspection.barriers) {
        usize recordedAt = list.Count();
        for (usize i = 0; i < list.Count(); ++i) {
            if (Matches(list.At(i), barrier)) {
                recordedAt = i;
                break;
            }
        }
        CAPTURE(recordedAt);
        REQUIRE(recordedAt < list.Count());
        if (barrier.emittedBeforePass == kNoPass) {
            CHECK(recordedAt > endPass);
        } else {
            CHECK(recordedAt < beginPass);
        }
    }
}

TEST_CASE("a pass's callback runs exactly once, inside the rendering instance") {
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);

    Result<PassBuilder> pass = graph.AddPass("Clear");
    REQUIRE(pass.has_value());
    const Result<TextureId> swapchain = pass->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(pass->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store, kCapturedClearValue));

    // **The callback reads the command log's length, which is how "inside" is observable at
    // all.** `PassCommandList` offers no operation -- that is ADR-0006's promise, kept by the
    // compiler -- so a callback cannot record anything of its own to be found later. What it
    // can do is look at what the *graph* has recorded so far, and the answer pins its position
    // between `BeginRendering` and `EndRendering`.
    int   invocations   = 0;
    usize countAtInvoke = 0;
    REQUIRE(pass->Record([&invocations, &countAtInvoke, &device](PassCommandList&) {
        ++invocations;
        countAtInvoke = device.Commands().Count();
    }));

    REQUIRE(graph.Compile());
    REQUIRE(graph.Execute(device, BeginFrame(device)));

    const RecordingCommandList& list = device.Commands();
    CHECK(invocations == 1);
    // Begin, the opening barrier and `BeginRendering` are recorded; `EndRendering` is not.
    CHECK(countAtInvoke == IndexOf(list, RecordedKind::EndRendering));
    CHECK(countAtInvoke == IndexOf(list, RecordedKind::BeginRendering) + 1u);
    CHECK(list.Violations() == 0u);
}

TEST_CASE("a culled pass's callback does not run") {
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);

    // A pass whose only output is a transient nothing reads. `CullPasses` drops it, so nothing
    // it declared reaches the command list -- neither its attachment nor its callback.
    Result<PassBuilder> culled = graph.AddPass("Culled");
    REQUIRE(culled.has_value());
    const Result<TextureId> scratch = culled->CreateTexture("Scratch", kTransientDescription);
    REQUIRE(scratch.has_value());
    REQUIRE(culled->ColorAttachment(*scratch, LoadOp::Clear, StoreOp::Store, ClearColor{}));

    int culledInvocations = 0;
    REQUIRE(culled->Record([&culledInvocations](PassCommandList&) { ++culledInvocations; }));

    DeclareFirstLightFrame(graph, image);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 2u);
    REQUIRE(inspection.passes[0].culled);

    const usize createdBefore = device.TexturesCreated();
    REQUIRE(graph.Execute(device, BeginFrame(device)));

    CHECK(culledInvocations == 0);
    // And no texture was created for the culled pass's transient, which is what culling buys in
    // memory rather than only in recorded commands.
    CHECK(device.TexturesCreated() == createdBefore);

    // The surviving frame is unchanged: the same six commands, with one rendering instance.
    const RecordingCommandList& list = device.Commands();
    CHECK(list.Count() == 6u);
    CHECK(list.Violations() == 0u);
}

TEST_CASE("passes record in execution order, not declaration order") {
    // **A frame the topological sort reorders, so this cannot pass by coincidence.** "Shade" is
    // declared first and reads the transient "Draw" writes, so the edge rule puts "Draw" before
    // it in every order the sort can produce -- and the recorded frame has to follow.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);

    int order[2]   = {-1, -1};
    int invocation = 0;

    Result<PassBuilder> shade = graph.AddPass("Shade");
    REQUIRE(shade.has_value());
    const Result<TextureId> offscreen = shade->CreateTexture("Offscreen", kTransientDescription);
    REQUIRE(offscreen.has_value());
    REQUIRE(shade->Read(*offscreen, ResourceAccess::SampledRead));
    const Result<TextureId> swapchain = shade->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(shade->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store, kCapturedClearValue));
    REQUIRE(shade->Record([&order, &invocation](PassCommandList&) { order[1] = invocation++; }));

    Result<PassBuilder> draw = graph.AddPass("Draw");
    REQUIRE(draw.has_value());
    REQUIRE(draw->ColorAttachment(*offscreen, LoadOp::Clear, StoreOp::Store, ClearColor{}));
    REQUIRE(draw->Record([&order, &invocation](PassCommandList&) { order[0] = invocation++; }));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 2u);
    // Declaration order and execution order genuinely disagree, which is what makes the
    // assertion below about the recording rather than about the array.
    CHECK(inspection.passes[0].executionOrder == 1u);
    CHECK(inspection.passes[1].executionOrder == 0u);

    REQUIRE(graph.Execute(device, BeginFrame(device)));

    // "Draw" ran first, though it was declared second.
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);

    // And the recording says the same: the first rendering instance is the offscreen target's,
    // the second is the swapchain's.
    const RecordingCommandList& list  = device.Commands();
    const usize                 first = IndexOf(list, RecordedKind::BeginRendering);
    REQUIRE(first < list.Count());
    REQUIRE(list.At(first).attachmentCount == 1u);
    CHECK(list.At(first).attachments[0].texture != image);
    CHECK(list.At(first).attachments[0].clearValue == ClearColor{});
    CHECK(list.Violations() == 0u);
}

TEST_CASE("a pass with no attachments gets no BeginRendering, and still records") {
    // A pass that declares its write through `Write` rather than through `ColorAttachment` has
    // no rendering instance at all -- which is what a compute pass will be, and what a pass that
    // only copies is today. `PassBuilder::ColorAttachment` states it from the declaration side.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);

    Result<PassBuilder> pass = graph.AddPass("NoAttachment");
    REQUIRE(pass.has_value());
    const Result<TextureId> swapchain = pass->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(pass->Write(*swapchain, ResourceAccess::ColorAttachmentWrite));

    int invocations = 0;
    REQUIRE(pass->Record([&invocations](PassCommandList&) { ++invocations; }));
    REQUIRE(graph.Compile());
    REQUIRE(graph.Execute(device, BeginFrame(device)));

    const RecordingCommandList& list = device.Commands();
    REQUIRE(list.Count() == 4u);
    CHECK(list.At(0).kind == RecordedKind::Begin);
    CHECK(list.At(1).kind == RecordedKind::TextureBarrier);
    CHECK(list.At(2).kind == RecordedKind::TextureBarrier);
    CHECK(list.At(3).kind == RecordedKind::End);
    CHECK(IndexOf(list, RecordedKind::BeginRendering) == list.Count());
    CHECK(IndexOf(list, RecordedKind::EndRendering) == list.Count());
    CHECK(invocations == 1);
    CHECK(list.Violations() == 0u);
}

TEST_CASE("Execute refuses a graph that never compiled, and records nothing") {
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);
    Result<PassBuilder> pass  = graph.AddPass("Clear");
    REQUIRE(pass.has_value());
    const Result<TextureId> swapchain = pass->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(pass->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store, kCapturedClearValue));

    const Status refused = graph.Execute(device, BeginFrame(device));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);

    // **Untouched, not merely un-ended.** The refusal is made before `Begin`, so the caller's
    // list is exactly as it was handed over.
    CHECK(device.Commands().Count() == 0u);
    CHECK_FALSE(device.Commands().IsRecording());
    CHECK(device.Commands().Violations() == 0u);
}

TEST_CASE("Execute refuses a graph whose compile failed, and records nothing") {
    // **Distinct from the case above, and `GraphPhase` is what distinguishes them.** A failed
    // compile leaves a report full of diagnostics and, in some cases, a partial barrier list;
    // recording it would put part of a frame nobody asked for into the caller's command list.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Reader");
    REQUIRE(pass.has_value());
    const Result<TextureId> unwritten = pass->CreateTexture("Unwritten", kTransientDescription);
    REQUIRE(unwritten.has_value());
    REQUIRE(pass->Read(*unwritten, ResourceAccess::SampledRead));
    REQUIRE_FALSE(graph.Compile().has_value());
    REQUIRE(graph.Inspect().phase == GraphPhase::CompileFailed);

    const Status refused = graph.Execute(device, BeginFrame(device));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK(device.Commands().Count() == 0u);
    CHECK(device.Commands().Violations() == 0u);
}

TEST_CASE("a device that cannot create a transient refuses before the list is touched") {
    // **The ordering that keeps the untouched class as wide as it can be.** Transients are
    // created before `Begin`, so a device that refuses one costs the caller nothing but the
    // textures it did create -- which the graph owns and `Reset` destroys.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);

    Result<PassBuilder> draw = graph.AddPass("Draw");
    REQUIRE(draw.has_value());
    const Result<TextureId> offscreen = draw->CreateTexture("Offscreen", kTransientDescription);
    REQUIRE(offscreen.has_value());
    REQUIRE(draw->ColorAttachment(*offscreen, LoadOp::Clear, StoreOp::Store, ClearColor{}));

    Result<PassBuilder> present = graph.AddPass("Present");
    REQUIRE(present.has_value());
    REQUIRE(present->Read(*offscreen, ResourceAccess::SampledRead));
    const Result<TextureId> swapchain =
        present->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(
        present->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store, kCapturedClearValue));
    REQUIRE(graph.Compile());

    // Fill the device so that the transient's creation has nowhere to go.
    while (device.LiveTextures() < RecordingDevice::kMaxTextures) {
        REQUIRE(device.CreateTexture(kTransientDescription).has_value());
    }

    const Status refused = graph.Execute(device, BeginFrame(device));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::OutOfMemory);
    CHECK(device.Commands().Count() == 0u);
    CHECK(device.Commands().Violations() == 0u);
}

TEST_CASE("a refusal after Begin leaves the list recording and abandoned mid-frame") {
    // **The outcome `RenderGraph::Execute` documents and a caller has to be able to reason
    // about.** `BeginRendering` returns a `Status`, so a frame can be refused after the list has
    // been begun -- and there is no unwind: `ICommandList` cannot discard what was recorded, and
    // `End` on a list with an open rendering instance is itself refused. The caller's answer is
    // to drop the recording rather than submit it.
    //
    // The frame here is the A3 one with the swapchain image destroyed between compilation and
    // execution, which is what a `Recreate` does to every handle a swapchain issued.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);
    DeclareFirstLightFrame(graph, image);

    ICommandList& commands = BeginFrame(device);
    device.DestroyTexture(image);

    const Status refused = graph.Execute(device, commands);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);

    const RecordingCommandList& list = device.Commands();
    REQUIRE(list.Count() == 3u);
    CHECK(list.At(0).kind == RecordedKind::Begin);
    // The opening barrier named a handle the device no longer has, which the stub records as a
    // violation where a real `ICommandList::Barrier` ends the process.
    CHECK(list.At(1).kind == RecordedKind::Refused);
    CHECK(list.At(2).kind == RecordedKind::Refused);
    CHECK(list.Violations() == 2u);
    CHECK(list.IsRecording());
}

TEST_CASE("Execute creates a texture per live transient and Reset destroys them") {
    // **The precondition this pins is `RenderGraph::Reset`'s**: the graph owns the transients
    // `Execute` made and destroys them there, because `Execute` returns before the list is
    // submitted and destroying them at the end of it would be a use-after-free.
    SystemAllocator allocator;
    RecordingDevice device;

    const TextureHandle image = AcquireImage(device);
    REQUIRE(device.LiveTextures() == 1u);

    {
        RenderGraph graph(allocator, RenderGraph::Config{});

        Result<PassBuilder> draw = graph.AddPass("Draw");
        REQUIRE(draw.has_value());
        const Result<TextureId> offscreen =
            draw->CreateTexture("Offscreen", kTransientDescription);
        REQUIRE(offscreen.has_value());
        REQUIRE(draw->ColorAttachment(*offscreen, LoadOp::Clear, StoreOp::Store, ClearColor{}));

        Result<PassBuilder> present = graph.AddPass("Present");
        REQUIRE(present.has_value());
        REQUIRE(present->Read(*offscreen, ResourceAccess::SampledRead));
        const Result<TextureId> swapchain =
            present->ImportTexture("Swapchain", SwapchainImport(image));
        REQUIRE(swapchain.has_value());
        REQUIRE(present->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store,
                                         kCapturedClearValue));
        REQUIRE(graph.Compile());

        REQUIRE(graph.Execute(device, BeginFrame(device)));
        CHECK(device.LiveTextures() == 2u);
        CHECK(device.TexturesCreated() == 2u);

        // **A second `Execute` of one build creates nothing twice**, which is what makes a retry
        // after a refused one leak nothing.
        REQUIRE(graph.Execute(device, BeginFrame(device)));
        CHECK(device.TexturesCreated() == 2u);
        CHECK(device.LiveTextures() == 2u);

        graph.Reset();
        // The transient is gone; the import is not, because the graph never owned it.
        CHECK(device.LiveTextures() == 1u);
        CHECK(device.Resolves(image));
    }

    // And the destructor does the same for a graph that was never reset.
    {
        RenderGraph graph(allocator, RenderGraph::Config{});

        Result<PassBuilder> draw = graph.AddPass("Draw");
        REQUIRE(draw.has_value());
        const Result<TextureId> offscreen =
            draw->CreateTexture("Offscreen", kTransientDescription);
        REQUIRE(offscreen.has_value());
        REQUIRE(draw->ColorAttachment(*offscreen, LoadOp::Clear, StoreOp::Store, ClearColor{}));

        Result<PassBuilder> present = graph.AddPass("Present");
        REQUIRE(present.has_value());
        REQUIRE(present->Read(*offscreen, ResourceAccess::SampledRead));
        const Result<TextureId> swapchain =
            present->ImportTexture("Swapchain", SwapchainImport(image));
        REQUIRE(swapchain.has_value());
        REQUIRE(present->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store,
                                         kCapturedClearValue));
        REQUIRE(graph.Compile());
        REQUIRE(graph.Execute(device, BeginFrame(device)));
        CHECK(device.LiveTextures() == 2u);
    }
    CHECK(device.LiveTextures() == 1u);
}

TEST_CASE("a transient's barriers name the texture Execute created for it") {
    // The other half of the case above: the handle the graph made is the handle it records, so
    // a transient's read-after-write barrier is a barrier a driver could act on.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);

    Result<PassBuilder> draw = graph.AddPass("Draw");
    REQUIRE(draw.has_value());
    const Result<TextureId> offscreen = draw->CreateTexture("Offscreen", kTransientDescription);
    REQUIRE(offscreen.has_value());
    REQUIRE(draw->ColorAttachment(*offscreen, LoadOp::Clear, StoreOp::Store, ClearColor{}));

    Result<PassBuilder> present = graph.AddPass("Present");
    REQUIRE(present.has_value());
    REQUIRE(present->Read(*offscreen, ResourceAccess::SampledRead));
    const Result<TextureId> swapchain =
        present->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(
        present->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store, kCapturedClearValue));
    REQUIRE(graph.Compile());
    REQUIRE(graph.Execute(device, BeginFrame(device)));

    const RecordingCommandList& list = device.Commands();
    CHECK(list.Violations() == 0u);

    // Every barrier the frame recorded names a texture the device still has, and the ones that
    // are not the import name the transient.
    usize transientBarriers = 0;
    for (usize i = 0; i < list.Count(); ++i) {
        if (list.At(i).kind != RecordedKind::TextureBarrier) {
            continue;
        }
        CHECK(device.Resolves(list.At(i).texture));
        transientBarriers += list.At(i).texture == image ? 0u : 1u;
    }
    // `Undefined -> ColorAttachment` for the write, then `ColorAttachment -> ShaderReadOnly` for
    // the read. The derivation's own count is asserted in TestDeriveBarriers.cpp; what this says
    // is that both reached the list.
    CHECK(transientBarriers == 2u);
}

TEST_CASE("Execute allocates nothing") {
    // **Declaration and compilation already promise this; execution is a path a frame runs and
    // has to keep it.** The transient textures are the one thing `Execute` acquires, and they
    // come from the device's fixed pool rather than from the graph's allocator -- so a frame
    // with transients is the case that could have broken it, and is the case used here.
    SystemAllocator allocator;
    RecordingDevice device;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const TextureHandle image = AcquireImage(device);

    Result<PassBuilder> draw = graph.AddPass("Draw");
    REQUIRE(draw.has_value());
    const Result<TextureId> offscreen = draw->CreateTexture("Offscreen", kTransientDescription);
    REQUIRE(offscreen.has_value());
    REQUIRE(draw->ColorAttachment(*offscreen, LoadOp::Clear, StoreOp::Store, ClearColor{}));

    Result<PassBuilder> present = graph.AddPass("Present");
    REQUIRE(present.has_value());
    REQUIRE(present->Read(*offscreen, ResourceAccess::SampledRead));
    const Result<TextureId> swapchain =
        present->ImportTexture("Swapchain", SwapchainImport(image));
    REQUIRE(swapchain.has_value());
    REQUIRE(
        present->ColorAttachment(*swapchain, LoadOp::Clear, StoreOp::Store, kCapturedClearValue));
    REQUIRE(graph.Compile());

    const usize before = allocator.BytesAllocated();
    REQUIRE(before > 0u);  // the pools exist, so the comparison below is not two zeroes
    REQUIRE(graph.Execute(device, BeginFrame(device)));
    CHECK(allocator.BytesAllocated() == before);

    // And `Reset` gives nothing back either: the pools keep their capacity, which is the whole
    // point of resetting rather than reconstructing.
    graph.Reset();
    CHECK(allocator.BytesAllocated() == before);
}
