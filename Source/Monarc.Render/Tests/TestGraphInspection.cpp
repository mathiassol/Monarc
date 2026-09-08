#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Render/GraphInspection.h>
#include <Monarc/Render/RenderGraph.h>

#include <iterator>
#include <string_view>
#include <type_traits>

// **The one file in this suite where a string comparison is the right assertion, because the
// text is what is under test.** Everywhere else the inspection's fields are what a test looks
// at -- see TestPassDeclaration.cpp -- and that order is deliberate: reviews on the Phase A3
// branch deleted assertions that matched substrings of a rendered report and passed for the
// wrong reasons. `WriteInspectionText` is a projection of the structure, and a projection has
// to be checked against its output.

using Monarc::Result;
using Monarc::SystemAllocator;
using Monarc::Render::BarrierCauseKind;
using Monarc::Render::DiagnosticKind;
using Monarc::Render::GraphInspection;
using Monarc::Render::GraphPhase;
using Monarc::Render::GraphQueue;
using Monarc::Render::InspectionText;
using Monarc::Render::kNoPass;
using Monarc::Render::PassBuilder;
using Monarc::Render::RenderGraph;
using Monarc::Render::ResourceAccess;
using Monarc::Render::ResourceLifetime;
using Monarc::Render::ResourceOrigin;
using Monarc::Render::TextureId;
using Monarc::Render::TextureImport;
using Monarc::Render::TextureState;
using Monarc::Render::ToString;
using Monarc::Render::WriteInspectionText;
using Monarc::RHI::Access;
using Monarc::RHI::Format;
using Monarc::RHI::PipelineStage;
using Monarc::RHI::TextureDescription;
using Monarc::RHI::TextureHandle;
using Monarc::RHI::TextureLayout;
using Monarc::RHI::TextureUsage;

namespace {

constexpr TextureDescription kSwapchainDescription{
    Monarc::RHI::Extent2D{1280, 720}, Format::B8G8R8A8_UNORM, TextureUsage::ColorAttachment};

/// Declares the frame `Monarc.FirstLight` will declare in Task 4 -- one pass writing an
/// imported swapchain image -- and compiles it.
///
/// The states are Phase A3's measured ones; see TestPassDeclaration.cpp for where they come
/// from. Shared by the exact-text case and the stability case so that both describe the same
/// frame.
void DeclareFirstLightFrame(RenderGraph& graph) {
    Result<PassBuilder> pass = graph.AddPass("Present");
    REQUIRE(pass.has_value());
    const Result<TextureId> image = pass->ImportTexture(
        "Swapchain",
        TextureImport(TextureHandle::ForTesting(4, 1), kSwapchainDescription,
                      TextureState{TextureLayout::Undefined,
                                   PipelineStage::ColorAttachmentOutput, Access::None},
                      TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                   Access::None}));
    REQUIRE(image.has_value());
    REQUIRE(pass->Write(*image, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(graph.Compile());
}

/// Renders `inspection` into `buffer` and hands back what it wrote as a view.
///
/// `WriteInspectionText` does not NUL-terminate -- `written` is the length -- so this is how
/// every caller reads the output back.
[[nodiscard]] std::string_view Render(const GraphInspection& inspection,
                                      std::span<char>        buffer) {
    const InspectionText result = WriteInspectionText(inspection, buffer);
    REQUIRE_FALSE(result.Truncated());
    return std::string_view(buffer.data(), result.written);
}

constexpr GraphPhase kAllPhases[] = {GraphPhase::Declaring, GraphPhase::Compiled,
                                     GraphPhase::CompileFailed};

constexpr ResourceOrigin kAllOrigins[] = {ResourceOrigin::Transient, ResourceOrigin::Imported};

constexpr GraphQueue kAllQueues[] = {GraphQueue::Graphics};

constexpr BarrierCauseKind kAllCauseKinds[] = {BarrierCauseKind::ImportIncoming,
                                               BarrierCauseKind::PassAccess,
                                               BarrierCauseKind::ImportOutgoing};

constexpr DiagnosticKind kAllDiagnosticKinds[] = {
    DiagnosticKind::PassPoolExhausted,       DiagnosticKind::ResourcePoolExhausted,
    DiagnosticKind::AccessPoolExhausted,     DiagnosticKind::UnknownResource,
    DiagnosticKind::UnknownPass,             DiagnosticKind::AccessDirectionMismatch,
    DiagnosticKind::AccessNamesNoTexture,    DiagnosticKind::DuplicateAccess,
    DiagnosticKind::DuplicateImport,         DiagnosticKind::InvalidImport,
    DiagnosticKind::RecordAlreadySet,        DiagnosticKind::AlreadyCompiled,
};

// ---------------------------------------------------------------------------------------
// The completeness guard the five lists above need, and the mechanism is the one
// Monarc.RHI.Vulkan/Tests/TestVulkanBarrierTranslate.cpp introduced after `kAllLayouts` went
// stale in Phase A3 Task 4 and nothing said so. See TestAccess.cpp for the argument at length;
// the short version is that each `IsEnumerator` below is a `default`-less switch, so a new
// enumerator breaks the build there first, and the assertion that the value one past a list's
// last index is *not* an enumerator then breaks until the list grows to match.
//
// All five of these enums are sequential from zero, which GraphInspection.h keeps.
// ---------------------------------------------------------------------------------------

/// @{
[[nodiscard]] constexpr bool IsEnumerator(GraphPhase phase) {
    switch (phase) {
        case GraphPhase::Declaring:
        case GraphPhase::Compiled:
        case GraphPhase::CompileFailed:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsEnumerator(ResourceOrigin origin) {
    switch (origin) {
        case ResourceOrigin::Transient:
        case ResourceOrigin::Imported:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsEnumerator(GraphQueue queue) {
    switch (queue) {
        case GraphQueue::Graphics:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsEnumerator(BarrierCauseKind kind) {
    switch (kind) {
        case BarrierCauseKind::ImportIncoming:
        case BarrierCauseKind::PassAccess:
        case BarrierCauseKind::ImportOutgoing:
            return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsEnumerator(DiagnosticKind kind) {
    switch (kind) {
        case DiagnosticKind::PassPoolExhausted:
        case DiagnosticKind::ResourcePoolExhausted:
        case DiagnosticKind::AccessPoolExhausted:
        case DiagnosticKind::UnknownResource:
        case DiagnosticKind::UnknownPass:
        case DiagnosticKind::AccessDirectionMismatch:
        case DiagnosticKind::AccessNamesNoTexture:
        case DiagnosticKind::DuplicateAccess:
        case DiagnosticKind::DuplicateImport:
        case DiagnosticKind::InvalidImport:
        case DiagnosticKind::RecordAlreadySet:
        case DiagnosticKind::AlreadyCompiled:
            return true;
    }
    return false;
}
/// @}

/// Whether `values` is `0`, `1`, `2`, ... in order.
template <typename Enum, Monarc::usize N>
[[nodiscard]] constexpr bool IsSequenceInOrder(const Enum (&values)[N]) {
    for (Monarc::usize i = 0; i < N; ++i) {
        if (static_cast<Monarc::usize>(values[i]) != i) {
            return false;
        }
    }
    return true;
}

/// Whether every entry of `values` gets a distinct name that is not the not-a-value marker.
template <typename Enum, Monarc::usize N>
void CheckNamesAreDistinct(const Enum (&values)[N]) {
    for (Monarc::usize i = 0; i < N; ++i) {
        const std::string_view name = ToString(values[i]);
        CAPTURE(name);
        CHECK_FALSE(name.empty());
        CHECK(name.front() != '<');
        for (Monarc::usize j = i + 1; j < N; ++j) {
            CHECK(name != std::string_view(ToString(values[j])));
        }
    }
}

}  // namespace

static_assert(IsSequenceInOrder(kAllPhases));
static_assert(!IsEnumerator(static_cast<GraphPhase>(std::size(kAllPhases))),
              "kAllPhases is missing a GraphPhase enumerator");

static_assert(IsSequenceInOrder(kAllOrigins));
static_assert(!IsEnumerator(static_cast<ResourceOrigin>(std::size(kAllOrigins))),
              "kAllOrigins is missing a ResourceOrigin enumerator");

static_assert(IsSequenceInOrder(kAllQueues));
static_assert(!IsEnumerator(static_cast<GraphQueue>(std::size(kAllQueues))),
              "kAllQueues is missing a GraphQueue enumerator -- a second queue arrived, and "
              "GraphQueue's own comment says what has to arrive with it");

static_assert(IsSequenceInOrder(kAllCauseKinds));
static_assert(!IsEnumerator(static_cast<BarrierCauseKind>(std::size(kAllCauseKinds))),
              "kAllCauseKinds is missing a BarrierCauseKind enumerator");

static_assert(IsSequenceInOrder(kAllDiagnosticKinds));
static_assert(!IsEnumerator(static_cast<DiagnosticKind>(std::size(kAllDiagnosticKinds))),
              "kAllDiagnosticKinds is missing a DiagnosticKind enumerator");

TEST_CASE("every inspection enumerator has its own spelling") {
    CheckNamesAreDistinct(kAllPhases);
    CheckNamesAreDistinct(kAllOrigins);
    CheckNamesAreDistinct(kAllQueues);
    CheckNamesAreDistinct(kAllCauseKinds);
    CheckNamesAreDistinct(kAllDiagnosticKinds);
}

TEST_CASE("a value outside each enumerator set gets a name of its own") {
    // Not any real enumerator's spelling. Conflating a named state with an invalid one
    // misdirects whoever reads the line -- `ToString(Format)` in Monarc/RHI/Types.h states the
    // rule this follows.
    CHECK(std::string_view(ToString(static_cast<GraphPhase>(99))) == "<invalid GraphPhase>");
    CHECK(std::string_view(ToString(static_cast<ResourceOrigin>(99))) ==
          "<invalid ResourceOrigin>");
    CHECK(std::string_view(ToString(static_cast<GraphQueue>(99))) == "<invalid GraphQueue>");
    CHECK(std::string_view(ToString(static_cast<BarrierCauseKind>(99))) ==
          "<invalid BarrierCauseKind>");
    CHECK(std::string_view(ToString(static_cast<DiagnosticKind>(99))) ==
          "<invalid DiagnosticKind>");
}

TEST_CASE("an empty declaring graph renders its header and nothing else") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    char buffer[512] = {};
    CHECK(Render(graph.Inspect(), buffer) ==
          "graph build=0 phase=Declaring\n"
          "counts passes=0 resources=0 accesses=0 barriers=0 diagnostics=0 dropped=0\n");
}

TEST_CASE("the FirstLight frame renders exactly this text") {
    // **The stable, diffable rendering, pinned in full.** One entity per line, `key=value`
    // throughout, a fixed field order. Pinning the whole thing rather than a substring is what
    // "stable" has to mean: a format change is a deliberate edit here, and a field that
    // silently stopped being emitted is a failure rather than a shorter report nobody noticed.
    //
    // The lifetime and alias fields read `none` because Task 2 computes them and has not
    // landed; there is no `barrier` line because Task 3 derives them and has not either. Both
    // are asserted structurally in TestPassDeclaration.cpp too.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});
    DeclareFirstLightFrame(graph);

    char buffer[1024] = {};
    CHECK(Render(graph.Inspect(), buffer) ==
          "graph build=0 phase=Compiled\n"
          "counts passes=1 resources=1 accesses=1 barriers=0 diagnostics=0 dropped=0\n"
          "pass 0 order=0 queue=Graphics culled=no record=no name=\"Present\"\n"
          "resource 0 id=0:0 origin=Imported format=B8G8R8A8_UNORM extent=1280x720 usage=0x1 "
          "lifetime=none..none alias=none name=\"Swapchain\"\n"
          "resource 0 import texture=4:1 "
          "incoming=Undefined/ColorAttachmentOutput(0x20)/None(0x0) "
          "outgoing=PresentSource/None(0x0)/None(0x0)\n"
          "access 0 pass=0 resource=0:0 access=ColorAttachmentWrite\n");
}

TEST_CASE("the same declaration renders identically from two graphs") {
    // The property a diffable artifact needs, and the one an ordering that depended on a hash
    // or a pointer would break. Two independent graphs rather than two calls on one, so that
    // an address baked into the output would differ.
    SystemAllocator allocator;
    RenderGraph     first(allocator, RenderGraph::Config{});
    RenderGraph     second(allocator, RenderGraph::Config{});
    DeclareFirstLightFrame(first);
    DeclareFirstLightFrame(second);

    char firstBuffer[1024]  = {};
    char secondBuffer[1024] = {};
    CHECK(Render(first.Inspect(), firstBuffer) == Render(second.Inspect(), secondBuffer));
}

TEST_CASE("a transient renders no import line") {
    // The second line belongs to an imported resource only. A graph full of transients would
    // otherwise carry a line of `none`s per resource, and a diff of one would be mostly noise.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Offscreen");
    REQUIRE(pass.has_value());
    const Result<TextureId> target = pass->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());
    REQUIRE(pass->Write(*target, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(graph.Compile());

    char buffer[1024] = {};
    CHECK(Render(graph.Inspect(), buffer) ==
          "graph build=0 phase=Compiled\n"
          "counts passes=1 resources=1 accesses=1 barriers=0 diagnostics=0 dropped=0\n"
          "pass 0 order=0 queue=Graphics culled=no record=no name=\"Offscreen\"\n"
          "resource 0 id=0:0 origin=Transient format=B8G8R8A8_UNORM extent=1280x720 usage=0x1 "
          "lifetime=none..none alias=none name=\"Target\"\n"
          "access 0 pass=0 resource=0:0 access=ColorAttachmentWrite\n");
}

TEST_CASE("an access line renders its pass, not its own row number") {
    // **The `access` line's `pass=` field had no text coverage until this case.** Every other
    // exact-text case here declares one access from pass 0, so the row index and the pass index
    // are both `0` and a rendering that emitted either would produce the same report --
    // replacing `access.pass` with the loop counter passed the whole suite. The data is covered
    // structurally in TestPassDeclaration.cpp; this is the projection.
    //
    // Pass 0 creates the resource and declares no access to it, which is legal and is
    // `PassBuilder::CreateTexture`'s stated rule: creating a resource is not accessing it. Pass
    // 1 then writes and reads it -- the read-modify-write case -- so the first access row is 0
    // and belongs to pass 1, and the two numbers cannot be confused for one another.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> producer = graph.AddPass("Producer");
    REQUIRE(producer.has_value());
    const Result<TextureId> target = producer->CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());

    Result<PassBuilder> consumer = graph.AddPass("Consumer");
    REQUIRE(consumer.has_value());
    REQUIRE(consumer->Write(*target, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(consumer->Read(*target, ResourceAccess::ColorAttachmentRead));
    REQUIRE(graph.Compile());

    char buffer[1024] = {};
    CHECK(Render(graph.Inspect(), buffer) ==
          "graph build=0 phase=Compiled\n"
          "counts passes=2 resources=1 accesses=2 barriers=0 diagnostics=0 dropped=0\n"
          "pass 0 order=0 queue=Graphics culled=no record=no name=\"Producer\"\n"
          "pass 1 order=1 queue=Graphics culled=no record=no name=\"Consumer\"\n"
          "resource 0 id=0:0 origin=Transient format=B8G8R8A8_UNORM extent=1280x720 usage=0x1 "
          "lifetime=none..none alias=none name=\"Target\"\n"
          "access 0 pass=1 resource=0:0 access=ColorAttachmentWrite\n"
          "access 1 pass=1 resource=0:0 access=ColorAttachmentRead\n");
}

TEST_CASE("the pass lines come out in execution order, culled passes last") {
    // **Render-Graph.md asks for a "pass list in execution order", and the text is where that
    // is honoured -- `GraphInspection::passes` stays in declaration order.** Hand-built,
    // because it is the only way to reach the property today: every graph a declaration can
    // produce has `executionOrder == index`, so a report of a real graph renders identically
    // whether or not anything sorts. Task 2's reordering is what makes the difference visible
    // in a real frame, and this case is what makes it visible now.
    //
    // Declaration order here is Late, Early, Culled; execution order is Early, Late, and then
    // the culled pass, which sorts last for free because `kNoPass` is the largest `u32` and is
    // still marked in place by `order=none culled=yes`.
    Monarc::Render::PassInspection passes[3] = {};
    passes[0].name           = "Late";
    passes[0].index          = 0;
    passes[0].executionOrder = 1;
    passes[1].name           = "Early";
    passes[1].index          = 1;
    passes[1].executionOrder = 0;
    passes[2].name           = "Culled";
    passes[2].index          = 2;
    passes[2].executionOrder = kNoPass;
    passes[2].culled         = true;

    GraphInspection inspection{};
    inspection.phase  = GraphPhase::Compiled;
    inspection.passes = passes;

    char buffer[1024] = {};
    CHECK(Render(inspection, buffer) ==
          "graph build=0 phase=Compiled\n"
          "counts passes=3 resources=0 accesses=0 barriers=0 diagnostics=0 dropped=0\n"
          "pass 1 order=0 queue=Graphics culled=no record=no name=\"Early\"\n"
          "pass 0 order=1 queue=Graphics culled=no record=no name=\"Late\"\n"
          "pass 2 order=none queue=Graphics culled=yes record=no name=\"Culled\"\n");
}

TEST_CASE("passes with no order yet keep declaration order") {
    // The tie case, and the reason the ordering breaks ties by array position rather than
    // leaving it to whatever a sort does. `AddPass` leaves `executionOrder` at `kNoPass` until
    // `Compile` settles one, so *every* pass of a graph inspected mid-declaration has the same
    // key -- and a report that shuffled them would make an uncompiled graph's text unstable,
    // which is the one property `WriteInspectionText` promises above all others.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    REQUIRE(graph.AddPass("First").has_value());
    REQUIRE(graph.AddPass("Second").has_value());
    REQUIRE(graph.AddPass("Third").has_value());

    char buffer[1024] = {};
    CHECK(Render(graph.Inspect(), buffer) ==
          "graph build=0 phase=Declaring\n"
          "counts passes=3 resources=0 accesses=0 barriers=0 diagnostics=0 dropped=0\n"
          "pass 0 order=none queue=Graphics culled=no record=no name=\"First\"\n"
          "pass 1 order=none queue=Graphics culled=no record=no name=\"Second\"\n"
          "pass 2 order=none queue=Graphics culled=no record=no name=\"Third\"\n");
}

TEST_CASE("a refused declaration renders as a diagnostic line") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Consumer");
    REQUIRE(pass.has_value());
    static_cast<void>(pass->Read(TextureId::ForTesting(7, 0), ResourceAccess::SampledRead));
    REQUIRE_FALSE(graph.Compile().has_value());

    char buffer[1024] = {};
    CHECK(Render(graph.Inspect(), buffer) ==
          "graph build=0 phase=CompileFailed\n"
          "counts passes=1 resources=0 accesses=0 barriers=0 diagnostics=1 dropped=0\n"
          "pass 0 order=none queue=Graphics culled=no record=no name=\"Consumer\"\n"
          "diagnostic 0 kind=UnknownResource code=NotFound pass=0 resource=7:0 "
          "message=\"PassBuilder::Read/Write: that id names no resource in the current "
          "build\"\n");
}

TEST_CASE("a hand-built inspection renders its Task 2 and Task 3 fields") {
    // **The one case that exercises the fields no task fills yet.** A lifetime, an alias group
    // and a derived barrier with its cause are built by hand here, because Tasks 2 and 3 are
    // what will produce them from a declaration and neither has landed. Without this, the
    // rendering of a `barrier` line would ship untested and the first real barrier would be
    // the first thing to run it.
    //
    // Structured-first is why this is possible at all: the inspection types are plain
    // aggregates, so a test can construct the report the graph will one day compute.
    Monarc::Render::PassInspection passes[2] = {};
    passes[0].name           = "Depth";
    passes[0].index          = 0;
    passes[0].executionOrder = 0;
    passes[0].hasRecord      = true;
    passes[1].name           = "Culled";
    passes[1].index          = 1;
    passes[1].executionOrder = kNoPass;
    passes[1].culled         = true;

    Monarc::Render::ResourceInspection resources[1] = {};
    resources[0].name        = "Depth";
    resources[0].id          = TextureId::ForTesting(0, 3);
    resources[0].origin      = ResourceOrigin::Transient;
    resources[0].description = kSwapchainDescription;
    resources[0].lifetime    = ResourceLifetime{0, 1};
    resources[0].aliasGroup  = 2;

    Monarc::Render::DerivedBarrier barriers[1] = {};
    barriers[0].resource          = TextureId::ForTesting(0, 3);
    barriers[0].emittedBeforePass = kNoPass;
    barriers[0].layoutBefore      = TextureLayout::ColorAttachment;
    barriers[0].layoutAfter       = TextureLayout::PresentSource;
    barriers[0].syncBefore        = PipelineStage::ColorAttachmentOutput;
    barriers[0].syncAfter         = PipelineStage::None;
    barriers[0].accessBefore      = Access::ColorAttachmentWrite;
    barriers[0].accessAfter       = Access::None;
    barriers[0].cause.before      = Monarc::Render::BarrierCauseSide{
        BarrierCauseKind::PassAccess, 0, ResourceAccess::ColorAttachmentWrite};
    barriers[0].cause.after = Monarc::Render::BarrierCauseSide{
        BarrierCauseKind::ImportOutgoing, kNoPass, ResourceAccess::ColorAttachmentRead};

    GraphInspection inspection{};
    inspection.phase              = GraphPhase::Compiled;
    inspection.buildGeneration    = 3;
    inspection.passes             = passes;
    inspection.resources          = resources;
    inspection.barriers           = barriers;
    inspection.diagnosticsDropped = 5;

    char buffer[2048] = {};
    CHECK(Render(inspection, buffer) ==
          "graph build=3 phase=Compiled\n"
          "counts passes=2 resources=1 accesses=0 barriers=1 diagnostics=0 dropped=5\n"
          "pass 0 order=0 queue=Graphics culled=no record=yes name=\"Depth\"\n"
          "pass 1 order=none queue=Graphics culled=yes record=no name=\"Culled\"\n"
          "resource 0 id=0:3 origin=Transient format=B8G8R8A8_UNORM extent=1280x720 usage=0x1 "
          "lifetime=0..1 alias=2 name=\"Depth\"\n"
          "barrier 0 resource=0:3 before-pass=none layout=ColorAttachment->PresentSource "
          "sync=ColorAttachmentOutput(0x20)->None(0x0) "
          "access=ColorAttachmentWrite(0x100)->None(0x0) "
          "cause=PassAccess:0:ColorAttachmentWrite->ImportOutgoing:none:ColorAttachmentRead\n");
}

TEST_CASE("the widest ids and sentinels render without being clipped") {
    // **Pins the fixed buffer `WriteInspectionText`'s per-field helper uses.** Every field it
    // renders is at most two `u32`s in decimal with a separator between them -- 21 characters
    // -- and the helper's buffer is 24 including the terminator. Nothing else in this file
    // reaches that width: the FirstLight frame's ids are `0:0` and `4:1`. A clipped field
    // would not be an error anywhere, just a shorter number, so a case that renders the
    // widest possible values is the only thing that would notice.
    //
    // 4294967294 rather than 4294967295: the latter is `ResourceId`'s invalid index, which
    // renders as `none` and would test the other branch.
    constexpr Monarc::u32 kWidest = 4294967294U;

    Monarc::Render::PassInspection passes[1] = {};
    passes[0].name           = "Pass";
    passes[0].index          = kWidest;
    passes[0].executionOrder = kWidest;

    Monarc::Render::ResourceInspection resources[1] = {};
    resources[0].name       = "Resource";
    resources[0].id         = TextureId::ForTesting(kWidest, kWidest);
    resources[0].lifetime   = ResourceLifetime{kWidest, kWidest};
    resources[0].aliasGroup = kWidest;

    GraphInspection inspection{};
    inspection.buildGeneration = kWidest;
    inspection.passes          = passes;
    inspection.resources       = resources;

    char                   buffer[2048] = {};
    const std::string_view text         = Render(inspection, buffer);
    CAPTURE(text);
    CHECK(text.find("pass 4294967294 order=4294967294 ") != std::string_view::npos);
    CHECK(text.find("id=4294967294:4294967294 ") != std::string_view::npos);
    CHECK(text.find("lifetime=4294967294..4294967294 alias=4294967294 ") !=
          std::string_view::npos);
}

TEST_CASE("truncation is reported, and the buffer is filled to its end") {
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});
    DeclareFirstLightFrame(graph);

    char full[1024] = {};
    const InspectionText whole = WriteInspectionText(graph.Inspect(), full);
    REQUIRE_FALSE(whole.Truncated());
    REQUIRE(whole.written == whole.needed);
    REQUIRE(whole.needed > 40u);

    // A buffer too small by design. The report is clipped, `needed` still says how much there
    // was, and what did fit is the *start* of the full report -- a clip that had scrambled the
    // output would make the truncation flag useless.
    char            small[40] = {};
    std::span<char> smallSpan(small);
    const InspectionText clipped = WriteInspectionText(graph.Inspect(), smallSpan);
    CHECK(clipped.Truncated());
    CHECK(clipped.written == smallSpan.size());
    CHECK(clipped.needed == whole.needed);
    CHECK(std::string_view(small, clipped.written) ==
          std::string_view(full, clipped.written));
}

TEST_CASE("a zero-length buffer counts without writing") {
    // The "how much room do I need" call, and the one that would form a pointer from an empty
    // span's null `data()` if `WriteInspectionText` were written carelessly. Under the two
    // sanitizer presets this case is what would catch that.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});
    DeclareFirstLightFrame(graph);

    const InspectionText counted = WriteInspectionText(graph.Inspect(), std::span<char>{});
    CHECK(counted.written == 0u);
    CHECK(counted.needed > 0u);
    CHECK(counted.Truncated());

    char                 full[1024] = {};
    const InspectionText whole      = WriteInspectionText(graph.Inspect(), full);
    CHECK(counted.needed == whole.needed);
}

TEST_CASE("a whole report that exactly fits is not reported as truncated") {
    // The boundary. `Truncated()` is `needed > written`, so an off-by-one in either -- writing
    // one character short, or counting one too many -- shows up only here.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});
    DeclareFirstLightFrame(graph);

    const InspectionText counted = WriteInspectionText(graph.Inspect(), std::span<char>{});
    REQUIRE(counted.needed > 0u);
    REQUIRE(counted.needed < 1024u);

    char                 buffer[1024] = {};
    std::span<char>      exact(buffer, counted.needed);
    const InspectionText fitted = WriteInspectionText(graph.Inspect(), exact);
    CHECK_FALSE(fitted.Truncated());
    CHECK(fitted.written == counted.needed);
    CHECK(fitted.needed == counted.needed);

    std::span<char>      oneShort(buffer, counted.needed - 1);
    const InspectionText clipped = WriteInspectionText(graph.Inspect(), oneShort);
    CHECK(clipped.Truncated());
    CHECK(clipped.written == counted.needed - 1);
}

// A lifetime that names no pass is empty, whichever end is missing. `IsEmpty` reads only
// `firstPass`, which is right -- a resource with a first write always has a last use -- and
// this is where that premise is written down rather than assumed.
static_assert(ResourceLifetime{}.IsEmpty());
static_assert(ResourceLifetime{kNoPass, kNoPass}.IsEmpty());
static_assert(!ResourceLifetime{0, 0}.IsEmpty());
static_assert(!ResourceLifetime{3, 7}.IsEmpty());

// The two sentinels are deliberately named separately -- one is a pass index and one is a
// group number -- and there is no assertion that they are equal. Pinning that would forbid a
// change either is entitled to make on its own, which is the opposite of what naming them
// separately was for.

// Every inspection type is a plain aggregate a test can build by hand, which is what the
// hand-built case above depends on and what "structured first, text second" means in practice.
static_assert(std::is_trivially_copyable_v<Monarc::Render::PassInspection>);
static_assert(std::is_trivially_copyable_v<Monarc::Render::ResourceInspection>);
static_assert(std::is_trivially_copyable_v<Monarc::Render::AccessInspection>);
static_assert(std::is_trivially_copyable_v<Monarc::Render::DerivedBarrier>);
static_assert(std::is_trivially_copyable_v<Monarc::Render::GraphDiagnostic>);
static_assert(std::is_aggregate_v<Monarc::Render::DerivedBarrier>);
static_assert(std::is_aggregate_v<Monarc::Render::GraphInspection>);
