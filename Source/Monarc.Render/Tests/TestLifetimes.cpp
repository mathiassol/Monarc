#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Render/RenderGraph.h>

// Resource lifetimes: the span of execution order over which each resource is live.
//
// Device-free, for the reason Private/Compile.cpp states about the whole of compilation.
//
// **Two things about the numbers, both of which a case here can get wrong quietly.** They are
// *execution* positions and not declaration indices -- so a pass declared third can be
// position one -- and they are computed over the passes that **survive culling**, which is
// what makes a lifetime shrink when its last reader does not run. `firstPass` is the first
// surviving *write* and `lastPass` the last surviving access of either direction; a read is
// not a first use, because a resource is not live before something puts contents in it.

using Monarc::Result;
using Monarc::SystemAllocator;
using Monarc::Render::GraphInspection;
using Monarc::Render::kNoPass;
using Monarc::Render::PassBuilder;
using Monarc::Render::RenderGraph;
using Monarc::Render::ResourceAccess;
using Monarc::Render::ResourceLifetime;
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

/// Adds a pass that imports its own anchor and writes it, so that culling keeps it. See
/// TestCull.cpp: a graph that imports nothing is culled entirely, and a culled pass contributes
/// nothing to any lifetime -- so a lifetime case without an anchor would assert about a frame
/// that does not run.
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

TEST_CASE("a transient written and read in one pass is live for that one position") {
    // **Two of them, in two different passes**, because with one the lifetime is `0..0` and a
    // computation that returned the constant zero for both ends would pass. The second is
    // `1..1`, which only a real position can produce.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             firstPass = AnchoredPass(graph, "First", 0);
    const Result<TextureId> early     = firstPass.CreateTexture("Early", kBaseDescription);
    REQUIRE(early.has_value());
    REQUIRE(firstPass.Write(*early, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(firstPass.Read(*early, ResourceAccess::ColorAttachmentRead));

    PassBuilder             secondPass = AnchoredPass(graph, "Second", 1);
    const Result<TextureId> late       = secondPass.CreateTexture("Late", kBaseDescription);
    REQUIRE(late.has_value());
    REQUIRE(secondPass.Write(*late, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(secondPass.Read(*late, ResourceAccess::ColorAttachmentRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 4u);
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{0, 0});
    CHECK(inspection.resources[3].lifetime == ResourceLifetime{1, 1});
    // The anchors are imported, and their lifetimes are computed the same way -- each is
    // written by the one pass that imported it.
    CHECK(inspection.resources[0].lifetime == ResourceLifetime{0, 0});
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{1, 1});
}

TEST_CASE("a transient read several passes later is live across the gap") {
    // Written at position 0 and read at position 3, with two passes in between that never touch
    // it. **Two transients with different spans in one graph**, so that a computation returning
    // the whole frame's span for everything would fail: the second is written at 1 and read at
    // 2, inside the first's span rather than equal to it.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             producer = AnchoredPass(graph, "Producer", 0);
    const Result<TextureId> wide     = producer.CreateTexture("Wide", kBaseDescription);
    REQUIRE(wide.has_value());
    const Result<TextureId> narrow = producer.CreateTexture("Narrow", kBaseDescription);
    REQUIRE(narrow.has_value());
    REQUIRE(producer.Write(*wide, ResourceAccess::ColorAttachmentWrite));

    PassBuilder middle = AnchoredPass(graph, "Middle", 1);
    REQUIRE(middle.Write(*narrow, ResourceAccess::ColorAttachmentWrite));

    PassBuilder bridge = AnchoredPass(graph, "Bridge", 2);
    REQUIRE(bridge.Read(*narrow, ResourceAccess::SampledRead));

    PassBuilder consumer = AnchoredPass(graph, "Consumer", 3);
    REQUIRE(consumer.Read(*wide, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 6u);
    CHECK(inspection.resources[1].name == "Wide");
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{0, 3});
    CHECK(inspection.resources[2].name == "Narrow");
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{1, 2});
}

TEST_CASE("a transient written twice is live from the first write, not the last") {
    // **The distinction a maximum would get wrong.** A `firstPass` that took the last write
    // would report a resource as dead while a pass was still writing it.
    //
    // **The two writes are a write-only pass and a read-modify-write pass, and that is the only
    // shape this graph has for a twice-written resource that something reads.** Two passes that
    // write one resource *without* reading it are unordered with respect to each other -- there
    // is no write-after-write edge -- so a read of that resource is refused with
    // `DiagnosticKind::UnorderedOverwrite` rather than ordered by a tie-break. A
    // `LoadOp::Load` modifier is ordered after the writer by the edge on the resource itself,
    // so its write is a genuine second write at a genuine later position.
    //
    // Two double-written transients, whose writes are in the opposite declaration order to one
    // another: `Ascending`'s first write is declared by the earlier pass, `Descending`'s by the
    // later one. A computation that took the first write *in declaration order* rather than in
    // execution order would get `Descending` wrong -- `Blend`'s write of it is declared first
    // and executes second.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner     = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> ascending = owner.CreateTexture("Ascending", kBaseDescription);
    REQUIRE(ascending.has_value());
    const Result<TextureId> descending = owner.CreateTexture("Descending", kBaseDescription);
    REQUIRE(descending.has_value());
    REQUIRE(owner.Write(*ascending, ResourceAccess::ColorAttachmentWrite));

    // Modifies both: after `Owner` for `Ascending`, and after the pass declared *below* it for
    // `Descending`, because in both cases it reads what the other pass writes.
    PassBuilder blend = AnchoredPass(graph, "Blend", 1);
    REQUIRE(blend.Read(*ascending, ResourceAccess::ColorAttachmentRead));
    REQUIRE(blend.Write(*ascending, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(blend.Read(*descending, ResourceAccess::ColorAttachmentRead));
    REQUIRE(blend.Write(*descending, ResourceAccess::ColorAttachmentWrite));

    PassBuilder late = AnchoredPass(graph, "Late", 2);
    REQUIRE(late.Write(*descending, ResourceAccess::ColorAttachmentWrite));

    PassBuilder reader = AnchoredPass(graph, "Reader", 3);
    REQUIRE(reader.Read(*ascending, ResourceAccess::SampledRead));
    REQUIRE(reader.Read(*descending, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // Execution order is Owner, Late, Blend, Reader -- pass 2 before pass 1, because pass 1
    // reads what pass 2 writes.
    REQUIRE(inspection.passes.size() == 4u);
    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == 2u);
    CHECK(inspection.passes[2].executionOrder == 1u);
    CHECK(inspection.passes[3].executionOrder == 3u);

    REQUIRE(inspection.resources.size() == 6u);
    CHECK(inspection.resources[1].name == "Ascending");
    // Written at 0 by Owner and at 2 by Blend, read at 3.
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{0, 3});
    CHECK(inspection.resources[2].name == "Descending");
    // Written at 1 by Late and at 2 by Blend -- so the *earlier execution position* is the pass
    // declared later -- and read at 3.
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{1, 3});
}

TEST_CASE("a transient whose last reader is culled has a shorter lifetime") {
    // **The plan's own case, and the reason culling has to run before lifetimes.** The same
    // producer and the same two readers appear in both graphs below; the only difference is
    // whether the later reader's own output is consumed. When it is not, that reader is culled
    // and the transient's lifetime shrinks -- computing lifetimes first would have recorded a
    // reader that never runs.
    SystemAllocator allocator;

    ResourceLifetime withLateReader{};
    ResourceLifetime withoutLateReader{};

    for (int consumed = 0; consumed < 2; ++consumed) {
        RenderGraph graph(allocator, RenderGraph::Config{});

        PassBuilder             producer = AnchoredPass(graph, "Producer", 0);
        const Result<TextureId> target   = producer.CreateTexture("Target", kBaseDescription);
        REQUIRE(target.has_value());
        REQUIRE(producer.Write(*target, ResourceAccess::ColorAttachmentWrite));

        PassBuilder early = AnchoredPass(graph, "EarlyReader", 1);
        REQUIRE(early.Read(*target, ResourceAccess::SampledRead));

        // The late reader writes a transient of its own. Whether anything reads it is the one
        // difference between the two graphs.
        Result<PassBuilder> late = graph.AddPass("LateReader");
        REQUIRE(late.has_value());
        REQUIRE(late->Read(*target, ResourceAccess::SampledRead));
        const Result<TextureId> spill = late->CreateTexture("Spill", kBaseDescription);
        REQUIRE(spill.has_value());
        REQUIRE(late->Write(*spill, ResourceAccess::ColorAttachmentWrite));

        if (consumed != 0) {
            PassBuilder sink = AnchoredPass(graph, "Sink", 2);
            REQUIRE(sink.Read(*spill, ResourceAccess::SampledRead));
        }

        REQUIRE(graph.Compile());

        const GraphInspection inspection = graph.Inspect();
        REQUIRE(inspection.resources.size() >= 4u);
        CHECK(inspection.resources[1].name == "Target");
        REQUIRE(inspection.passes.size() >= 3u);
        // The late reader is pass 2 in both graphs, and is culled in exactly one of them.
        CHECK(inspection.passes[2].culled == (consumed == 0));

        if (consumed != 0) {
            withLateReader = inspection.resources[1].lifetime;
        } else {
            withoutLateReader = inspection.resources[1].lifetime;
        }
    }

    // Written at 0, read at 1 and at 2.
    CHECK(withLateReader == ResourceLifetime{0, 2});
    // The same declarations, with the last reader culled: the last use is now the early reader.
    CHECK(withoutLateReader == ResourceLifetime{0, 1});
    CHECK(withoutLateReader.lastPass < withLateReader.lastPass);
}

TEST_CASE("a resource no surviving pass touches has an empty lifetime") {
    // **Two ways to be untouched, and they are different declarations with the same answer.**
    // One resource is created and never accessed at all -- legal, and
    // `PassBuilder::CreateTexture` says so. The other is written, by a pass that is culled. A
    // computation that only looked at whether *any* access existed would report a real lifetime
    // for the second.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner    = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> untouched = owner.CreateTexture("Untouched", kBaseDescription);
    REQUIRE(untouched.has_value());
    const Result<TextureId> orphaned = owner.CreateTexture("Orphaned", kBaseDescription);
    REQUIRE(orphaned.has_value());

    Result<PassBuilder> ghost = graph.AddPass("Ghost");
    REQUIRE(ghost.has_value());
    REQUIRE(ghost->Write(*orphaned, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 2u);
    CHECK(inspection.passes[1].culled);
    REQUIRE(inspection.resources.size() == 3u);
    CHECK(inspection.resources[1].name == "Untouched");
    CHECK(inspection.resources[1].lifetime.HasNoWrite());
    CHECK(inspection.resources[1].lifetime.lastPass == kNoPass);
    CHECK(inspection.resources[2].name == "Orphaned");
    CHECK(inspection.resources[2].lifetime.HasNoWrite());
    CHECK(inspection.resources[2].lifetime.lastPass == kNoPass);
    // And here the two queries agree, which is the other half of the pair: a resource with no
    // surviving use of either kind is unused as well as unwritten. The read-only import case
    // below is where they disagree.
    CHECK(inspection.resources[1].lifetime.IsUnused());
    CHECK(inspection.resources[2].lifetime.IsUnused());
}

TEST_CASE("an imported resource only read has a last pass and no first one") {
    // **The one place `firstPass` and `lastPass` come apart, and it is documented rather than
    // accidental.** `firstPass` is the first *write*, because a resource is not live before
    // something puts contents in it -- and an imported resource's contents came from outside
    // the graph, which is what its declared `incoming` state says. So a read-only import has a
    // real last use and no first write.
    //
    // **Which is why `ResourceLifetime` has two queries and not one**: `HasNoWrite()` is true
    // here and `IsUnused()` is false, for a resource two passes below genuinely read. Task 3
    // asks the second question -- does this resource have a lifetime to derive barriers over --
    // and a single query reading `firstPass` would answer it wrongly for exactly the resource
    // class A4's headline barriers are about.
    //
    // Two read-only imports, read at different positions, so the answer is not one constant.
    // Only a transient can be refused for this shape -- see
    // `DiagnosticKind::TransientNeverWritten`.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             first = AnchoredPass(graph, "First", 0);
    const Result<TextureId> early = first.ImportTexture("EarlySource", Anchor(1));
    REQUIRE(early.has_value());
    const Result<TextureId> late = first.ImportTexture("LateSource", Anchor(2));
    REQUIRE(late.has_value());
    REQUIRE(first.Read(*early, ResourceAccess::SampledRead));

    PassBuilder second = AnchoredPass(graph, "Second", 3);
    REQUIRE(second.Read(*late, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 4u);
    CHECK(inspection.resources[1].name == "EarlySource");
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{kNoPass, 0});
    CHECK(inspection.resources[2].name == "LateSource");
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{kNoPass, 1});
    CHECK(inspection.resources[1].lifetime.HasNoWrite());
    CHECK(inspection.resources[2].lifetime.HasNoWrite());
    CHECK_FALSE(inspection.resources[1].lifetime.IsUnused());
    CHECK_FALSE(inspection.resources[2].lifetime.IsUnused());
    // And the graph compiled: reading an import first is legal, unlike reading a transient
    // nothing wrote.
    CHECK(inspection.diagnostics.empty());
}

TEST_CASE("a lifetime is a span of execution positions, not of declaration indices") {
    // **The two index spaces, pulled apart.** The producer is declared last and runs first, so
    // the transient's lifetime is `0..1` where a computation reading `PassInspection::index`
    // would say `1..2`. Nothing else in this file separates the two, because in every other
    // case the sort leaves declaration order alone.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner  = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> target = owner.CreateTexture("Target", kBaseDescription);
    REQUIRE(target.has_value());

    PassBuilder consumer = AnchoredPass(graph, "Consumer", 1);
    REQUIRE(consumer.Read(*target, ResourceAccess::SampledRead));

    PassBuilder producer = AnchoredPass(graph, "Producer", 2);
    REQUIRE(producer.Write(*target, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 3u);
    CHECK(inspection.passes[1].executionOrder == 2u);
    CHECK(inspection.passes[2].executionOrder == 1u);

    REQUIRE(inspection.resources.size() == 4u);
    CHECK(inspection.resources[1].name == "Target");
    // Written by pass 2 at position 1, read by pass 1 at position 2.
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{1, 2});
}
