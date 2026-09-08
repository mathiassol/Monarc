// The one translation unit in Monarc.RHI.Tests that provides doctest's main().
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Monarc/RHI/Types.h>

#include <iterator>
#include <string_view>

using Monarc::RHI::BytesPerPixel;
using Monarc::RHI::Extent2D;
using Monarc::RHI::Format;
using Monarc::RHI::ToString;

namespace {

/// Every enumerator of Format, once. The switches in Types.cpp carry no `default`, so the
/// compiler already refuses a new enumerator that nobody gave a case to. This list covers
/// the half the compiler cannot: that the case it was given returns a real name and a real
/// size rather than one belonging to a neighbouring row.
///
/// That the list is *complete* is checked by "kAllFormats lists every Format enumerator"
/// below. It has to be a run-time case rather than a static_assert because it asks ToString,
/// and ToString is not constexpr.
constexpr Format kAllFormats[] = {
    Format::Unknown,
    Format::R8G8B8A8_UNORM,
    Format::B8G8R8A8_UNORM,
};

std::string_view Name(Format format) { return std::string_view(ToString(format)); }

/// A value Format can hold but that no enumerator names. Well-defined: Format has a fixed
/// underlying type, so every u32 is a valid value of it.
constexpr Format kNotAFormat = static_cast<Format>(4242);

}  // namespace

TEST_CASE("a format's name is the enumerator's own spelling") {
    CHECK(Name(Format::Unknown) == "Unknown");
    CHECK(Name(Format::R8G8B8A8_UNORM) == "R8G8B8A8_UNORM");
    CHECK(Name(Format::B8G8R8A8_UNORM) == "B8G8R8A8_UNORM");
}

TEST_CASE("kAllFormats lists every Format enumerator") {
    // The list is only worth iterating if it is complete: an enumerator with a correct case
    // label in Types.cpp but no entry here would be skipped silently by every loop below,
    // and nothing else in the build would notice.
    //
    // C++ cannot ask an enum how many enumerators it has, so the check is indirect: it asks
    // whether the index one past the end of the list names a format. ToString answers with
    // its own not-a-format marker for any value no enumerator names, and comparing against
    // ToString of a value that is definitely not a format is how that marker is identified
    // without this file hardcoding its spelling. Append an enumerator, give it its cases,
    // and leave the list alone, and that index becomes a named format -- the two names
    // differ and this fails. Update both and the index is unnamed again.
    //
    // What it rests on: Format's enumerators are one contiguous run from Unknown = 0, which
    // Types.h keeps by giving no explicit value after it. An enumerator added with a value
    // outside that run would still slip past. The guard covers the append, which is how both
    // of the formats after Unknown got there.
    const Format onePastTheList = static_cast<Format>(std::size(kAllFormats));
    CHECK(Name(onePastTheList) == Name(kNotAFormat));
}

TEST_CASE("every format is named exactly once") {
    // The pair this matters most for is B8G8R8A8_UNORM and R8G8B8A8_UNORM, which differ only
    // in channel order and are the same size -- a copy-paste in Types.cpp giving them one
    // name is exactly the confusion the exact-value readback test in Task 3 exists to catch,
    // and the loop below covers it along with every other pair.
    //
    // Two enumerators sharing a *value* needs no test: Types.cpp would then have duplicate
    // case labels, which is "error C2196: case value already used" on cl and "duplicate case
    // value" on clang-cl. No build that compiles can have that fault, so a CHECK that two
    // format enumerators differ would only ever be asserting 1 != 2.
    for (const Format outer : kAllFormats) {
        for (const Format inner : kAllFormats) {
            if (outer != inner) {
                CHECK(Name(outer) != Name(inner));
            }
        }
    }
}

TEST_CASE("a value that is not a Format is not reported as one") {
    // The fallback after each switch. Format::Unknown means "not decided yet" and is a state
    // the engine sets on purpose; a value outside the enumerator set means something is
    // wrong. Naming both "Unknown" would hide the second behind the first in exactly the
    // logs someone would be reading to find it.
    CHECK(Name(kNotAFormat) != "Unknown");
    CHECK_FALSE(Name(kNotAFormat).empty());
    CHECK(BytesPerPixel(kNotAFormat) == 0u);
}

TEST_CASE("a format's size is its channel widths in bytes") {
    CHECK(BytesPerPixel(Format::R8G8B8A8_UNORM) == 4u);
    CHECK(BytesPerPixel(Format::B8G8R8A8_UNORM) == 4u);
}

TEST_CASE("Unknown is the only named format with no size") {
    // A3's readback test multiplies this by a pixel count to size a staging buffer, so a
    // real format that reported zero by accident would copy nothing and then assert on empty
    // bytes rather than on the wrong colour.
    CHECK(BytesPerPixel(Format::Unknown) == 0u);
    for (const Format format : kAllFormats) {
        if (format != Format::Unknown) {
            CHECK(BytesPerPixel(format) > 0u);
        }
    }
}

TEST_CASE("a default extent is empty") {
    CHECK(Extent2D{}.IsEmpty());
    CHECK(Extent2D{} == Extent2D{0, 0});
}

TEST_CASE("an extent is empty when either dimension is zero") {
    // A minimised window reports 0 x 0; a window dragged to zero width on one axis only is
    // just as unusable as a swapchain source, so one zero is enough.
    CHECK(Extent2D{0, 720}.IsEmpty());
    CHECK(Extent2D{1280, 0}.IsEmpty());
    CHECK_FALSE(Extent2D{1280, 720}.IsEmpty());
    CHECK_FALSE(Extent2D{1, 1}.IsEmpty());
}

TEST_CASE("extents compare on both dimensions") {
    CHECK(Extent2D{1280, 720} == Extent2D{1280, 720});
    CHECK(Extent2D{1280, 720} != Extent2D{1280, 721});
    CHECK(Extent2D{1280, 720} != Extent2D{1281, 720});
    // Not merely "the same numbers": a portrait window is not a landscape one.
    CHECK(Extent2D{1280, 720} != Extent2D{720, 1280});
}

// Usable in a constant expression, so a swapchain extent can be checked at compile time
// where it is known -- and so the comparison operators are exercised at constant-evaluation
// time as well as at run time.
static_assert(Extent2D{}.IsEmpty());
static_assert(!Extent2D{4, 4}.IsEmpty());
static_assert(Extent2D{4, 4} == Extent2D{4, 4});
static_assert(Extent2D{4, 4} != Extent2D{4, 5});
