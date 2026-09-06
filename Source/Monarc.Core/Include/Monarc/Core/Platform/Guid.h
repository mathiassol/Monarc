#pragma once

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Hash.h>
#include <Monarc/Core/Types.h>

#include <compare>

namespace Monarc {

/// A 128-bit identity value.
///
/// Lives in namespace Monarc, not Monarc::Platform: it is a value type the whole engine
/// passes around and stores -- asset references, per ADR-0008 -- and only its generation
/// draws on anything platform-specific. Comparison, formatting and parsing below are pure
/// bit and string manipulation, portable wherever this header is included.
///
/// Trivially copyable, and default-constructs to nil at compile time.
class Guid {
public:
    constexpr Guid() noexcept = default;

    /// Draws 128 bits from the platform's cryptographic entropy source. Deliberately not
    /// rand(), not time-based, and not a process-local counter: ADR-0008 makes this asset
    /// identity, so two machines generating one at the same moment must not collide, and
    /// none of those three sources make that guarantee -- only real entropy does.
    [[nodiscard]] static Guid Generate();

    [[nodiscard]] constexpr bool IsNil() const noexcept { return m_high == 0 && m_low == 0; }

    [[nodiscard]] constexpr bool operator==(const Guid&) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const Guid&) const noexcept = default;

    /// Length of the canonical string form below, excluding the terminator.
    static constexpr usize kStringLength = 36;

    /// Writes the canonical lowercase, hyphenated form -- xxxxxxxx-xxxx-xxxx-xxxx-
    /// xxxxxxxxxxxx, exactly kStringLength characters -- then a terminator, to out. out
    /// must have room for kStringLength + 1 bytes.
    void Format(char* out) const noexcept;

    /// Parses the canonical form, in either case. Rejects anything else -- wrong length, a
    /// missing separator, a non-hex character -- rather than guessing at what was meant.
    /// Checks the length before indexing into text at all, so a short input is rejected
    /// outright rather than read past its end.
    [[nodiscard]] static Result<Guid> Parse(StringView text);

private:
    u64 m_high = 0;
    u64 m_low  = 0;
};

template <>
struct Hasher<Guid> {
    // Guid is trivially copyable with no padding (two consecutive u64 members), so hashing
    // its raw bytes is equivalent to hashing (m_high, m_low) directly -- without needing
    // access to either, or Guid needing to expose them.
    [[nodiscard]] u64 operator()(const Guid& guid) const { return HashBytes(&guid, sizeof(guid)); }
};

}  // namespace Monarc
