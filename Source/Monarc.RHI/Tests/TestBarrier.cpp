#include <doctest/doctest.h>

#include <Monarc/RHI/Barrier.h>
#include <Monarc/RHI/Handles.h>

#include <format>
#include <iterator>
#include <string_view>
#include <type_traits>

using Monarc::RHI::Access;
using Monarc::RHI::BarrierDescription;
using Monarc::RHI::BufferBarrier;
using Monarc::RHI::BufferHandle;
using Monarc::RHI::Describe;
using Monarc::RHI::GlobalBarrier;
using Monarc::RHI::HasAny;
using Monarc::RHI::kBarrierDescriptionLength;
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

/// What a `CollisionReport` reads when no two members of a set collide.
constexpr std::string_view kNoCollision = "no two collide";

/// The first colliding pair in a set, by name -- or `kNoCollision`.
///
/// **One assertion per property rather than one per pair, and the collapse cost nothing that
/// was measured to matter.** Written as `n * (n - 1) / 2` separate `CHECK`s, the three
/// distinctness properties below would report 286 assertions between them -- 105 for fifteen
/// stages, 153 for eighteen accesses, 28 for eight layouts. As three named reports they cost
/// three, so 283 identical greens are gone and no detection is: a duplicated `ToString` case
/// collides exactly one pair, so it turned exactly one of the 286 red -- and it turns one of
/// these three red, with the pair named.
///
/// **The numbers that belong to this file are 15 cases and 46 assertions**, measured with
/// `--source-file=*TestBarrier.cpp`, out of the binary's 51 and 174. An earlier version of this
/// note said "this file's total went from 453 to 169 when they became three", and neither
/// figure was this file's total: both were the whole binary's, at a commit several behind. That
/// is precisely the mistake the next paragraph warns about, made inside the warning.
///
/// **This property does have to be here, and that is worth separating from the count.** A
/// duplicated `ToString` case is invisible to everything else in either suite: the mutation
/// above left `Monarc.RHI.Vulkan.Tests` fully green at 324 of 324, because a name is not
/// something translation round-trips. The injectivity properties in
/// Monarc.RHI.Vulkan/Tests/TestVulkanBarrierTranslate.cpp are *not* in that position -- the
/// round trip detects a duplicated Vulkan bit independently -- and that file says so where it
/// keeps them. The comparison is still every pair, because that is the property; what is gone
/// is 283 identical greens in a total, and a count nobody can interpret is a number that gets
/// quoted as coverage.
///
/// A named pair and not a bool, because that is the whole difference between a failure that
/// is actionable and one that is not: with eighteen enumerators, "a duplicate exists" leaves
/// the reader to find it. The shape is `AdapterUuidString`'s -- a fixed buffer returned by
/// value, so nothing allocates -- and the buffer's initialiser is the passing text, so a
/// green assertion reads as the property it is asserting.
///
/// **Each half carries its numeric value as well as its spelling**, and that is not
/// decoration: when the thing being compared *is* the spelling, a collision means both halves
/// print the same word. Measured -- a duplicated `ToString` case reported `HostRead and
/// HostRead collide`, which names the symptom and not the two rows. With the values it reads
/// `HostRead[8192] and HostRead[16384]`, and 1 << 13 against 1 << 14 says which two.
struct CollisionReport {
    char text[160] = "no two collide";

    [[nodiscard]] std::string_view View() const { return std::string_view(text); }
};

/// `key` projects an enumerator onto whatever must be distinct: its `ToString` spelling here,
/// a Vulkan bit or layout in Monarc.RHI.Vulkan's copy of this helper.
template <typename Enum, Monarc::usize N, typename Key>
[[nodiscard]] CollisionReport FirstCollision(const Enum (&values)[N], Key key) {
    for (Monarc::usize i = 0; i < N; ++i) {
        for (Monarc::usize j = i + 1; j < N; ++j) {
            if (key(values[i]) == key(values[j])) {
                CollisionReport report;
                // format_to_n and not format: ADR-0003's subset allows this one entry point
                // into <format>, and JobSystem.cpp's worker naming is the precedent.
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
    // copy-pasted case label produces and what a spot check would miss. One assertion per set
    // and the colliding pair named in it -- see `FirstCollision` for why, and for what the
    // 286-assertion version of these three loops was measured to buy.
    CHECK(FirstCollision(kAllStages, Name<PipelineStage>).View() == kNoCollision);
    CHECK(FirstCollision(kAllAccesses, Name<Access>).View() == kNoCollision);
    CHECK(FirstCollision(kAllLayouts, Name<TextureLayout>).View() == kNoCollision);
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

    // Distinguishable from a barrier that does something, so the no-change value is not a
    // sentinel that compares equal to everything.
    //
    // The companion `none == GlobalBarrier{}` was deleted: `operator==` is `= default`, so a
    // member-wise comparison of two identically default-initialised objects is true at compile
    // time whatever the members hold, and no defaulted implementation could make it false.
    // This line is the one with a mutation that turns it red.
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
// `Describe`, which is the text a backend's barrier refusal logs.
// ---------------------------------------------------------------------------------------
//
// **These four cases are here, with no device and no Vulkan, because the shipped caller of
// `Describe` cannot be tested at all.** `ICommandList::Barrier` refuses a stale handle by
// logging the description and then ending the process -- `MONARC_DEBUG_BREAK()` and
// `std::abort()`, unconditionally, because `MONARC_CHECK` alters no control flow and a
// *skipped* barrier is a synchronisation hole rather than a refused operation (Device.h says
// it at length). Nothing survives that call to read the line it wrote, on any handler.
//
// So the composition is what gets tested, and the composition is pure. Until this change the
// property lived in Monarc.RHI.Vulkan's device suite, which needs a GPU and therefore never
// ran in CI; these run everywhere. What was lost in the move is stated in
// Monarc.RHI.Vulkan/TestsDevice/TestVulkanDevice.cpp, where the case used to be.
//
// **Whole-string comparisons and not `find()`**, which is the rule
// Monarc.RHI.Vulkan/Tests/TestVulkanBackend.cpp:112 and TestVulkanLoader.cpp:40 arrived at the
// hard way -- Docs/Status.md records why those two compare a whole message: a `find()` had let
// a dangling `Error::message` through on all six presets, `clang-asan` included. The same
// looseness costs more here, not less. A description clipped by `format_to_n` is right up to
// the point it was cut, so every `find()` for something before the cut still passes; and a
// description carrying an extra field nobody asked for passes every `find()` there is.
//
// The case that pinned this before searched for four substrings: the layout pair, `sync None`,
// `ColorAttachmentOutput` and `ColorAttachmentWrite`. Not one of them looked at a number, so
// dropping every hex from the description would have left all four green -- and dropping
// `accessBefore` would too, since `sync None` is the *other* `None`. Both change the string,
// so both turn a comparison below red.

TEST_CASE("a texture barrier's description names its layout pair and both scopes, with hex") {
    // The barrier the device suite used to refuse, so the text pinned here is the text the
    // shipped path composes: `ColorAttachmentOutput` is `1 << 5` and `ColorAttachmentWrite`
    // is `1 << 8`, which is where `0x20` and `0x100` come from.
    //
    // Every pair is asymmetric on purpose, as in the accessor case above: `Undefined` against
    // `ColorAttachment`, `None` against a real stage, `None` against a real access. A
    // `Describe` that had its before and after arguments the wrong way round produces a
    // different string here rather than the same one.
    constexpr TextureBarrier barrier(kTexture, TextureLayout::Undefined,
                                     TextureLayout::ColorAttachment, PipelineStage::None,
                                     PipelineStage::ColorAttachmentOutput, Access::None,
                                     Access::ColorAttachmentWrite);

    const BarrierDescription description = Describe(barrier);
    CHECK(description.View() ==
          "texture (slot 3, generation 1): layout Undefined -> ColorAttachment, "
          "sync None (0x0) -> ColorAttachmentOutput (0x20), "
          "access None (0x0) -> ColorAttachmentWrite (0x100)");
}

TEST_CASE("a buffer barrier's description names both scopes and no layout") {
    // The other overload, and the barrier is not the texture one with fields removed: a buffer
    // has no layout in either API the model is shaped from, and the absence of the word here
    // is what a `Describe(BufferBarrier)` grown a layout pair by copy-paste would break.
    //
    // A companion `find("layout") == npos` was written and then deleted. It does go red on
    // that copy-paste -- measured -- but so does the comparison below it, for the same
    // mutation and in the same run, so it detected nothing the comparison did not. That is
    // the test this branch applied to the monotonicity case in
    // Monarc.RHI/Tests/TestCapabilities.cpp -- whose one real fact was already pinned three
    // times over -- and it gets the same answer.
    constexpr BufferBarrier barrier{kBuffer, PipelineStage::Copy, PipelineStage::Host,
                                    Access::TransferWrite, Access::HostRead};

    const BarrierDescription description = Describe(barrier);
    CHECK(description.View() ==
          "buffer (slot 7, generation 2): sync Copy (0x80) -> Host (0x800), "
          "access TransferWrite (0x1000) -> HostRead (0x2000)");
}

TEST_CASE("a mask of several stages is described by its hex, which is why the hex is there") {
    // **The case the hex exists for.** A `syncBefore` of several stages is a mask and not an
    // enumerator, so `ToString` reports its not-a-single-value name -- honest, and not
    // decodable on its own. `Copy | Blit` is `0x80 | 0x100`, and `0x180` is what says which
    // two stages the caller meant.
    constexpr BufferBarrier barrier{kBuffer, PipelineStage::Copy | PipelineStage::Blit,
                                    PipelineStage::AllCommands,
                                    Access::TransferRead | Access::TransferWrite,
                                    Access::MemoryWrite};

    // The two markers are spelled by `ToString` rather than written out here, exactly as the
    // cases above identify them through `Name(kFarBit)`: what this case pins is that a mask's
    // name and its number both reach the line and in that order, not what the marker reads.
    char       expected[kBarrierDescriptionLength];
    const auto written = std::format_to_n(
        expected, sizeof(expected) - 1,
        "buffer (slot 7, generation 2): sync {} (0x180) -> AllCommands (0x2000), "
        "access {} (0x1800) -> MemoryWrite (0x10000)",
        Name(PipelineStage::Copy | PipelineStage::Blit),
        Name(Access::TransferRead | Access::TransferWrite));
    *written.out = '\0';

    const BarrierDescription description = Describe(barrier);
    CHECK(description.View() == std::string_view(expected));
}

TEST_CASE("the widest description either barrier can produce arrives whole") {
    // **`kBarrierDescriptionLength`'s arithmetic, checked rather than trusted.** The bound in
    // Barrier.h is taken with each field's longest name and its widest hex treated as
    // independent, which they are not -- a name and a number are two views of one value -- so
    // it is an upper bound and not the length of anything. This builds the widest description
    // that actually exists and asserts it was not clipped.
    //
    // The widest values are not the ones a barrier would normally hold. Two `u32`s at
    // `0xffffffff` are ten decimal digits each and eight hex digits; a layout and a stage
    // outside their enumerator sets get the not-a-value markers, which are longer than every
    // real spelling (`<not a single PipelineStage>` is 28 chars against
    // `ColorAttachmentOutput`'s 21); and the widest *access* field is not the marker but
    // `DepthStencilAttachmentWrite`, whose 27 chars plus `400`'s three beat the marker's 21
    // plus eight by one.
    constexpr Monarc::u32 kWidestNumber = 0xFFFFFFFFU;
    constexpr auto        kWidestLayout = static_cast<TextureLayout>(kWidestNumber);
    constexpr auto        kWidestStage  = static_cast<PipelineStage>(kWidestNumber);
    constexpr Access      kWidestAccess = Access::DepthStencilAttachmentWrite;

    constexpr TextureBarrier widestTexture(
        TextureHandle::ForTesting(kWidestNumber, kWidestNumber), kWidestLayout, kWidestLayout,
        kWidestStage, kWidestStage, kWidestAccess, kWidestAccess);
    constexpr BufferBarrier widestBuffer{
        BufferHandle::ForTesting(kWidestNumber, kWidestNumber), kWidestStage, kWidestStage,
        kWidestAccess, kWidestAccess};

    // `format_to_n` truncates silently, so a description that did not fit is exactly one
    // character short of the buffer and there is nothing else to see -- which is what these
    // compare against. Measured at three capacities: at 320 the widest texture description is
    // 283 chars and the widest buffer one 223, both green; at 240 the texture assertion alone
    // goes red (`239 < 239`) and the buffer stays green; at 220 both go red. Two assertions
    // and not one for that middle result: one constant serves two functions, and Barrier.h's
    // arithmetic is written for the longer of them.
    CHECK(Describe(widestTexture).View().size() < kBarrierDescriptionLength - 1);
    CHECK(Describe(widestBuffer).View().size() < kBarrierDescriptionLength - 1);
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

// Trivially copyable, all three, which is what lets a barrier be an element of a plain array
// a backend can hand to `vkCmdPipelineBarrier2` -- the shape ADR-0005's split into three
// variants is built for. `TextureBarrier` having a user-provided constructor is what makes
// this worth asserting rather than obvious: a constructor is not what costs triviality, but a
// member with an initialiser that did something would be.
static_assert(std::is_trivially_copyable_v<GlobalBarrier>);
static_assert(std::is_trivially_copyable_v<BufferBarrier>);
static_assert(std::is_trivially_copyable_v<TextureBarrier>);
