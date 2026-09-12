#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Render/RenderGraph.h>

#include <iterator>

// The barrier derivation: the transitions the declared reads and writes imply.
//
// **Device-free, and that is the point of the phase rather than a convenience.** Every case
// below declares a frame, compiles it and asserts on `GraphInspection::barriers` field by field.
// There is no GPU, no Vulkan driver and no display anywhere in this file -- the rule the phase
// plan states outright, that "a barrier regression should fail a unit test, not a screenshot
// comparison".
//
// **The model the cases assert against is in Private/DeriveBarriers.cpp**, and the short version
// is: a resource passes through a chain of states -- its import's declared incoming state or its
// creation, then one state per surviving pass that touches it, then its import's declared
// outgoing state -- and each gap becomes a barrier unless the two states are identical and
// neither side is a pass that writes.
//
// **Every case here asserts on the two counts that fail differently.** An unnecessary barrier is
// a performance bug and a dropped necessary one is a correctness bug, so a case that only
// counted barriers could not tell them apart. The shape used throughout is: assert the total,
// then assert each barrier the frame should have field by field, so that an extra one fails the
// total and a missing one fails a field.

using Monarc::ErrorCode;
using Monarc::Result;
using Monarc::Status;
using Monarc::SystemAllocator;
using Monarc::Render::BarrierCause;
using Monarc::Render::BarrierCauseKind;
using Monarc::Render::BarrierCauseSide;
using Monarc::Render::DerivedBarrier;
using Monarc::Render::DiagnosticKind;
using Monarc::Render::GraphInspection;
using Monarc::Render::GraphPhase;
using Monarc::Render::IsWrite;
using Monarc::Render::kNoPass;
using Monarc::Render::PassBuilder;
using Monarc::Render::RenderGraph;
using Monarc::Render::ResourceAccess;
using Monarc::Render::RequirementOf;
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

constexpr TextureDescription kSwapchainDescription{
    Monarc::RHI::Extent2D{1280, 720}, Format::B8G8R8A8_UNORM, TextureUsage::ColorAttachment};

/// The three stages `RequirementOf` gives every shader access, spelled once.
constexpr PipelineStage kShaderStages =
    PipelineStage::VertexShader | PipelineStage::FragmentShader | PipelineStage::ComputeShader;

/// The import `Monarc.FirstLight` will declare for the swapchain image it clears and presents.
///
/// **Every one of the six values below is a statement about the world outside the graph, and
/// five of them are forced rather than chosen.** The sixth is argued at the headline case.
///
/// - `incoming.layout = Undefined`: `vkAcquireNextImageKHR` does not preserve the contents of
///   the image it hands back, so the only honest layout for it is the one that says the contents
///   are not defined. `RHI::TextureLayout::Undefined`'s own comment says a transition out of it
///   discards them, which is exactly right for an image about to be cleared on load.
/// - `incoming.access = None`: nothing this queue performed needs making available. The image's
///   previous contents are being discarded.
/// - `outgoing.layout = PresentSource`: `ISwapchain::Present` requires it --
///   `VUID-VkPresentInfoKHR-pImageIndices-01430`, recorded at `TextureLayout::PresentSource`.
/// - `outgoing.stage = None` and `outgoing.access = None`: there is no *command* after the
///   transition. What reads the image next is the presentation engine, by way of the
///   render-finished semaphore, which `SubmitList` signals at `ALL_COMMANDS` -- so the
///   dependency that covers it is a semaphore rather than a barrier scope.
[[nodiscard]] TextureImport SwapchainImport(Monarc::u32 index = 4) {
    return TextureImport(
        TextureHandle::ForTesting(index, 1), kSwapchainDescription,
        TextureState{TextureLayout::Undefined, PipelineStage::ColorAttachmentOutput, Access::None},
        TextureState{TextureLayout::PresentSource, PipelineStage::None, Access::None});
}

/// An import whose two ends are chosen to frame a case rather than to model a swapchain.
///
/// The states differ from every access requirement below, so the two framing barriers are always
/// emitted -- which is what lets a case assert that a *middle* gap produced nothing without that
/// assertion also passing for a derivation that produced nothing at all.
[[nodiscard]] TextureImport FramedImport(Monarc::u32 index) {
    return TextureImport(
        TextureHandle::ForTesting(index, 1), kSwapchainDescription,
        TextureState{TextureLayout::TransferSource, PipelineStage::Copy, Access::TransferRead},
        TextureState{TextureLayout::PresentSource, PipelineStage::None, Access::None});
}

/// Adds a pass that imports its own anchor and writes it, so that culling keeps it.
///
/// TestCull.cpp's rule: a graph that imports nothing is culled entirely, and a culled pass
/// contributes no barrier -- so a case about a transient needs something outside the graph to
/// consume, or it would be asserting about a frame that does not run. Each anchor is a separate
/// imported resource with its own barriers, which is why every case below asserts on the
/// barriers naming the resource it is about rather than on the whole list.
[[nodiscard]] PassBuilder AnchoredPass(RenderGraph& graph, const char* name,
                                       Monarc::u32 anchorIndex) {
    Result<PassBuilder> pass = graph.AddPass(name);
    REQUIRE(pass.has_value());
    const Result<TextureId> anchor = pass->ImportTexture("Anchor", SwapchainImport(anchorIndex));
    REQUIRE(anchor.has_value());
    REQUIRE(pass->Write(*anchor, ResourceAccess::ColorAttachmentWrite));
    return *pass;
}

/// How many of `barriers` name `resource`.
[[nodiscard]] Monarc::usize CountFor(const GraphInspection& inspection, TextureId resource) {
    Monarc::usize count = 0;
    for (const DerivedBarrier& barrier : inspection.barriers) {
        count += barrier.resource == resource ? 1u : 0u;
    }
    return count;
}

/// The `index`-th barrier naming `resource`, in the order the frame records them.
///
/// Fails the case rather than returning a default when there is no such barrier, so that a
/// derivation which dropped one fails at the assertion about the barrier rather than silently
/// comparing against a zeroed value that happens to differ.
[[nodiscard]] const DerivedBarrier& BarrierFor(const GraphInspection& inspection,
                                               TextureId resource, Monarc::usize index) {
    Monarc::usize seen = 0;
    for (const DerivedBarrier& barrier : inspection.barriers) {
        if (barrier.resource != resource) {
            continue;
        }
        if (seen == index) {
            return barrier;
        }
        ++seen;
    }
    REQUIRE_MESSAGE(false, "no such barrier for that resource");
    return inspection.barriers[0];
}

/// A cause side naming a pass's access.
[[nodiscard]] constexpr BarrierCauseSide ByPass(Monarc::u32 order, ResourceAccess access) {
    return BarrierCauseSide{BarrierCauseKind::PassAccess, order, access};
}

/// A cause side that is one end of a chain: an import's two declared states, or a transient's
/// creation. `access` is filler for all three -- `BarrierCauseSide::access` says so -- so it
/// stays at its default here rather than being given a value a reader might take for real.
[[nodiscard]] constexpr BarrierCauseSide ByEnd(BarrierCauseKind kind) {
    return BarrierCauseSide{kind, kNoPass, ResourceAccess::ColorAttachmentRead};
}

// ---------------------------------------------------------------------------------------
// A3's measured barriers, decoded from the captures.
//
// **The reference is Build/Captures/monarc-firstlight-nvidia-rtx3070ti_frame345.xml and
// Build/Captures/monarc-firstlight-intel-uhd730_frame331.xml**, the two RenderDoc XML dumps
// Phase A3 left behind of the hand-written frame. Both contain exactly two
// `vkCmdPipelineBarrier2` chunks, each with one `VkImageMemoryBarrier2`, and **the two adapters'
// values are identical** -- decoded field by field from both files rather than from one.
//
// The raw numbers are in the XML beside the spellings, and are repeated here so that the mapping
// from a Vulkan enumerator to an `RHI` one is visible rather than assumed:
//
//   barrier 0 (before rendering)          barrier 1 (to present)
//   oldLayout      0  UNDEFINED           oldLayout      2          COLOR_ATTACHMENT_OPTIMAL
//   newLayout      2  COLOR_ATTACHMENT..  newLayout      1000001002 PRESENT_SRC_KHR
//   srcStageMask   1024 COLOR_ATT_OUTPUT  srcStageMask   1024       COLOR_ATTACHMENT_OUTPUT
//   dstStageMask   1024 COLOR_ATT_OUTPUT  dstStageMask   0          NONE
//   srcAccessMask  0  NONE                srcAccessMask  256        COLOR_ATTACHMENT_WRITE
//   dstAccessMask  256 COLOR_ATT_WRITE    dstAccessMask  0          NONE
//
// **These constants are the capture, not the derivation**, which is the whole point of naming
// them: the headline case compares what the graph produced against values transcribed from a
// file, so a change to the derivation that also changed the expectation would have to change
// this table and stop matching the citation above it.
// ---------------------------------------------------------------------------------------

constexpr TextureLayout kCapturedFirstOldLayout  = TextureLayout::Undefined;
constexpr TextureLayout kCapturedFirstNewLayout  = TextureLayout::ColorAttachment;
constexpr PipelineStage kCapturedFirstSrcStage   = PipelineStage::ColorAttachmentOutput;
constexpr PipelineStage kCapturedFirstDstStage   = PipelineStage::ColorAttachmentOutput;
constexpr Access        kCapturedFirstSrcAccess  = Access::None;
constexpr Access        kCapturedFirstDstAccess  = Access::ColorAttachmentWrite;

// ---------------------------------------------------------------------------------------
// The premise the emission rule's one-sidedness rests on, pinned where it is relied on.
//
// Private/DeriveBarriers.cpp reads `passWrites` of the step *after* a gap and not of the step
// before it, and argues that between two passes the two cannot disagree while their states are
// equal: a pass step's access set is the union of `RequirementOf`'s access bits, and the write
// bits come only from accesses that write -- so two pass steps with equal access sets either both
// wrote or neither did.
//
// That holds because no read access and no write access share an access bit, which is what the
// assertions below say. They are `static_assert`s because `RequirementOf` is `constexpr`, and
// they are here rather than in TestAccess.cpp because it is this file's rule that needs them:
// giving `ColorAttachmentRead` the `ColorAttachmentWrite` bit would leave every row of
// TestAccess.cpp's map to be changed to match, and would make the one-sided rule wrong.
// ---------------------------------------------------------------------------------------

constexpr ResourceAccess kAllAccesses[] = {
    ResourceAccess::ColorAttachmentRead,  ResourceAccess::ColorAttachmentWrite,
    ResourceAccess::DepthStencilAttachmentRead, ResourceAccess::DepthStencilAttachmentWrite,
    ResourceAccess::SampledRead,          ResourceAccess::StorageRead,
    ResourceAccess::StorageWrite,         ResourceAccess::IndirectRead,
};

/// TestAccess.cpp's completeness mechanism, repeated because this file's list is its own: a
/// `default`-less switch makes a new enumerator a compile error here, and the assertion that the
/// value one past the list's end is not an enumerator then fails until the list grows to match.
[[nodiscard]] constexpr bool IsEnumerator(ResourceAccess access) {
    switch (access) {
        case ResourceAccess::ColorAttachmentRead:
        case ResourceAccess::ColorAttachmentWrite:
        case ResourceAccess::DepthStencilAttachmentRead:
        case ResourceAccess::DepthStencilAttachmentWrite:
        case ResourceAccess::SampledRead:
        case ResourceAccess::StorageRead:
        case ResourceAccess::StorageWrite:
        case ResourceAccess::IndirectRead:
            return true;
    }
    return false;
}

static_assert(!IsEnumerator(static_cast<ResourceAccess>(std::size(kAllAccesses))),
              "kAllAccesses is missing a ResourceAccess enumerator");

/// The union of the access bits every access on one side of the read/write split contributes.
///
/// Derived from `IsWrite` rather than from a second hand-written list, so that a new access is
/// classified by the same function the derivation uses.
[[nodiscard]] constexpr Access BitsOfAccessesThat(bool write) {
    Access bits = Access::None;
    for (const ResourceAccess access : kAllAccesses) {
        if (Monarc::Render::IsWrite(access) == write) {
            bits |= RequirementOf(access).scope.access;
        }
    }
    return bits;
}

static_assert(!Monarc::RHI::HasAny(BitsOfAccessesThat(false), BitsOfAccessesThat(true)),
              "a read access shares an access bit with a write access, which makes the barrier "
              "derivation's one-sided emission rule wrong -- see Private/DeriveBarriers.cpp");
static_assert(BitsOfAccessesThat(true) != Access::None,
              "no access writes, so the assertion above is vacuous");

constexpr TextureLayout kCapturedSecondOldLayout = TextureLayout::ColorAttachment;
constexpr TextureLayout kCapturedSecondNewLayout = TextureLayout::PresentSource;
constexpr PipelineStage kCapturedSecondSrcStage  = PipelineStage::ColorAttachmentOutput;
constexpr PipelineStage kCapturedSecondDstStage  = PipelineStage::None;
constexpr Access        kCapturedSecondSrcAccess = Access::ColorAttachmentWrite;
constexpr Access        kCapturedSecondDstAccess = Access::None;

}  // namespace

TEST_CASE("the FirstLight frame derives exactly A3's two barriers") {
    // **The phase's headline, and the reference is measured rather than plausible.** The
    // declaration below is the one `Monarc.FirstLight` will make once Task 4 replaces its
    // hand-written frame: one pass, one imported swapchain image, incoming `Undefined`, outgoing
    // `PresentSource`, cleared on load -- so the pass declares one `ColorAttachmentWrite` and
    // records nothing, because a `LoadOp::Clear` is an attachment property rather than a
    // command.
    //
    // **What is asserted is equivalence with Build/Captures/*.xml**, whose two
    // `vkCmdPipelineBarrier2` calls are transcribed into the `kCaptured*` constants above, with
    // the file names and the raw Vulkan numbers beside them.
    //
    // **Six of the twelve values come from the declaration and six from `RequirementOf`, and
    // reading this case as equivalence depends on knowing which.** The six that the declaration
    // controls are the import's two states; the six that it does not are
    // `RequirementOf(ColorAttachmentWrite)`'s layout, stage and access appearing on the after
    // side of the first barrier and the before side of the second, which Access.h pins with
    // `static_assert`s and no declaration can move. Of the six declared values, five are forced
    // by the swapchain contract -- `SwapchainImport` above gives the citation for each -- and
    // exactly one is a choice: `incoming.stage`. *"the derived barriers are a function of the
    // declared import states"* below is the case that shows it is a choice rather than a
    // constant, so that this one cannot be read as a test that would pass whatever was declared.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Clear");
    REQUIRE(pass.has_value());
    const Result<TextureId> image = pass->ImportTexture("Swapchain", SwapchainImport());
    REQUIRE(image.has_value());
    REQUIRE(pass->Write(*image, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.barriers.size() == 2u);

    const DerivedBarrier& first = inspection.barriers[0];
    CHECK(first.resource == *image);
    CHECK(first.layoutBefore == kCapturedFirstOldLayout);
    CHECK(first.layoutAfter == kCapturedFirstNewLayout);
    CHECK(first.syncBefore == kCapturedFirstSrcStage);
    CHECK(first.syncAfter == kCapturedFirstDstStage);
    CHECK(first.accessBefore == kCapturedFirstSrcAccess);
    CHECK(first.accessAfter == kCapturedFirstDstAccess);
    // Recorded in front of the one pass that runs, which is the placement A3's capture shows:
    // the barrier chunk sits between `vkBeginCommandBuffer` and `vkCmdBeginRendering`.
    CHECK(first.emittedBeforePass == 0u);
    // And the derivation says *why*, which is what makes a wrong barrier debuggable as a
    // derivation rather than as archaeology.
    CHECK(first.cause ==
          BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                       ByPass(0, ResourceAccess::ColorAttachmentWrite)});

    const DerivedBarrier& second = inspection.barriers[1];
    CHECK(second.resource == *image);
    CHECK(second.layoutBefore == kCapturedSecondOldLayout);
    CHECK(second.layoutAfter == kCapturedSecondNewLayout);
    CHECK(second.syncBefore == kCapturedSecondSrcStage);
    CHECK(second.syncAfter == kCapturedSecondDstStage);
    CHECK(second.accessBefore == kCapturedSecondSrcAccess);
    CHECK(second.accessAfter == kCapturedSecondDstAccess);
    // After every pass, which is where the capture's second chunk is: after
    // `vkCmdEndRendering` and before `vkEndCommandBuffer`.
    CHECK(second.emittedBeforePass == kNoPass);
    CHECK(second.cause == BarrierCause{ByPass(0, ResourceAccess::ColorAttachmentWrite),
                                       ByEnd(BarrierCauseKind::ImportOutgoing)});
}

TEST_CASE("the derived barriers are a function of the declared import states") {
    // **The case that stops the headline above from being readable as a tuned match.** An
    // equivalence test whose expected values were adjusted until they matched asserts nothing;
    // this is the instrument that says which values the declaration controls, by declaring
    // something else and showing the derivation follow.
    //
    // The frame is the headline's, with exactly one value changed: the import's incoming *stage*
    // becomes `None` -- the naive reading of "an acquired image, nothing has touched it". The
    // derivation then produces `syncBefore = None` on the opening barrier, where A3's capture has
    // `COLOR_ATTACHMENT_OUTPUT`. Everything else is unchanged, which is what pins that this one
    // field and no other rides on that half of the declaration.
    //
    // **The declaration the headline uses is the honest one, and this is the evidence rather than
    // the counterexample.** `VulkanDeviceState::SubmitList` waits on the acquire semaphore at
    // `VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT`, and a layout transition is a write that
    // has to be ordered after that wait -- so the transition's first synchronisation scope has to
    // include the stage the semaphore was waited at. A `None` first scope is an empty one, which
    // orders the transition after nothing. That is why `incoming.stage` is
    // `ColorAttachmentOutput` in `SwapchainImport` above: not to match the capture, but because
    // it is what the submission does.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Clear");
    REQUIRE(pass.has_value());
    const Result<TextureId> image = pass->ImportTexture(
        "Swapchain",
        TextureImport(
            TextureHandle::ForTesting(4, 1), kSwapchainDescription,
            TextureState{TextureLayout::Undefined, PipelineStage::None, Access::None},
            TextureState{TextureLayout::PresentSource, PipelineStage::None, Access::None}));
    REQUIRE(image.has_value());
    REQUIRE(pass->Write(*image, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.barriers.size() == 2u);
    CHECK(inspection.barriers[0].syncBefore == PipelineStage::None);
    CHECK(inspection.barriers[0].syncBefore != kCapturedFirstSrcStage);
    // The five values the declaration did not change are unchanged, so this case says "one
    // field followed one declaration" rather than "the output moved".
    CHECK(inspection.barriers[0].layoutBefore == kCapturedFirstOldLayout);
    CHECK(inspection.barriers[0].layoutAfter == kCapturedFirstNewLayout);
    CHECK(inspection.barriers[0].syncAfter == kCapturedFirstDstStage);
    CHECK(inspection.barriers[0].accessBefore == kCapturedFirstSrcAccess);
    CHECK(inspection.barriers[0].accessAfter == kCapturedFirstDstAccess);
    // And the closing barrier, which the changed field is not on either side of, still matches
    // the capture in all six.
    CHECK(inspection.barriers[1].layoutBefore == kCapturedSecondOldLayout);
    CHECK(inspection.barriers[1].layoutAfter == kCapturedSecondNewLayout);
    CHECK(inspection.barriers[1].syncBefore == kCapturedSecondSrcStage);
    CHECK(inspection.barriers[1].syncAfter == kCapturedSecondDstStage);
    CHECK(inspection.barriers[1].accessBefore == kCapturedSecondSrcAccess);
    CHECK(inspection.barriers[1].accessAfter == kCapturedSecondDstAccess);
}

TEST_CASE("write-after-write yields one barrier, even though the two states are equal") {
    // **The case that decides the emission rule, because it is the one a rule written on state
    // equality alone gets wrong.** Two passes writing one colour attachment with the same access
    // need it in the same layout with the same scopes, so the state before and the state after
    // are byte-identical -- and the second write still has to be ordered after the first and see
    // its result. A derivation that suppressed a barrier whenever the two states matched would
    // drop exactly this one, and would still pass every read-after-read case in this file.
    //
    // **Two of them, with different accesses**, so that an answer hardcoded for the colour
    // attachment fails: `Slow` is written twice as a colour attachment and `Fast` twice as a
    // storage image, whose layout is `General` and whose stages are the three shader ones.
    //
    // Two passes that write an imported resource and do not read it is the shape that reaches
    // the derivation: both survive culling, because a pass writing an import is never culled,
    // and nothing reads either resource so `DiagnosticKind::UnorderedOverwrite` does not fire --
    // that refusal is about a *read* whose writer is undetermined, and there is no read here.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> firstPass = graph.AddPass("First");
    REQUIRE(firstPass.has_value());
    const Result<TextureId> slow = firstPass->ImportTexture("Slow", SwapchainImport(1));
    REQUIRE(slow.has_value());
    const Result<TextureId> fast = firstPass->ImportTexture("Fast", SwapchainImport(2));
    REQUIRE(fast.has_value());
    REQUIRE(firstPass->Write(*slow, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(firstPass->Write(*fast, ResourceAccess::StorageWrite));

    Result<PassBuilder> secondPass = graph.AddPass("Second");
    REQUIRE(secondPass.has_value());
    REQUIRE(secondPass->Write(*slow, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(secondPass->Write(*fast, ResourceAccess::StorageWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // Three per resource: the import's opening transition, the write-after-write, and the
    // import's closing transition. Six in total, and a seventh would be an unnecessary barrier.
    REQUIRE(inspection.barriers.size() == 6u);
    REQUIRE(CountFor(inspection, *slow) == 3u);
    REQUIRE(CountFor(inspection, *fast) == 3u);

    const DerivedBarrier& slowWaw = BarrierFor(inspection, *slow, 1);
    CHECK(slowWaw.layoutBefore == TextureLayout::ColorAttachment);
    CHECK(slowWaw.layoutAfter == TextureLayout::ColorAttachment);
    CHECK(slowWaw.syncBefore == PipelineStage::ColorAttachmentOutput);
    CHECK(slowWaw.syncAfter == PipelineStage::ColorAttachmentOutput);
    CHECK(slowWaw.accessBefore == Access::ColorAttachmentWrite);
    CHECK(slowWaw.accessAfter == Access::ColorAttachmentWrite);
    CHECK(slowWaw.emittedBeforePass == 1u);
    CHECK(slowWaw.cause == BarrierCause{ByPass(0, ResourceAccess::ColorAttachmentWrite),
                                        ByPass(1, ResourceAccess::ColorAttachmentWrite)});

    const DerivedBarrier& fastWaw = BarrierFor(inspection, *fast, 1);
    CHECK(fastWaw.layoutBefore == TextureLayout::General);
    CHECK(fastWaw.layoutAfter == TextureLayout::General);
    CHECK(fastWaw.syncBefore == kShaderStages);
    CHECK(fastWaw.syncAfter == kShaderStages);
    CHECK(fastWaw.accessBefore == Access::ShaderStorageWrite);
    CHECK(fastWaw.accessAfter == Access::ShaderStorageWrite);
    CHECK(fastWaw.emittedBeforePass == 1u);
    CHECK(fastWaw.cause == BarrierCause{ByPass(0, ResourceAccess::StorageWrite),
                                        ByPass(1, ResourceAccess::StorageWrite)});
}

TEST_CASE("read-after-read yields no barrier between the readers") {
    // **The other half of the rule the write-after-write case decides.** Two passes reading one
    // resource the same way have no hazard between them and no layout to change, so the gap
    // between them is a transition that is not a transition and must not be emitted -- an
    // unnecessary barrier is a performance bug.
    //
    // **The imports are framed rather than swapchain-shaped, and that is what makes this case
    // able to fail in both directions.** `FramedImport`'s declared states differ from every read
    // requirement, so the opening and closing transitions are always emitted: two barriers per
    // resource. A derivation that emitted the middle gap too would give three, and one that
    // derived nothing at all would give zero -- so "no barrier between the readers" is asserted
    // as a number that is neither.
    //
    // Two of them, read two different ways, so a rule that happened to hold for the sampled case
    // alone fails.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             firstPass = AnchoredPass(graph, "First", 10);
    const Result<TextureId> sampled   = firstPass.ImportTexture("Sampled", FramedImport(1));
    REQUIRE(sampled.has_value());
    const Result<TextureId> stored = firstPass.ImportTexture("Stored", FramedImport(2));
    REQUIRE(stored.has_value());
    REQUIRE(firstPass.Read(*sampled, ResourceAccess::SampledRead));
    REQUIRE(firstPass.Read(*stored, ResourceAccess::StorageRead));

    PassBuilder secondPass = AnchoredPass(graph, "Second", 11);
    REQUIRE(secondPass.Read(*sampled, ResourceAccess::SampledRead));
    REQUIRE(secondPass.Read(*stored, ResourceAccess::StorageRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    CHECK(CountFor(inspection, *sampled) == 2u);
    CHECK(CountFor(inspection, *stored) == 2u);

    // The two that are there are the chain's two ends, and neither is emitted in front of the
    // second reader -- which is where the barrier a wrong rule would add would go.
    CHECK(BarrierFor(inspection, *sampled, 0).emittedBeforePass == 0u);
    CHECK(BarrierFor(inspection, *sampled, 0).cause ==
          BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                       ByPass(0, ResourceAccess::SampledRead)});
    CHECK(BarrierFor(inspection, *sampled, 1).emittedBeforePass == kNoPass);
    CHECK(BarrierFor(inspection, *sampled, 1).cause ==
          BarrierCause{ByPass(1, ResourceAccess::SampledRead),
                       ByEnd(BarrierCauseKind::ImportOutgoing)});

    CHECK(BarrierFor(inspection, *stored, 0).emittedBeforePass == 0u);
    CHECK(BarrierFor(inspection, *stored, 1).emittedBeforePass == kNoPass);
    // The closing one's before side is the *second* reader's access, not the first's, which is
    // what says the chain advanced through both readers rather than skipping one.
    CHECK(BarrierFor(inspection, *stored, 1).cause ==
          BarrierCause{ByPass(1, ResourceAccess::StorageRead),
                       ByEnd(BarrierCauseKind::ImportOutgoing)});
}

TEST_CASE("a gap that changes only the synchronisation scope is still a transition") {
    // **A layout change is not the only thing a barrier is for**, and this is the case that says
    // so: both gaps below leave the resource in the layout it was already in and differ only in
    // which stages and accesses are named. A derivation that tested the layouts instead of the
    // whole state would emit nothing for either, and every other case in this file would stay
    // green -- which is how the gap was found, by mutating the rule to compare layouts.
    //
    // The import declares itself already in `ShaderReadOnly`, visible to the fragment shader
    // only; the pass samples it, which `RequirementOf` scopes to all three shader stages. The
    // vertex and compute reads are not covered by whatever made the fragment read visible, so
    // the barrier that widens the scope is a real one. The outgoing state narrows it back, which
    // is the same shape at the other end of the chain.
    //
    // **An import's declared state is the only thing that produces this shape today**, which is
    // why the case is built out of one rather than out of two reading passes: every read a
    // `TextureId` can carry names a layout of its own, so two consecutive *pass* reads either
    // share an access and a state entirely or need a layout transition anyway.
    // Private/DeriveBarriers.cpp works that through where it explains why there is no run of
    // reads to merge.
    //
    // Two of them, in two different layouts.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             pass    = AnchoredPass(graph, "Pass", 10);
    const Result<TextureId> sampled = pass.ImportTexture(
        "Sampled",
        TextureImport(TextureHandle::ForTesting(1, 1), kSwapchainDescription,
                      TextureState{TextureLayout::ShaderReadOnly, PipelineStage::FragmentShader,
                                   Access::ShaderSampledRead},
                      TextureState{TextureLayout::ShaderReadOnly, PipelineStage::FragmentShader,
                                   Access::ShaderSampledRead}));
    REQUIRE(sampled.has_value());
    const Result<TextureId> stored = pass.ImportTexture(
        "Stored",
        TextureImport(TextureHandle::ForTesting(2, 1), kSwapchainDescription,
                      TextureState{TextureLayout::General, PipelineStage::ComputeShader,
                                   Access::ShaderStorageRead},
                      TextureState{TextureLayout::General, PipelineStage::ComputeShader,
                                   Access::ShaderStorageRead}));
    REQUIRE(stored.has_value());
    REQUIRE(pass.Read(*sampled, ResourceAccess::SampledRead));
    REQUIRE(pass.Read(*stored, ResourceAccess::StorageRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(CountFor(inspection, *sampled) == 2u);
    REQUIRE(CountFor(inspection, *stored) == 2u);

    const DerivedBarrier& widen = BarrierFor(inspection, *sampled, 0);
    CHECK(widen.layoutBefore == widen.layoutAfter);
    CHECK(widen.layoutBefore == TextureLayout::ShaderReadOnly);
    CHECK(widen.syncBefore == PipelineStage::FragmentShader);
    CHECK(widen.syncAfter == kShaderStages);
    CHECK(widen.accessBefore == Access::ShaderSampledRead);
    CHECK(widen.accessAfter == Access::ShaderSampledRead);
    CHECK(widen.cause == BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                                      ByPass(0, ResourceAccess::SampledRead)});

    const DerivedBarrier& narrow = BarrierFor(inspection, *sampled, 1);
    CHECK(narrow.layoutBefore == narrow.layoutAfter);
    CHECK(narrow.syncBefore == kShaderStages);
    CHECK(narrow.syncAfter == PipelineStage::FragmentShader);
    CHECK(narrow.emittedBeforePass == kNoPass);

    // And the same two gaps in `General`, so the answer is not one layout's accident.
    CHECK(BarrierFor(inspection, *stored, 0).layoutBefore == TextureLayout::General);
    CHECK(BarrierFor(inspection, *stored, 0).layoutAfter == TextureLayout::General);
    CHECK(BarrierFor(inspection, *stored, 0).syncBefore == PipelineStage::ComputeShader);
    CHECK(BarrierFor(inspection, *stored, 0).syncAfter == kShaderStages);
    CHECK(BarrierFor(inspection, *stored, 1).syncAfter == PipelineStage::ComputeShader);
}

TEST_CASE("read-after-write yields one barrier in the write-then-read direction") {
    // **Direction is the whole assertion**, because a derivation that swapped the two sides
    // would emit the same number of barriers with the same six values in the wrong places, and a
    // case that counted would not notice. Every field is checked against the side it belongs to:
    // the write's layout, stage and access on the *before* side, the read's on the *after* side.
    //
    // Two of them, with different read accesses and therefore different after-states: a colour
    // attachment written and then sampled, and one written and then read as a storage image. A
    // swap in either would put `ShaderReadOnly` or `General` where `ColorAttachment` belongs.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             producer = AnchoredPass(graph, "Producer", 10);
    const Result<TextureId> sampled  = producer.CreateTexture("Sampled", kSwapchainDescription);
    REQUIRE(sampled.has_value());
    const Result<TextureId> stored = producer.CreateTexture("Stored", kSwapchainDescription);
    REQUIRE(stored.has_value());
    REQUIRE(producer.Write(*sampled, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(producer.Write(*stored, ResourceAccess::ColorAttachmentWrite));

    PassBuilder consumer = AnchoredPass(graph, "Consumer", 11);
    REQUIRE(consumer.Read(*sampled, ResourceAccess::SampledRead));
    REQUIRE(consumer.Read(*stored, ResourceAccess::StorageRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // Two each: the transient's creation transition, then the read-after-write.
    REQUIRE(CountFor(inspection, *sampled) == 2u);
    REQUIRE(CountFor(inspection, *stored) == 2u);

    const DerivedBarrier& sampledRaw = BarrierFor(inspection, *sampled, 1);
    CHECK(sampledRaw.layoutBefore == TextureLayout::ColorAttachment);
    CHECK(sampledRaw.layoutAfter == TextureLayout::ShaderReadOnly);
    CHECK(sampledRaw.syncBefore == PipelineStage::ColorAttachmentOutput);
    CHECK(sampledRaw.syncAfter == kShaderStages);
    CHECK(sampledRaw.accessBefore == Access::ColorAttachmentWrite);
    CHECK(sampledRaw.accessAfter == Access::ShaderSampledRead);
    CHECK(sampledRaw.emittedBeforePass == 1u);
    CHECK(sampledRaw.cause == BarrierCause{ByPass(0, ResourceAccess::ColorAttachmentWrite),
                                           ByPass(1, ResourceAccess::SampledRead)});

    const DerivedBarrier& storedRaw = BarrierFor(inspection, *stored, 1);
    CHECK(storedRaw.layoutBefore == TextureLayout::ColorAttachment);
    CHECK(storedRaw.layoutAfter == TextureLayout::General);
    CHECK(storedRaw.syncBefore == PipelineStage::ColorAttachmentOutput);
    CHECK(storedRaw.syncAfter == kShaderStages);
    CHECK(storedRaw.accessBefore == Access::ColorAttachmentWrite);
    CHECK(storedRaw.accessAfter == Access::ShaderStorageRead);
    CHECK(storedRaw.emittedBeforePass == 1u);
    CHECK(storedRaw.cause == BarrierCause{ByPass(0, ResourceAccess::ColorAttachmentWrite),
                                          ByPass(1, ResourceAccess::StorageRead)});
}

TEST_CASE("a transient's first barrier comes from its creation, not from an import") {
    // **A transient has no declared incoming state and still needs an opening transition**, and
    // the phase plan's Task 3 line does not mention one: it names an import's two declared states
    // as the first and last transitions and says nothing about a resource the graph creates. A
    // derivation that took the line literally would render into an image still in
    // `TextureLayout::Undefined`, which is what `IDevice::CreateTexture` asks for.
    //
    // **So this is an addition rather than a checkbox**, recorded as one here so that a later
    // reader does not take it for a requirement and preserve it for the wrong reason. What the
    // cause says is the honest version: `TransientCreation`, which claims no declaration the
    // resource does not have and names no pass that did not access it.
    //
    // Two of them, written by two different passes so that the position on the barrier is a real
    // one rather than a constant zero, and with two different accesses so that the after-state is
    // not one answer either.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             firstPass = AnchoredPass(graph, "First", 10);
    const Result<TextureId> early     = firstPass.CreateTexture("Early", kSwapchainDescription);
    REQUIRE(early.has_value());
    REQUIRE(firstPass.Write(*early, ResourceAccess::ColorAttachmentWrite));

    PassBuilder             secondPass = AnchoredPass(graph, "Second", 11);
    const Result<TextureId> late       = secondPass.CreateTexture("Late", kSwapchainDescription);
    REQUIRE(late.has_value());
    REQUIRE(secondPass.Write(*late, ResourceAccess::StorageWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // One each: the creation transition. A transient gets no closing one -- the graph destroys
    // it, and a transition into a layout nothing will read is work for nobody.
    REQUIRE(CountFor(inspection, *early) == 1u);
    REQUIRE(CountFor(inspection, *late) == 1u);

    const DerivedBarrier& earlyFirst = BarrierFor(inspection, *early, 0);
    CHECK(earlyFirst.layoutBefore == TextureLayout::Undefined);
    CHECK(earlyFirst.layoutAfter == TextureLayout::ColorAttachment);
    CHECK(earlyFirst.syncBefore == PipelineStage::None);
    CHECK(earlyFirst.syncAfter == PipelineStage::ColorAttachmentOutput);
    CHECK(earlyFirst.accessBefore == Access::None);
    CHECK(earlyFirst.accessAfter == Access::ColorAttachmentWrite);
    CHECK(earlyFirst.emittedBeforePass == 0u);
    CHECK(earlyFirst.cause == BarrierCause{ByEnd(BarrierCauseKind::TransientCreation),
                                           ByPass(0, ResourceAccess::ColorAttachmentWrite)});

    const DerivedBarrier& lateFirst = BarrierFor(inspection, *late, 0);
    CHECK(lateFirst.layoutBefore == TextureLayout::Undefined);
    CHECK(lateFirst.layoutAfter == TextureLayout::General);
    CHECK(lateFirst.syncBefore == PipelineStage::None);
    CHECK(lateFirst.syncAfter == kShaderStages);
    CHECK(lateFirst.accessBefore == Access::None);
    CHECK(lateFirst.accessAfter == Access::ShaderStorageWrite);
    CHECK(lateFirst.emittedBeforePass == 1u);
    CHECK(lateFirst.cause == BarrierCause{ByEnd(BarrierCauseKind::TransientCreation),
                                          ByPass(1, ResourceAccess::StorageWrite)});
}

TEST_CASE("a read-modify-write pass combines its two accesses and emits nothing between them") {
    // **The first of the two write-after-read shapes the graph can actually be given.** The
    // phase plan's checklist asks for an inter-pass one and its own open questions explain that
    // there is none: a writer precedes a reader in every order the sort can produce, and the
    // frame that would need otherwise is refused as `DiagnosticKind::UnorderedOverwrite`. What is
    // left is this -- one pass that reads and then writes one resource -- and the import's
    // outgoing transition, which the case below covers.
    //
    // **A read-modify-write pass produces one state, not two, so there is no gap inside it.**
    // That is forced rather than chosen: a barrier cannot be recorded inside a rendering
    // instance, so there is nowhere to put one between a pass's own two accesses. The
    // write-after-read is expressed in the combined access mask instead -- the barrier that
    // reaches the pass makes the resource visible for *both* -- and a derivation that dropped the
    // read half would produce a mask with only `ColorAttachmentWrite` in it.
    //
    // Two of them: `Blend` is read-modify-written by the second pass after a plain write, and
    // `Load` by the second pass after nothing at all, so its combined state is what the
    // transient's creation transitions into.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             producer = AnchoredPass(graph, "Producer", 10);
    const Result<TextureId> blend    = producer.CreateTexture("Blend", kSwapchainDescription);
    REQUIRE(blend.has_value());
    REQUIRE(producer.Write(*blend, ResourceAccess::ColorAttachmentWrite));

    PassBuilder             modifier = AnchoredPass(graph, "Modifier", 11);
    const Result<TextureId> load     = modifier.CreateTexture("Load", kSwapchainDescription);
    REQUIRE(load.has_value());
    REQUIRE(modifier.Read(*blend, ResourceAccess::ColorAttachmentRead));
    REQUIRE(modifier.Write(*blend, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(modifier.Read(*load, ResourceAccess::ColorAttachmentRead));
    REQUIRE(modifier.Write(*load, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // Two for `Blend`: its creation transition, and the one into the modifying pass. **Not
    // three** -- a third would be a barrier between the modifier's own read and its own write,
    // in a place a command list has no room for one.
    REQUIRE(CountFor(inspection, *blend) == 2u);
    // One for `Load`: its creation transition straight into the combined state.
    REQUIRE(CountFor(inspection, *load) == 1u);

    const DerivedBarrier& intoModifier = BarrierFor(inspection, *blend, 1);
    CHECK(intoModifier.layoutBefore == TextureLayout::ColorAttachment);
    CHECK(intoModifier.layoutAfter == TextureLayout::ColorAttachment);
    CHECK(intoModifier.accessBefore == Access::ColorAttachmentWrite);
    // **Both halves of the read-modify-write, which is the assertion that fails if the read is
    // dropped.** The combined access is the union of the pass's two declarations.
    CHECK(intoModifier.accessAfter ==
          (Access::ColorAttachmentRead | Access::ColorAttachmentWrite));
    CHECK(intoModifier.emittedBeforePass == 1u);
    // The pass wrote, so the write is the access the report blames on the after side, even
    // though the read was declared first.
    CHECK(intoModifier.cause == BarrierCause{ByPass(0, ResourceAccess::ColorAttachmentWrite),
                                             ByPass(1, ResourceAccess::ColorAttachmentWrite)});

    const DerivedBarrier& intoLoad = BarrierFor(inspection, *load, 0);
    CHECK(intoLoad.layoutBefore == TextureLayout::Undefined);
    CHECK(intoLoad.layoutAfter == TextureLayout::ColorAttachment);
    CHECK(intoLoad.accessBefore == Access::None);
    CHECK(intoLoad.accessAfter == (Access::ColorAttachmentRead | Access::ColorAttachmentWrite));
    CHECK(intoLoad.cause == BarrierCause{ByEnd(BarrierCauseKind::TransientCreation),
                                         ByPass(1, ResourceAccess::ColorAttachmentWrite)});
}

TEST_CASE("an imported resource's outgoing transition follows a last access that is a read") {
    // **The second write-after-read shape**, and the one that is a genuine
    // write-after-read in ADR-0005's terms: the last thing the frame did to the resource was read
    // it, and the state the importer requires it to be left in is a different one. The barrier's
    // before side is the read and its after side is the declared outgoing state.
    //
    // Two of them, with different outgoing states, so a derivation that emitted a constant after
    // side fails: one is handed back ready to present and the other ready to be copied from.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             reader    = AnchoredPass(graph, "Reader", 10);
    const Result<TextureId> presented = reader.ImportTexture(
        "Presented",
        TextureImport(TextureHandle::ForTesting(1, 1), kSwapchainDescription,
                      TextureState{TextureLayout::ShaderReadOnly, PipelineStage::FragmentShader,
                                   Access::ShaderSampledRead},
                      TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                   Access::None}));
    REQUIRE(presented.has_value());
    const Result<TextureId> copied = reader.ImportTexture(
        "Copied",
        TextureImport(TextureHandle::ForTesting(2, 1), kSwapchainDescription,
                      TextureState{TextureLayout::ShaderReadOnly, PipelineStage::FragmentShader,
                                   Access::ShaderSampledRead},
                      TextureState{TextureLayout::TransferSource, PipelineStage::Copy,
                                   Access::TransferRead}));
    REQUIRE(copied.has_value());
    REQUIRE(reader.Read(*presented, ResourceAccess::SampledRead));
    REQUIRE(reader.Read(*copied, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // **Both barriers, for a resource with no first write at all** -- which is the query
    // `ResourceLifetime` was split in two for. A read-only import `HasNoWrite()` and is
    // `IsUnused()` false, and asking the first question here would skip exactly these.
    REQUIRE(inspection.resources.size() == 3u);
    CHECK(inspection.resources[1].lifetime.HasNoWrite());
    CHECK_FALSE(inspection.resources[1].lifetime.IsUnused());
    REQUIRE(CountFor(inspection, *presented) == 2u);
    REQUIRE(CountFor(inspection, *copied) == 2u);

    const DerivedBarrier& outPresented = BarrierFor(inspection, *presented, 1);
    CHECK(outPresented.layoutBefore == TextureLayout::ShaderReadOnly);
    CHECK(outPresented.layoutAfter == TextureLayout::PresentSource);
    CHECK(outPresented.syncBefore == kShaderStages);
    CHECK(outPresented.syncAfter == PipelineStage::None);
    CHECK(outPresented.accessBefore == Access::ShaderSampledRead);
    CHECK(outPresented.accessAfter == Access::None);
    CHECK(outPresented.emittedBeforePass == kNoPass);
    CHECK(outPresented.cause == BarrierCause{ByPass(0, ResourceAccess::SampledRead),
                                             ByEnd(BarrierCauseKind::ImportOutgoing)});

    const DerivedBarrier& outCopied = BarrierFor(inspection, *copied, 1);
    CHECK(outCopied.layoutBefore == TextureLayout::ShaderReadOnly);
    CHECK(outCopied.layoutAfter == TextureLayout::TransferSource);
    CHECK(outCopied.syncBefore == kShaderStages);
    CHECK(outCopied.syncAfter == PipelineStage::Copy);
    CHECK(outCopied.accessBefore == Access::ShaderSampledRead);
    CHECK(outCopied.accessAfter == Access::TransferRead);
    CHECK(outCopied.emittedBeforePass == kNoPass);
}

TEST_CASE("three consecutive accesses yield two barriers, not one merged or three") {
    // **The count is the assertion and both ways of getting it wrong are named.** A derivation
    // that merged the chain would give one barrier between the three accesses; one that emitted a
    // gap that is not there would give three. Two is the answer, and each is checked for the pair
    // of states it is between so that "two" cannot be two of the wrong ones.
    //
    // The chain is written, read-modify-written, read -- which is the three-access shape this
    // graph can actually be given. Write, read, write is not: two passes that write a resource
    // without reading it, with a read between them, is
    // `DiagnosticKind::UnorderedOverwrite`.
    //
    // Two of them: an imported resource, where the two inter-pass barriers sit between the
    // import's own two, and a transient, where the first is the creation transition.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             producer = AnchoredPass(graph, "Producer", 10);
    const Result<TextureId> borrowed = producer.ImportTexture("Borrowed", FramedImport(1));
    REQUIRE(borrowed.has_value());
    const Result<TextureId> owned = producer.CreateTexture("Owned", kSwapchainDescription);
    REQUIRE(owned.has_value());
    REQUIRE(producer.Write(*borrowed, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(producer.Write(*owned, ResourceAccess::ColorAttachmentWrite));

    PassBuilder modifier = AnchoredPass(graph, "Modifier", 11);
    REQUIRE(modifier.Read(*borrowed, ResourceAccess::ColorAttachmentRead));
    REQUIRE(modifier.Write(*borrowed, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(modifier.Read(*owned, ResourceAccess::ColorAttachmentRead));
    REQUIRE(modifier.Write(*owned, ResourceAccess::ColorAttachmentWrite));

    PassBuilder consumer = AnchoredPass(graph, "Consumer", 12);
    REQUIRE(consumer.Read(*borrowed, ResourceAccess::SampledRead));
    REQUIRE(consumer.Read(*owned, ResourceAccess::SampledRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    // Four for the import: its incoming transition, the two between its three pass states, and
    // its outgoing one.
    REQUIRE(CountFor(inspection, *borrowed) == 4u);
    // Three for the transient: its creation transition and the same two.
    REQUIRE(CountFor(inspection, *owned) == 3u);

    // The two inter-pass barriers of the import, by the pair of states each is between.
    CHECK(BarrierFor(inspection, *borrowed, 1).cause ==
          BarrierCause{ByPass(0, ResourceAccess::ColorAttachmentWrite),
                       ByPass(1, ResourceAccess::ColorAttachmentWrite)});
    CHECK(BarrierFor(inspection, *borrowed, 1).emittedBeforePass == 1u);
    CHECK(BarrierFor(inspection, *borrowed, 2).cause ==
          BarrierCause{ByPass(1, ResourceAccess::ColorAttachmentWrite),
                       ByPass(2, ResourceAccess::SampledRead)});
    CHECK(BarrierFor(inspection, *borrowed, 2).emittedBeforePass == 2u);
    CHECK(BarrierFor(inspection, *borrowed, 2).layoutAfter == TextureLayout::ShaderReadOnly);

    // And the transient's, which are the same two gaps reached from a different first state.
    CHECK(BarrierFor(inspection, *owned, 0).cause ==
          BarrierCause{ByEnd(BarrierCauseKind::TransientCreation),
                       ByPass(0, ResourceAccess::ColorAttachmentWrite)});
    CHECK(BarrierFor(inspection, *owned, 1).emittedBeforePass == 1u);
    CHECK(BarrierFor(inspection, *owned, 2).emittedBeforePass == 2u);
    CHECK(BarrierFor(inspection, *owned, 2).layoutAfter == TextureLayout::ShaderReadOnly);
}

TEST_CASE("a transition that is not a transition is representable and is not emitted") {
    // **The plan's third checkbox, and the shape that separates all three ways of getting it
    // wrong in one case.** Each import below declares an incoming state byte-identical to what its
    // one reading pass needs -- same layout, same stage, same access -- so that gap is not a
    // transition and must produce nothing. Each import's *outgoing* state differs, so that gap
    // does produce one.
    //
    // The answer is therefore exactly one barrier per resource: a derivation that emitted the
    // no-op gives two, one that suppressed too much gives zero, and one that emitted the wrong
    // single barrier fails the fields below.
    //
    // **Read and not written, which is what makes these gaps no-ops at all.** A pass that *wrote*
    // into a state identical to the declared incoming one still gets its barrier -- the rule
    // reads whether a pass wrote the state after the gap, and the write-after-write case is why.
    // See Private/DeriveBarriers.cpp; the case below this one pins that half.
    //
    // **Representable is the other half of the checkbox**, and `DerivedBarrier` holds such a
    // value without complaint: `handBuilt` at the foot is one. What the derivation must not do is
    // put one in the list.
    //
    // Two of them, matching on two different states, so an answer that held for the sampled case
    // alone fails.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             pass    = AnchoredPass(graph, "Pass", 10);
    const Result<TextureId> sampled = pass.ImportTexture(
        "Sampled",
        TextureImport(TextureHandle::ForTesting(1, 1), kSwapchainDescription,
                      TextureState{TextureLayout::ShaderReadOnly, kShaderStages,
                                   Access::ShaderSampledRead},
                      TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                   Access::None}));
    REQUIRE(sampled.has_value());
    const Result<TextureId> storage = pass.ImportTexture(
        "Storage",
        TextureImport(TextureHandle::ForTesting(2, 1), kSwapchainDescription,
                      TextureState{TextureLayout::General, kShaderStages,
                                   Access::ShaderStorageRead},
                      TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                   Access::None}));
    REQUIRE(storage.has_value());
    REQUIRE(pass.Read(*sampled, ResourceAccess::SampledRead));
    REQUIRE(pass.Read(*storage, ResourceAccess::StorageRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(CountFor(inspection, *sampled) == 1u);
    REQUIRE(CountFor(inspection, *storage) == 1u);

    // The one that is there is the outgoing transition, not the suppressed opening one -- which
    // its cause says, because the before side is the pass's access rather than the import's
    // incoming state.
    CHECK(BarrierFor(inspection, *sampled, 0).cause ==
          BarrierCause{ByPass(0, ResourceAccess::SampledRead),
                       ByEnd(BarrierCauseKind::ImportOutgoing)});
    CHECK(BarrierFor(inspection, *sampled, 0).layoutBefore == TextureLayout::ShaderReadOnly);
    CHECK(BarrierFor(inspection, *sampled, 0).layoutAfter == TextureLayout::PresentSource);
    CHECK(BarrierFor(inspection, *storage, 0).cause ==
          BarrierCause{ByPass(0, ResourceAccess::StorageRead),
                       ByEnd(BarrierCauseKind::ImportOutgoing)});
    CHECK(BarrierFor(inspection, *storage, 0).layoutBefore == TextureLayout::General);

    // Representable: a no-op barrier is a value this type holds without complaint. Nothing
    // derived it, which is the point -- the derivation's job is not to make it unspellable.
    DerivedBarrier handBuilt{};
    handBuilt.layoutBefore = TextureLayout::ColorAttachment;
    handBuilt.layoutAfter  = TextureLayout::ColorAttachment;
    handBuilt.syncBefore   = PipelineStage::ColorAttachmentOutput;
    handBuilt.syncAfter    = PipelineStage::ColorAttachmentOutput;
    handBuilt.accessBefore = Access::ColorAttachmentWrite;
    handBuilt.accessAfter  = Access::ColorAttachmentWrite;
    CHECK(handBuilt == handBuilt);
    CHECK(handBuilt.layoutBefore == handBuilt.layoutAfter);
}

TEST_CASE("a pass writing into the state its import declared still gets its barrier") {
    // **The other side of the no-op rule, and the one a symmetric rule gets wrong.** The import
    // below declares an incoming state identical to what its one pass needs -- and that pass
    // *writes*. Suppressing the gap would leave whatever produced the declared incoming state
    // unordered against this frame's write of the same image at the same stage, which is a
    // write-after-write hazard the graph has no other way to cover: an import is not an access,
    // so there is nothing else in the chain to order against.
    //
    // **So the rule reads the side after the gap, not both sides**, and this case is what makes
    // the difference observable. Together with the case above -- same shape, a read instead of a
    // write, no barrier -- the pair pins the rule rather than one of its outcomes.
    //
    // Two of them, on two different states, so neither answer is one resource's accident.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    const Result<TextureId> attachment = pass->ImportTexture(
        "Attachment",
        TextureImport(TextureHandle::ForTesting(1, 1), kSwapchainDescription,
                      TextureState{TextureLayout::ColorAttachment,
                                   PipelineStage::ColorAttachmentOutput,
                                   Access::ColorAttachmentWrite},
                      TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                   Access::None}));
    REQUIRE(attachment.has_value());
    const Result<TextureId> storage = pass->ImportTexture(
        "Storage",
        TextureImport(TextureHandle::ForTesting(2, 1), kSwapchainDescription,
                      TextureState{TextureLayout::General, kShaderStages,
                                   Access::ShaderStorageWrite},
                      TextureState{TextureLayout::PresentSource, PipelineStage::None,
                                   Access::None}));
    REQUIRE(storage.has_value());
    REQUIRE(pass->Write(*attachment, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(pass->Write(*storage, ResourceAccess::StorageWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(CountFor(inspection, *attachment) == 2u);
    REQUIRE(CountFor(inspection, *storage) == 2u);

    // The opening barrier is there, and every one of its six values is the same on both sides --
    // which is exactly the "same stage, same access, same layout" shape, emitted on purpose.
    const DerivedBarrier& opening = BarrierFor(inspection, *attachment, 0);
    CHECK(opening.layoutBefore == opening.layoutAfter);
    CHECK(opening.syncBefore == opening.syncAfter);
    CHECK(opening.accessBefore == opening.accessAfter);
    CHECK(opening.layoutBefore == TextureLayout::ColorAttachment);
    CHECK(opening.accessBefore == Access::ColorAttachmentWrite);
    CHECK(opening.cause == BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                                        ByPass(0, ResourceAccess::ColorAttachmentWrite)});

    const DerivedBarrier& storageOpening = BarrierFor(inspection, *storage, 0);
    CHECK(storageOpening.layoutBefore == TextureLayout::General);
    CHECK(storageOpening.layoutAfter == TextureLayout::General);
    CHECK(storageOpening.accessBefore == Access::ShaderStorageWrite);
    CHECK(storageOpening.accessAfter == Access::ShaderStorageWrite);
    CHECK(storageOpening.cause == BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                                               ByPass(0, ResourceAccess::StorageWrite)});
}

TEST_CASE("an outgoing state a writing pass already matches is still a write-after-write") {
    // **The gap at the far end of the chain, and the one a rule that read only the side after it
    // dropped in silence.** Each import below declares an outgoing state byte-identical to what
    // its one *writing* pass leaves it in -- same layout, same stage, same access -- and the gap
    // is a transition anyway, because the emission rule's write term reads both sides of it.
    //
    // **What makes it a hazard is that an outgoing state is a dst scope and not a layout wish.**
    // `StepResource` puts `outgoing.stage` and `outgoing.access` into `syncAfter` and
    // `accessAfter` -- the half of a barrier that names the work it is made visible *to* -- and
    // `SwapchainImport` above reads them the same way, giving `None` because "there is no
    // *command* after the transition". So an outgoing state naming a **write** access is a
    // statement that external commands will write this image, and this frame wrote it too.
    // Nothing else orders the two: two submissions are not ordered by being submitted in order,
    // and with no layout change there is nothing for a validation layer to object to either.
    // Dropping this barrier is silent corruption, so every field of it is asserted rather than
    // counted.
    //
    // The opening transition is emitted as well -- `Undefined` is not `ColorAttachment` -- so
    // the count is two, and the closing one is identified by its cause rather than by position.
    //
    // Two of them, on two different already-matched states, so neither answer is one resource's
    // accident. The storage pair is the shape a compute frame produces: a graph that storage-
    // writes an image the importer goes on to storage-write itself.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    Result<PassBuilder> pass = graph.AddPass("Pass");
    REQUIRE(pass.has_value());
    const Result<TextureId> attachment = pass->ImportTexture(
        "Attachment",
        TextureImport(TextureHandle::ForTesting(1, 1), kSwapchainDescription,
                      TextureState{TextureLayout::Undefined, PipelineStage::ColorAttachmentOutput,
                                   Access::None},
                      TextureState{TextureLayout::ColorAttachment,
                                   PipelineStage::ColorAttachmentOutput,
                                   Access::ColorAttachmentWrite}));
    REQUIRE(attachment.has_value());
    const Result<TextureId> storage = pass->ImportTexture(
        "Storage",
        TextureImport(TextureHandle::ForTesting(2, 1), kSwapchainDescription,
                      TextureState{TextureLayout::Undefined, PipelineStage::ColorAttachmentOutput,
                                   Access::None},
                      TextureState{TextureLayout::General, kShaderStages,
                                   Access::ShaderStorageWrite}));
    REQUIRE(storage.has_value());
    REQUIRE(pass->Write(*attachment, ResourceAccess::ColorAttachmentWrite));
    REQUIRE(pass->Write(*storage, ResourceAccess::StorageWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(CountFor(inspection, *attachment) == 2u);
    REQUIRE(CountFor(inspection, *storage) == 2u);

    // The opening one is still the opening one.
    CHECK(BarrierFor(inspection, *attachment, 0).emittedBeforePass == 0u);
    CHECK(BarrierFor(inspection, *attachment, 0).cause ==
          BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                       ByPass(0, ResourceAccess::ColorAttachmentWrite)});

    // And the closing one is the write-after-write: identical on both sides, which is what makes
    // it invisible to any rule written on equality alone, and recorded after every pass.
    const DerivedBarrier& closing = BarrierFor(inspection, *attachment, 1);
    CHECK(closing.layoutBefore == closing.layoutAfter);
    CHECK(closing.syncBefore == closing.syncAfter);
    CHECK(closing.accessBefore == closing.accessAfter);
    CHECK(closing.layoutBefore == TextureLayout::ColorAttachment);
    CHECK(closing.syncBefore == PipelineStage::ColorAttachmentOutput);
    CHECK(closing.accessBefore == Access::ColorAttachmentWrite);
    CHECK(closing.emittedBeforePass == kNoPass);
    CHECK(closing.cause == BarrierCause{ByPass(0, ResourceAccess::ColorAttachmentWrite),
                                        ByEnd(BarrierCauseKind::ImportOutgoing)});

    CHECK(BarrierFor(inspection, *storage, 0).emittedBeforePass == 0u);
    CHECK(BarrierFor(inspection, *storage, 0).cause ==
          BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                       ByPass(0, ResourceAccess::StorageWrite)});

    const DerivedBarrier& storageClosing = BarrierFor(inspection, *storage, 1);
    CHECK(storageClosing.layoutBefore == TextureLayout::General);
    CHECK(storageClosing.layoutAfter == TextureLayout::General);
    CHECK(storageClosing.syncBefore == kShaderStages);
    CHECK(storageClosing.syncAfter == kShaderStages);
    CHECK(storageClosing.accessBefore == Access::ShaderStorageWrite);
    CHECK(storageClosing.accessAfter == Access::ShaderStorageWrite);
    CHECK(storageClosing.emittedBeforePass == kNoPass);
    CHECK(storageClosing.cause == BarrierCause{ByPass(0, ResourceAccess::StorageWrite),
                                               ByEnd(BarrierCauseKind::ImportOutgoing)});
}

TEST_CASE("an outgoing state a reading pass already matches is not a transition") {
    // **The half of the same gap that stays suppressed, and the pair is what pins the rule
    // rather than one of its outcomes.** The case above and this one declare the same shape --
    // an outgoing state byte-identical to what the last pass left the resource in -- and differ
    // in one thing: whether that pass wrote. It did there and it does not here, so there is
    // nothing to order and nothing is emitted.
    //
    // **This is the reading an outgoing state still has where it is a requirement already met.**
    // Equal states with no write on either side mean the importer asked for the scope the
    // resource is already visible in: read-after-read across the graph boundary, which is not a
    // hazard in Vulkan or anywhere else. A rule that emitted here would put a barrier in every
    // frame that hands a sampled texture back unchanged.
    //
    // The opening transition is still emitted -- the declared incoming state differs -- so the
    // count is one rather than zero, and a derivation that dropped everything fails here too.
    //
    // Two of them, on two different already-matched read states.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             pass    = AnchoredPass(graph, "Pass", 10);
    const Result<TextureId> sampled = pass.ImportTexture(
        "Sampled",
        TextureImport(TextureHandle::ForTesting(1, 1), kSwapchainDescription,
                      TextureState{TextureLayout::TransferSource, PipelineStage::Copy,
                                   Access::TransferRead},
                      TextureState{TextureLayout::ShaderReadOnly, kShaderStages,
                                   Access::ShaderSampledRead}));
    REQUIRE(sampled.has_value());
    const Result<TextureId> storage = pass.ImportTexture(
        "Storage",
        TextureImport(TextureHandle::ForTesting(2, 1), kSwapchainDescription,
                      TextureState{TextureLayout::TransferSource, PipelineStage::Copy,
                                   Access::TransferRead},
                      TextureState{TextureLayout::General, kShaderStages,
                                   Access::ShaderStorageRead}));
    REQUIRE(storage.has_value());
    REQUIRE(pass.Read(*sampled, ResourceAccess::SampledRead));
    REQUIRE(pass.Read(*storage, ResourceAccess::StorageRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(CountFor(inspection, *sampled) == 1u);
    REQUIRE(CountFor(inspection, *storage) == 1u);
    // The one that is there is the opening transition, so nothing was emitted after the pass.
    CHECK(BarrierFor(inspection, *sampled, 0).emittedBeforePass == 0u);
    CHECK(BarrierFor(inspection, *sampled, 0).cause ==
          BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                       ByPass(0, ResourceAccess::SampledRead)});
    CHECK(BarrierFor(inspection, *sampled, 0).layoutAfter == TextureLayout::ShaderReadOnly);
    CHECK(BarrierFor(inspection, *storage, 0).emittedBeforePass == 0u);
    CHECK(BarrierFor(inspection, *storage, 0).cause ==
          BarrierCause{ByEnd(BarrierCauseKind::ImportIncoming),
                       ByPass(0, ResourceAccess::StorageRead)});
    CHECK(BarrierFor(inspection, *storage, 0).layoutAfter == TextureLayout::General);
}

TEST_CASE("culled passes contribute no barriers") {
    // **A pass that does not run cannot need a barrier**, and the two graphs below are the same
    // declarations with one difference: whether anything consumes what the middle pass wrote.
    // Comparing the two is what makes this a statement about culling rather than about a frame
    // that happened to have few barriers.
    //
    // The culled pass reads the anchor's transient *and* writes one of its own, so it is a pass
    // with a real access to a resource that survives -- a derivation that walked every declared
    // pass rather than the surviving ones would put a barrier in front of it.
    SystemAllocator allocator;

    Monarc::usize withSink    = 0;
    Monarc::usize withoutSink = 0;
    Monarc::usize sharedWith  = 0;
    Monarc::usize sharedWithout = 0;

    for (int consumed = 0; consumed < 2; ++consumed) {
        RenderGraph graph(allocator, RenderGraph::Config{});

        PassBuilder             producer = AnchoredPass(graph, "Producer", 10);
        const Result<TextureId> shared   = producer.CreateTexture("Shared", kSwapchainDescription);
        REQUIRE(shared.has_value());
        REQUIRE(producer.Write(*shared, ResourceAccess::ColorAttachmentWrite));

        Result<PassBuilder> middle = graph.AddPass("Middle");
        REQUIRE(middle.has_value());
        REQUIRE(middle->Read(*shared, ResourceAccess::SampledRead));
        const Result<TextureId> spill = middle->CreateTexture("Spill", kSwapchainDescription);
        REQUIRE(spill.has_value());
        REQUIRE(middle->Write(*spill, ResourceAccess::ColorAttachmentWrite));

        if (consumed != 0) {
            PassBuilder sink = AnchoredPass(graph, "Sink", 11);
            REQUIRE(sink.Read(*spill, ResourceAccess::SampledRead));
        }

        REQUIRE(graph.Compile());

        const GraphInspection inspection = graph.Inspect();
        REQUIRE(inspection.passes.size() >= 2u);
        CHECK(inspection.passes[1].culled == (consumed == 0));

        if (consumed != 0) {
            withSink   = CountFor(inspection, *spill);
            sharedWith = CountFor(inspection, *shared);
        } else {
            withoutSink   = CountFor(inspection, *spill);
            sharedWithout = CountFor(inspection, *shared);
        }
    }

    // With the sink: `Spill` is created and transitioned into the middle pass's write, then into
    // the sink's read. Two barriers.
    CHECK(withSink == 2u);
    // Without it, the middle pass is culled, so `Spill` is written by nothing that runs and is
    // never transitioned at all.
    CHECK(withoutSink == 0u);

    // And the *other* resource the culled pass touched shows the same thing from the other side:
    // with the sink, `Shared` gets its creation transition and the read-after-write into the
    // middle pass; without it, only the creation transition, because the read does not happen.
    CHECK(sharedWith == 2u);
    CHECK(sharedWithout == 1u);
}

TEST_CASE("no barrier is derived for a resource a pass declared and no pass used") {
    // **Three ways for a resource to go untouched, and they are three different declarations
    // with the same answer.** One transient is created and never accessed at all -- legal, and
    // `PassBuilder::CreateTexture` says so. One is written only by a pass that is culled. And one
    // is an *imported* resource whose only reader is culled, which is the case the next assertion
    // block is about.
    //
    // A derivation that started every resource's chain regardless would give the first two a
    // creation transition each, and the third its two declared ends.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             owner     = AnchoredPass(graph, "Owner", 10);
    const Result<TextureId> untouched = owner.CreateTexture("Untouched", kSwapchainDescription);
    REQUIRE(untouched.has_value());
    const Result<TextureId> orphaned = owner.CreateTexture("Orphaned", kSwapchainDescription);
    REQUIRE(orphaned.has_value());

    Result<PassBuilder> ghost = graph.AddPass("Ghost");
    REQUIRE(ghost.has_value());
    REQUIRE(ghost->Write(*orphaned, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 2u);
    CHECK(inspection.passes[1].culled);
    CHECK(CountFor(inspection, *untouched) == 0u);
    CHECK(CountFor(inspection, *orphaned) == 0u);
    // The anchor is the control: the same graph does derive barriers, so zero is a statement
    // about these two resources rather than about a derivation that produced nothing.
    REQUIRE(inspection.resources.size() == 3u);
    CHECK(CountFor(inspection, inspection.resources[0].id) == 2u);
}

TEST_CASE("an imported resource no surviving pass touches is left alone") {
    // **The decision, stated: the graph does not transition a resource it did not use.** The
    // alternative is to emit `incoming -> outgoing` for it, and that would be a barrier for an
    // operation the frame did not perform -- with a cause naming two import ends and no access
    // between them, in a field that exists precisely so that a barrier is traceable to the
    // accesses that asked for it.
    //
    // **Only a read-only import can reach this state**, which is worth knowing because it bounds
    // what the decision costs: culling never drops a pass that *writes* an imported resource, so
    // an import the frame produced into always gets both its transitions. What is left is an
    // import the frame was going to read and then did not, and the report says so plainly -- the
    // reading pass is marked `culled` and the resource's lifetime is empty.
    //
    // Two of them, so that the answer is not one resource's accident.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder anchor = AnchoredPass(graph, "Anchor", 10);
    static_cast<void>(anchor);

    Result<PassBuilder> ghost = graph.AddPass("Ghost");
    REQUIRE(ghost.has_value());
    const Result<TextureId> first = ghost->ImportTexture("FirstSource", FramedImport(1));
    REQUIRE(first.has_value());
    const Result<TextureId> second = ghost->ImportTexture("SecondSource", FramedImport(2));
    REQUIRE(second.has_value());
    REQUIRE(ghost->Read(*first, ResourceAccess::SampledRead));
    REQUIRE(ghost->Read(*second, ResourceAccess::StorageRead));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 2u);
    // The reading pass is culled: it reads two imports and writes nothing, so nothing consumes
    // anything it produced.
    CHECK(inspection.passes[1].culled);
    REQUIRE(inspection.resources.size() == 3u);
    CHECK(inspection.resources[1].lifetime.IsUnused());
    CHECK(inspection.resources[2].lifetime.IsUnused());
    CHECK(CountFor(inspection, *first) == 0u);
    CHECK(CountFor(inspection, *second) == 0u);
    // The declared states are still reported, so a caller who wanted the transition can see both
    // that it was declared and that the frame did not use the resource.
    CHECK(inspection.resources[1].incoming.layout == TextureLayout::TransferSource);
    CHECK(inspection.resources[1].outgoing.layout == TextureLayout::PresentSource);
    // And the anchor still gets its two, so zero is about these resources.
    CHECK(CountFor(inspection, inspection.resources[0].id) == 2u);
}

TEST_CASE("barriers come out in the order a frame records them") {
    // **The list's order is a promise `GraphInspection::barriers` makes and Task 4's `Execute`
    // will lean on**, so it is asserted rather than assumed: by the execution position each is
    // emitted in front of, with the end-of-frame ones last.
    //
    // The frame is arranged so that the order is not the resources' declaration order and not the
    // passes' either: the producer is declared *after* its consumer and runs before it, so a
    // derivation that walked declaration order would emit the consumer's barriers first.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             consumer = AnchoredPass(graph, "Consumer", 10);
    const Result<TextureId> target   = consumer.CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());
    REQUIRE(consumer.Read(*target, ResourceAccess::SampledRead));

    PassBuilder producer = AnchoredPass(graph, "Producer", 11);
    REQUIRE(producer.Write(*target, ResourceAccess::ColorAttachmentWrite));

    REQUIRE(graph.Compile());

    const GraphInspection inspection = graph.Inspect();
    REQUIRE(inspection.passes.size() == 2u);
    // Declared first, runs second.
    CHECK(inspection.passes[0].executionOrder == 1u);
    CHECK(inspection.passes[1].executionOrder == 0u);

    // Non-decreasing `emittedBeforePass` throughout, with `kNoPass` -- the largest `u32` -- at
    // the end for free.
    REQUIRE(inspection.barriers.size() >= 2u);
    for (Monarc::usize i = 1; i < inspection.barriers.size(); ++i) {
        CAPTURE(i);
        CHECK(inspection.barriers[i - 1].emittedBeforePass <=
              inspection.barriers[i].emittedBeforePass);
    }
    // And the first barrier in the list belongs to the pass that runs first, which is the one
    // declared second.
    CHECK(inspection.barriers[0].emittedBeforePass == 0u);
    CHECK(inspection.barriers.back().emittedBeforePass == kNoPass);
}

TEST_CASE("a full barrier pool is refused rather than grown") {
    // **The pool discipline every other pool in this class keeps**, and the derivation is the one
    // that fills a pool the *graph* sizes rather than the caller. The frame below needs four
    // barriers and is given room for two.
    //
    // Two configurations, so that the refusal is about the capacity rather than about the frame:
    // with room for one the build is refused with one barrier derived, and with room for four the
    // same declarations compile.
    SystemAllocator allocator;

    for (Monarc::u32 room : {1u, 4u}) {
        CAPTURE(room);
        RenderGraph::Config config{};
        config.maxBarriers = room;
        RenderGraph graph(allocator, config);

        Result<PassBuilder> pass = graph.AddPass("Pass");
        REQUIRE(pass.has_value());
        const Result<TextureId> first = pass->ImportTexture("First", SwapchainImport(1));
        REQUIRE(first.has_value());
        const Result<TextureId> second = pass->ImportTexture("Second", SwapchainImport(2));
        REQUIRE(second.has_value());
        REQUIRE(pass->Write(*first, ResourceAccess::ColorAttachmentWrite));
        REQUIRE(pass->Write(*second, ResourceAccess::ColorAttachmentWrite));

        const Status compiled = graph.Compile();
        const GraphInspection inspection = graph.Inspect();

        if (room == 4u) {
            CHECK(compiled.has_value());
            CHECK(inspection.phase == GraphPhase::Compiled);
            CHECK(inspection.barriers.size() == 4u);
            continue;
        }

        REQUIRE_FALSE(compiled.has_value());
        CHECK(compiled.error().code == ErrorCode::OutOfMemory);
        CHECK(inspection.phase == GraphPhase::CompileFailed);
        // **The barriers derived before the overflow are kept**, which is what says how far the
        // derivation got. One, and the pool held one.
        CHECK(inspection.barriers.size() == 1u);
        REQUIRE(inspection.diagnostics.size() == 1u);
        CHECK(inspection.diagnostics[0].kind == DiagnosticKind::BarrierPoolExhausted);
        CHECK(inspection.diagnostics[0].code == ErrorCode::OutOfMemory);
        // The refusal names the pass that the refused barrier would have been recorded in front
        // of, in **declaration** order, and the resource it was about.
        CHECK(inspection.diagnostics[0].pass == 0u);
        CHECK(inspection.diagnostics[0].resource == *second);
        // The order, culling, lifetimes and alias groups the earlier stages settled are still
        // reported -- this refusal happens after them, unlike every other one `Compile` makes.
        REQUIRE(inspection.passes.size() == 1u);
        CHECK(inspection.passes[0].executionOrder == 0u);
        CHECK_FALSE(inspection.passes[0].culled);
    }
}

TEST_CASE("compiling a frame with barriers allocates nothing") {
    // **The pools are bought at construction and never grown**, which the class comment claims
    // for the whole of `RenderGraph` and which the derivation is the newest thing that could
    // break: it appends to `m_barriers` and keeps a running step per resource. Both are reserved
    // and filled in the constructor, so a compile must move the allocator's total by zero.
    //
    // Measured across `Compile` rather than across the graph's life, so the constructor's one
    // allocation is outside the window and the number being compared is a difference rather than
    // a total that would have to be updated whenever a capacity changed.
    SystemAllocator allocator;
    RenderGraph     graph(allocator, RenderGraph::Config{});

    PassBuilder             producer = AnchoredPass(graph, "Producer", 10);
    const Result<TextureId> target   = producer.CreateTexture("Target", kSwapchainDescription);
    REQUIRE(target.has_value());
    REQUIRE(producer.Write(*target, ResourceAccess::ColorAttachmentWrite));

    PassBuilder consumer = AnchoredPass(graph, "Consumer", 11);
    REQUIRE(consumer.Read(*target, ResourceAccess::SampledRead));

    const Monarc::usize before = allocator.BytesAllocated();
    REQUIRE(graph.Compile());
    CHECK(allocator.BytesAllocated() == before);

    // And the compile it measured is a real one, so "allocated nothing" is not "did nothing".
    CHECK(graph.Inspect().barriers.size() == 6u);
}
