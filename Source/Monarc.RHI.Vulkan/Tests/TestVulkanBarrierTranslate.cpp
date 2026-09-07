// Device-free tests for the barrier model's translation -- ADR-0005's shape, as
// Private/Translate.h maps it onto `synchronization2`.
//
// **All of it, though A3 records three barriers.** That is what the phase plan asks for:
// translation for the whole model, two barriers used, and the rest carried here. So these
// cases are the coverage claim for every stage, access and layout enumerator Monarc declares.
//
// Its own file rather than more of TestVulkanTranslate.cpp, because the barrier translators are
// half the module's pure surface and their round-trip property is a different kind of
// statement from "this enum maps to that enum".

#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Device.h>
#include <Monarc/RHI/Handles.h>

#include <Translate.h>

#include <format>
#include <initializer_list>
#include <iterator>
#include <string_view>

using Monarc::RHI::Access;
using Monarc::RHI::BufferBarrier;
using Monarc::RHI::BufferHandle;
using Monarc::RHI::BufferUsage;
using Monarc::RHI::GlobalBarrier;
using Monarc::RHI::LoadOp;
using Monarc::RHI::MemoryLocation;
using Monarc::RHI::PipelineStage;
using Monarc::RHI::StoreOp;
using Monarc::RHI::TextureBarrier;
using Monarc::RHI::TextureHandle;
using Monarc::RHI::TextureLayout;
using Monarc::RHI::TextureUsage;
using Monarc::RHI::Detail::FindMemoryType;
using Monarc::RHI::Detail::FromVulkan;
using Monarc::RHI::Detail::FromVulkanAccess;
using Monarc::RHI::Detail::FromVulkanStages;
using Monarc::RHI::Detail::kNoMemoryType;
using Monarc::RHI::Detail::ToVulkan;
using Monarc::RHI::Detail::ToVulkanBit;

namespace {

/// Every enumerator of each of the three sets, once.
///
/// Duplicated from Monarc.RHI/Tests/TestBarrier.cpp on purpose, and that file is where the
/// lists' *completeness* is checked -- the same division `kAllFormats` already has between
/// TestTypes.cpp and TestVulkanTranslate.cpp. Sharing them would mean a header in
/// `Monarc.RHI/Tests/` that `Monarc.RHI.Vulkan/Tests/` includes across a module boundary, which
/// is worse than a list that is checked in one place and used in two.
constexpr PipelineStage kAllStages[] = {
    PipelineStage::None,
    PipelineStage::DrawIndirect,
    PipelineStage::VertexShader,
    PipelineStage::FragmentShader,
    PipelineStage::EarlyFragmentTests,
    PipelineStage::LateFragmentTests,
    PipelineStage::ColorAttachmentOutput,
    PipelineStage::ComputeShader,
    PipelineStage::Copy,
    PipelineStage::Blit,
    PipelineStage::Resolve,
    PipelineStage::Clear,
    PipelineStage::Host,
    PipelineStage::AllGraphics,
    PipelineStage::AllCommands,
};

constexpr Access kAllAccesses[] = {
    Access::None,
    Access::IndirectCommandRead,
    Access::IndexRead,
    Access::VertexAttributeRead,
    Access::UniformRead,
    Access::ShaderSampledRead,
    Access::ShaderStorageRead,
    Access::ShaderStorageWrite,
    Access::ColorAttachmentRead,
    Access::ColorAttachmentWrite,
    Access::DepthStencilAttachmentRead,
    Access::DepthStencilAttachmentWrite,
    Access::TransferRead,
    Access::TransferWrite,
    Access::HostRead,
    Access::HostWrite,
    Access::MemoryRead,
    Access::MemoryWrite,
};

constexpr TextureLayout kAllLayouts[] = {
    TextureLayout::Undefined,
    TextureLayout::General,
    TextureLayout::ColorAttachment,
    TextureLayout::DepthStencilAttachment,
    TextureLayout::DepthStencilReadOnly,
    TextureLayout::ShaderReadOnly,
    TextureLayout::TransferSource,
    TextureLayout::TransferDestination,
};

/// The union of every named bit in a set.
template <typename Enum, Monarc::usize N>
[[nodiscard]] constexpr Enum UnionOf(const Enum (&values)[N]) {
    Enum all = static_cast<Enum>(0);
    for (Monarc::usize i = 0; i < N; ++i) {
        all = all | values[i];
    }
    return all;
}

/// A bit no enumerator of either flag set names. Reachable only through a cast, which is the
/// point: the enums have a fixed underlying type, so this value exists whether or not anyone
/// means it to.
constexpr Monarc::u32 kUnnamedBit = 1U << 30;

constexpr TextureHandle kTexture = TextureHandle::ForTesting(3, 1);
constexpr BufferHandle  kBuffer  = BufferHandle::ForTesting(7, 2);

// A non-dispatchable Vulkan handle is an opaque pointer on a 64-bit target and a `uint64_t` on
// a 32-bit one, and the casts below assume the first. Asserted rather than assumed, so a
// 32-bit configuration fails to compile here with a sentence rather than at the cast with a
// diagnostic about reinterpret_cast between integer types.
static_assert(sizeof(VkImage) == sizeof(Monarc::uptr),
              "these fabricated handles assume a non-dispatchable Vulkan handle is "
              "pointer-sized, which holds on 64-bit targets");

/// A `VkImage` value that is not `VK_NULL_HANDLE` and is never dereferenced.
///
/// The barrier translators copy the handle into the structure they build and do nothing else
/// with it -- Translate.h's membership note says so explicitly -- so a fabricated one is a
/// legitimate argument, and it is the only way to assert that the handle is carried across at
/// all with no device to make a real image on.
[[nodiscard]] VkImage FabricatedImage() {
    return reinterpret_cast<VkImage>(static_cast<Monarc::uptr>(0x1234));
}

[[nodiscard]] VkBuffer FabricatedBuffer() {
    return reinterpret_cast<VkBuffer>(static_cast<Monarc::uptr>(0x5678));
}

/// What a `CollisionReport` reads when no two members of a set translate alike.
constexpr std::string_view kNoCollision = "no two collide";

/// Monarc.RHI/Tests/TestBarrier.cpp's `CollisionReport` and `FirstCollision`, duplicated for
/// the reason the enumerator lists above are: sharing them would mean a header in
/// `Monarc.RHI/Tests/` included across a module boundary. That file carries the argument for
/// the shape -- one assertion per property, the colliding pair named in it, and the
/// measurements that decided against one assertion per pair.
struct CollisionReport {
    char text[160] = "no two collide";

    [[nodiscard]] std::string_view View() const { return std::string_view(text); }
};

template <typename Enum, Monarc::usize N, typename Key>
[[nodiscard]] CollisionReport FirstCollision(const Enum (&values)[N], Key key) {
    for (Monarc::usize i = 0; i < N; ++i) {
        for (Monarc::usize j = i + 1; j < N; ++j) {
            if (key(values[i]) == key(values[j])) {
                CollisionReport report;
                const auto written = std::format_to_n(
                    report.text, sizeof(report.text) - 1, "{}[{}] and {}[{}] collide",
                    ToString(values[i]), static_cast<Monarc::u32>(values[i]),
                    ToString(values[j]), static_cast<Monarc::u32>(values[j]));
                *written.out = '\0';
                return report;
            }
        }
    }
    return CollisionReport{};
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Per-enumerator translation: exhaustive, injective, and reversible.
// ---------------------------------------------------------------------------------------

TEST_CASE("each pipeline stage maps to the Vulkan bit that means the same thing") {
    // Spot-named rather than looped, because a loop can only check the *shape* of the mapping
    // and these are the rows a copy-paste gets wrong. Every one of them is a stage A3 or the
    // barrier tests below actually name.
    CHECK(ToVulkanBit(PipelineStage::None) == VK_PIPELINE_STAGE_2_NONE);
    CHECK(ToVulkanBit(PipelineStage::ColorAttachmentOutput) ==
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(ToVulkanBit(PipelineStage::Copy) == VK_PIPELINE_STAGE_2_COPY_BIT);
    CHECK(ToVulkanBit(PipelineStage::Host) == VK_PIPELINE_STAGE_2_HOST_BIT);
    CHECK(ToVulkanBit(PipelineStage::AllCommands) == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    // Blit, Resolve and Clear are the three most likely to be conflated, since Vulkan 1.0 had
    // one TRANSFER stage covering all of them and the 1.3 bits are separate.
    CHECK(ToVulkanBit(PipelineStage::Blit) == VK_PIPELINE_STAGE_2_BLIT_BIT);
    CHECK(ToVulkanBit(PipelineStage::Resolve) == VK_PIPELINE_STAGE_2_RESOLVE_BIT);
    CHECK(ToVulkanBit(PipelineStage::Clear) == VK_PIPELINE_STAGE_2_CLEAR_BIT);
}

TEST_CASE("no two pipeline stages map to one Vulkan bit, and only None maps to zero") {
    // Injectivity, which a duplicated case label breaks: two stages sharing a bit make
    // `FromVulkanStages` unable to tell them apart.
    //
    // **This line is the diagnosis and not the detector, and the distinction is worth stating
    // because the 105-comparison loop it replaces was justified as the detector.** The
    // round-trip case below already fails on any duplicate -- if two stages map to one bit,
    // the reverse translator can only return one of them, so the other's round trip is red --
    // and it does so independently of this. What it cannot do is say which two: it compares
    // two enumerator values and prints two integers. Measured, by giving `EarlyFragmentTests`
    // the vertex-shader bit -- a pair neither spot check above pins: the round trip reported
    // `CHECK( 2 == 8 )`, the whole-mask round trip `CHECK( 16375 == 16383 )`, and this line
    // `VertexShader[2] and EarlyFragmentTests[8] collide`. Three reds, and the 105-assertion
    // version scored the same three, since a single duplicate collides exactly one pair. One
    // assertion for the name is worth keeping; a hundred and five for a property two other
    // cases already detect is not.
    CHECK(FirstCollision(kAllStages, [](PipelineStage stage) {
              return ToVulkanBit(stage);
          }).View() == kNoCollision);

    // Zero is a legal stage mask meaning "no synchronisation", so a row accidentally left at
    // zero is not something Vulkan would reject -- it is a synchronisation hole. Kept as one
    // assertion per enumerator, and that is not an inconsistency with the line above: these
    // are fifteen independent facts, one per row, where the pairs were one property asserted
    // 105 times.
    for (const PipelineStage stage : kAllStages) {
        CHECK((ToVulkanBit(stage) == 0) == (stage == PipelineStage::None));
    }
}

TEST_CASE("no two accesses map to one Vulkan bit, and only None maps to zero") {
    // The stage case's note applies unchanged: the round trip detects a duplicate, this names
    // the pair, and nine of the eighteen rows are unpinned by the spot checks above.
    CHECK(FirstCollision(kAllAccesses, [](Access access) {
              return ToVulkanBit(access);
          }).View() == kNoCollision);

    for (const Access access : kAllAccesses) {
        CHECK((ToVulkanBit(access) == 0) == (access == Access::None));
    }
}

TEST_CASE("each access maps to the Vulkan bit that means the same thing") {
    CHECK(ToVulkanBit(Access::None) == VK_ACCESS_2_NONE);
    CHECK(ToVulkanBit(Access::ColorAttachmentWrite) == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(ToVulkanBit(Access::TransferRead) == VK_ACCESS_2_TRANSFER_READ_BIT);
    CHECK(ToVulkanBit(Access::TransferWrite) == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(ToVulkanBit(Access::HostRead) == VK_ACCESS_2_HOST_READ_BIT);

    // The read/write pairs, because a barrier that named the write side where the read side
    // belonged would synchronise the wrong direction and validation would not object.
    CHECK(ToVulkanBit(Access::ShaderStorageRead) == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    CHECK(ToVulkanBit(Access::ShaderStorageWrite) == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    CHECK(ToVulkanBit(Access::MemoryRead) == VK_ACCESS_2_MEMORY_READ_BIT);
    CHECK(ToVulkanBit(Access::MemoryWrite) == VK_ACCESS_2_MEMORY_WRITE_BIT);
}

TEST_CASE("each texture layout maps to the Vulkan layout that means the same thing") {
    CHECK(ToVulkan(TextureLayout::Undefined) == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(ToVulkan(TextureLayout::General) == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(ToVulkan(TextureLayout::ColorAttachment) ==
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(ToVulkan(TextureLayout::DepthStencilAttachment) ==
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    CHECK(ToVulkan(TextureLayout::DepthStencilReadOnly) ==
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    CHECK(ToVulkan(TextureLayout::ShaderReadOnly) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(ToVulkan(TextureLayout::TransferSource) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(ToVulkan(TextureLayout::TransferDestination) ==
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // **No distinctness assertion here, and its 28-comparison loop was deleted rather than
    // collapsed.** The eight `CHECK`s above are exhaustive over `kAllLayouts` and pin each
    // row to a distinct Vulkan layout, so two rows sharing one is already a named failure on
    // whichever row was changed -- and the round-trip case below catches it a second time.
    // A third statement of the same fact could not fail without one of those failing first,
    // which is the category this phase's review pass has been deleting. The two flag sets
    // keep theirs because their spot checks are not exhaustive; see them for what that line
    // is for.
}

TEST_CASE("every stage, access and layout survives a round trip through Vulkan") {
    // The property the phase plan asks for, at the enumerator level. It is what makes the
    // reverse translators' lack of a compile-time exhaustiveness guarantee tolerable: a row
    // missing from `FromVulkanStages`' if-chain turns one of these red.
    for (const PipelineStage stage : kAllStages) {
        CHECK(FromVulkanStages(ToVulkan(stage)) == stage);
    }
    for (const Access access : kAllAccesses) {
        CHECK(FromVulkanAccess(ToVulkan(access)) == access);
    }
    for (const TextureLayout layout : kAllLayouts) {
        CHECK(FromVulkan(ToVulkan(layout)) == layout);
    }
}

// ---------------------------------------------------------------------------------------
// Masks.
// ---------------------------------------------------------------------------------------

TEST_CASE("a mask translates to the union of its bits, and back again") {
    constexpr PipelineStage stages = PipelineStage::Copy | PipelineStage::Host;
    CHECK(ToVulkan(stages) ==
          (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_HOST_BIT));
    CHECK(FromVulkanStages(ToVulkan(stages)) == stages);

    constexpr Access accesses = Access::TransferWrite | Access::HostRead;
    CHECK(ToVulkan(accesses) == (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT));
    CHECK(FromVulkanAccess(ToVulkan(accesses)) == accesses);

    // Distributive over the union, which is the property the bit-walk in `ForEachBit` exists
    // to provide: a translator that switched on the mask as a whole would return its
    // not-recognised fallback for every mask of more than one bit.
    CHECK(ToVulkan(stages) ==
          (ToVulkan(PipelineStage::Copy) | ToVulkan(PipelineStage::Host)));
    CHECK(ToVulkan(accesses) == (ToVulkan(Access::TransferWrite) | ToVulkan(Access::HostRead)));
}

TEST_CASE("the mask of every named bit round-trips, which no smaller mask would prove") {
    // Every stage at once. A mask this wide is what catches a `ForEachBit` that stopped at the
    // first bit, or a reverse translator whose accumulation replaced rather than accumulated:
    // both pass every single-bit case above.
    constexpr PipelineStage allStages = UnionOf(kAllStages);
    CHECK(FromVulkanStages(ToVulkan(allStages)) == allStages);

    constexpr Access allAccesses = UnionOf(kAllAccesses);
    CHECK(FromVulkanAccess(ToVulkan(allAccesses)) == allAccesses);
}

TEST_CASE("an empty mask translates to zero and not to the not-recognised fallback") {
    // `PipelineStage::None` is zero, so the bit walk never runs -- which means this is really
    // asserting that the walk's *initial value* is zero rather than its fallback. A version
    // that seeded the result with the unrecognised-value answer would return ALL_COMMANDS for
    // a barrier that asked for no synchronisation at all.
    CHECK(ToVulkan(PipelineStage::None) == 0);
    CHECK(ToVulkan(Access::None) == 0);
    CHECK(FromVulkanStages(0) == PipelineStage::None);
    CHECK(FromVulkanAccess(0) == Access::None);
}

TEST_CASE("a bit no enumerator names translates conservatively, never to nothing") {
    // Translate.h's stated decision, and the one place this file's expectations differ from
    // `ToVulkan(Format)`'s. A format has an obviously-invalid answer that makes Vulkan reject
    // the call; a stage mask has none, because every bit pattern is a legal mask. So the choice
    // is between under- and over-synchronising, and only one of those is safe.
    CHECK(ToVulkanBit(static_cast<PipelineStage>(kUnnamedBit)) ==
          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(ToVulkanBit(static_cast<Access>(kUnnamedBit)) ==
          (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));

    // And through the mask walk, so an unnamed bit riding along with named ones does not lose
    // the named ones either.
    CHECK(ToVulkan(PipelineStage::Copy | static_cast<PipelineStage>(kUnnamedBit)) ==
          (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));

    // A layout does have an invalid answer, and it is not UNDEFINED: UNDEFINED is a legal
    // `oldLayout` that discards the texture's contents, so using it here would turn a bad value
    // into silent data loss. MAX_ENUM is rejected wherever it appears.
    CHECK(ToVulkan(static_cast<TextureLayout>(4242)) == VK_IMAGE_LAYOUT_MAX_ENUM);
}

TEST_CASE("a Vulkan bit Monarc does not model is dropped rather than guessed at") {
    // The reverse direction is not exhaustive and cannot be -- Vulkan names far more stages
    // than Monarc does. What it must not do is invent one: `VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT`
    // is a real stage Monarc has no enumerator for, and the honest answer is to leave it out
    // while keeping everything beside it.
    CHECK(FromVulkanStages(VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT) == PipelineStage::None);
    CHECK(FromVulkanStages(VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                           VK_PIPELINE_STAGE_2_COPY_BIT) == PipelineStage::Copy);

    CHECK(FromVulkanAccess(VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT) == Access::None);
    CHECK(FromVulkan(VK_IMAGE_LAYOUT_PREINITIALIZED) == TextureLayout::Undefined);
}

// ---------------------------------------------------------------------------------------
// The barrier structures.
// ---------------------------------------------------------------------------------------

TEST_CASE("a global barrier translates to a structure Vulkan will accept, and back unchanged") {
    constexpr GlobalBarrier barrier{PipelineStage::ColorAttachmentOutput, PipelineStage::Copy,
                                    Access::ColorAttachmentWrite, Access::TransferRead};

    const VkMemoryBarrier2 translated = ToVulkan(barrier);

    // sType, and it is not a formality: a structure handed to vkCmdPipelineBarrier2 with a
    // zero sType is rejected, and nothing else in this pipeline would notice the omission --
    // `{}` initialises it to zero, so forgetting the line is silent until a device runs it.
    CHECK(translated.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    CHECK(translated.srcStageMask == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(translated.dstStageMask == VK_PIPELINE_STAGE_2_COPY_BIT);
    CHECK(translated.srcAccessMask == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(translated.dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);

    // Round-tripping the whole struct rather than field by field, which is the property the
    // plan names. Every field of `barrier` is distinct from every other, so a translator that
    // wrote src where dst belonged fails this.
    CHECK(FromVulkan(translated) == barrier);
}

TEST_CASE("a barrier with no state change translates to a real structure, not to nothing") {
    // "Not silently dropped", at the level where dropping could actually happen. Barrier.h
    // states that `GlobalBarrier{}` is a value a caller can mean, and this is what says
    // translation honours it: a fully-formed VkMemoryBarrier2 with NONE masks, which is a
    // barrier Vulkan executes rather than an empty structure it would reject.
    const VkMemoryBarrier2 translated = ToVulkan(GlobalBarrier{});
    CHECK(translated.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    CHECK(translated.srcStageMask == VK_PIPELINE_STAGE_2_NONE);
    CHECK(translated.dstStageMask == VK_PIPELINE_STAGE_2_NONE);
    CHECK(translated.srcAccessMask == VK_ACCESS_2_NONE);
    CHECK(translated.dstAccessMask == VK_ACCESS_2_NONE);
    CHECK(FromVulkan(translated) == GlobalBarrier{});

    // And the texture variant's own no-change form: equal layouts survive translation as equal
    // layouts, rather than being folded to UNDEFINED -- which would discard the contents.
    constexpr TextureBarrier unchanged(kTexture, TextureLayout::General, TextureLayout::General,
                                       PipelineStage::None, PipelineStage::None, Access::None,
                                       Access::None);
    const VkImageMemoryBarrier2 image = ToVulkan(unchanged, VK_NULL_HANDLE);
    CHECK(image.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    CHECK(image.oldLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(image.newLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(FromVulkan(image, kTexture) == unchanged);
}

TEST_CASE("a buffer barrier translates whole and reverses unchanged") {
    constexpr BufferBarrier barrier{kBuffer, PipelineStage::Copy, PipelineStage::Host,
                                    Access::TransferWrite, Access::HostRead};

    const VkBufferMemoryBarrier2 translated = ToVulkan(barrier, FabricatedBuffer());

    CHECK(translated.sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);

    // The handle is carried across. Asserted directly rather than through the round trip,
    // because the reverse takes the RHI handle as a parameter -- feeding it back in would make
    // that half of the round trip vacuous. This is the assertion that fails if the translator
    // never assigns `buffer`.
    CHECK(translated.buffer == FabricatedBuffer());

    // Whole-buffer range and no queue-family transfer, which Translate.h states. VK_WHOLE_SIZE
    // rather than a size the translator would have to be told, and IGNORED on both sides
    // because Monarc has one queue -- a barrier that named a real family on one side and
    // IGNORED on the other is a validation error.
    CHECK(translated.offset == 0);
    CHECK(translated.size == VK_WHOLE_SIZE);
    CHECK(translated.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(translated.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);

    CHECK(FromVulkan(translated, kBuffer) == barrier);
}

TEST_CASE("a texture barrier translates whole, including the subresource range") {
    constexpr TextureBarrier barrier(kTexture, TextureLayout::ColorAttachment,
                                     TextureLayout::TransferSource,
                                     PipelineStage::ColorAttachmentOutput, PipelineStage::Copy,
                                     Access::ColorAttachmentWrite, Access::TransferRead);

    const VkImageMemoryBarrier2 translated = ToVulkan(barrier, FabricatedImage());

    CHECK(translated.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    CHECK(translated.image == FabricatedImage());
    CHECK(translated.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(translated.newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(translated.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(translated.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);

    // Colour aspect, and every mip and layer. The aspect is a consequence of `Format` having
    // no depth format rather than a simplification -- Translate.h says so -- and a barrier with
    // an aspect the image does not have is a validation error rather than a wrong picture.
    CHECK(translated.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(translated.subresourceRange.baseMipLevel == 0);
    CHECK(translated.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS);
    CHECK(translated.subresourceRange.baseArrayLayer == 0);
    CHECK(translated.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS);

    CHECK(FromVulkan(translated, kTexture) == barrier);
}

TEST_CASE("the two barriers the readback records survive translation exactly") {
    // These are not arbitrary values: they are the two barriers Task 3's readback test
    // actually records, so this case is the device-free half of the coverage for the path that
    // runs on real hardware. If one of them translated wrongly, the readback would fail on a
    // GPU and be silent in CI; this is what makes it loud in CI too.
    constexpr TextureBarrier toAttachment(kTexture, TextureLayout::Undefined,
                                          TextureLayout::ColorAttachment, PipelineStage::None,
                                          PipelineStage::ColorAttachmentOutput, Access::None,
                                          Access::ColorAttachmentWrite);
    constexpr TextureBarrier toTransfer(kTexture, TextureLayout::ColorAttachment,
                                        TextureLayout::TransferSource,
                                        PipelineStage::ColorAttachmentOutput,
                                        PipelineStage::Copy, Access::ColorAttachmentWrite,
                                        Access::TransferRead);

    CHECK(FromVulkan(ToVulkan(toAttachment, VK_NULL_HANDLE), kTexture) == toAttachment);
    CHECK(FromVulkan(ToVulkan(toTransfer, VK_NULL_HANDLE), kTexture) == toTransfer);

    // The discard is deliberate and worth pinning: the first barrier's old layout is UNDEFINED,
    // which tells the driver the texture's contents need not be preserved -- correct for a
    // texture about to be cleared, and wrong for one about to be blended into.
    CHECK(ToVulkan(toAttachment, VK_NULL_HANDLE).oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
}

// ---------------------------------------------------------------------------------------
// Resource creation and rendering, which are not part of the barrier model.
// ---------------------------------------------------------------------------------------

TEST_CASE("texture and buffer usages translate to the Vulkan bits with the same meaning") {
    CHECK(ToVulkanBit(TextureUsage::None) == 0);
    CHECK(ToVulkanBit(TextureUsage::ColorAttachment) == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    CHECK(ToVulkanBit(TextureUsage::TransferSource) == VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // Source against destination is the pair worth naming: swapping them creates a buffer that
    // cannot receive a copy, which is the exact failure the readback test would report as a
    // validation error rather than as wrong bytes.
    CHECK(ToVulkanBit(BufferUsage::None) == 0);
    CHECK(ToVulkanBit(BufferUsage::TransferSource) == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CHECK(ToVulkanBit(BufferUsage::TransferDestination) == VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // The mask the readback's texture is created with.
    CHECK(ToVulkan(TextureUsage::ColorAttachment | TextureUsage::TransferSource) ==
          (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
}

TEST_CASE("load and store operations translate to the Vulkan ones with the same meaning") {
    // LoadOp::Clear is how Monarc clears -- there is no separate clear command in the
    // interface -- so this row is the one the whole readback rests on.
    CHECK(ToVulkan(LoadOp::Clear) == VK_ATTACHMENT_LOAD_OP_CLEAR);
    CHECK(ToVulkan(LoadOp::Load) == VK_ATTACHMENT_LOAD_OP_LOAD);
    CHECK(ToVulkan(LoadOp::DontCare) == VK_ATTACHMENT_LOAD_OP_DONT_CARE);

    CHECK(ToVulkan(StoreOp::Store) == VK_ATTACHMENT_STORE_OP_STORE);
    CHECK(ToVulkan(StoreOp::DontCare) == VK_ATTACHMENT_STORE_OP_DONT_CARE);

    // A value no enumerator names becomes DONT_CARE and not LOAD, which Translate.h explains:
    // of the three, LOAD is the one that would render over whatever was there and produce a
    // frame that looks almost right. DONT_CARE leaves the attachment undefined, which cannot
    // be mistaken for a correct result.
    CHECK(ToVulkan(static_cast<LoadOp>(99)) == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    CHECK(ToVulkan(static_cast<StoreOp>(99)) == VK_ATTACHMENT_STORE_OP_DONT_CARE);
}

TEST_CASE("a host-visible buffer asks for coherent memory as well as visible") {
    // Coherence is what lets `MapBufferForRead` hand back a span a caller may simply read:
    // without it, a device write is available but not visible until
    // vkInvalidateMappedMemoryRanges, which this module does not call and does not resolve.
    // So this assertion and that absence are one decision.
    CHECK(ToVulkan(MemoryLocation::HostVisible) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    CHECK(ToVulkan(MemoryLocation::DeviceLocal) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

// ---------------------------------------------------------------------------------------
// Memory type selection, against layouts this machine does not have.
// ---------------------------------------------------------------------------------------

namespace {

/// A memory layout built by hand, as a driver would report one.
[[nodiscard]] VkPhysicalDeviceMemoryProperties MemoryLayout(
    std::initializer_list<VkMemoryPropertyFlags> types) {
    // `memoryTypes` is a fixed array of VK_MAX_MEMORY_TYPES, so a longer list would be an
    // overrun in the test's own helper. Nothing here passes more than three; the check is so
    // that a later case which does is told, rather than corrupting the stack.
    MONARC_CHECK(types.size() <= VK_MAX_MEMORY_TYPES,
                 "MemoryLayout was given more types than VkPhysicalDeviceMemoryProperties "
                 "can hold");

    VkPhysicalDeviceMemoryProperties properties{};
    properties.memoryTypeCount = static_cast<Monarc::u32>(types.size());
    Monarc::u32 index          = 0;
    for (const VkMemoryPropertyFlags flags : types) {
        properties.memoryTypes[index].propertyFlags = flags;
        ++index;
    }
    return properties;
}

constexpr VkMemoryPropertyFlags kDeviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr VkMemoryPropertyFlags kHostCoherent =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

/// Every memory type is allowed. `VkMemoryRequirements::memoryTypeBits` is a mask over the
/// device's type list, and this is what a resource with no restrictions reports.
constexpr Monarc::u32 kAnyType = 0xFFFF'FFFFU;

}  // namespace

TEST_CASE("memory selection takes the first type that is both allowed and sufficient") {
    const VkPhysicalDeviceMemoryProperties properties =
        MemoryLayout({kDeviceLocal, kHostCoherent, kHostCoherent | kDeviceLocal});

    CHECK(FindMemoryType(properties, kAnyType, kDeviceLocal) == 0);
    CHECK(FindMemoryType(properties, kAnyType, kHostCoherent) == 1);

    // A *subset* test and not equality. The type at index 2 has more properties than were
    // asked for and is still a valid answer -- and on an integrated part like the Intel UHD
    // 730, host-visible memory that is also device-local is the only kind there is, so an
    // equality test would find nothing at all on that adapter.
    CHECK(FindMemoryType(properties, 1U << 2, kHostCoherent) == 2);
}

TEST_CASE("a memory type the resource may not use is skipped however well it matches") {
    const VkPhysicalDeviceMemoryProperties properties =
        MemoryLayout({kHostCoherent, kHostCoherent});

    // Type 0 matches the properties perfectly and the resource cannot use it. Dropping the
    // `allowedTypeBits` half of the test would return 0 here, and the bind that followed would
    // be a validation error -- the far side of a mapped pointer into the wrong memory.
    CHECK(FindMemoryType(properties, 1U << 1, kHostCoherent) == 1);
    CHECK(FindMemoryType(properties, 0, kHostCoherent) == kNoMemoryType);
}

TEST_CASE("a device with no suitable memory type reports so rather than picking one") {
    // A layout with no host-visible type at all, which is not a device that exists but is
    // exactly what the guard is for: `AllocateFor` turns `kNoMemoryType` into
    // ErrorCode::Unsupported, where a fallback to index 0 would hand back device-local memory
    // for a host-visible request and fail at vkMapMemory instead.
    const VkPhysicalDeviceMemoryProperties deviceOnly = MemoryLayout({kDeviceLocal});
    CHECK(FindMemoryType(deviceOnly, kAnyType, kHostCoherent) == kNoMemoryType);

    // And a device reporting no memory types whatsoever, so the loop's bound is what stops it
    // rather than the properties happening not to match.
    const VkPhysicalDeviceMemoryProperties none = MemoryLayout({});
    CHECK(FindMemoryType(none, kAnyType, kDeviceLocal) == kNoMemoryType);
}
