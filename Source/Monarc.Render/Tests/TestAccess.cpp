// The one translation unit in Monarc.Render.Tests that provides doctest's main(), which is
// hand-written rather than generated because this suite has to install a log sink before any
// case runs -- see `TestSupport::LogCapture` for why.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/RHI/Barrier.h>
#include <Monarc/Render/Access.h>

#include <TestSupport/GraphLogCapture.h>

#include <iterator>
#include <string_view>
#include <type_traits>

using Monarc::Render::AccessRequirement;
using Monarc::Render::AccessScope;
using Monarc::Render::IsWrite;
using Monarc::Render::RequirementOf;
using Monarc::Render::ResourceAccess;
using Monarc::Render::ToString;
using Monarc::RHI::Access;
using Monarc::RHI::PipelineStage;
using Monarc::RHI::TextureLayout;

namespace {

constexpr ResourceAccess kAllAccesses[] = {
    ResourceAccess::ColorAttachmentRead,        ResourceAccess::ColorAttachmentWrite,
    ResourceAccess::DepthStencilAttachmentRead, ResourceAccess::DepthStencilAttachmentWrite,
    ResourceAccess::SampledRead,                ResourceAccess::StorageRead,
    ResourceAccess::StorageWrite,               ResourceAccess::IndirectRead,
};

// ---------------------------------------------------------------------------------------
// The list above is only worth iterating if it is complete, and a comment saying it is is not
// what makes it so. These are the two `static_assert`s that do, and the mechanism is the one
// Monarc.RHI.Vulkan/Tests/TestVulkanBarrierTranslate.cpp introduced after `kAllLayouts` went
// stale in Phase A3 Task 4 and nothing said so.
//
// C++ cannot ask an enum how many enumerators it has, and `ResourceAccess` has no `Count`
// sentinel -- deliberately, since one would need a case in `RequirementOf` and in `ToString`,
// both `default`-less. What `/w44062` on MSVC and `-Wswitch` on clang-cl give instead is the
// converse: a `default`-less switch is a fatal warning the day an enumerator it does not name
// appears. So `IsEnumerator` below is that switch, and the pair closes both directions:
//
//   1. The value one past the list's last index is **not** an enumerator. A missing enumerator
//      makes it one, and `IsEnumerator` cannot be taught the new value without failing to
//      compile first -- so the sequence is: add an enumerator, the build breaks in
//      `RequirementOf` and here, name it in both, and this assertion breaks until the list
//      grows too.
//   2. The list is in the enum's own order with no gaps and no repeats, which is what makes
//      "one past the last index" the right value to probe -- and it catches a list that
//      swapped a missing row for a duplicated one, which (1) alone would not.
//
// Both rest on `ResourceAccess` being sequential from zero, which Access.h keeps.
// ---------------------------------------------------------------------------------------

/// Whether `access` is an enumerator of `ResourceAccess`, as a `default`-less switch.
///
/// The trailing `return false` is not dead: the enum has a fixed underlying type and can hold
/// values outside its enumerator set -- which is exactly what the assertion below hands it --
/// and with every case covered MSVC still asks what those return (`warning C4715`, fatal
/// through `/WX`).
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

static_assert(IsSequenceInOrder(kAllAccesses),
              "kAllAccesses must be the ResourceAccess enumerators in declaration order, since "
              "the enum is sequential from zero -- see the note above for why the completeness "
              "assertion below depends on it");
static_assert(!IsEnumerator(static_cast<ResourceAccess>(std::size(kAllAccesses))),
              "kAllAccesses is missing a ResourceAccess enumerator: the value one past its "
              "last index is a named access, so this list no longer covers the set the cases "
              "below iterate");

/// The stages a shader-visible access is synchronised against.
///
/// Named once rather than repeated three times below, because the three shader accesses share
/// it for a stated reason -- a graph knows a texture is sampled without knowing by which
/// stage -- and three copies of the mask would let one drift.
constexpr PipelineStage kShaderStages =
    PipelineStage::VertexShader | PipelineStage::FragmentShader | PipelineStage::ComputeShader;

constexpr PipelineStage kDepthStages =
    PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests;

}  // namespace

// ---------------------------------------------------------------------------------------
// Every row of `RequirementOf`, asserted at compile time against the value ADR-0005's model
// says it should carry.
//
// **These are the assertions barrier derivation rests on, so they name values rather than
// relations.** A test that checked "the colour accesses agree with each other" would pass with
// both of them wrong in the same direction; Task 3's headline case asserts the two barriers
// Phase A3 measured, and those barriers are made of exactly the stage, access and layout
// below. Swapping any two rows of `RequirementOf`, or dropping a stage from a mask, fails here.
//
// `static_assert` rather than `CHECK` because `RequirementOf` is `constexpr` -- see Access.h,
// which is `constexpr` in the header for this reason. A wrong row is then a build failure
// rather than a red test.
// ---------------------------------------------------------------------------------------

static_assert(RequirementOf(ResourceAccess::ColorAttachmentRead) ==
              AccessRequirement{{PipelineStage::ColorAttachmentOutput, Access::ColorAttachmentRead},
                                true, TextureLayout::ColorAttachment, false});

static_assert(RequirementOf(ResourceAccess::ColorAttachmentWrite) ==
              AccessRequirement{
                  {PipelineStage::ColorAttachmentOutput, Access::ColorAttachmentWrite},
                  true, TextureLayout::ColorAttachment, true});

static_assert(RequirementOf(ResourceAccess::DepthStencilAttachmentRead) ==
              AccessRequirement{{kDepthStages, Access::DepthStencilAttachmentRead}, true,
                                TextureLayout::DepthStencilReadOnly, false});

static_assert(RequirementOf(ResourceAccess::DepthStencilAttachmentWrite) ==
              AccessRequirement{{kDepthStages, Access::DepthStencilAttachmentWrite}, true,
                                TextureLayout::DepthStencilAttachment, true});

static_assert(RequirementOf(ResourceAccess::SampledRead) ==
              AccessRequirement{{kShaderStages, Access::ShaderSampledRead}, true,
                                TextureLayout::ShaderReadOnly, false});

static_assert(RequirementOf(ResourceAccess::StorageRead) ==
              AccessRequirement{{kShaderStages, Access::ShaderStorageRead}, true,
                                TextureLayout::General, false});

static_assert(RequirementOf(ResourceAccess::StorageWrite) ==
              AccessRequirement{{kShaderStages, Access::ShaderStorageWrite}, true,
                                TextureLayout::General, true});

static_assert(RequirementOf(ResourceAccess::IndirectRead) ==
              AccessRequirement{{PipelineStage::DrawIndirect, Access::IndirectCommandRead}, false,
                                TextureLayout::Undefined, false});

// The colour attachment's two accesses share a layout, and **that is what makes a
// read-modify-write attachment representable in one pass at all.** A pass declaring both asks
// for `ColorAttachment` twice rather than for two layouts at once, which is the difference
// between the legal case the plan names and the depth case Access.h states as an open
// question. Giving `ColorAttachmentRead` its own layout -- `General`, say -- would break this
// and nothing else in the file, which is why it is asserted separately from the rows above.
static_assert(RequirementOf(ResourceAccess::ColorAttachmentRead).layout ==
              RequirementOf(ResourceAccess::ColorAttachmentWrite).layout);

// The depth pair does *not* share one, and the assertion is here so that the day someone
// decides it should, the decision is visible rather than incidental. Access.h's note on
// `DepthStencilAttachmentWrite` is the argument.
static_assert(RequirementOf(ResourceAccess::DepthStencilAttachmentRead).layout !=
              RequirementOf(ResourceAccess::DepthStencilAttachmentWrite).layout);

TEST_CASE("IsWrite agrees with the direction each access names") {
    // Not a tautology despite `IsWrite` being defined as `RequirementOf(...).writes`: what is
    // asserted is which accesses write, and the answer comes from the enumerator's name rather
    // than from the function. Marking `SampledRead` as a write, or `StorageWrite` as a read,
    // passes every other case in this file -- the rows above would have to be changed to
    // match, and then this is what disagrees.
    CHECK_FALSE(IsWrite(ResourceAccess::ColorAttachmentRead));
    CHECK(IsWrite(ResourceAccess::ColorAttachmentWrite));
    CHECK_FALSE(IsWrite(ResourceAccess::DepthStencilAttachmentRead));
    CHECK(IsWrite(ResourceAccess::DepthStencilAttachmentWrite));
    CHECK_FALSE(IsWrite(ResourceAccess::SampledRead));
    CHECK_FALSE(IsWrite(ResourceAccess::StorageRead));
    CHECK(IsWrite(ResourceAccess::StorageWrite));
    CHECK_FALSE(IsWrite(ResourceAccess::IndirectRead));

    // Exactly three of the eight write, which is what catches a *new* enumerator that was
    // given the wrong direction: the completeness guards force it into `kAllAccesses`, and
    // then this count is what disagrees.
    int writes = 0;
    for (const ResourceAccess access : kAllAccesses) {
        writes += IsWrite(access) ? 1 : 0;
    }
    CHECK(writes == 3);
}

TEST_CASE("every access carries a real synchronisation scope") {
    // A scope of `None`/`None` is ADR-0005's "execution dependency only" barrier -- ordering
    // with no cache maintenance -- and no *declared access* can mean it: a pass that touches a
    // resource touches it at some stage, in some way. An enumerator added with its scope left
    // at the struct's defaults would derive a barrier that synchronises nothing, and that is
    // exactly the silent failure this case exists to catch.
    for (const ResourceAccess access : kAllAccesses) {
        CAPTURE(ToString(access));
        const AccessRequirement requirement = RequirementOf(access);
        CHECK(requirement.scope.stage != PipelineStage::None);
        CHECK(requirement.scope.access != Access::None);
    }
}

TEST_CASE("exactly one access names no texture, and it is the buffer-only one") {
    // `IndirectRead` is the whole of the exception, and Access.h's membership note turns on
    // that being true: the enum names a buffer access because it is one switch row, and
    // `PassBuilder` refuses it on a texture. A second access quietly marked `namesTexture =
    // false` would be silently unusable everywhere rather than refused in one place.
    int textureless = 0;
    for (const ResourceAccess access : kAllAccesses) {
        textureless += RequirementOf(access).namesTexture ? 0 : 1;
    }
    CHECK(textureless == 1);
    CHECK_FALSE(RequirementOf(ResourceAccess::IndirectRead).namesTexture);
}

TEST_CASE("no texture access asks for the undefined layout") {
    // `TextureLayout::Undefined` is a legitimate layout and a transition *out* of it discards
    // whatever the memory held. An access that asked a texture *into* it would therefore be a
    // declaration that throws the resource's contents away, which no read or write means --
    // and it is the value a new row would land on if its layout were left to the struct's
    // default.
    for (const ResourceAccess access : kAllAccesses) {
        CAPTURE(ToString(access));
        const AccessRequirement requirement = RequirementOf(access);
        if (requirement.namesTexture) {
            CHECK(requirement.layout != TextureLayout::Undefined);
        }
    }
}

TEST_CASE("ToString names every access, distinctly") {
    for (Monarc::usize i = 0; i < std::size(kAllAccesses); ++i) {
        const std::string_view name = ToString(kAllAccesses[i]);
        CAPTURE(name);
        CHECK_FALSE(name.empty());
        // A name is the enumerator's own spelling, so it cannot be the not-a-value marker.
        CHECK(name.front() != '<');
        for (Monarc::usize j = i + 1; j < std::size(kAllAccesses); ++j) {
            CHECK(name != std::string_view(ToString(kAllAccesses[j])));
        }
    }
}

TEST_CASE("ToString gives a value outside the set a name of its own") {
    // Not "Undefined" and not any real access's spelling. Conflating a named state with an
    // invalid one misdirects whoever reads the line -- `ToString(Format)` in
    // Monarc/RHI/Types.h states the rule this follows.
    const std::string_view name =
        ToString(static_cast<ResourceAccess>(std::size(kAllAccesses) + 7));
    CHECK(name == "<invalid ResourceAccess>");
}

// `AccessScope` is a distinct type from `RHI::Access`, whose name it shares a word with. The
// assertion is that it is a two-field aggregate rather than an alias of either half -- a scope
// collapsed to a bare stage or a bare access would make a barrier side unable to say both.
static_assert(!std::is_same_v<AccessScope, PipelineStage>);
static_assert(!std::is_same_v<AccessScope, Access>);
static_assert(std::is_trivially_copyable_v<AccessScope>);
static_assert(std::is_trivially_copyable_v<AccessRequirement>);

int main(int argc, char** argv) {
    // **Installed before any case runs, and put back below rather than left in place.**
    // `RenderGraph::Refuse` logs at `Error` beside every diagnostic it records, and this suite
    // refuses about thirty times on purpose -- printed, that is thirty `[Error]` lines in a
    // green CI run, which is exactly how the word stops meaning anything. `LogCapture`'s own
    // comment carries the argument and the second reason: captured, the line can be asserted,
    // and TestPassDeclaration.cpp asserts it.
    const Monarc::LogSink previous = Monarc::Render::TestSupport::LogCapture::Install();

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    const int result = context.run();

    // Put the real sink back before anything doctest owns is destroyed, so that a message
    // emitted during shutdown is printed rather than swallowed.
    Monarc::Render::TestSupport::LogCapture::Restore(previous);
    return result;
}
