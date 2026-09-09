#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Render/RenderGraph.h>

// Culling: which passes run, and which the graph decides nothing consumes.
//
// Device-free, for the reason Private/Compile.cpp states about the whole of compilation.
//
// **The rule under test, in one sentence: a pass survives because a pass that itself survives
// reads something it wrote, or because it wrote an imported resource.** Three consequences run
// through every case below and are worth stating once here rather than being rediscovered per
// case:
//
//   - **A pass writing nothing at all never survives.** It has no output for anything to
//     consume, and a recording callback is not an output -- see `CullPasses`.
//   - **A pass that only reads survives no better.** Reading is not producing.
//   - **A graph that imports nothing is culled entirely**, which is why nearly every case in
//     this suite anchors on an imported resource. That is not scaffolding around the rule, it
//     *is* the rule: if nothing outside the graph consumes anything the frame produced, the
//     frame's whole output is unobserved.

using Monarc::Result;
using Monarc::SystemAllocator;
using Monarc::Render::GraphInspection;
using Monarc::Render::GraphPhase;
using Monarc::Render::kNoPass;
using Monarc::Render::PassBuilder;
using Monarc::Render::PassInspection;
using Monarc::Render::RenderGraph;
using Monarc::Render::ResourceAccess;
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

constexpr TextureDescription kBaseDescription{
    Monarc::RHI::Extent2D{1280, 720}, Format::B8G8R8A8_UNORM, TextureUsage::ColorAttachment};

[[nodiscard]] TextureImport Anchor(Monarc::u32 index) {
    return TextureImport(TextureHandle::ForTesting(index, 1), kBaseDescription,
                         TextureState{TextureLayout::Undefined,
                                      PipelineStage::ColorAttachmentOutput, Access::None},
                         TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                      Access::None});
}

/// Adds a pass that imports its own anchor and writes it, so that something outside the graph
/// consumes its output and culling keeps it.
[[nodiscard]] PassBuilder AnchoredPass(RenderGraph& graph, const char* name,
                                       Monarc::u32 anchorIndex) {
    Result<PassBuilder> pass = graph.AddPass(name);
    REQUIRE(pass.has_value());
    const Result<TextureId> image = pass->ImportTexture("Anchor", Anchor(anchorIndex));
    REQUIRE(image.has_value());
    REQUIRE(pass->Write(*image, ResourceAccess::ColorAttachmentWrite));
    return *pass;
}

}  // namespace

TEST_CASE("a diamond survives whole, and the branch beside it does not") {
    // **The plan's diamond, with a fifth pass hanging off it that nothing consumes.** Both
    // branches of the diamond survive, which is what a diamond is for -- a shape with two
    // parallel producers is the one a rule that kept only a single chain alive would fail. And
    // the fifth pass reads the left branch's output too, so it is not culled for want of an
    // input: it is culled because nothing reads *its* output.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> root = graph.AddPass("Root");
    REQUIRE(root.has_value());
    const Result<TextureId> shared = root->CreateTexture("Shared", kBaseDescription);
    REQUIRE(shared.has_value());
    const Result<TextureId> left = root->CreateTexture("Left", kBaseDescription);
    REQUIRE(left.has_value());
    const Result<TextureId> right = root->CreateTexture("Right", kBaseDescription);
    REQUIRE(right.has_value());
    const Result<TextureId> waste = root->CreateTexture("Waste", kBaseDescription);
    REQUIRE(waste.has_value());
    REQUIRE(root->Write(*shared, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> leftBranch = graph.AddPass("LeftBranch");
    REQUIRE(leftBranch.has_value());
    REQUIRE(leftBranch->Read(*shared, ResourceAccess::SampledRead));
    REQUIRE(leftBranch->Write(*left, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> rightBranch = graph.AddPass("RightBranch");
    REQUIRE(rightBranch.has_value());
    REQUIRE(rightBranch->Read(*shared, ResourceAccess::SampledRead));
    REQUIRE(rightBranch->Write(*right, ResourceAccess::ColorAttachmentWrite));

    PassBuilder join = AnchoredPass(graph, "Join", 0);
    REQUIRE(join.Read(*left, ResourceAccess::SampledRead));
    REQUIRE(join.Read(*right, ResourceAccess::SampledRead));

    Result<PassBuilder> deadEnd = graph.AddPass("DeadEnd");
    REQUIRE(deadEnd.has_value());
    REQUIRE(deadEnd->Read(*left, ResourceAccess::SampledRead));
    REQUIRE(deadEnd->Write(*waste, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 5u);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK_FALSE(inspection.passes[1].culled);
    CHECK_FALSE(inspection.passes[2].culled);
    CHECK_FALSE(inspection.passes[3].culled);
    CHECK(inspection.passes[4].culled);

    // The survivors are numbered densely, so the culled pass leaves no gap. Root first, then
    // the two branches in declaration order -- the tie-break -- then the join.
    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == 1u);
    CHECK(inspection.passes[2].executionOrder == 2u);
    CHECK(inspection.passes[3].executionOrder == 3u);
    CHECK(inspection.passes[4].executionOrder == kNoPass);
}

TEST_CASE("a chain survives to its end, and culling is transitive back up a chain") {
    // **Two chains of three in one graph, one anchored and one not, which is the shape that
    // separates "survives" from "culled" without two graphs to compare.** The anchored chain's
    // last pass writes an imported resource, so all three of its passes survive. The other
    // chain's last pass writes a transient nothing reads, so all three of *its* passes are
    // culled -- transitively, two links back up from the pass that was culled first.
    //
    // A single-level rule -- "culled if no pass reads my output" applied once -- would keep the
    // first two passes of the dead chain, because their outputs *are* read, by passes that do
    // not run.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> liveFirst = graph.AddPass("LiveFirst");
    REQUIRE(liveFirst.has_value());
    const Result<TextureId> liveA = liveFirst->CreateTexture("LiveA", kBaseDescription);
    REQUIRE(liveA.has_value());
    const Result<TextureId> liveB = liveFirst->CreateTexture("LiveB", kBaseDescription);
    REQUIRE(liveB.has_value());
    const Result<TextureId> deadA = liveFirst->CreateTexture("DeadA", kBaseDescription);
    REQUIRE(deadA.has_value());
    const Result<TextureId> deadB = liveFirst->CreateTexture("DeadB", kBaseDescription);
    REQUIRE(deadB.has_value());
    const Result<TextureId> deadC = liveFirst->CreateTexture("DeadC", kBaseDescription);
    REQUIRE(deadC.has_value());
    REQUIRE(liveFirst->Write(*liveA, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> liveSecond = graph.AddPass("LiveSecond");
    REQUIRE(liveSecond.has_value());
    REQUIRE(liveSecond->Read(*liveA, ResourceAccess::SampledRead));
    REQUIRE(liveSecond->Write(*liveB, ResourceAccess::ColorAttachmentWrite));

    PassBuilder liveThird = AnchoredPass(graph, "LiveThird", 0);
    REQUIRE(liveThird.Read(*liveB, ResourceAccess::SampledRead));

    Result<PassBuilder> deadFirst = graph.AddPass("DeadFirst");
    REQUIRE(deadFirst.has_value());
    REQUIRE(deadFirst->Write(*deadA, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> deadSecond = graph.AddPass("DeadSecond");
    REQUIRE(deadSecond.has_value());
    REQUIRE(deadSecond->Read(*deadA, ResourceAccess::SampledRead));
    REQUIRE(deadSecond->Write(*deadB, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> deadThird = graph.AddPass("DeadThird");
    REQUIRE(deadThird.has_value());
    REQUIRE(deadThird->Read(*deadB, ResourceAccess::SampledRead));
    REQUIRE(deadThird->Write(*deadC, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 6u);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK_FALSE(inspection.passes[1].culled);
    CHECK_FALSE(inspection.passes[2].culled);
    CHECK(inspection.passes[3].culled);
    CHECK(inspection.passes[4].culled);
    CHECK(inspection.passes[5].culled);

    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == 1u);
    CHECK(inspection.passes[2].executionOrder == 2u);
    for (Monarc::u32 i = 3; i < 6u; ++i) {
        CHECK(inspection.passes[i].executionOrder == kNoPass);
    }
}

TEST_CASE("a culled pass in the middle of the order leaves no gap behind it") {
    // **`PassInspection::executionOrder` is a position among the passes that *run*, so the
    // survivors are numbered densely.** Every other case in this file has its culled passes at
    // the end of the sorted order, where a numbering that used the sorted position instead
    // would produce the same answers; here the culled pass sits between two survivors, so the
    // last pass is at position 1 rather than 2.
    //
    // It matters beyond tidiness: a lifetime is a pair of these numbers, and a gap in them
    // would make a resource's span include a position no pass occupies.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             head  = AnchoredPass(graph, "Head", 0);
    const Result<TextureId> chain = head.CreateTexture("Chain", kBaseDescription);
    REQUIRE(chain.has_value());
    REQUIRE(head.Write(*chain, ResourceAccess::ColorAttachmentWrite));

    // Reads the chain, so it sorts between the two anchored passes -- and writes only a
    // transient nothing reads, so it does not run.
    Result<PassBuilder> dropped = graph.AddPass("Dropped");
    REQUIRE(dropped.has_value());
    REQUIRE(dropped->Read(*chain, ResourceAccess::SampledRead));
    const Result<TextureId> waste = dropped->CreateTexture("Waste", kBaseDescription);
    REQUIRE(waste.has_value());
    REQUIRE(dropped->Write(*waste, ResourceAccess::ColorAttachmentWrite));

    PassBuilder tail = AnchoredPass(graph, "Tail", 1);
    REQUIRE(tail.Read(*chain, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 3u);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK(inspection.passes[1].culled);
    CHECK_FALSE(inspection.passes[2].culled);

    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == kNoPass);
    // 1 and not 2: the culled pass took no position.
    CHECK(inspection.passes[2].executionOrder == 1u);

    // And the lifetime is in that same dense space: written at 0, last read at 1.
    CHECK(inspection.resources[1].name == "Chain");
    CHECK(inspection.resources[1].lifetime.firstPass == 0u);
    CHECK(inspection.resources[1].lifetime.lastPass == 1u);
}

TEST_CASE("a pass whose only consumer is culled is culled too") {
    // The plan's own wording, at the smallest size it can be asserted at: the producer's output
    // *is* read, and by exactly one pass, and that pass does not run.
    //
    // Two producers rather than one, reading the same doomed consumer, because a rule that
    // followed only the first edge it found would keep the second.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             anchor = AnchoredPass(graph, "Anchor", 0);
    const Result<TextureId> first  = anchor.CreateTexture("First", kBaseDescription);
    REQUIRE(first.has_value());
    const Result<TextureId> second = anchor.CreateTexture("Second", kBaseDescription);
    REQUIRE(second.has_value());
    const Result<TextureId> waste = anchor.CreateTexture("Waste", kBaseDescription);
    REQUIRE(waste.has_value());

    Result<PassBuilder> producerA = graph.AddPass("ProducerA");
    REQUIRE(producerA.has_value());
    REQUIRE(producerA->Write(*first, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> producerB = graph.AddPass("ProducerB");
    REQUIRE(producerB.has_value());
    REQUIRE(producerB->Write(*second, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> consumer = graph.AddPass("Consumer");
    REQUIRE(consumer.has_value());
    REQUIRE(consumer->Read(*first, ResourceAccess::SampledRead));
    REQUIRE(consumer->Read(*second, ResourceAccess::SampledRead));
    REQUIRE(consumer->Write(*waste, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 4u);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK(inspection.passes[1].culled);
    CHECK(inspection.passes[2].culled);
    CHECK(inspection.passes[3].culled);
    CHECK(inspection.passes[0].executionOrder == 0u);
}

TEST_CASE("a pass writing an imported resource is never culled, even unread") {
    // **Nothing in the graph reads either imported resource, and both writers survive**, because
    // something outside the graph consumes them -- which is what `ResourceOrigin::Imported`
    // means and the whole reason the origin is declared rather than inferred.
    //
    // Two importers and one pass writing only a transient, in one graph, so that the case
    // distinguishes the origin from "writes anything at all": all three passes write something,
    // and exactly the two writing imports survive.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    // Neither builder is kept: these two passes declare nothing beyond the import and the write
    // to it, which is the whole point of the case.
    static_cast<void>(AnchoredPass(graph, "FirstImporter", 0));
    static_cast<void>(AnchoredPass(graph, "SecondImporter", 1));

    Result<PassBuilder> transientOnly = graph.AddPass("TransientOnly");
    REQUIRE(transientOnly.has_value());
    const Result<TextureId> offscreen =
        transientOnly->CreateTexture("Offscreen", kBaseDescription);
    REQUIRE(offscreen.has_value());
    REQUIRE(transientOnly->Write(*offscreen, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 3u);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK_FALSE(inspection.passes[1].culled);
    CHECK(inspection.passes[2].culled);
    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == 1u);
    CHECK(inspection.passes[2].executionOrder == kNoPass);
}

TEST_CASE("reading an imported resource does not keep a pass alive") {
    // **Reading is not producing**, and an imported resource on the *read* side buys nothing.
    // Two readers of the same import: one writes another import and survives, one writes
    // nothing and does not. A rule that had treated any access to an imported resource as an
    // anchor would keep both.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner  = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> source = owner.ImportTexture("Source", Anchor(1));
    REQUIRE(source.has_value());

    PassBuilder writesOutput = AnchoredPass(graph, "WritesOutput", 2);
    REQUIRE(writesOutput.Read(*source, ResourceAccess::SampledRead));

    Result<PassBuilder> readsOnly = graph.AddPass("ReadsOnly");
    REQUIRE(readsOnly.has_value());
    REQUIRE(readsOnly->Read(*source, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 3u);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK_FALSE(inspection.passes[1].culled);
    CHECK(inspection.passes[2].culled);
}

TEST_CASE("a graph that imports nothing is culled entirely") {
    // **The plan's "a graph where everything is culled", and it is the ordinary consequence of
    // the rule rather than a degenerate input.** Three passes in a chain, all writing and
    // reading transients, with nothing outside the graph consuming the last one: no pass has an
    // observable output, so no pass runs.
    //
    // It compiles. Culling everything is a decision, not a refusal -- a frame with nothing to
    // do is a frame with nothing to do -- and the report says so field by field.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    const Result<TextureId> a = first->CreateTexture("A", kBaseDescription);
    REQUIRE(a.has_value());
    const Result<TextureId> b = first->CreateTexture("B", kBaseDescription);
    REQUIRE(b.has_value());
    REQUIRE(first->Write(*a, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE(second.has_value());
    REQUIRE(second->Read(*a, ResourceAccess::SampledRead));
    REQUIRE(second->Write(*b, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> third = graph.AddPass("Third");
    REQUIRE(third.has_value());
    REQUIRE(third->Read(*b, ResourceAccess::SampledRead));

    CHECK(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::Compiled);
    CHECK(inspection.diagnostics.empty());
    REQUIRE(inspection.passes.size() == 3u);
    for (const PassInspection& pass : inspection.passes) {
        CHECK(pass.culled);
        CHECK(pass.executionOrder == kNoPass);
    }
    // No pass runs, so nothing is live at any position: both resources' lifetimes are empty and
    // neither is grouped with anything.
    REQUIRE(inspection.resources.size() == 2u);
    for (const Monarc::Render::ResourceInspection& resource : inspection.resources) {
        CHECK(resource.lifetime.HasNoWrite());
        CHECK(resource.lifetime.lastPass == kNoPass);
        CHECK(resource.aliasGroup == Monarc::Render::kNoAliasGroup);
    }
}

TEST_CASE("a recording callback does not keep a pass alive") {
    // **Stated in `CullPasses` and asserted here, because it is the decision most likely to be
    // reversed by somebody debugging a pass that did not run.** ADR-0006's contract is that a
    // pass declares its reads and writes and the graph decides the rest; a callback is neither.
    // If `PassInspection::hasRecord` kept a pass alive, culling would be nearly inert -- every
    // pass that records anything would survive -- and a feature could opt out of the mechanism
    // by capturing a lambda.
    //
    // Two passes with callbacks, one anchored and one not, so the case says the callback is
    // irrelevant rather than that callbacks are fatal.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder anchored = AnchoredPass(graph, "Anchored", 0);
    REQUIRE(anchored.Record([](Monarc::Render::PassCommandList&) {}));

    Result<PassBuilder> unanchored = graph.AddPass("Unanchored");
    REQUIRE(unanchored.has_value());
    REQUIRE(unanchored->Record([](Monarc::Render::PassCommandList&) {}));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 2u);
    CHECK(inspection.passes[0].hasRecord);
    CHECK(inspection.passes[1].hasRecord);
    CHECK_FALSE(inspection.passes[0].culled);
    CHECK(inspection.passes[1].culled);
}
