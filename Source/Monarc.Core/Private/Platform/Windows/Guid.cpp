#include <Monarc/Core/Platform/Guid.h>

#include <Monarc/Core/Assert.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace Monarc {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// Writes digitCount lowercase hex characters of value's low digitCount * 4 bits, most
/// significant nibble first, to out[0 .. digitCount).
void WriteHex(char* out, u64 value, int digitCount) {
    for (int i = digitCount - 1; i >= 0; --i) {
        out[i] = kHexDigits[value & 0xF];
        value >>= 4;
    }
}

/// Parses exactly digitCount hex characters at text[offset .. offset + digitCount) into
/// out. Returns false, leaving out unspecified, on the first non-hex character -- Parse
/// below turns that into ErrorCode::InvalidArgument rather than guessing.
[[nodiscard]] bool ParseHex(StringView text, usize offset, int digitCount, u64& out) {
    u64 value = 0;
    for (int i = 0; i < digitCount; ++i) {
        const char c = text[offset + static_cast<usize>(i)];
        u64        nibble;
        if (c >= '0' && c <= '9') {
            nibble = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nibble = static_cast<u64>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    out = value;
    return true;
}

}  // namespace

Guid Guid::Generate() {
    struct {
        u64 high;
        u64 low;
    } bits{};
    static_assert(sizeof(bits) == 16, "Guid::Generate expects exactly 128 bits of entropy");

    // BCRYPT_USE_SYSTEM_PREFERRED_RNG with a null algorithm handle is CNG's documented
    // shorthand for "use the platform's default random number generator" and needs no
    // BCryptOpenAlgorithmProvider/Close pair to manage -- see the header comment on
    // Generate for why this, and not rand()/time/a counter, is the only acceptable source.
    const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&bits),
                                             sizeof(bits), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    MONARC_CHECK(BCRYPT_SUCCESS(status),
                 "BCryptGenRandom failed to produce entropy for Guid::Generate");

    Guid result;
    result.m_high = bits.high;
    result.m_low  = bits.low;
    return result;
}

void Guid::Format(char* out) const noexcept {
    // Canonical 8-4-4-4-12 hex layout. The high 64 bits supply the first three groups
    // (8 + 4 + 4 = 16 hex digits), the low 64 bits the last two (4 + 12 = 16 digits) --
    // together the 32 hex digits this 128-bit value is made of.
    WriteHex(out, m_high >> 32, 8);
    out[8] = '-';
    WriteHex(out + 9, (m_high >> 16) & 0xFFFFull, 4);
    out[13] = '-';
    WriteHex(out + 14, m_high & 0xFFFFull, 4);
    out[18] = '-';
    WriteHex(out + 19, m_low >> 48, 4);
    out[23] = '-';
    WriteHex(out + 24, m_low & 0xFFFF'FFFF'FFFFull, 12);
    out[36] = '\0';
}

Result<Guid> Guid::Parse(StringView text) {
    // Checked before any indexed access below, so a short input is rejected outright
    // rather than read past its end.
    if (text.size() != kStringLength) {
        return Err(ErrorCode::InvalidArgument, "text must be exactly 36 characters");
    }
    if (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return Err(ErrorCode::InvalidArgument, "text is missing a '-' separator");
    }

    u64 g0 = 0, g1 = 0, g2 = 0, g3 = 0, g4 = 0;
    if (!ParseHex(text, 0, 8, g0) || !ParseHex(text, 9, 4, g1) ||
        !ParseHex(text, 14, 4, g2) || !ParseHex(text, 19, 4, g3) ||
        !ParseHex(text, 24, 12, g4)) {
        return Err(ErrorCode::InvalidArgument, "text contains a non-hex character");
    }

    Guid result;
    result.m_high = (g0 << 32) | (g1 << 16) | g2;
    result.m_low  = (g3 << 48) | g4;
    return result;
}

}  // namespace Monarc
