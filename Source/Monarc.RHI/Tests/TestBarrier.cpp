#include <doctest/doctest.h>

#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Handles.h>

#include <iterator>
#include <string_view>
#include <type_traits>

using Monarc::RHI::Access;
using Monarc::RHI::BufferBarrier;
using Monarc::RHI::BufferHandle;
using Monarc::RHI::GlobalBarrier;
using Monarc::RHI::HasAny;
using Monarc::RHI::PipelineStage;
using Monarc::RHI::TextureBarrier;
using Monarc::RHI::TextureHandle;
using Monarc::RHI::TextureLayout;
using Monarc::RHI::ToString;

namespace {

/// Handles no device returned. `ForTesting` is the convention for saying so at the call site
/// -- see `Handle` in Monarc/RHI/Handles.h -- and nothing in this file resolves them, because
/// nothing here has a device to resolve them against.
constexpr TextureHandle kTexture = TextureHandle::ForTesting(3, 1);
constexpr BufferHandle  kBuffer  = BufferHandle::ForTesting(7, 2);

/// Every `PipelineStage` enumerator, once, in declaration order.
///
/// The `default`-less switches in Private/Barrier.cpp and in Monarc.RHI.Vulkan's Translate.cpp
/// already refuse an enumerator nobody gave a case to. This list covers the half the compiler
/// cannot -- that the case it was given is the right one -- and "kAllStages lists every
/// enumerator" below is what keeps the list itself honest.
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

/// Every `Access` enumerator, once, in declaration order. `kAllStages`'s reasoning.
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

/// Every `TextureLayout` enumerator, once, in declaration order.
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

/// A bit position well past every named one, so a value cast from it is definitely not an
/// enumerator. Used to identify the not-a-value marker without this file hardcoding its
/// spelling, exactly as TestTypes.cpp identifies `ToString(Format)`'s.
constexpr Monarc::u32 kFarBit = 1U << 30;

template <typename Enum>
[[nodiscard]] std::string_view Name(Enum value) {
    return std::string_view(ToString(value));
}

/// Whether two values of `E` can be or-ed together. A named variable template rather than a
/// bare requires-expression inside a `static_assert`, which is TestHandles.cpp's own form and
/// for its reason: the requirement reads as a property with a name rather than as syntax.
template <typename E>
constexpr bool kOrable = requires(E a, E b) { a | b; };

/// Whether `B` has a `LayoutBefore()` to call. Used to pin that the two barriers without
/// layouts do not have one.
template <typename B>
constexpr bool kHasLayoutBefore = requires(B barrier) { barrier.LayoutBefore(); };

}  // namespace

// ---------------------------------------------------------------------------------------
// The lists above are only worth iterating if they are complete.
// ---------------------------------------------------------------------------------------

TEST_CASE("kAllStages lists every PipelineStage enumerator") {
    // C++ cannot ask an enum how many enumerators it has, so this is indirect, and it is
    // TestTypes.cpp's trick adapted from a sequential enum to a bit-flags one: `PipelineStage`
    // is `None` plus one contiguous run of bits from `1 << 0`, so the bit one past the last
    // named one must not be named. Append an enumerator, give it its `ToString` case, and
    // leave this list alone, and that bit becomes a real stage -- the two names then differ
    // and this fails. Update both and it is unnamed again.
    //
    // What it rests on is that contiguous run, which Barrier.h keeps by writing the shifts out
    // in order. An enumerator added at some far bit position would still slip past; the guard
    // covers the append, which is how both flag sets have actually grown.
    const Monarc::u32 pastTheEnd = 1U << (std::size(kAllStages) - 1);
    CHECK(Name(static_cast<PipelineStage>(pastTheEnd)) ==
          Name(static_cast<PipelineStage>(kFarBit)));
}

TEST_CASE("kAllAccesses lists every Access enumerator") {
    const Monarc::u32 pastTheEnd = 1U << (std::size(kAllAccesses) - 1);
    CHECK(Name(static_cast<Access>(pastTheEnd)) == Name(static_cast<Access>(kFarBit)));
}

TEST_CASE("kAllLayouts lists every TextureLayout enumerator") {
    // A sequential enum from zero rather than a bit run, so the index one past the end is the
    // value to probe -- TestTypes.cpp's own form.
    CHECK(Name(static_cast<TextureLayout>(std::size(kAllLayouts))) ==
          Name(static_cast<TextureLayout>(4242)));
}

TEST_CASE("each enumerator's name is its own spelling, and no two share one") {
    CHECK(Name(PipelineStage::ColorAttachmentOutput) == "ColorAttachmentOutput");
    CHECK(Name(Access::TransferWrite) == "TransferWrite");
    CHECK(Name(TextureLayout::TransferSource) == "TransferSource");

    // Distinctness across the whole of each set, because two rows returning one name is what a
    // copy-pasted case label produces and what a spot check would miss.
    for (Monarc::usize i = 0; i < std::size(kAllStages); ++i) {
        for (Monarc::usize j = i + 1; j < std::size(kAllStages); ++j) {
            CHECK(Name(kAllStages[i]) != Name(kAllStages[j]));
        }
    }
    for (Monarc::usize i = 0; i < std::size(kAllAccesses); ++i) {
        for (Monarc::usize j = i + 1; j < std::size(kAllAccesses); ++j) {
            CHECK(Name(kAllAccesses[i]) != Name(kAllAccesses[j]));
        }
    }
    for (Monarc::usize i = 0; i < std::size(kAllLayouts); ++i) {
        for (Monarc::usize j = i + 1; j < std::size(kAllLayouts); ++j) {
            CHECK(Name(kAllLayouts[i]) != Name(kAllLayouts[j]));
        }
    }
}

TEST_CASE("a mask of several stages has no enumerator name, and says so") {
    // Not a curiosity: `ToString` is documented to name *enumerators*, and a mask is not one.
    // A version that returned the lowest set bit's name would report `Copy` for a barrier that
    // also waits on `Blit`, which is a log line that misleads about synchronisation.
    CHECK(Name(PipelineStage::Copy | PipelineStage::Blit) ==
          Name(static_cast<PipelineStage>(kFarBit)));
    CHECK(Name(Access::TransferRead | Access::TransferWrite) ==
          Name(static_cast<Access>(kFarBit)));
}

// ---------------------------------------------------------------------------------------
// The flag-set operators.
// ---------------------------------------------------------------------------------------

TEST_CASE("a union of stages contains each of its members and nothing else") {
    constexpr PipelineStage both = PipelineStage::Copy | PipelineStage::Host;

    CHECK(HasAny(both, PipelineStage::Copy));
    CHECK(HasAny(both, PipelineStage::Host));
    CHECK_FALSE(HasAny(both, PipelineStage::Blit));

    // An empty set intersects nothing, which is the answer a caller asking "does this barrier
    // touch the transfer stages" wants. An implementation using `==` rather than a bit test
    // would answer true here for every mask.
    CHECK_FALSE(HasAny(both, PipelineStage::None));
    CHECK_FALSE(HasAny(PipelineStage::None, PipelineStage::None));

    CHECK((both & PipelineStage::Copy) == PipelineStage::Copy);
    CHECK((both & PipelineStage::Blit) == PipelineStage::None);
}

TEST_CASE("or-assignment accumulates rather than replacing") {
    Access accumulated = Access::None;
    accumulated |= Access::TransferRead;
    accumulated |= Access::HostWrite;

    // The first assertion is what an `operator|=` written as a plain assignment would fail,
    // and it is the mistake worth guarding: `FromVulkanStages` and `FromVulkanAccess` in
    // Monarc.RHI.Vulkan build their whole result by accumulating into one of these, so a
    // replacing version would return only the last bit it looked at.
    CHECK(HasAny(accumulated, Access::TransferRead));
    CHECK(HasAny(accumulated, Access::HostWrite));
    CHECK(accumulated == (Access::TransferRead | Access::HostWrite));
}

// ---------------------------------------------------------------------------------------
// A barrier that changes nothing is a value, and stays one.
// ---------------------------------------------------------------------------------------

TEST_CASE("a global barrier with no state change is representable and default-constructible") {
    // The plan asks that a no-change barrier be representable and not silently dropped. For
    // the global variant "representable" is `GlobalBarrier{}` compiling and meaning it: every
    // field `None`, which is the value a render graph produces for a pass with no state to
    // change. A model in which "no change" had to be spelled as "no barrier" would push that
    // decision out to every caller.
    constexpr GlobalBarrier none{};
    CHECK(none.syncBefore == PipelineStage::None);
    CHECK(none.syncAfter == PipelineStage::None);
    CHECK(none.accessBefore == Access::None);
    CHECK(none.accessAfter == Access::None);

    // Equal to itself and distinguishable from a barrier that does something, so the no-change
    // value is not a sentinel that compares equal to everything.
    CHECK(none == GlobalBarrier{});
    CHECK(none != GlobalBarrier{PipelineStage::None, PipelineStage::AllCommands, Access::None,
                                Access::MemoryRead});
}

TEST_CASE("a texture barrier keeps a no-change layout pair rather than folding it away") {
    // "Not silently dropped", at the level Monarc.RHI can state it: the constructor stores the
    // pair it was given even when the two are equal. An implementation that normalised an
    // equal pair to `Undefined` -- on the reasoning that a transition to the same layout is a
    // no-op -- would turn a barrier that preserves the texture's contents into one that
    // discards them, and would fail the second assertion here.
    //
    // Whether the *backend* records it is a separate claim, and it lives beside the code that
    // records: Monarc.RHI.Vulkan/Tests/TestVulkanBarrierTranslate.cpp asserts that translation
    // produces a real barrier structure for a no-change barrier rather than nothing.
    constexpr TextureBarrier unchanged(kTexture, TextureLayout::General, TextureLayout::General,
                                       PipelineStage::None, PipelineStage::None, Access::None,
                                       Access::None);
    CHECK(unchanged.LayoutBefore() == TextureLayout::General);
    CHECK(unchanged.LayoutAfter() == TextureLayout::General);
    CHECK(unchanged.Texture() == kTexture);
}

TEST_CASE("a texture barrier reports every field it was given, in the right place") {
    constexpr TextureBarrier barrier(kTexture, TextureLayout::Undefined,
                                     TextureLayout::ColorAttachment, PipelineStage::None,
                                     PipelineStage::ColorAttachmentOutput, Access::None,
                                     Access::ColorAttachmentWrite);

    // Six distinct values in six accessors, which is what catches a constructor that
    // initialised a member from the wrong parameter -- the failure mode a seven-argument
    // constructor actually has. Every pair here is asymmetric on purpose: `Undefined` against
    // `ColorAttachment`, `None` against `ColorAttachmentOutput`, `None` against
    // `ColorAttachmentWrite`, so a before/after swap fails rather than reading the same.
    CHECK(barrier.Texture() == kTexture);
    CHECK(barrier.LayoutBefore() == TextureLayout::Undefined);
    CHECK(barrier.LayoutAfter() == TextureLayout::ColorAttachment);
    CHECK(barrier.SyncBefore() == PipelineStage::None);
    CHECK(barrier.SyncAfter() == PipelineStage::ColorAttachmentOutput);
    CHECK(barrier.AccessBefore() == Access::None);
    CHECK(barrier.AccessAfter() == Access::ColorAttachmentWrite);
}

TEST_CASE("a buffer barrier reports every field it was given, in the right place") {
    constexpr BufferBarrier barrier{kBuffer, PipelineStage::Copy, PipelineStage::Host,
                                    Access::TransferWrite, Access::HostRead};
    CHECK(barrier.buffer == kBuffer);
    CHECK(barrier.syncBefore == PipelineStage::Copy);
    CHECK(barrier.syncAfter == PipelineStage::Host);
    CHECK(barrier.accessBefore == Access::TransferWrite);
    CHECK(barrier.accessAfter == Access::HostRead);
}

// ---------------------------------------------------------------------------------------
// The type-level properties. static_assert, because that is what "rejected at the type
// level rather than at runtime" means: the failure is a build, not a test run.
// ---------------------------------------------------------------------------------------

// **The requirement the plan states, as a compile-time fact.** A texture barrier without a
// layout pair must be rejected at the type level, and it has to be: `TextureLayout::Undefined`
// is a legitimate before-layout -- it is what a freshly created texture is in -- so a barrier
// whose layouts had been left to a zero-initialised default is byte-for-byte identical to one
// whose author meant `Undefined`, and a run-time check has nothing left to look at.
//
// Three assertions, because there are three ways to omit them and only the first is obvious:
static_assert(!std::is_default_constructible_v<TextureBarrier>,
              "TextureBarrier must not be default-constructible: a default would have to pick "
              "a layout pair, and Undefined is a pair someone may mean");

static_assert(!std::is_aggregate_v<TextureBarrier>,
              "TextureBarrier must not be an aggregate: aggregate initialisation would let the "
              "trailing members be omitted and zero-initialised");

static_assert(!std::is_constructible_v<TextureBarrier, TextureHandle, PipelineStage,
                                       PipelineStage, Access, Access>,
              "a TextureBarrier built from a handle and the four sync fields alone -- the "
              "layout pair simply left out -- must not compile");

// And the constructor that does name them must, or the three above would be satisfied by a
// type nobody can build at all.
static_assert(std::is_constructible_v<TextureBarrier, TextureHandle, TextureLayout,
                                      TextureLayout, PipelineStage, PipelineStage, Access,
                                      Access>,
              "the seven-argument constructor is the one way to build a TextureBarrier");

// The other side of "three distinct types, not one type with optional fields": the two
// barriers that have no layouts must not grow accessors for them. Adding a layout pair to
// `GlobalBarrier` -- the change this guards against -- turns these red rather than passing
// unnoticed. The third line is what keeps the first two from being satisfied by nothing having
// the accessor at all.
static_assert(!kHasLayoutBefore<GlobalBarrier>,
              "a global barrier has no layout: it is about memory, not about one texture");
static_assert(!kHasLayoutBefore<BufferBarrier>,
              "a buffer has no layout in either API the barrier model is shaped from");
static_assert(kHasLayoutBefore<TextureBarrier>);

// `TextureLayout`'s values are alternatives and not bits: a texture is in exactly one layout,
// which is the whole reason a transition needs naming. So `ColorAttachment | TransferSource`
// is not a layout, it is a bug -- and this is what the opt-in `Detail::kIsFlagSet` marker in
// Barrier.h buys over a blanket `requires std::is_enum_v<E>`, which would have given every
// enum in the module an `operator|`. The two positive lines are what keep this from being
// satisfied by the operators not existing.
static_assert(!kOrable<TextureLayout>);
static_assert(kOrable<PipelineStage>);
static_assert(kOrable<Access>);

// `GlobalBarrier` and `BufferBarrier` stay aggregates, which is what makes `GlobalBarrier{}`
// -- the no-change barrier the case above exercises -- expressible at all. The asymmetry with
// `TextureBarrier` is the design and not an inconsistency: those two have no field whose
// default could be mistaken for a decision.
static_assert(std::is_aggregate_v<GlobalBarrier>);
static_assert(std::is_aggregate_v<BufferBarrier>);
static_assert(std::is_default_constructible_v<GlobalBarrier>);
static_assert(std::is_default_constructible_v<BufferBarrier>);

// Trivially copyable, all three. A barrier is a value a render graph will build by the
// thousand into an array and hand to a backend; a type with a non-trivial copy would make that
// array's construction a loop of constructor calls.
static_assert(std::is_trivially_copyable_v<GlobalBarrier>);
static_assert(std::is_trivially_copyable_v<BufferBarrier>);
static_assert(std::is_trivially_copyable_v<TextureBarrier>);
