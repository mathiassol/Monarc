#include <Monarc/RHI/Adapter.h>

namespace Monarc::RHI {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// Byte offsets a dash goes *before*, giving the 8-4-4-4-12 grouping. Four dashes, so four
/// entries; a byte index rather than a character index, because the loop below walks bytes.
constexpr usize kDashBefore[] = {4, 6, 8, 10};

[[nodiscard]] constexpr bool IsDashBefore(usize byteIndex) {
    for (const usize position : kDashBefore) {
        if (position == byteIndex) {
            return true;
        }
    }
    return false;
}

}  // namespace

AdapterUuidString ToString(const AdapterUuid& uuid) {
    AdapterUuidString result;
    usize             out = 0;
    for (usize i = 0; i < AdapterUuid::kSize; ++i) {
        if (IsDashBefore(i)) {
            result.text[out++] = '-';
        }
        result.text[out++] = kHexDigits[(uuid.bytes[i] >> 4) & 0xF];
        result.text[out++] = kHexDigits[uuid.bytes[i] & 0xF];
    }
    // The terminator is already there -- AdapterUuidString value-initialises its array -- but
    // written explicitly so that this function's contract does not depend on a default
    // initialiser in a different file staying as it is.
    result.text[out] = '\0';
    return result;
}

usize DeduplicateAdapters(std::span<AdapterInfo> adapters) {
    // O(n^2) against the kept prefix, deliberately. n is the number of physical devices on a
    // machine -- five on the development machine, and a number that cannot plausibly reach
    // three digits -- so a hash set would cost an allocation and a Hasher specialisation to
    // save nothing measurable, and this way the function allocates nothing at all and can be
    // called from an error path.
    usize kept = 0;
    for (usize i = 0; i < adapters.size(); ++i) {
        bool alreadySeen = false;
        for (usize j = 0; j < kept; ++j) {
            if (adapters[j].uuid == adapters[i].uuid) {
                alreadySeen = true;
                break;
            }
        }
        if (alreadySeen) {
            continue;
        }
        // Comparing against the *kept* prefix and not against everything before i is what
        // makes "keeps the first" true: adapters[j] for j < kept is a survivor, so a
        // duplicate is only ever measured against the entry that will represent it.
        if (kept != i) {
            adapters[kept] = adapters[i];
        }
        ++kept;
    }
    return kept;
}

}  // namespace Monarc::RHI
