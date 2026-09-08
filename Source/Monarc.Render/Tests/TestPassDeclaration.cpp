#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>
#include <Monarc/Render/RenderGraph.h>

#include <TestSupport/GraphLogCapture.h>

#include <string_view>
#include <type_traits>

// **Not one line of this file needs a device, a Vulkan driver or a display, and that is the
// property Phase A4 is built around rather than a convenience.** Everything asserted here --
// what a pass declares, what the graph refuses, and what compilation settles -- is computation
// over declarations. `RenderGraph`'s constructor takes an allocator and a `Config`; the device
// and command list are parameters of `Execute`, which nothing here calls. Phase A3's problem
// was that most of its code could not run in CI; this is the inversion.

using Monarc::ErrorCode;
using Monarc::Result;
using Monarc::Status;
using Monarc::SystemAllocator;
using Monarc::Render::AccessInspection;
using Monarc::Render::DiagnosticKind;
using Monarc::Render::GraphInspection;
using Monarc::Render::GraphPhase;
using Monarc::Render::GraphQueue;
using Monarc::Render::kNoAliasGroup;
using Monarc::Render::kNoPass;
using Monarc::Render::PassBuilder;
using Monarc::Render::PassCommandList;
using Monarc::Render::RenderGraph;
using Monarc::Render::ResourceAccess;
using Monarc::Render::ResourceOrigin;
using Monarc::Render::TextureId;
using Monarc::Render::TextureImport;
using Monarc::Render::TextureState;
using Monarc::RHI::Access;
using Monarc::RHI::Format;
using Monarc::RHI::PipelineStage;
using Monarc::RHI::TextureDescription;
using Monarc::RHI::TextureHandle;
using Monarc::RHI::TextureLayout;
using Monarc::RHI::TextureUsage;

namespace {

/// The description of the swapchain image `Monarc.FirstLight` will import in Task 4, at the
/// size and format Phase A3's captures were taken at.
constexpr TextureDescription kSwapchainDescription{
    Monarc::RHI::Extent2D{1280, 720}, Format::B8G8R8A8_UNORM, TextureUsage::ColorAttachment};

/// The swapchain image's declared states, and the values are Phase A3's measured ones.
///
/// A3's `RecordFrame` barriers the acquired image `Undefined -> ColorAttachment` with
/// `ColorAttachmentOutput` on **both** sides -- the before scope chains with the acquire
/// semaphore's wait stage -- and then `ColorAttachment -> PresentSource` with `None` after,
/// because what reads the image next is the presentation engine by way of a semaphore. Those
/// are the two states an import declares, and Task 3's derivation is what has to reproduce
/// them. Written here so that Task 1's round-trip asserts the same values Task 3 will.
/// @{
constexpr TextureState kSwapchainIncoming{TextureLayout::Undefined,
                                          PipelineStage::ColorAttachmentOutput, Access::None};
constexpr TextureState kSwapchainOutgoing{TextureLayout::PresentSource, PipelineStage::None,
                                          Access::None};
/// @}

/// A handle standing in for one an `ISwapchain` would hand out. `ForTesting` because no device
/// created it -- see `RHI::Handle`'s class comment on what that name buys.
const TextureHandle kSwapchainImage = TextureHandle::ForTesting(4, 1);

[[nodiscard]] TextureImport SwapchainImport(TextureHandle image = kSwapchainImage) {
    return TextureImport(image, kSwapchainDescription, kSwapchainIncoming, kSwapchainOutgoing);
}

/// The accesses of `inspection` that name `pass`, counted.
[[nodiscard]] int AccessesOfPass(const GraphInspection& inspection, Monarc::u32 pass) {
    int count = 0;
    for (const AccessInspection& access : inspection.accesses) {
        count += access.pass == pass ? 1 : 0;
    }
    return count;
}

}  // namespace

TEST_CASE("a fresh graph is declaring, empty, and at build generation zero") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::Declaring);
    CHECK(inspection.buildGeneration == 0u);
    CHECK(graph.BuildGeneration() == 0u);
    CHECK(inspection.passes.empty());
    CHECK(inspection.resources.empty());
    CHECK(inspection.accesses.empty());
    CHECK(inspection.barriers.empty());
    CHECK(inspection.diagnostics.empty());
    CHECK(inspection.diagnosticsDropped == 0u);
}

TEST_CASE("a declaration round-trips through inspection unchanged") {
    // **The headline case of Task 1.** Everything a pass can declare goes in, and every field
    // of it comes back out of `Inspect()`. If any of these could not be read back, the graph
    // would be holding a decision no test could reach -- which is the failure mode
    // Docs/Rendering/Render-Graph.md predicts becomes urgent exactly when it is first needed.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    constexpr TextureDescription depthDescription{Monarc::RHI::Extent2D{640, 480},
                                                  Format::R8G8B8A8_UNORM,
                                                  TextureUsage::ColorAttachment};

    Result<PassBuilder> offscreen = graph.AddPass("Offscreen");
    REQUIRE(offscreen.has_value());
    const Result<TextureId> scratch = offscreen->CreateTexture("Scratch", depthDescription);
    REQUIRE(scratch.has_value());
    REQUIRE(offscreen->Write(*scratch, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> present = graph.AddPass("Present");
    REQUIRE(present.has_value());
    const Result<TextureId> image = present->ImportTexture("Swapchain", SwapchainImport());
    REQUIRE(image.has_value());
    REQUIRE(present->Read(*scratch, ResourceAccess::SampledRead));
    REQUIRE(present->Write(*image, ResourceAccess::ColorAttachmentWrite));

    int invocations = 0;
    REQUIRE(present->Record([&invocations](PassCommandList&) { ++invocations; }));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.phase == GraphPhase::Compiled);
    REQUIRE(inspection.passes.size() == 2u);
    REQUIRE(inspection.resources.size() == 2u);
    REQUIRE(inspection.accesses.size() == 3u);
    CHECK(inspection.diagnostics.empty());
    CHECK(inspection.diagnosticsDropped == 0u);

    // Passes, in declaration order, with the recording callback recorded as a fact about the
    // pass that set one and not about the other.
    CHECK(inspection.passes[0].name == "Offscreen");
    CHECK(inspection.passes[0].index == 0u);
    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK(inspection.passes[0].queue == GraphQueue::Graphics);
    CHECK_FALSE(inspection.passes[0].hasRecord);

    CHECK(inspection.passes[1].name == "Present");
    CHECK(inspection.passes[1].index == 1u);
    CHECK(inspection.passes[1].executionOrder == 1u);
    CHECK(inspection.passes[1].hasRecord);

    // The transient: its description comes back byte for byte, it has no physical texture, and
    // its import states are the defaults because it has none.
    CHECK(inspection.resources[0].name == "Scratch");
    CHECK(inspection.resources[0].id == *scratch);
    CHECK(inspection.resources[0].origin == ResourceOrigin::Transient);
    CHECK(inspection.resources[0].description.extent == depthDescription.extent);
    CHECK(inspection.resources[0].description.format == depthDescription.format);
    CHECK(inspection.resources[0].description.usage == depthDescription.usage);
    CHECK_FALSE(inspection.resources[0].importedTexture.IsValid());
    CHECK(inspection.resources[0].incoming == TextureState{});
    CHECK(inspection.resources[0].outgoing == TextureState{});

    // The import: the handle, and both states, exactly as declared. This is the pair Task 3's
    // derivation turns into the two barriers Phase A3 hand-wrote.
    CHECK(inspection.resources[1].name == "Swapchain");
    CHECK(inspection.resources[1].id == *image);
    CHECK(inspection.resources[1].origin == ResourceOrigin::Imported);
    CHECK(inspection.resources[1].importedTexture == kSwapchainImage);
    CHECK(inspection.resources[1].incoming == kSwapchainIncoming);
    CHECK(inspection.resources[1].outgoing == kSwapchainOutgoing);
    CHECK(inspection.resources[1].description.format == Format::B8G8R8A8_UNORM);

    // Accesses, flat and in declaration order.
    CHECK(inspection.accesses[0] ==
          AccessInspection{0u, *scratch, ResourceAccess::ColorAttachmentWrite});
    CHECK(inspection.accesses[1] == AccessInspection{1u, *scratch, ResourceAccess::SampledRead});
    CHECK(inspection.accesses[2] ==
          AccessInspection{1u, *image, ResourceAccess::ColorAttachmentWrite});

    // The two resources got different ids, both stamped with this build's generation. Two
    // declarations collapsing onto one id would make every later access ambiguous.
    CHECK(*scratch != *image);
    CHECK(scratch->generation == graph.BuildGeneration());
    CHECK(image->generation == graph.BuildGeneration());

    // The callback was stored and **not** invoked. Invoking it is `Execute`'s, which Task 4
    // writes; a graph that ran a pass body during compilation would be recording commands with
    // no command list.
    CHECK(invocations == 0);
}

TEST_CASE("a pass may declare the same resource as both a read and a write") {
    // Legal, and what a read-modify-write colour attachment is -- a `LoadOp::Load` target or a
    // blend. The plan asks that it be representable *and reported*, so both halves are
    // asserted: the declarations succeed, and inspection carries two accesses rather than one
    // collapsed usage.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Blend");
    REQUIRE(pass.has_value());
    const Result<TextureId> target = pass->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());

    CHECK(pass->Read(*target, ResourceAccess::ColorAttachmentRead));
    CHECK(pass->Write(*target, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.accesses.size() == 2u);
    CHECK(AccessesOfPass(inspection, 0u) == 2);
    CHECK(inspection.accesses[0].resource == *target);
    CHECK(inspection.accesses[1].resource == *target);
    CHECK(inspection.accesses[0].access == ResourceAccess::ColorAttachmentRead);
    CHECK(inspection.accesses[1].access == ResourceAccess::ColorAttachmentWrite);
    CHECK(inspection.diagnostics.empty());
}

TEST_CASE("declaring a resource no pass created is refused, and the diagnostic names it") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Consumer");
    REQUIRE(pass.has_value());

    // An id no graph handed out. `ForTesting` is the name that makes that visible at the call
    // site -- see `ResourceId`'s class comment.
    const TextureId invented = TextureId::ForTesting(7, graph.BuildGeneration());

    const Status refused = pass->Read(invented, ResourceAccess::SampledRead);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::NotFound);

    // **"An error naming it" is the diagnostic, not the `Error`.** `Error::message` is a
    // non-owning view and therefore a string literal, so the returned error cannot carry which
    // resource; the structured record can, and this is what a test asserts on. See
    // `GraphDiagnostic`.
    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::UnknownResource);
    CHECK(inspection.diagnostics[0].code == ErrorCode::NotFound);
    CHECK(inspection.diagnostics[0].pass == 0u);
    CHECK(inspection.diagnostics[0].resource == invented);
    CHECK_FALSE(inspection.diagnostics[0].message.empty());

    // Nothing was recorded as an access.
    CHECK(inspection.accesses.empty());
}

TEST_CASE("a refusal is logged with the detail its Error cannot carry") {
    // **`RenderGraph::Refuse`'s comment claims the log line carries the composed detail, and
    // this is what holds it to that.** `Error::message` is a non-owning view and therefore a
    // string literal, so the returned error cannot name the resource; the house rule sends
    // composed detail to `MONARC_LOG` at the failure site, and a claim about a log line nobody
    // reads back is a claim nothing checks. The suite captures the sink for exactly this --
    // see `TestSupport::LogCapture`.
    using Monarc::Render::TestSupport::LogCapture;

    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Consumer");
    REQUIRE(pass.has_value());

    LogCapture::Clear();
    REQUIRE(LogCapture::Count() == 0u);
    static_cast<void>(pass->Read(TextureId::ForTesting(7, graph.BuildGeneration()),
                                 ResourceAccess::SampledRead));

    REQUIRE(LogCapture::Count() == 1u);
    CHECK_FALSE(LogCapture::Truncated());
    const std::string_view line = LogCapture::Line(0);
    CAPTURE(line);
    // The three things the `Error` could not say: which kind of refusal, which pass, and which
    // resource -- the id in `index:generation` form, which is the "naming it" the plan asks
    // for. Plus the literal, so the line is self-contained for a human reading a log with no
    // debugger attached.
    CHECK(line.find("UnknownResource") != std::string_view::npos);
    CHECK(line.find("pass 0") != std::string_view::npos);
    CHECK(line.find("resource 7:0") != std::string_view::npos);
    CHECK(line.find("names no resource") != std::string_view::npos);

    // One line per refusal, not one per diagnostic slot: a graph whose diagnostic pool is full
    // still logs, which is the half of the record that never truncates.
    RenderGraph::Config tiny{};
    tiny.maxDiagnostics = 1;
    RenderGraph small(allocator, tiny);
    Result<PassBuilder> smallPass = small.AddPass("Consumer");
    REQUIRE(smallPass.has_value());

    LogCapture::Clear();
    for (int i = 0; i < 3; ++i) {
        static_cast<void>(smallPass->Read(TextureId::ForTesting(7, small.BuildGeneration()),
                                          ResourceAccess::SampledRead));
    }
    CHECK(small.Inspect().diagnostics.size() == 1u);
    CHECK(small.Inspect().diagnosticsDropped == 2u);
    CHECK(LogCapture::Count() == 3u);
}

TEST_CASE("an id from a previous build is refused") {
    // The build generation's whole purpose. Resource indices start again from zero on every
    // build, so this id's *index* names a live declaration -- it is only the generation that
    // says it belongs to a build that is gone. Dropping the generation check leaves this test
    // passing the read and failing here.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    const Result<TextureId> stale = first->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(stale.has_value());
    CHECK(stale->index == 0u);

    graph.Reset();
    CHECK(graph.BuildGeneration() == 1u);

    Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE(second.has_value());
    // A resource at the same index, so that the index alone cannot tell the two apart.
    const Result<TextureId> fresh = second->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(fresh.has_value());
    CHECK(fresh->index == stale->index);
    CHECK(fresh->generation != stale->generation);

    const Status refused = second->Write(*stale, ResourceAccess::ColorAttachmentWrite);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::NotFound);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::UnknownResource);
    CHECK(inspection.diagnostics[0].resource == *stale);

    // And the fresh id at the same index works, which is what makes the refusal above about
    // the generation rather than about the index.
    CHECK(second->Write(*fresh, ResourceAccess::ColorAttachmentWrite));
}

TEST_CASE("Read refuses a write access, and Write refuses a read access") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    const Result<TextureId> target = pass->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());

    const Status readWrote = pass->Read(*target, ResourceAccess::ColorAttachmentWrite);
    REQUIRE_FALSE(readWrote.has_value());
    CHECK(readWrote.error().code == ErrorCode::InvalidArgument);

    const Status wroteRead = pass->Write(*target, ResourceAccess::SampledRead);
    REQUIRE_FALSE(wroteRead.has_value());
    CHECK(wroteRead.error().code == ErrorCode::InvalidArgument);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 2u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::AccessDirectionMismatch);
    CHECK(inspection.diagnostics[0].resource == *target);
    CHECK(inspection.diagnostics[1].kind == DiagnosticKind::AccessDirectionMismatch);
    CHECK(inspection.accesses.empty());
}

TEST_CASE("an access that can only name a buffer is refused on a texture") {
    // `IndirectRead` is in `ResourceAccess` because ADR-0005's model arrives whole and it
    // costs one switch row -- and no `TextureId` can carry it, because a buffer has no layout.
    // Access.h and the foot of ResourceId.h are where the two membership rules that produce
    // that are argued; this is where the consequence is enforced.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Indirect");
    REQUIRE(pass.has_value());
    const Result<TextureId> target = pass->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());

    const Status refused = pass->Read(*target, ResourceAccess::IndirectRead);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::AccessNamesNoTexture);
    CHECK(inspection.diagnostics[0].resource == *target);
}

TEST_CASE("declaring the same access to the same resource twice is refused") {
    // The rule that makes one pass's accesses to one resource a set, which is what lets Task
    // 3's derivation walk consecutive accesses without deduplicating first.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    const Result<TextureId> target = pass->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());

    CHECK(pass->Write(*target, ResourceAccess::ColorAttachmentWrite));
    const Status again = pass->Write(*target, ResourceAccess::ColorAttachmentWrite);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == ErrorCode::AlreadyExists);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.accesses.size() == 1u);
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::DuplicateAccess);
}

TEST_CASE("the same access from a different pass is not a duplicate") {
    // The companion to the case above, and the one that stops the duplicate rule from being
    // written on (resource, access) alone -- which would make a texture readable by exactly
    // one pass.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> producer = graph.AddPass("Producer");
    REQUIRE(producer.has_value());
    const Result<TextureId> target = producer->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());
    CHECK(producer->Read(*target, ResourceAccess::SampledRead));

    Result<PassBuilder> consumer = graph.AddPass("Consumer");
    REQUIRE(consumer.has_value());
    CHECK(consumer->Read(*target, ResourceAccess::SampledRead));

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.accesses.size() == 2u);
    CHECK(inspection.diagnostics.empty());
}

TEST_CASE("importing the same texture twice is refused") {
    // Two identities for one image would give the derivation two independent chains of
    // accesses over it, and two independent sets of barriers -- each correct alone and wrong
    // together.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    const Result<TextureId> once = first->ImportTexture("Swapchain", SwapchainImport());
    REQUIRE(once.has_value());

    Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE(second.has_value());
    const Result<TextureId> twice = second->ImportTexture("Swapchain", SwapchainImport());
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == ErrorCode::AlreadyExists);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources.size() == 1u);
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::DuplicateImport);
    // The diagnostic names the id the *first* import produced, which is what a reader needs in
    // order to find the declaration that already owns the handle.
    CHECK(inspection.diagnostics[0].resource == *once);
    // **And it names the pass that made the refused declaration, which is pass 1 here.** This
    // is the one case in the suite that refuses from a non-zero pass, and until this line every
    // assertion on `GraphDiagnostic::pass` anywhere checked `== 0` -- so `RenderGraph::Refuse`
    // could have hard-coded zero and the whole suite would have agreed with it.
    CHECK(inspection.diagnostics[0].pass == 1u);
    // Every refusal a declaration produces is about one pass, so none of them belongs to a
    // multi-row report. See `GraphDiagnostic::group`.
    CHECK(inspection.diagnostics[0].group == Monarc::Render::kNoDiagnosticGroup);

    // A different texture imports fine, which is what stops the rule from being "one import
    // per build".
    Result<PassBuilder> third = graph.AddPass("Third");
    REQUIRE(third.has_value());
    CHECK(third->ImportTexture("Other", SwapchainImport(TextureHandle::ForTesting(9, 0)))
              .has_value());
}

TEST_CASE("importing an invalid handle is refused") {
    // What an unchecked `ISwapchain::Acquire` leaves behind. Refused here because the
    // alternative is finding out during execution, where `ICommandList::Barrier`'s answer to a
    // handle it cannot resolve is to end the process -- see that method's comment in
    // Monarc/RHI/Device.h.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Present");
    REQUIRE(pass.has_value());

    const Result<TextureId> imported = pass->ImportTexture("Nothing", SwapchainImport(TextureHandle{}));
    REQUIRE_FALSE(imported.has_value());
    CHECK(imported.error().code == ErrorCode::InvalidArgument);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources.empty());
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::InvalidImport);
}

TEST_CASE("a builder kept across Reset is refused") {
    // The same recycling hazard the build generation closes for ids, for passes. Pass indices
    // start again from zero, so this builder's index names a live pass and only its generation
    // says otherwise -- which is why every declaration checks both.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> stale = graph.AddPass("Before");
    REQUIRE(stale.has_value());
    graph.Reset();
    Result<PassBuilder> fresh = graph.AddPass("After");
    REQUIRE(fresh.has_value());
    REQUIRE(stale->Index() == fresh->Index());

    const Result<TextureId> created = stale->CreateTexture("Target", kSwapchainDescription);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == ErrorCode::NotFound);

    const Result<TextureId> imported = stale->ImportTexture("Swapchain", SwapchainImport());
    REQUIRE_FALSE(imported.has_value());
    CHECK(imported.error().code == ErrorCode::NotFound);

    const Result<TextureId> target = fresh->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());
    const Status wrote = stale->Write(*target, ResourceAccess::ColorAttachmentWrite);
    REQUIRE_FALSE(wrote.has_value());
    CHECK(wrote.error().code == ErrorCode::NotFound);

    const Status recorded = stale->Record([](PassCommandList&) {});
    REQUIRE_FALSE(recorded.has_value());
    CHECK(recorded.error().code == ErrorCode::NotFound);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 4u);
    for (const Monarc::Render::GraphDiagnostic& diagnostic : inspection.diagnostics) {
        CHECK(diagnostic.kind == DiagnosticKind::UnknownPass);
    }
    // The fresh pass declared its resource and nothing else; the stale builder wrote nothing.
    CHECK(inspection.resources.size() == 1u);
    CHECK(inspection.accesses.empty());
    CHECK_FALSE(inspection.passes[0].hasRecord);
}

TEST_CASE("a full pass pool refuses rather than growing") {
    SystemAllocator     allocator;
    RenderGraph::Config config{};
    config.maxPasses = 1;
    RenderGraph graph(allocator, config);

    REQUIRE(graph.AddPass("First").has_value());
    const Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == ErrorCode::OutOfMemory);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.passes.size() == 1u);
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::PassPoolExhausted);
}

TEST_CASE("a full resource pool refuses rather than growing") {
    SystemAllocator     allocator;
    RenderGraph::Config config{};
    config.maxResources = 1;
    RenderGraph graph(allocator, config);

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    REQUIRE(pass->CreateTexture("First", kSwapchainDescription).has_value());

    const Result<TextureId> second = pass->CreateTexture("Second", kSwapchainDescription);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == ErrorCode::OutOfMemory);

    // The import path has its own capacity check, on the far side of the duplicate scan, so it
    // needs its own case rather than inheriting this one.
    const Result<TextureId> imported = pass->ImportTexture("Swapchain", SwapchainImport());
    REQUIRE_FALSE(imported.has_value());
    CHECK(imported.error().code == ErrorCode::OutOfMemory);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources.size() == 1u);
    REQUIRE(inspection.diagnostics.size() == 2u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::ResourcePoolExhausted);
    CHECK(inspection.diagnostics[1].kind == DiagnosticKind::ResourcePoolExhausted);
}

TEST_CASE("a full access pool refuses rather than growing") {
    SystemAllocator     allocator;
    RenderGraph::Config config{};
    config.maxAccesses = 1;
    RenderGraph graph(allocator, config);

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    const Result<TextureId> target = pass->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());

    REQUIRE(pass->Write(*target, ResourceAccess::ColorAttachmentWrite));
    const Status second = pass->Read(*target, ResourceAccess::ColorAttachmentRead);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == ErrorCode::OutOfMemory);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.accesses.size() == 1u);
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::AccessPoolExhausted);
}

TEST_CASE("a full diagnostic pool counts what it dropped") {
    // A truncated diagnostics list that read as a complete one would hide the very refusal it
    // was recording. The counter is what makes "these are all of them" a checkable claim.
    SystemAllocator     allocator;
    RenderGraph::Config config{};
    config.maxDiagnostics = 2;
    RenderGraph graph(allocator, config);

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    const TextureId invented = TextureId::ForTesting(7, graph.BuildGeneration());

    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(pass->Read(invented, ResourceAccess::SampledRead).has_value());
    }

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.diagnostics.size() == 2u);
    CHECK(inspection.diagnosticsDropped == 3u);
}

TEST_CASE("a pass sets at most one recording callback") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    CHECK_FALSE(graph.Inspect().passes[0].hasRecord);

    REQUIRE(pass->Record([](PassCommandList&) {}));
    CHECK(graph.Inspect().passes[0].hasRecord);

    const Status again = pass->Record([](PassCommandList&) {});
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == ErrorCode::AlreadyExists);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::RecordAlreadySet);
    CHECK(inspection.diagnostics[0].pass == 0u);
}

TEST_CASE("a recording callback is destroyed by Reset and by the destructor") {
    // Nothing else in the suite would notice a leaked callback: the storage is inline in the
    // pass slot, so a callable whose destructor never ran leaves no allocation for the
    // sanitizer presets to find. A counter in the callable's destructor is the only witness.
    struct Witness {
        int* destructions;

        explicit Witness(int* counter) : destructions(counter) {}
        Witness(const Witness& other) : destructions(other.destructions) {}
        Witness& operator=(const Witness&) = delete;
        ~Witness() { ++*destructions; }

        void operator()(PassCommandList&) const {}
    };

    int destructions = 0;
    {
        SystemAllocator allocator;
        RenderGraph     graph(allocator, RenderGraph::Config{});

        Result<PassBuilder> first = graph.AddPass("First");
        REQUIRE(first.has_value());
        REQUIRE(first->Record(Witness{&destructions}));
        // The temporary handed to `Record` is copied into the slot and then destroyed, so the
        // count is already 1 before `Reset` destroys the stored copy.
        const int afterDeclaring = destructions;

        graph.Reset();
        CHECK(destructions == afterDeclaring + 1);

        Result<PassBuilder> second = graph.AddPass("Second");
        REQUIRE(second.has_value());
        REQUIRE(second->Record(Witness{&destructions}));
    }
    // The graph is gone; the second callback went with it. Two stored copies destroyed, plus
    // the two temporaries at the call sites.
    CHECK(destructions == 4);
}

TEST_CASE("Compile settles execution order and culls nothing") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    for (int i = 0; i < 3; ++i) {
        REQUIRE(graph.AddPass("Pass").has_value());
    }

    // No execution order before compilation, which is what stops a mid-declaration inspection
    // from being read as though the order were decided.
    const GraphInspection declaring = graph.Inspect();
    for (const Monarc::Render::PassInspection& pass : declaring.passes) {
        CHECK(pass.executionOrder == kNoPass);
    }

    REQUIRE(graph.Compile());

    const GraphInspection compiled = graph.Inspect();
    CHECK(compiled.phase == GraphPhase::Compiled);
    REQUIRE(compiled.passes.size() == 3u);
    for (Monarc::u32 i = 0; i < 3u; ++i) {
        CHECK(compiled.passes[i].index == i);
        CHECK(compiled.passes[i].executionOrder == i);
        CHECK_FALSE(compiled.passes[i].culled);
    }
}

TEST_CASE("Compile computes no lifetimes, no alias groups and no barriers in Task 1") {
    // **Asserted so that a green suite cannot be read as evidence that any of them works.**
    // The plan's Task 2 computes lifetimes and alias groups and Task 3 derives barriers; until
    // then these are the honest answers, and each of these three lines is what will have to
    // change when the task that owns it lands.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> producer = graph.AddPass("Producer");
    REQUIRE(producer.has_value());
    const Result<TextureId> target = producer->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());
    REQUIRE(producer->Write(*target, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> consumer = graph.AddPass("Consumer");
    REQUIRE(consumer.has_value());
    REQUIRE(consumer->Read(*target, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // Written and read by two different passes, so Task 2 will give this a lifetime of 0..1.
    CHECK(inspection.resources[0].lifetime.IsEmpty());
    CHECK(inspection.resources[0].lifetime.firstPass == kNoPass);
    CHECK(inspection.resources[0].lifetime.lastPass == kNoPass);
    CHECK(inspection.resources[0].aliasGroup == kNoAliasGroup);
    // Write-then-sampled-read across two passes is one barrier, and Task 3 is what derives it.
    CHECK(inspection.barriers.empty());
}

TEST_CASE("Compile refuses a second time, and refuses further declarations") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    REQUIRE(graph.AddPass("Pass").has_value());
    REQUIRE(graph.Compile());

    const Status again = graph.Compile();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == ErrorCode::InvalidArgument);

    const Result<PassBuilder> late = graph.AddPass("Late");
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().code == ErrorCode::InvalidArgument);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 2u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::AlreadyCompiled);
    CHECK(inspection.diagnostics[1].kind == DiagnosticKind::AlreadyCompiled);
    CHECK(inspection.passes.size() == 1u);
}

TEST_CASE("a build with a refused declaration does not compile") {
    // The caller here ignores the refusal deliberately -- which takes a cast, since every
    // declaration is `[[nodiscard]]`. A graph that compiled anyway would hand back a frame
    // missing exactly the access somebody got wrong.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    static_cast<void>(pass->Read(TextureId::ForTesting(3, graph.BuildGeneration()),
                                 ResourceAccess::SampledRead));

    const Status compiled = graph.Compile();
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == ErrorCode::NotFound);

    // **The inspection stays readable, which is the point of a `CompileFailed` phase rather
    // than an unusable graph**: the diagnostics are what say which declaration was at fault,
    // and a phase that hid them would hide them exactly when they are wanted.
    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::CompileFailed);
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::UnknownResource);
    // No summary diagnostic was added on top of the one that already said what happened.
    CHECK(inspection.diagnosticsDropped == 0u);
}

TEST_CASE("a refused declaration still blocks Compile when no diagnostic could be recorded") {
    // **A configuration hole, found in review rather than by a test that already existed.**
    // The refusal above rests on the diagnostics list being non-empty, and a `Config` with
    // `maxDiagnostics` at zero records nothing and counts everything -- so this graph refuses
    // a declaration, holds an empty list, and would have compiled. Zero is a value a caller
    // can legitimately choose, which is what makes this a bug rather than a hypothetical.
    SystemAllocator     allocator;
    RenderGraph::Config config{};
    config.maxDiagnostics = 0;
    RenderGraph graph(allocator, config);

    Result<PassBuilder> pass = graph.AddPass("Consumer");
    REQUIRE(pass.has_value());
    static_cast<void>(pass->Read(TextureId::ForTesting(3, graph.BuildGeneration()),
                                 ResourceAccess::SampledRead));

    REQUIRE(graph.Inspect().diagnostics.empty());
    REQUIRE(graph.Inspect().diagnosticsDropped == 1u);

    const Status compiled = graph.Compile();
    REQUIRE_FALSE(compiled.has_value());
    // `Unknown`, because the refusal that would have named a code is the thing that was
    // dropped. Reporting the dropped count is what is left to report.
    CHECK(compiled.error().code == ErrorCode::Unknown);
    CHECK(graph.Inspect().phase == GraphPhase::CompileFailed);
}

TEST_CASE("Reset clears the build, keeps the pools, and bumps the generation") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    const Result<TextureId> target = pass->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());
    REQUIRE(pass->Write(*target, ResourceAccess::ColorAttachmentWrite));
    static_cast<void>(pass->Read(TextureId::ForTesting(9, graph.BuildGeneration()),
                                 ResourceAccess::SampledRead));
    REQUIRE_FALSE(graph.Compile().has_value());
    REQUIRE(graph.Inspect().phase == GraphPhase::CompileFailed);

    graph.Reset();

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::Declaring);
    CHECK(inspection.buildGeneration == 1u);
    CHECK(inspection.passes.empty());
    CHECK(inspection.resources.empty());
    CHECK(inspection.accesses.empty());
    CHECK(inspection.diagnostics.empty());
    CHECK(inspection.diagnosticsDropped == 0u);

    // And the graph is usable again, which is what "reset" has to mean for something declared
    // and thrown away every frame.
    Result<PassBuilder> next = graph.AddPass("Next");
    REQUIRE(next.has_value());
    const Result<TextureId> fresh = next->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(fresh.has_value());
    CHECK(fresh->generation == 1u);
    CHECK(next->Write(*fresh, ResourceAccess::ColorAttachmentWrite));
    CHECK(graph.Compile());
}

TEST_CASE("a resource nothing accesses is not an error") {
    // Legal, and Task 3's own checklist depends on it staying legal: "a barrier is never
    // derived for a resource a pass declared and no pass used" is only a case if such a
    // resource can exist.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    REQUIRE(pass->CreateTexture("Unused", kSwapchainDescription).has_value());

    CHECK(graph.Compile());
    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources.size() == 1u);
    CHECK(inspection.accesses.empty());
    CHECK(inspection.diagnostics.empty());
}

TEST_CASE("a pass with no declarations at all is not an error") {
    // Whether such a pass survives culling is Task 2's question -- nothing consumes its
    // outputs because it has none. Declaring it is not itself a refusal.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    REQUIRE(graph.AddPass("Empty").has_value());
    CHECK(graph.Compile());
    CHECK(graph.Inspect().diagnostics.empty());
}

// ---------------------------------------------------------------------------------------
// ADR-0006's central promise, kept by the compiler.
//
// **"A pass may not record a barrier" is a `static_assert` here rather than a rule in a
// document, and the positive probe beside each negative one is what stops the pair from being
// vacuous.** A `requires` clause that was simply misspelled would report false for both types;
// asserting that `RHI::ICommandList` *can* do each of these is what proves the question was
// asked correctly.
//
// `PassCommandList` offers no recording call at all, which is exactly right for Phase A4 --
// see that class's comment for the enumeration of why each of `ICommandList`'s five operations
// belongs to somebody else. What these assertions pin is that `Barrier` is not among the ones
// it will ever gain, and that there is no way through the wrapper to the list underneath.
//
// **Every member of the class is covered, and that is not what this block asserted first
// time.** The `is_constructible_v` assertion below fires when the private section is opened
// wholesale, and that was taken for coverage of the members inside it -- wrongly. Opening
// *only* `Commands()` left the whole suite green while the single escape hatch to the raw
// `RHI::ICommandList&` stood open, and opening only `m_commands` did the same. Both are now
// detected on their own; see `kCanGetCommandList`.
//
// **The audit that produced those two turned up nothing else, and the class is small enough to
// enumerate.** Its declared members are the four deleted copy and move operations, the private
// constructor, `Commands()` and `m_commands`, plus an implicit destructor. Only the last three
// can yield an `RHI::ICommandList&`, and all three are now pinned -- the constructor by
// `is_constructible_v`, the other two by the requirements below. The four deleted operations
// yield nothing whatever their access: a deleted function is a diagnostic wherever it is named,
// and even undeleted, assignment needs a `PassCommandList` on both sides and so hands a pass no
// list it did not already have. The destructor produces no reference at all. There is no
// `operator RHI::ICommandList&`, which `is_convertible_v` covers, and no base class, which
// `is_base_of_v` covers.
//
// **The diagnostics, measured on both compilers, because a `static_assert` says the property
// holds and not what a caller who breaks it is told.** Each was compiled as its own
// translation unit under the module's own flags, on MSVC 19.51 and clang-cl 22.1:
//
// - `commands.Barrier(RHI::GlobalBarrier{});` -- MSVC `error C2039: 'Barrier': is not a member
//   of 'Monarc::Render::PassCommandList'`; clang-cl `error: no member named 'Barrier' in
//   'Monarc::Render::PassCommandList'`. This is ADR-0006's central promise, as a compiler
//   error.
// - `return commands.Commands();` -- MSVC `error C2248: 'Monarc::Render::PassCommandList::
//   Commands': cannot access private member declared in class`; clang-cl `error: 'Commands' is
//   a private member of 'Monarc::Render::PassCommandList'`. The seam is `RenderGraph`'s alone.
// - `PassCommandList copy = commands;` -- MSVC `error C2280: ... attempting to reference a
//   deleted function`; clang-cl `error: call to deleted constructor of 'PassCommandList'`.
//   Without this a pass could keep a copy past its own callback.
//
// **And the two assertions' own failures, measured the way every guard in this tree is: by
// breaking the thing they guard.** With `Commands()` alone made public, MSVC gives `error
// C2338: static assertion failed: 'PassCommandList::Commands() is reachable -- ...'` and
// clang-cl `error: static assertion failed due to requirement
// '!kCanGetCommandList<Monarc::Render::PassCommandList>': ...`. With `m_commands` alone made
// public, the same two forms name `kCanReachCommandListPointer`. And with the requirement
// itself misspelled to `list.Command()`, the *positive* assertion on
// `ReachableCommandList` is what fails -- MSVC `error C2607`, clang-cl `error: static
// assertion failed due to requirement
// 'kCanGetCommandList<(anonymous namespace)::ReachableCommandList>'` -- which is the
// vacuity check, and the reason the probe type exists.
// ---------------------------------------------------------------------------------------

namespace {

template <typename T>
constexpr bool kCanGlobalBarrier =
    requires(T& list, Monarc::RHI::GlobalBarrier barrier) { list.Barrier(barrier); };

template <typename T>
constexpr bool kCanTextureBarrier = requires(T& list, const Monarc::RHI::TextureBarrier& barrier) {
    list.Barrier(barrier);
};

template <typename T>
constexpr bool kCanBufferBarrier =
    requires(T& list, Monarc::RHI::BufferBarrier barrier) { list.Barrier(barrier); };

template <typename T>
constexpr bool kCanEndRendering = requires(T& list) { list.EndRendering(); };

/// Detects the accessor that hands back the wrapped list, and the wrapped pointer itself.
///
/// **These are the two members whose exposure defeats the whole class, and each is detected on
/// its own because `is_constructible_v` below only notices the whole private section
/// opening.** A review made *only* `Commands()` public and every assertion in this file stayed
/// green: the constructor was still private, so `is_constructible_v` was still false, and the
/// single escape hatch to the raw `RHI::ICommandList&` was open with nothing saying so. The
/// pointer has the same hole -- `commands.m_commands->Barrier(...)` needs no accessor at all.
///
/// Access checking happens during a requirement's substitution, so each of these is false
/// while its member is private and true the moment it is not. That is the property being
/// relied on, and `ReachableCommandList` below is what proves the requirement can say `true`.
/// @{
template <typename T>
constexpr bool kCanGetCommandList = requires(T& list) { list.Commands(); };

template <typename T>
constexpr bool kCanReachCommandListPointer = requires(T& list) { list.m_commands; };
/// @}

/// The positive probe those two need: the same two members, public.
///
/// `RHI::ICommandList` cannot serve as the probe the way it does for `Barrier` and
/// `EndRendering` -- it has neither member -- so without a type that does have them, a
/// misspelled requirement (`list.Command()`, say) would report false for `PassCommandList`
/// too and forbid nothing. Declared and never defined: a requirement needs the declaration
/// and nothing ever calls it.
struct ReachableCommandList {
    Monarc::RHI::ICommandList& Commands() const;
    Monarc::RHI::ICommandList* m_commands;
};

}  // namespace

static_assert(kCanGlobalBarrier<Monarc::RHI::ICommandList>);
static_assert(kCanTextureBarrier<Monarc::RHI::ICommandList>);
static_assert(kCanBufferBarrier<Monarc::RHI::ICommandList>);
static_assert(kCanEndRendering<Monarc::RHI::ICommandList>);

static_assert(!kCanGlobalBarrier<PassCommandList>);
static_assert(!kCanTextureBarrier<PassCommandList>);
static_assert(!kCanBufferBarrier<PassCommandList>);
static_assert(!kCanEndRendering<PassCommandList>);

// No way out of the wrapper either: it is not an `ICommandList`, does not convert to one,
// cannot be copied out of a callback that received one by reference, and cannot be built from
// an `ICommandList` by a caller that has one. Without all four, a pass could recover the list
// and barrier through it, and the assertions above would forbid nothing.
static_assert(!std::is_base_of_v<Monarc::RHI::ICommandList, PassCommandList>);
static_assert(!std::is_convertible_v<PassCommandList&, Monarc::RHI::ICommandList&>);
static_assert(!std::is_copy_constructible_v<PassCommandList>);
static_assert(!std::is_move_constructible_v<PassCommandList>);
static_assert(!std::is_constructible_v<PassCommandList, Monarc::RHI::ICommandList&>);

// And the two seams the four above do not cover, each with its positive probe beside it. See
// `kCanGetCommandList`: these are the assertions that fire when one member of the private
// section is opened rather than all of it.
static_assert(kCanGetCommandList<ReachableCommandList>);
static_assert(kCanReachCommandListPointer<ReachableCommandList>);
static_assert(!kCanGetCommandList<PassCommandList>,
              "PassCommandList::Commands() is reachable -- the single escape hatch to the raw "
              "RHI::ICommandList is open, and ADR-0006's central promise is gone");
static_assert(!kCanReachCommandListPointer<PassCommandList>,
              "PassCommandList::m_commands is reachable -- a pass can barrier through the "
              "wrapped pointer without needing an accessor at all");

// ---------------------------------------------------------------------------------------
// `TextureImport` requires every field, which is the property `RHI::TextureBarrier` argues for
// at length: an import whose states were defaulted is byte-identical to one whose author meant
// `Undefined` and `None`, so a run-time check has nothing to look at.
//
// **All three ways of getting it wrong were compiled on both compilers, and each is an
// error.** As with `RHI::TextureBarrier`, the third is the one a count-based check would miss.
//
// - `TextureImport import;` -- MSVC `error C2512: 'Monarc::Render::TextureImport': no
//   appropriate default constructor available`; clang-cl `error: no matching constructor for
//   initialization of 'TextureImport'` with `note: candidate constructor not viable: requires
//   4 arguments, but 0 were provided`.
// - Three braced arguments, which is what would have worked on an aggregate -- MSVC `error
//   C2440: 'initializing': cannot convert from 'initializer list' to
//   'Monarc::Render::TextureImport'` with `note: ... function does not take 3 arguments`;
//   clang-cl the same "no matching constructor" with `requires 4 arguments, but 3 were
//   provided`.
// - Four arguments with the description and the first state in each other's places -- MSVC
//   `error C2665: ... no overloaded function could convert all the argument types` with `note:
//   ... cannot convert argument 2 from 'Monarc::Render::TextureState' to
//   'Monarc::RHI::TextureDescription'`; clang-cl `note: candidate constructor not viable: no
//   known conversion from 'TextureState' to 'RHI::TextureDescription' for 2nd argument`.
//   Distinct types with no implicit conversion between them are what buys that one.
// ---------------------------------------------------------------------------------------

static_assert(!std::is_default_constructible_v<TextureImport>);
static_assert(std::is_constructible_v<TextureImport, TextureHandle, TextureDescription,
                                      TextureState, TextureState>);
static_assert(
    !std::is_constructible_v<TextureImport, TextureHandle, TextureDescription, TextureState>);
static_assert(!std::is_constructible_v<TextureImport, TextureHandle, TextureDescription>);
static_assert(!std::is_constructible_v<TextureImport, TextureHandle>);

// A graph cannot be relocated out from under a `PassBuilder` that names it by pointer.
static_assert(!std::is_copy_constructible_v<RenderGraph>);
static_assert(!std::is_move_constructible_v<RenderGraph>);

// A builder is a value: it is returned by `AddPass` through a `Result`, which needs it movable.
static_assert(std::is_trivially_copyable_v<PassBuilder>);
