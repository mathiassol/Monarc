#pragma once

#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Capabilities.h>

#include <span>

namespace Monarc::RHI {

/// A device's stable identity.
///
/// This is `VkPhysicalDeviceIDProperties::deviceUUID` -- core since Vulkan 1.1, queried
/// through `vkGetPhysicalDeviceProperties2` -- and it is the identity the Vulkan spec
/// actually guarantees. The two obvious alternatives do not: a device *name* is not unique
/// (the development machine reports four physical devices all called "Intel(R) UHD Graphics
/// 730"), and PCI bus information comes from an extension that a driver need not implement.
///
/// The size is fixed at 16 bytes by the spec. Monarc.RHI.Vulkan static_asserts that against
/// VK_UUID_SIZE rather than trusting this comment.
struct AdapterUuid {
    static constexpr usize kSize = 16;

    u8 bytes[kSize] = {};
};

// Free functions in AdapterUuid's own namespace, the house form Types.h's Extent2D uses.
// Written out rather than defaulted because an explicit loop is what makes it obvious that
// all sixteen bytes participate -- the single-byte-difference case in
// Tests/TestAdapter.cpp exists because a comparison that stopped early, or compared the
// address of the array rather than its contents, would collapse two real devices into one.
[[nodiscard]] constexpr bool operator==(const AdapterUuid& a, const AdapterUuid& b) {
    for (usize i = 0; i < AdapterUuid::kSize; ++i) {
        if (a.bytes[i] != b.bytes[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool operator!=(const AdapterUuid& a, const AdapterUuid& b) {
    return !(a == b);
}

/// Chars ToString(const AdapterUuid&) writes, including the terminator: 32 hex digits, four
/// dashes, and a NUL.
inline constexpr usize kAdapterUuidStringLength = 37;

/// A formatted UUID, owning its own characters.
///
/// A returned struct rather than a `std::span<char>` out-parameter, so there is no buffer
/// size to get wrong at a call site and nothing to document about one. `AdapterInfo` holds a
/// UUID as bytes because that is what comparison needs; this is for a log line.
struct AdapterUuidString {
    char text[kAdapterUuidStringLength] = {};
};

/// Formats `uuid` in the 8-4-4-4-12 grouping `vulkaninfo` prints -- lowercase hex, big-endian
/// byte order, e.g. `759c8156-7b91-7099-6511-5bd91de66f76`. The grouping is chosen so a
/// Monarc log line and a `vulkaninfo` dump can be compared by eye without transposing
/// anything, which is how the four-duplicate-Intel case was identified in the first place.
[[nodiscard]] AdapterUuidString ToString(const AdapterUuid& uuid);

/// Longest adapter name Monarc stores. `VK_MAX_PHYSICAL_DEVICE_NAME_SIZE` is 256 and
/// Monarc.RHI.Vulkan static_asserts that this is no smaller.
inline constexpr usize kMaxAdapterNameLength = 256;

/// Copies `source` into `destination`, truncating to fit and always null-terminating.
/// `nullptr` produces an empty string.
///
/// **This is the truncation `AdapterInfo::name` promises, as a function rather than as an
/// idiom.** It was open-coded in two places -- Monarc.RHI.Vulkan's enumeration and
/// Tests/TestAdapter.cpp's own adapter builder -- so the logic existed twice and neither copy
/// tested the other. Changing the bound from `kMaxAdapterNameLength - 1` to
/// `kMaxAdapterNameLength`, a one-byte overrun of a fixed array, passed both suites.
///
/// A reference to an array of exactly `kMaxAdapterNameLength` and not a `char*` with a size:
/// there is then no length for a caller to pass wrongly, and a shorter buffer does not
/// compile.
void CopyAdapterName(char (&destination)[kMaxAdapterNameLength], const char* source);

/// One physical device, as the RHI describes it. No backend type appears here.
///
/// The name is an owned fixed-size array rather than a pointer or a `String`, for the same
/// reason `Error::message` is a view over a literal: an AdapterInfo is copied around, sorted
/// and compacted, and a name pointing into a driver-owned structure would be a dangling
/// pointer the moment enumeration returned. Trivially copyable, which DeduplicateAdapters
/// below relies on.
struct AdapterInfo {
    /// Null-terminated. Truncated rather than dropped if a driver ever reports a longer
    /// name than kMaxAdapterNameLength; a truncated name is still recognisable, where an
    /// empty one is not.
    char name[kMaxAdapterNameLength] = {};

    AdapterUuid uuid = {};
    DeviceType  type = DeviceType::Other;

    /// PCI-style vendor and device ids, as the driver reports them. Not an identity -- the
    /// four duplicate Intel entries on the development machine share these too -- but useful
    /// in a log, and the thing a vendor-specific workaround will key off later.
    u32 vendorId = 0;
    u32 deviceId = 0;

    /// The device's own API version lives in `capabilities.apiVersion`, and is not repeated
    /// here: two copies of one number is one more than can be kept in step.
    Capabilities capabilities = {};

    /// `DetermineTier(capabilities)`, cached at enumeration so a caller comparing tiers does
    /// not re-derive it per comparison. Tests/TestAdapter.cpp does not check that these
    /// agree -- Monarc.RHI.Vulkan's device test does, on real hardware, where a disagreement
    /// would mean enumeration filled one of them by hand.
    CapabilityTier tier = CapabilityTier::Unsupported;
};

/// Removes adapters whose UUID has already been seen, keeping the **first** of each and the
/// relative order of everything kept, and returns how many are left at the front of
/// `adapters`.
///
/// A pure function over a mutable span: it allocates nothing, calls nothing, and compacts in
/// place, which is what lets CI test the real deduplication with no Vulkan device present at
/// all. The A3 plan wrote the parameter as a *const* span; that was indicative rather than
/// considered, since a const span cannot compact.
///
/// The tail past the returned count is left as it was, exactly as `std::unique` leaves its
/// own -- unspecified, not cleared. Zeroing it would replace stale duplicates with
/// zero-UUID, empty-named entries that look just as much like adapters and would be no
/// safer to read.
///
/// This exists because of an observed defect, not a hypothetical one:
/// `vkEnumeratePhysicalDevices` returns **five** physical devices on the development machine
/// -- one NVIDIA and four Intel entries sharing a single UUID, because each virtual display
/// adapter installed on that machine re-registers the Intel ICD. Without this the adapter
/// list a player sees has four identical rows in it. See Docs/Status.md.
[[nodiscard]] usize DeduplicateAdapters(std::span<AdapterInfo> adapters);

}  // namespace Monarc::RHI
