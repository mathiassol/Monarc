#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc::RHI {

/// A pixel format, spelled the way the graphics APIs spell them: channels in memory order,
/// each with its width and its interpretation. No backend value is encoded here and none is
/// implied: a backend translates this to its own enum, so the ordering below is nobody's
/// ABI. See Docs/Rendering/RHI.md.
///
/// **A format arrives with its first user, and not before.** The two below are the two
/// Phase A3 uses -- B8G8R8A8_UNORM for the swapchain, R8G8B8A8_UNORM for the texture the
/// readback test clears -- and the list stays that short deliberately. Every addition costs
/// a row in each switch in Types.cpp, an entry in the test's enumerator list, and a claim
/// about a size that nothing in the build verifies against real hardware.
///
/// Depth and stencil formats are absent for a second reason on top of that one: their
/// footprint is per *aspect*, so BytesPerPixel could not answer honestly for something like
/// D24_UNORM_S8_UINT. They arrive with the first pass that renders depth, alongside whatever
/// aspect-aware query it needs.
///
/// Phase A3 uses UNORM and not sRGB throughout. That is a decision, not an oversight:
/// whether a clear value on an sRGB image is encoded or written through unchanged is a real
/// subtlety with real vendor history, and A3 has no business settling colour management
/// before there is a shader and a reason to care.
enum class Format : u32 {
    Unknown = 0,
    R8G8B8A8_UNORM,
    B8G8R8A8_UNORM,
};

/// The enumerator's own spelling, for logs and test failures. Never nullptr.
///
/// A value outside the enumerator set gets a name of its own rather than "Unknown". An enum
/// with a fixed underlying type can hold one, and reporting it as a real format would send
/// whoever is reading the log after the wrong thing.
[[nodiscard]] const char* ToString(Format format);

/// Bytes one pixel of `format` occupies. Zero for Format::Unknown and for any value outside
/// the enumerator set -- callers sizing a staging buffer multiply this by a pixel count, so a
/// real format that reported zero by accident would produce an empty copy rather than an
/// error.
[[nodiscard]] u32 BytesPerPixel(Format format);

/// A size in pixels.
///
/// Zero in either dimension is a legitimate state, not an error: a minimised window
/// reports 0 x 0, and a swapchain cannot be created from that. Naming the state is what
/// lets the frame loop park rather than fail (Phase A3 Task 4).
struct Extent2D {
    u32 width  = 0;
    u32 height = 0;

    [[nodiscard]] constexpr bool IsEmpty() const { return width == 0 || height == 0; }
};

// Free functions in Extent2D's own namespace rather than friends, for the two reasons
// Monarc/Jobs/JobHandle.h gives at length: argument-dependent lookup looks at the operands'
// namespace rather than at whichever class declared a friend, and MSVC accepts
// [[nodiscard]] on a friend declaration where clang-cl correctly rejects it.
[[nodiscard]] constexpr bool operator==(const Extent2D& a, const Extent2D& b) {
    return a.width == b.width && a.height == b.height;
}

[[nodiscard]] constexpr bool operator!=(const Extent2D& a, const Extent2D& b) {
    return !(a == b);
}

}  // namespace Monarc::RHI
