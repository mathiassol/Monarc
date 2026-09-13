#pragma once

#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>
#include <Monarc/RHI/Swapchain.h>
#include <Monarc/Render/Access.h>
#include <Monarc/Render/PassBuilder.h>

namespace Monarc::Render::TestSupport {

// ---------------------------------------------------------------------------------------
// A3's measured barriers, decoded from the captures, and the import that produces them.
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
// them: a case compares what the graph produced against values transcribed from a file, so a
// change to the derivation that also changed the expectation would have to change this table and
// stop matching the citation above it.
//
// **In `TestSupport/` rather than in one test file, because two suites now assert on them.**
// Tests/TestDeriveBarriers.cpp asserts that the *derivation* produces these twelve values;
// Tests/TestExecute.cpp asserts that the twelve reach an `RHI::ICommandList` in the right order
// and on the right side of `BeginRendering`. A second transcription would be a second thing to
// keep true, and the failure mode is the one this codebase keeps finding: one copy updated and
// the other left agreeing with nothing.
// ---------------------------------------------------------------------------------------

inline constexpr RHI::TextureLayout kCapturedFirstOldLayout  = RHI::TextureLayout::Undefined;
inline constexpr RHI::TextureLayout kCapturedFirstNewLayout = RHI::TextureLayout::ColorAttachment;
inline constexpr RHI::PipelineStage kCapturedFirstSrcStage =
    RHI::PipelineStage::ColorAttachmentOutput;
inline constexpr RHI::PipelineStage kCapturedFirstDstStage =
    RHI::PipelineStage::ColorAttachmentOutput;
inline constexpr RHI::Access kCapturedFirstSrcAccess = RHI::Access::None;
inline constexpr RHI::Access kCapturedFirstDstAccess = RHI::Access::ColorAttachmentWrite;

inline constexpr RHI::TextureLayout kCapturedSecondOldLayout =
    RHI::TextureLayout::ColorAttachment;
inline constexpr RHI::TextureLayout kCapturedSecondNewLayout = RHI::TextureLayout::PresentSource;
inline constexpr RHI::PipelineStage kCapturedSecondSrcStage =
    RHI::PipelineStage::ColorAttachmentOutput;
inline constexpr RHI::PipelineStage kCapturedSecondDstStage = RHI::PipelineStage::None;
inline constexpr RHI::Access kCapturedSecondSrcAccess = RHI::Access::ColorAttachmentWrite;
inline constexpr RHI::Access kCapturedSecondDstAccess = RHI::Access::None;

/// The clear value A3's capture holds: `0.25098040699958801`, which is `64 / 255` as a float.
///
/// One `vkCmdBeginRendering` with `LOAD_OP_CLEAR` and that value in the red channel -- the frame
/// A3 hand-wrote cleared to `(64, 128, 192, 255)` BGRA and read exactly that back. The channel
/// order here is the attachment's own, which for `B8G8R8A8_UNORM` means `r` is the value that
/// lands in the blue byte; `RHI::ClearColor` states that.
inline constexpr RHI::ClearColor kCapturedClearValue{64.0F / 255.0F, 128.0F / 255.0F,
                                                     192.0F / 255.0F, 1.0F};

/// The swapchain image's size and format in A3's captures.
inline constexpr RHI::TextureDescription kSwapchainDescription{
    RHI::Extent2D{1280, 720}, RHI::Format::B8G8R8A8_UNORM, RHI::TextureUsage::ColorAttachment};

/// The import `Monarc.FirstLight` declares for the swapchain image it clears and presents.
///
/// **The six declared values are not written here, and that is what keeps the two suites'
/// equivalence cases from being tautologies.** They are `RHI::kSwapchainImageIncoming` and
/// `kSwapchainImageOutgoing` in Monarc/RHI/Swapchain.h, where the argument for each lives: five
/// forced by the swapchain contract, one -- `incoming.stage` -- chosen against
/// `VulkanDeviceState::SubmitList`'s acquire-semaphore wait. The
/// twelve `kCaptured*` values above are the other half: transcribed from A3's RenderDoc files
/// and belonging to no declaration, so a case that feeds these two states through the derivation
/// and compares the result against those twelve is comparing two independent things.
///
/// **Which pins `incoming.stage` from one end only, and that is worth being exact about.**
/// Change the constant and the equivalence case fails against the capture. Change the acquire
/// wait stage in Monarc.RHI.Vulkan instead, which is still a literal of its own, and nothing
/// here moves -- the two are a pair that has to agree, and only one of them is under test.
///
/// `image` is a parameter because the two suites need different handles: a derivation test needs
/// none that resolves anywhere, and an execution test needs one its stub device really made,
/// which is what a swapchain image is -- see `RHI::AcquiredImage`, which registers every
/// swapchain image in the device's own texture pool.
[[nodiscard]] inline TextureImport SwapchainImport(RHI::TextureHandle image) {
    return TextureImport(image, kSwapchainDescription, RHI::kSwapchainImageIncoming,
                         RHI::kSwapchainImageOutgoing);
}

}  // namespace Monarc::Render::TestSupport
