#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Render/RenderGraph.h>

// Alias grouping: which transients the graph decides could share memory.
//
// Device-free, for the reason Private/Compile.cpp states about the whole of compilation.
//
// ---------------------------------------------------------------------------------------
// **Every case in this file asserts a decision. Not one of them asserts that a byte of memory
// was saved, because none was.**
//
// The graph groups transients whose lifetimes do not overlap and whose descriptions agree, and
// reports the grouping. Nothing honours it: `RenderGraph::Execute` refuses rather than
// recording, and two resources sharing one allocation needs sub-allocation from an allocator
// Monarc does not have -- its memory is one allocation per resource, which Phase A3 recorded as
// a placeholder. So a green suite here means the grouping is right; it says nothing whatsoever
// about memory. `GroupAliases` in Private/Compile.cpp repeats this at the code that makes the
// decision, which is the other place somebody would look.
// ---------------------------------------------------------------------------------------
//
// **The grouping shape, since several cases turn on it.** A group has at least two members: a
// resource that shares memory with nothing carries `kNoAliasGroup`, and so a lone candidate is
// not a group of one. Imported resources are never candidates -- the graph does not own their
// memory -- and neither is a resource with an empty lifetime, which after culling means nothing
// that runs writes it.

using Monarc::Result;
using Monarc::SystemAllocator;
using Monarc::Render::GraphInspection;
using Monarc::Render::kNoAliasGroup;
using Monarc::Render::PassBuilder;
using Monarc::Render::RenderGraph;
using Monarc::Render::ResourceAccess;
using Monarc::Render::ResourceLifetime;
using Monarc::Render::TextureId;
using Monarc::Render::TextureImport;
using Monarc::RHI::Access;
using Monarc::RHI::Extent2D;
using Monarc::RHI::Format;
using Monarc::RHI::PipelineStage;
using Monarc::RHI::TextureDescription;
using Monarc::RHI::TextureHandle;
using Monarc::RHI::TextureLayout;
using Monarc::RHI::TextureState;
using Monarc::RHI::TextureUsage;

namespace {

constexpr TextureDescription kBaseDescription{
    Extent2D{1280, 720}, Format::B8G8R8A8_UNORM, TextureUsage::ColorAttachment};

/// The same extent and usage as `kBaseDescription`, a different format. One of the two halves
/// of the compatibility rule.
constexpr TextureDescription kOtherFormat{Extent2D{1280, 720}, Format::R8G8B8A8_UNORM,
                                          TextureUsage::ColorAttachment};

/// The same format and usage as `kBaseDescription`, a different extent. The other half.
constexpr TextureDescription kOtherExtent{Extent2D{640, 360}, Format::B8G8R8A8_UNORM,
                                          TextureUsage::ColorAttachment};

[[nodiscard]] TextureImport Anchor(Monarc::u32 index) {
    return TextureImport(TextureHandle::ForTesting(index, 1), kBaseDescription,
                         TextureState{TextureLayout::Undefined,
                                      PipelineStage::ColorAttachmentOutput, Access::None},
                         TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                      Access::None});
}

/// Adds a pass that imports its own anchor and writes it, so that culling keeps it. Without one
/// the whole graph is culled, every lifetime is empty, and nothing is a candidate for grouping
/// -- see TestCull.cpp.
[[nodiscard]] PassBuilder AnchoredPass(RenderGraph& graph, const char* name,
                                       Monarc::u32 anchorIndex) {
    Result<PassBuilder> pass = graph.AddPass(name);
    REQUIRE(pass.has_value());
    const Result<TextureId> image = pass->ImportTexture("Anchor", Anchor(anchorIndex));
    REQUIRE(image.has_value());
    REQUIRE(pass->Write(*image, ResourceAccess::ColorAttachmentWrite));
    return *pass;
}

/// Adds two passes that give `target` the lifetime `[position, position + 1]`: the first writes
/// it and the second reads it, and each anchors itself so that neither is culled.
///
/// **This is how every case below dials a lifetime**, because a lifetime is not something a
/// declaration states -- it is derived from which passes touch the resource and where they end
/// up in execution order. Two passes per span, so a graph wanting three disjoint spans is six
/// passes long.
void GiveDisjointSpan(RenderGraph& graph, TextureId target, Monarc::u32 anchorIndex) {
    PassBuilder writer = AnchoredPass(graph, "Writer", anchorIndex);
    REQUIRE(writer.Write(target, ResourceAccess::ColorAttachmentWrite));
    PassBuilder reader = AnchoredPass(graph, "Reader", anchorIndex + 1);
    REQUIRE(reader.Read(target, ResourceAccess::SampledRead));
}

}  // namespace

TEST_CASE("two transients whose lifetimes do not overlap share a group") {
    // The plan's first aliasing case. `Early` is live over positions 0 and 1, `Late` over 2 and
    // 3, and the two descriptions agree -- so the graph decides they could share memory.
    //
    // The four anchors are imported and are therefore *not* grouped, which is the second half
    // of this case: two imported resources with identical descriptions and lifetimes as disjoint
    // as the transients' are still each on their own.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> early = owner.CreateTexture("Early", kBaseDescription);
    REQUIRE(early.has_value());
    const Result<TextureId> late = owner.CreateTexture("Late", kBaseDescription);
    REQUIRE(late.has_value());

    GiveDisjointSpan(graph, *early, 1);
    GiveDisjointSpan(graph, *late, 3);

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 7u);
    CHECK(inspection.resources[1].name == "Early");
    CHECK(inspection.resources[2].name == "Late");
    // Positions 1..2 and 3..4: the owner pass runs first and touches neither.
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{1, 2});
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{3, 4});

    CHECK(inspection.resources[1].aliasGroup != kNoAliasGroup);
    CHECK(inspection.resources[1].aliasGroup == inspection.resources[2].aliasGroup);

    // Every imported resource, on its own. Two of them have lifetimes as disjoint as the pair
    // above -- positions 1 and 2, say -- and identical descriptions, so a rule that had not
    // excluded imports would have grouped them.
    CHECK(inspection.resources[0].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[3].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[4].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[5].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[6].aliasGroup == kNoAliasGroup);
}

TEST_CASE("two transients whose lifetimes overlap share nothing") {
    // Written and read by the same pair of passes, so both are live over the same two positions.
    // Compatible descriptions and every other condition met: the overlap is the only thing
    // keeping them apart, which is what makes this the counterpart of the case above.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             producer = AnchoredPass(graph, "Producer", 0);
    const Result<TextureId> first    = producer.CreateTexture("First", kBaseDescription);
    REQUIRE(first.has_value());
    const Result<TextureId> second = producer.CreateTexture("Second", kBaseDescription);
    REQUIRE(second.has_value());
    REQUIRE(producer.Write(*first, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(producer.Write(*second, ResourceAccess::ColorAttachmentWrite));

    PassBuilder consumer = AnchoredPass(graph, "Consumer", 1);
    REQUIRE(consumer.Read(*first, ResourceAccess::SampledRead));
    REQUIRE(consumer.Read(*second, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 4u);
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{0, 1});
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{0, 1});
    CHECK(inspection.resources[1].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[2].aliasGroup == kNoAliasGroup);
}

TEST_CASE("touching at one position is an overlap") {
    // **The boundary, and it is the one an off-by-one in the overlap test would get wrong.**
    // `First` is live over 0..1 and `Second` over 1..2, so they are both live at position 1 --
    // a pass is one point in execution order, and two resources both live at it cannot be the
    // same memory. A test written as `firstEnd < secondStart` rather than `<=` would group them.
    //
    // The third transient is the control: live over 3..4, disjoint from both, and it does group
    // -- with the first of them, which is the lowest-numbered bin it fits.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> owner = graph.AddPass("Owner");
    REQUIRE(owner.has_value());
    const Result<TextureId> first = owner->CreateTexture("First", kBaseDescription);
    REQUIRE(first.has_value());
    const Result<TextureId> second = owner->CreateTexture("Second", kBaseDescription);
    REQUIRE(second.has_value());
    const Result<TextureId> third = owner->CreateTexture("Third", kBaseDescription);
    REQUIRE(third.has_value());
    REQUIRE(owner->Write(*first, ResourceAccess::ColorAttachmentWrite));

    PassBuilder middle = AnchoredPass(graph, "Middle", 0);
    REQUIRE(middle.Read(*first, ResourceAccess::SampledRead));
    REQUIRE(middle.Write(*second, ResourceAccess::ColorAttachmentWrite));

    PassBuilder after = AnchoredPass(graph, "After", 1);
    REQUIRE(after.Read(*second, ResourceAccess::SampledRead));

    PassBuilder thirdWriter = AnchoredPass(graph, "ThirdWriter", 2);
    REQUIRE(thirdWriter.Write(*third, ResourceAccess::ColorAttachmentWrite));

    PassBuilder thirdReader = AnchoredPass(graph, "ThirdReader", 3);
    REQUIRE(thirdReader.Read(*third, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 7u);
    CHECK(inspection.resources[0].lifetime == ResourceLifetime{0, 1});
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{1, 2});
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{3, 4});

    // The two that touch at position 1 are apart; the third joins the first.
    CHECK(inspection.resources[1].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[0].aliasGroup != kNoAliasGroup);
    CHECK(inspection.resources[0].aliasGroup == inspection.resources[2].aliasGroup);
}

TEST_CASE("non-overlapping lifetimes with incompatible descriptions share nothing") {
    // **The plan's third aliasing case, with both halves of the compatibility rule and a
    // control.** Four transients, every one of them live over a span disjoint from all the
    // others -- so lifetimes rule none of them out. `Twin` has the same description as `Base`
    // and groups with it; `WrongFormat` and `WrongExtent` differ in exactly one field each and
    // group with nothing.
    //
    // The control is what makes this case able to fail in the interesting direction: without a
    // pair that *does* group, a rule that grouped nothing at all would pass.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> base  = owner.CreateTexture("Base", kBaseDescription);
    REQUIRE(base.has_value());
    const Result<TextureId> twin = owner.CreateTexture("Twin", kBaseDescription);
    REQUIRE(twin.has_value());
    const Result<TextureId> wrongFormat = owner.CreateTexture("WrongFormat", kOtherFormat);
    REQUIRE(wrongFormat.has_value());
    const Result<TextureId> wrongExtent = owner.CreateTexture("WrongExtent", kOtherExtent);
    REQUIRE(wrongExtent.has_value());

    GiveDisjointSpan(graph, *base, 1);
    GiveDisjointSpan(graph, *twin, 3);
    GiveDisjointSpan(graph, *wrongFormat, 5);
    GiveDisjointSpan(graph, *wrongExtent, 7);

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 13u);
    CHECK(inspection.resources[1].name == "Base");
    CHECK(inspection.resources[2].name == "Twin");
    CHECK(inspection.resources[3].name == "WrongFormat");
    CHECK(inspection.resources[4].name == "WrongExtent");

    // All four spans disjoint, so nothing here is decided by a lifetime.
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{1, 2});
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{3, 4});
    CHECK(inspection.resources[3].lifetime == ResourceLifetime{5, 6});
    CHECK(inspection.resources[4].lifetime == ResourceLifetime{7, 8});

    CHECK(inspection.resources[1].aliasGroup != kNoAliasGroup);
    CHECK(inspection.resources[1].aliasGroup == inspection.resources[2].aliasGroup);
    CHECK(inspection.resources[3].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[4].aliasGroup == kNoAliasGroup);
}

TEST_CASE("three non-overlapping transients share one group") {
    // **A group is not a pair.** First-fit puts all three in the same bin because each fits
    // beside the two already there, and a fourth transient that overlaps all of them is on its
    // own -- which is also what says the group is a real placement rather than "every candidate
    // gets group zero".
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> a     = owner.CreateTexture("A", kBaseDescription);
    REQUIRE(a.has_value());
    const Result<TextureId> b = owner.CreateTexture("B", kBaseDescription);
    REQUIRE(b.has_value());
    const Result<TextureId> c = owner.CreateTexture("C", kBaseDescription);
    REQUIRE(c.has_value());
    const Result<TextureId> spanning = owner.CreateTexture("Spanning", kBaseDescription);
    REQUIRE(spanning.has_value());
    REQUIRE(owner.Write(*spanning, ResourceAccess::ColorAttachmentWrite));

    GiveDisjointSpan(graph, *a, 1);
    GiveDisjointSpan(graph, *b, 3);
    GiveDisjointSpan(graph, *c, 5);

    PassBuilder sink = AnchoredPass(graph, "Sink", 7);
    REQUIRE(sink.Read(*spanning, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{1, 2});
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{3, 4});
    CHECK(inspection.resources[3].lifetime == ResourceLifetime{5, 6});
    // Written by the first pass and read by the last, so it is live across all of them.
    CHECK(inspection.resources[4].lifetime == ResourceLifetime{0, 7});

    CHECK(inspection.resources[1].aliasGroup != kNoAliasGroup);
    CHECK(inspection.resources[1].aliasGroup == inspection.resources[2].aliasGroup);
    CHECK(inspection.resources[1].aliasGroup == inspection.resources[3].aliasGroup);
    CHECK(inspection.resources[4].aliasGroup == kNoAliasGroup);
}

TEST_CASE("two groups in one graph get different ids") {
    // **Four transients over two spans, two per span, so first-fit has to open a second bin.**
    // `EarlyA` and `LateA` are disjoint and take the first bin. `EarlyB` overlaps `EarlyA` and
    // so cannot join it; `LateB` overlaps `LateA` and cannot either -- but `EarlyB` and `LateB`
    // are disjoint from each other, so they pair up in a bin of their own.
    //
    // A grouping that handed every group the same id would pass every earlier case in this file
    // and fail here, which is the only thing that pins the numbering.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> owner = graph.AddPass("Owner");
    REQUIRE(owner.has_value());
    const Result<TextureId> earlyA = owner->CreateTexture("EarlyA", kBaseDescription);
    REQUIRE(earlyA.has_value());
    const Result<TextureId> lateA = owner->CreateTexture("LateA", kBaseDescription);
    REQUIRE(lateA.has_value());
    const Result<TextureId> earlyB = owner->CreateTexture("EarlyB", kBaseDescription);
    REQUIRE(earlyB.has_value());
    const Result<TextureId> lateB = owner->CreateTexture("LateB", kBaseDescription);
    REQUIRE(lateB.has_value());

    PassBuilder earlyWriter = AnchoredPass(graph, "EarlyWriter", 0);
    REQUIRE(earlyWriter.Write(*earlyA, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(earlyWriter.Write(*earlyB, ResourceAccess::ColorAttachmentWrite));

    PassBuilder earlyReader = AnchoredPass(graph, "EarlyReader", 1);
    REQUIRE(earlyReader.Read(*earlyA, ResourceAccess::SampledRead));
    REQUIRE(earlyReader.Read(*earlyB, ResourceAccess::SampledRead));

    PassBuilder lateWriter = AnchoredPass(graph, "LateWriter", 2);
    REQUIRE(lateWriter.Write(*lateA, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(lateWriter.Write(*lateB, ResourceAccess::ColorAttachmentWrite));

    PassBuilder lateReader = AnchoredPass(graph, "LateReader", 3);
    REQUIRE(lateReader.Read(*lateA, ResourceAccess::SampledRead));
    REQUIRE(lateReader.Read(*lateB, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.resources.size() == 8u);
    // The owner pass writes nothing and is culled, so the four writers and readers are
    // positions 0 to 3.
    CHECK(inspection.passes[0].culled);
    CHECK(inspection.resources[0].name == "EarlyA");
    CHECK(inspection.resources[0].lifetime == ResourceLifetime{0, 1});
    CHECK(inspection.resources[1].name == "LateA");
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{2, 3});
    CHECK(inspection.resources[2].name == "EarlyB");
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{0, 1});
    CHECK(inspection.resources[3].name == "LateB");
    CHECK(inspection.resources[3].lifetime == ResourceLifetime{2, 3});

    CHECK(inspection.resources[0].aliasGroup == inspection.resources[1].aliasGroup);
    CHECK(inspection.resources[2].aliasGroup == inspection.resources[3].aliasGroup);
    CHECK(inspection.resources[0].aliasGroup != inspection.resources[2].aliasGroup);
    CHECK(inspection.resources[0].aliasGroup != kNoAliasGroup);
    CHECK(inspection.resources[2].aliasGroup != kNoAliasGroup);
}

TEST_CASE("a transient nothing accesses groups with nothing") {
    // **The plan's last aliasing case.** A resource no surviving pass touches has no live range
    // to place, so there is nothing to place it beside -- grouping it would claim a decision
    // about memory it never needs. Two of them: one created and never accessed at all, and one
    // written only by a pass that was culled.
    //
    // The pair beside them does group, which is what stops this case from being satisfied by a
    // grouping that never groups anything.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner     = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> untouched = owner.CreateTexture("Untouched", kBaseDescription);
    REQUIRE(untouched.has_value());
    const Result<TextureId> orphaned = owner.CreateTexture("Orphaned", kBaseDescription);
    REQUIRE(orphaned.has_value());
    const Result<TextureId> early = owner.CreateTexture("Early", kBaseDescription);
    REQUIRE(early.has_value());
    const Result<TextureId> late = owner.CreateTexture("Late", kBaseDescription);
    REQUIRE(late.has_value());

    Result<PassBuilder> ghost = graph.AddPass("Ghost");
    REQUIRE(ghost.has_value());
    REQUIRE(ghost->Write(*orphaned, ResourceAccess::ColorAttachmentWrite));

    GiveDisjointSpan(graph, *early, 1);
    GiveDisjointSpan(graph, *late, 3);

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources[1].name == "Untouched");
    CHECK(inspection.resources[2].name == "Orphaned");
    CHECK(inspection.resources[1].lifetime.HasNoWrite());
    CHECK(inspection.resources[2].lifetime.HasNoWrite());
    CHECK(inspection.resources[1].aliasGroup == kNoAliasGroup);
    CHECK(inspection.resources[2].aliasGroup == kNoAliasGroup);

    CHECK(inspection.resources[3].name == "Early");
    CHECK(inspection.resources[4].name == "Late");
    CHECK(inspection.resources[3].aliasGroup != kNoAliasGroup);
    CHECK(inspection.resources[3].aliasGroup == inspection.resources[4].aliasGroup);
}

TEST_CASE("a transient written and never read still groups") {
    // **The other reading of "a resource never read", and the behaviour that actually falls out
    // of the field documentation.** `ResourceLifetime::lastPass` is the last pass that reads
    // *or writes*, so a transient written at position 0 and never read is live at exactly one
    // position -- which is the truth about it: its memory is needed while it is being written
    // to, and free afterwards. So it can share memory with something live later, and it does.
    //
    // Recorded as a case because the two readings of the phrase differ, and because a change of
    // mind about it should have to edit an assertion rather than pass quietly.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner    = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> writeOnly = owner.CreateTexture("WriteOnly", kBaseDescription);
    REQUIRE(writeOnly.has_value());
    const Result<TextureId> later = owner.CreateTexture("Later", kBaseDescription);
    REQUIRE(later.has_value());
    REQUIRE(owner.Write(*writeOnly, ResourceAccess::ColorAttachmentWrite));

    GiveDisjointSpan(graph, *later, 1);

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources[1].name == "WriteOnly");
    CHECK(inspection.resources[1].lifetime == ResourceLifetime{0, 0});
    CHECK(inspection.resources[2].name == "Later");
    CHECK(inspection.resources[2].lifetime == ResourceLifetime{1, 2});
    CHECK(inspection.resources[1].aliasGroup != kNoAliasGroup);
    CHECK(inspection.resources[1].aliasGroup == inspection.resources[2].aliasGroup);
}

TEST_CASE("a lone transient candidate is not a group of one") {
    // `kNoAliasGroup` means "shares memory with nothing", and one candidate shares memory with
    // nothing -- so first-fit's bin of one is discarded rather than reported. A grouping that
    // numbered every bin would give this resource group zero, which a reader would take for a
    // sharing decision that was never made.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> only  = owner.CreateTexture("Only", kBaseDescription);
    REQUIRE(only.has_value());
    GiveDisjointSpan(graph, *only, 1);

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.resources[1].name == "Only");
    CHECK_FALSE(inspection.resources[1].lifetime.HasNoWrite());
    CHECK(inspection.resources[1].aliasGroup == kNoAliasGroup);
}
