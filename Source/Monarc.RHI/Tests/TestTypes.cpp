// The one translation unit in Monarc.RHI.Tests that provides doctest's main().
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <Monarc/RHI/Types.h>

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
constexpr Format kAllFormats[] = {
    Format::Unknown,
    Format::R8G8B8A8_UNORM,
    Format::B8G8R8A8_UNORM,
};

// The list is only worth iterating if it is complete, and nothing else checks that -- a new
// enumerator with a correct case label but no entry here would be skipped silently by every
// loop below. Format's enumerators are contiguous from Unknown = 0 with no explicit values
// after it, so the last one's numeric value is the count minus one.
static_assert(sizeof(kAllFormats) / sizeof(kAllFormats[0]) ==
                  static_cast<Monarc::usize>(Format::B8G8R8A8_UNORM) + 1,
              "kAllFormats must list every Format enumerator");

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

TEST_CASE("every format is named exactly once") {
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

TEST_CASE("the two byte-order formats are distinct despite being the same size") {
    // B8G8R8A8 and R8G8B8A8 differ only in channel order, which is precisely the bug the
    // exact-value readback test in Task 3 exists to catch. Equal sizes must not tempt
    // anything into treating them as interchangeable.
    CHECK(Format::B8G8R8A8_UNORM != Format::R8G8B8A8_UNORM);
    CHECK(BytesPerPixel(Format::B8G8R8A8_UNORM) == BytesPerPixel(Format::R8G8B8A8_UNORM));
    CHECK(Name(Format::B8G8R8A8_UNORM) != Name(Format::R8G8B8A8_UNORM));
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
