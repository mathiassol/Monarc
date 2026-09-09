#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Render/RenderGraph.h>

// The dependency graph the declared reads and writes imply, the execution order derived from
// it, and the refusals it produces: a cycle, and a transient nothing writes.
//
// **Not one line of this file needs a device**, for the reason `RenderGraph`'s class comment
// gives and Private/Compile.cpp repeats: every stage under test reads three arrays of plain
// aggregates and writes back into fields of the same three. There is no `RHI::IDevice` and no
// command list anywhere in it.
//
// **The headline is that execution order is *derived* and not declaration order, and the two
// facts that follow from it are what most of this file is about.** Ids are handed out in
// declaration order, so a pass can only name a resource created at or before itself -- but
// which pass *reads* one is unconstrained, so a reader can be declared before its writer.
// Derive the order and a cycle becomes a shape a legal set of declarations can have; take the
// order from declaration order instead and every edge points forwards by construction, no
// cycle is reachable, and the cycle refusal is dead code. Every cycle case below is a real
// declaration that a real sort fails to order.

using Monarc::ErrorCode;
using Monarc::Result;
using Monarc::Status;
using Monarc::SystemAllocator;
using Monarc::Render::DiagnosticKind;
using Monarc::Render::GraphDiagnostic;
using Monarc::Render::GraphInspection;
using Monarc::Render::GraphPhase;
using Monarc::Render::kNoDiagnosticGroup;
using Monarc::Render::kNoPass;
using Monarc::Render::PassBuilder;
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

/// An import standing in for a swapchain image, with Phase A3's measured states.
///
/// **Every graph in this file that is meant to survive culling anchors on one of these**, and
/// the anchor is not scaffolding: a pass writing an imported resource is the only thing that
/// keeps a frame alive, because it is the only declaration that says something outside the
/// graph consumes what the frame produced. `index` distinguishes the handles, since importing
/// one texture twice in a build is refused.
[[nodiscard]] TextureImport Anchor(Monarc::u32 index) {
    return TextureImport(TextureHandle::ForTesting(index, 1), kBaseDescription,
                         TextureState{TextureLayout::Undefined,
                                      PipelineStage::ColorAttachmentOutput, Access::None},
                         TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                      Access::None});
}

/// Adds a pass that imports its own anchor and writes it, so that culling keeps it, and hands
/// back the builder for whatever else it declares.
[[nodiscard]] PassBuilder AnchoredPass(RenderGraph& graph, const char* name,
                                       Monarc::u32 anchorIndex) {
    Result<PassBuilder> pass = graph.AddPass(name);
    REQUIRE(pass.has_value());
    const Result<TextureId> image = pass->ImportTexture("Anchor", Anchor(anchorIndex));
    REQUIRE(image.has_value());
    REQUIRE(pass->Write(*image, ResourceAccess::ColorAttachmentWrite));
    return *pass;
}

/// The diagnostics of `inspection` whose kind is `kind`, counted.
[[nodiscard]] int DiagnosticsOfKind(const GraphInspection& inspection, DiagnosticKind kind) {
    int count = 0;
    for (const GraphDiagnostic& diagnostic : inspection.diagnostics) {
        count += diagnostic.kind == kind ? 1 : 0;
    }
    return count;
}

}  // namespace

TEST_CASE("execution order is derived from the dependencies, not from declaration order") {
    // **The crux of Task 2, and the case that would be impossible to write if order were
    // declaration order.** Pass 0 creates a transient and anchors itself; pass 1 reads it; pass
    // 2 writes it. The only dependency is 2 before 1, and the derived order says so: the pass
    // declared second runs third.
    //
    // Nothing here is culled -- each pass writes an imported resource of its own -- so the
    // order is the only thing under test.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             setup   = AnchoredPass(graph, "Setup", 0);
    const Result<TextureId> scratch = setup.CreateTexture("Scratch", kBaseDescription);
    REQUIRE(scratch.has_value());

    PassBuilder consumer = AnchoredPass(graph, "Consumer", 1);
    REQUIRE(consumer.Read(*scratch, ResourceAccess::SampledRead));

    PassBuilder producer = AnchoredPass(graph, "Producer", 2);
    REQUIRE(producer.Write(*scratch, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 3u);
    for (const Monarc::Render::PassInspection& pass : inspection.passes) {
        CHECK_FALSE(pass.culled);
    }

    CHECK(inspection.passes[0].executionOrder == 0u);
    // The two that matter: declaration order 1 then 2, execution order 2 then 1.
    CHECK(inspection.passes[1].executionOrder == 2u);
    CHECK(inspection.passes[2].executionOrder == 1u);
    CHECK(inspection.passes[2].executionOrder < inspection.passes[1].executionOrder);
    // And the declaration index is untouched by any of it, which is why the two are separate
    // fields.
    CHECK(inspection.passes[1].index == 1u);
    CHECK(inspection.passes[2].index == 2u);
}

TEST_CASE("declaration order breaks a tie, at the first choice and at a later one") {
    // **Two passes ready at once, twice over, and each time the lower declaration index goes
    // first.** The tie-break is not a nicety: `WriteInspectionText` renders a stable report and
    // Tests/TestGraphInspection.cpp pins exact text, so a sort whose tie-break depended on
    // iteration order would make a frame's report change between builds of one declaration.
    //
    // Pass 0 writes two transients; passes 2 and 3 each read one of them; pass 3 also writes a
    // third that pass 1 reads. So after pass 0 is placed, passes 2 and 3 are both ready -- a
    // tie at a step that is not the first -- and pass 1 becomes ready only once pass 3 has run.
    // The full order is 0, 2, 3, 1. A tie-break that took the *highest* index would place 3
    // before 2 and produce 0, 3, 2, 1, which every assertion below distinguishes.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             root = AnchoredPass(graph, "Root", 0);
    const Result<TextureId> left = root.CreateTexture("Left", kBaseDescription);
    REQUIRE(left.has_value());
    const Result<TextureId> right = root.CreateTexture("Right", kBaseDescription);
    REQUIRE(right.has_value());
    const Result<TextureId> tail = root.CreateTexture("Tail", kBaseDescription);
    REQUIRE(tail.has_value());
    REQUIRE(root.Write(*left, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(root.Write(*right, ResourceAccess::ColorAttachmentWrite));

    PassBuilder last = AnchoredPass(graph, "Last", 1);
    REQUIRE(last.Read(*tail, ResourceAccess::SampledRead));

    PassBuilder readsLeft = AnchoredPass(graph, "ReadsLeft", 2);
    REQUIRE(readsLeft.Read(*left, ResourceAccess::SampledRead));

    PassBuilder readsRight = AnchoredPass(graph, "ReadsRight", 3);
    REQUIRE(readsRight.Read(*right, ResourceAccess::SampledRead));
    REQUIRE(readsRight.Write(*tail, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 4u);
    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == 3u);
    CHECK(inspection.passes[2].executionOrder == 1u);
    CHECK(inspection.passes[3].executionOrder == 2u);
}

TEST_CASE("two passes joined by two resources are still ordered") {
    // **The edge multiplicity, which nothing else in the suite can reach.** The sort counts one
    // unsatisfied predecessor per (resource, write, read) triple and relaxes the same triples,
    // so the two have to agree: a count that saw one edge and a relaxation that cleared two
    // would wrap the counter past zero and stall, and a count that saw two with a relaxation
    // that cleared one would stall outright. Either way this graph -- which has a perfectly
    // good order -- would be refused as a cycle.
    //
    // Two resources between one pair of passes is the smallest declaration that can tell those
    // apart, since with one resource the multiplicity is one either way.
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
    CHECK(inspection.phase == GraphPhase::Compiled);
    CHECK(inspection.diagnostics.empty());
    REQUIRE(inspection.passes.size() == 2u);
    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == 1u);
}

TEST_CASE("a read-modify-write pass is not a cycle with itself") {
    // **A pass declaring both a read and a write of one resource is legal -- a `LoadOp::Load`
    // attachment or a blend -- and the edge rule has to exclude the reader being the writer or
    // it would be a one-pass cycle.** Both shapes it comes in are here: the imported case,
    // which is what a real blended present looks like, and the transient case, where a second
    // pass also writes the resource so that the self-edge is not the only edge on it.
    //
    // Under an edge rule without the exclusion, pass 0's counter would never reach zero and
    // this graph would be refused as a dependency cycle.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> blend = graph.AddPass("Blend");
    REQUIRE(blend.has_value());
    const Result<TextureId> image = blend->ImportTexture("Anchor", Anchor(0));
    REQUIRE(image.has_value());
    REQUIRE(blend->Read(*image, ResourceAccess::ColorAttachmentRead));
    REQUIRE(blend->Write(*image, ResourceAccess::ColorAttachmentWrite));
    const Result<TextureId> scratch = blend->CreateTexture("Scratch", kBaseDescription);
    REQUIRE(scratch.has_value());
    REQUIRE(blend->Write(*scratch, ResourceAccess::ColorAttachmentWrite));

    PassBuilder accumulate = AnchoredPass(graph, "Accumulate", 1);
    REQUIRE(accumulate.Read(*scratch, ResourceAccess::ColorAttachmentRead));
    REQUIRE(accumulate.Write(*scratch, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::Compiled);
    CHECK(inspection.diagnostics.empty());
    REQUIRE(inspection.passes.size() == 2u);
    CHECK(inspection.passes[0].executionOrder == 0u);
    CHECK(inspection.passes[1].executionOrder == 1u);
}

TEST_CASE("a two-pass cycle is refused, naming both passes in one group") {
    // **The smallest cycle a declaration can express, and it takes exactly two passes.** Pass 0
    // creates both resources, reads one and writes the other; pass 1 reads what pass 0 wrote and
    // writes what pass 0 reads. Each therefore depends on the other, and no order exists.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    const Result<TextureId> a = first->CreateTexture("A", kBaseDescription);
    REQUIRE(a.has_value());
    const Result<TextureId> b = first->CreateTexture("B", kBaseDescription);
    REQUIRE(b.has_value());
    REQUIRE(first->Read(*a, ResourceAccess::SampledRead));
    REQUIRE(first->Write(*b, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE(second.has_value());
    REQUIRE(second->Read(*b, ResourceAccess::SampledRead));
    REQUIRE(second->Write(*a, ResourceAccess::ColorAttachmentWrite));

    const Status compiled = graph.Compile();
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == ErrorCode::InvalidArgument);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::CompileFailed);
    CHECK(inspection.diagnosticsDropped == 0u);
    REQUIRE(inspection.diagnostics.size() == 2u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::DependencyCycle);
    CHECK(inspection.diagnostics[1].kind == DiagnosticKind::DependencyCycle);
    CHECK(inspection.diagnostics[0].pass == 0u);
    CHECK(inspection.diagnostics[1].pass == 1u);
    // One report, so one group -- and a real one, not the standalone sentinel every declaration
    // refusal carries.
    CHECK(inspection.diagnostics[0].group == inspection.diagnostics[1].group);
    CHECK(inspection.diagnostics[0].group != kNoDiagnosticGroup);

    // Nothing downstream ran, so the report says no order was settled rather than half of one.
    for (const Monarc::Render::PassInspection& pass : inspection.passes) {
        CHECK(pass.executionOrder == kNoPass);
        CHECK_FALSE(pass.culled);
    }
    for (const Monarc::Render::ResourceInspection& resource : inspection.resources) {
        CHECK(resource.lifetime.HasNoWrite());
    }
}

TEST_CASE("a three-pass cycle names all three passes in one group") {
    // The same refusal at a length one `pass` field could not have expressed even in principle,
    // and the reason `GraphDiagnostic::group` is a group id rather than a second pass field.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    const Result<TextureId> a = first->CreateTexture("A", kBaseDescription);
    REQUIRE(a.has_value());
    const Result<TextureId> b = first->CreateTexture("B", kBaseDescription);
    REQUIRE(b.has_value());
    const Result<TextureId> c = first->CreateTexture("C", kBaseDescription);
    REQUIRE(c.has_value());
    REQUIRE(first->Read(*a, ResourceAccess::SampledRead));
    REQUIRE(first->Write(*b, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE(second.has_value());
    REQUIRE(second->Read(*b, ResourceAccess::SampledRead));
    REQUIRE(second->Write(*c, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> third = graph.AddPass("Third");
    REQUIRE(third.has_value());
    REQUIRE(third->Read(*c, ResourceAccess::SampledRead));
    REQUIRE(third->Write(*a, ResourceAccess::ColorAttachmentWrite));

    REQUIRE_FALSE(graph.Compile().has_value());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 3u);
    CHECK(inspection.diagnosticsDropped == 0u);
    for (Monarc::u32 i = 0; i < 3u; ++i) {
        CHECK(inspection.diagnostics[i].kind == DiagnosticKind::DependencyCycle);
        CHECK(inspection.diagnostics[i].pass == i);
        CHECK(inspection.diagnostics[i].group == inspection.diagnostics[0].group);
        CHECK(inspection.diagnostics[i].group != kNoDiagnosticGroup);
    }
}

TEST_CASE("two cycles in one graph stay apart") {
    // **The case `GraphDiagnostic::group` was chosen for.** Four passes, two independent
    // cycles; under one `pass` field per row these are four unrelated refusals and no reader can
    // reassemble either cycle. With a group id they are two reports of two passes each, and the
    // report says which is which.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> leftFirst = graph.AddPass("LeftFirst");
    REQUIRE(leftFirst.has_value());
    const Result<TextureId> leftA = leftFirst->CreateTexture("LeftA", kBaseDescription);
    REQUIRE(leftA.has_value());
    const Result<TextureId> leftB = leftFirst->CreateTexture("LeftB", kBaseDescription);
    REQUIRE(leftB.has_value());
    REQUIRE(leftFirst->Read(*leftA, ResourceAccess::SampledRead));
    REQUIRE(leftFirst->Write(*leftB, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> leftSecond = graph.AddPass("LeftSecond");
    REQUIRE(leftSecond.has_value());
    REQUIRE(leftSecond->Read(*leftB, ResourceAccess::SampledRead));
    REQUIRE(leftSecond->Write(*leftA, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> rightFirst = graph.AddPass("RightFirst");
    REQUIRE(rightFirst.has_value());
    const Result<TextureId> rightA = rightFirst->CreateTexture("RightA", kBaseDescription);
    REQUIRE(rightA.has_value());
    const Result<TextureId> rightB = rightFirst->CreateTexture("RightB", kBaseDescription);
    REQUIRE(rightB.has_value());
    REQUIRE(rightFirst->Read(*rightA, ResourceAccess::SampledRead));
    REQUIRE(rightFirst->Write(*rightB, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> rightSecond = graph.AddPass("RightSecond");
    REQUIRE(rightSecond.has_value());
    REQUIRE(rightSecond->Read(*rightB, ResourceAccess::SampledRead));
    REQUIRE(rightSecond->Write(*rightA, ResourceAccess::ColorAttachmentWrite));

    REQUIRE_FALSE(graph.Compile().has_value());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 4u);
    CHECK(inspection.diagnosticsDropped == 0u);
    for (Monarc::u32 i = 0; i < 4u; ++i) {
        CHECK(inspection.diagnostics[i].kind == DiagnosticKind::DependencyCycle);
        CHECK(inspection.diagnostics[i].pass == i);
        CHECK(inspection.diagnostics[i].group != kNoDiagnosticGroup);
    }
    // Two reports, and the two halves do not bleed into one another: rows 0 and 1 share a
    // group, rows 2 and 3 share a group, and the two groups differ. A detector that put every
    // cycle in one group would pass the assertions above and fail this one.
    CHECK(inspection.diagnostics[0].group == inspection.diagnostics[1].group);
    CHECK(inspection.diagnostics[2].group == inspection.diagnostics[3].group);
    CHECK(inspection.diagnostics[0].group != inspection.diagnostics[2].group);
}

TEST_CASE("a pass that depends on a cycle is not itself in one") {
    // **The set a group reports is the passes with no order *among themselves*, and a pass that
    // merely reads what a cycle produces is not one of them.** It has no order either -- nothing
    // it waits for can ever run -- but its own declarations are correct, and naming it would put
    // a row on a pass whose author has nothing to change.
    //
    // **The dependent pass also reads a resource a cycle member reads, and that detail is
    // load-bearing.** Membership is decided by mutual *dependency*, and two passes reading one
    // resource have none: neither produces anything the other consumes. A detector whose
    // backwards walk followed reads as well as writes would find this pass reachable from the
    // cycle in both directions and put it in the report -- and with the two passes sharing only
    // the write edge, nothing would have said so.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    const Result<TextureId> a = first->CreateTexture("A", kBaseDescription);
    REQUIRE(a.has_value());
    const Result<TextureId> b = first->CreateTexture("B", kBaseDescription);
    REQUIRE(b.has_value());
    const Result<TextureId> downstream = first->CreateTexture("Downstream", kBaseDescription);
    REQUIRE(downstream.has_value());
    REQUIRE(first->Read(*a, ResourceAccess::SampledRead));
    REQUIRE(first->Write(*b, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE(second.has_value());
    REQUIRE(second->Read(*b, ResourceAccess::SampledRead));
    REQUIRE(second->Write(*a, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(second->Write(*downstream, ResourceAccess::ColorAttachmentWrite));

    PassBuilder dependent = AnchoredPass(graph, "Dependent", 0);
    REQUIRE(dependent.Read(*downstream, ResourceAccess::SampledRead));
    // Shares a read with pass 0, and shares no dependency with it.
    REQUIRE(dependent.Read(*a, ResourceAccess::ColorAttachmentRead));

    REQUIRE_FALSE(graph.Compile().has_value());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 2u);
    CHECK(inspection.diagnostics[0].pass == 0u);
    CHECK(inspection.diagnostics[1].pass == 1u);
    CHECK(inspection.diagnostics[0].group == inspection.diagnostics[1].group);
    // Pass 2 is the one that is not named, and the count above is what says so.
    CHECK(DiagnosticsOfKind(inspection, DiagnosticKind::DependencyCycle) == 2);
}

TEST_CASE("a cycle longer than the diagnostics pool leaves every surviving row grouped") {
    // **The accounting the group id was chosen to compose with.** A four-pass cycle into a pool
    // with room for two: what fits still says which report it belongs to, and
    // `diagnosticsDropped` says the list is not the whole story. An inline pass list would have
    // had to reproduce this truncation *inside* one row.
    SystemAllocator     allocator;
    RenderGraph::Config cramped{};
    cramped.maxDiagnostics = 2;
    RenderGraph graph(allocator, cramped);

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    TextureId ring[4] = {};
    for (Monarc::u32 i = 0; i < 4u; ++i) {
        const Result<TextureId> created = first->CreateTexture("Ring", kBaseDescription);
        REQUIRE(created.has_value());
        ring[i] = *created;
    }
    REQUIRE(first->Read(ring[0], ResourceAccess::SampledRead));
    REQUIRE(first->Write(ring[1], ResourceAccess::ColorAttachmentWrite));

    for (Monarc::u32 i = 1; i < 4u; ++i) {
        Result<PassBuilder> pass = graph.AddPass("Link");
        REQUIRE(pass.has_value());
        REQUIRE(pass->Read(ring[i], ResourceAccess::SampledRead));
        REQUIRE(pass->Write(ring[(i + 1) % 4], ResourceAccess::ColorAttachmentWrite));
    }

    REQUIRE_FALSE(graph.Compile().has_value());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 2u);
    CHECK(inspection.diagnosticsDropped == 2u);
    for (const GraphDiagnostic& diagnostic : inspection.diagnostics) {
        CHECK(diagnostic.kind == DiagnosticKind::DependencyCycle);
        CHECK(diagnostic.group != kNoDiagnosticGroup);
    }
    CHECK(inspection.diagnostics[0].group == inspection.diagnostics[1].group);
    // The head of the report survives, which is the drop policy the diagnostics list has: the
    // rows that fit are the first two passes of the cycle.
    CHECK(inspection.diagnostics[0].pass == 0u);
    CHECK(inspection.diagnostics[1].pass == 1u);
}

TEST_CASE("a transient two passes read and no pass writes is refused once, naming both") {
    // **The plan's open question, answered where it said it would be.** A transient has no
    // contents until something writes it, so a pass reading one nothing wrote reads undefined
    // memory. **Two readers, one row**: what is wrong is that nothing writes the resource, which
    // is one mistake however many passes read it, and the row names the resource plus the pass
    // that declared the earliest of those reads.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> ghost = owner.CreateTexture("Ghost", kBaseDescription);
    REQUIRE(ghost.has_value());

    PassBuilder firstReader = AnchoredPass(graph, "FirstReader", 1);
    REQUIRE(firstReader.Read(*ghost, ResourceAccess::SampledRead));

    PassBuilder secondReader = AnchoredPass(graph, "SecondReader", 2);
    REQUIRE(secondReader.Read(*ghost, ResourceAccess::SampledRead));

    const Status compiled = graph.Compile();
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == ErrorCode::InvalidArgument);

    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::CompileFailed);
    REQUIRE(inspection.diagnostics.size() == 1u);
    CHECK(inspection.diagnostics[0].kind == DiagnosticKind::TransientNeverWritten);
    CHECK(inspection.diagnostics[0].pass == 1u);
    CHECK(inspection.diagnostics[0].resource == *ghost);
    // A refusal about one resource, so it stands alone -- the sentinel, not a group.
    CHECK(inspection.diagnostics[0].group == kNoDiagnosticGroup);
}

TEST_CASE("an imported resource read and never written is not refused") {
    // **The other half of the same decision, and the reason it is a decision rather than a
    // rule.** An import declares its `incoming` state, so its contents came from outside the
    // graph and reading it first is meaningful. Refusing it would make a read-only external
    // texture -- an environment map, a previous frame's result -- undeclarable.
    //
    // Two imports, one read-only and one written, because a graph with only the read-only one
    // would be culled entirely and the compile would succeed for the wrong reason.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             pass   = AnchoredPass(graph, "Composite", 0);
    const Result<TextureId> source = pass.ImportTexture("Source", Anchor(1));
    REQUIRE(source.has_value());
    REQUIRE(pass.Read(*source, ResourceAccess::SampledRead));

    CHECK(graph.Compile());
    const GraphInspection inspection = graph.Inspect();
    CHECK(inspection.phase == GraphPhase::Compiled);
    CHECK(inspection.diagnostics.empty());
    CHECK_FALSE(inspection.passes[0].culled);
}

TEST_CASE("a read-modify-write of a transient nothing else writes is not refused") {
    // The one shape where "the first access is a read" and "no pass writes it" come apart. The
    // pass reads and writes the resource, so something does write it, and a rule that had looked
    // at the earliest *access declaration* rather than at whether a write exists would refuse a
    // legal blend.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             pass   = AnchoredPass(graph, "Blend", 0);
    const Result<TextureId> target = pass.CreateTexture("Target", kBaseDescription);
    REQUIRE(target.has_value());
    REQUIRE(pass.Read(*target, ResourceAccess::ColorAttachmentRead));
    REQUIRE(pass.Write(*target, ResourceAccess::ColorAttachmentWrite));

    CHECK(graph.Compile());
    CHECK(graph.Inspect().diagnostics.empty());
}

TEST_CASE("a second build reports its own decisions, not the previous build's") {
    // **Compilation's scratch is reserved once and reused every build, so every stage has to
    // write a slot before it reads one.** A graph is compiled, reset and compiled again here,
    // and the second build is deliberately the opposite of the first in every field this task
    // computes: the first has five passes that all run and a pair of transients that share an
    // alias group, the second has one pass that is culled and one resource that is used by
    // nothing.
    //
    // What that catches is a stage that carried a value across the `Reset` -- a culling flag
    // still false from a build where nothing was culled, a lifetime still naming positions from
    // a longer frame, an alias group from a pair of resources that no longer exist. A single
    // build cannot reach any of it.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner = AnchoredPass(graph, "Owner", 0);
    const Result<TextureId> early = owner.CreateTexture("Early", kBaseDescription);
    REQUIRE(early.has_value());
    const Result<TextureId> late = owner.CreateTexture("Late", kBaseDescription);
    REQUIRE(late.has_value());

    PassBuilder earlyWriter = AnchoredPass(graph, "EarlyWriter", 1);
    REQUIRE(earlyWriter.Write(*early, ResourceAccess::ColorAttachmentWrite));
    PassBuilder earlyReader = AnchoredPass(graph, "EarlyReader", 2);
    REQUIRE(earlyReader.Read(*early, ResourceAccess::SampledRead));
    PassBuilder lateWriter = AnchoredPass(graph, "LateWriter", 3);
    REQUIRE(lateWriter.Write(*late, ResourceAccess::ColorAttachmentWrite));
    PassBuilder lateReader = AnchoredPass(graph, "LateReader", 4);
    REQUIRE(lateReader.Read(*late, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection first = graph.Inspect();
    REQUIRE(first.passes.size() == 5u);
    for (const Monarc::Render::PassInspection& pass : first.passes) {
        CHECK_FALSE(pass.culled);
    }
    CHECK(first.resources[1].aliasGroup != Monarc::Render::kNoAliasGroup);
    CHECK(first.resources[1].aliasGroup == first.resources[2].aliasGroup);
    CHECK_FALSE(first.resources[1].lifetime.HasNoWrite());

    graph.Reset();

    Result<PassBuilder> solo = graph.AddPass("Solo");
    REQUIRE(solo.has_value());
    const Result<TextureId> only = solo->CreateTexture("Only", kBaseDescription);
    REQUIRE(only.has_value());
    REQUIRE(solo->Write(*only, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection second = graph.Inspect();
    CHECK(second.phase == GraphPhase::Compiled);
    CHECK(second.diagnostics.empty());
    REQUIRE(second.passes.size() == 1u);
    CHECK(second.passes[0].culled);
    CHECK(second.passes[0].executionOrder == kNoPass);
    REQUIRE(second.resources.size() == 1u);
    CHECK(second.resources[0].lifetime.HasNoWrite());
    CHECK(second.resources[0].lifetime.lastPass == kNoPass);
    CHECK(second.resources[0].aliasGroup == Monarc::Render::kNoAliasGroup);
}

TEST_CASE("a cycle and an unwritten transient are both reported") {
    // **Neither check short-circuits the other, because they are different mistakes** -- a cycle
    // is a mutual dependency and an unwritten transient is an absent one. A build can have both,
    // and a `Compile` that reported only whichever check ran first would hide half of what is
    // wrong from the one report its caller gets to read.
    //
    // The cycle's rows come first because that is the order the stages run in, and the code
    // returned is the cycle's for the same reason.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> first = graph.AddPass("First");
    REQUIRE(first.has_value());
    const Result<TextureId> a = first->CreateTexture("A", kBaseDescription);
    REQUIRE(a.has_value());
    const Result<TextureId> b = first->CreateTexture("B", kBaseDescription);
    REQUIRE(b.has_value());
    const Result<TextureId> ghost = first->CreateTexture("Ghost", kBaseDescription);
    REQUIRE(ghost.has_value());
    REQUIRE(first->Read(*a, ResourceAccess::SampledRead));
    REQUIRE(first->Write(*b, ResourceAccess::ColorAttachmentWrite));

    Result<PassBuilder> second = graph.AddPass("Second");
    REQUIRE(second.has_value());
    REQUIRE(second->Read(*b, ResourceAccess::SampledRead));
    REQUIRE(second->Write(*a, ResourceAccess::ColorAttachmentWrite));

    PassBuilder reader = AnchoredPass(graph, "Reader", 0);
    REQUIRE(reader.Read(*ghost, ResourceAccess::SampledRead));

    const Status compiled = graph.Compile();
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == ErrorCode::InvalidArgument);

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.diagnostics.size() == 3u);
    CHECK(DiagnosticsOfKind(inspection, DiagnosticKind::DependencyCycle) == 2);
    CHECK(DiagnosticsOfKind(inspection, DiagnosticKind::TransientNeverWritten) == 1);
    CHECK(inspection.diagnostics[2].kind == DiagnosticKind::TransientNeverWritten);
    CHECK(inspection.diagnostics[2].resource == *ghost);
    CHECK(inspection.diagnostics[2].group == kNoDiagnosticGroup);
}
