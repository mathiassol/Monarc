#pragma once

#include <Monarc/Core/Hash.h>
#include <Monarc/Core/Types.h>

namespace Monarc {

/// Names one submitted job: an index into JobSystem's fixed-capacity pool, paired with a
/// generation counter.
///
/// A pool slot is reused once its job completes, so the index alone cannot tell a job
/// apart from whatever gets submitted into that same slot next. The generation can:
/// JobSystem bumps it every time a slot is recycled, so a handle whose generation no
/// longer matches its slot names a job that is long gone rather than the new occupant --
/// see ADR-0002.
///
/// Trivially copyable. Default-constructs invalid (an index no real job ever has), and
/// nothing but JobSystem itself should construct a valid one -- ForTesting exists so that
/// any other construction in the codebase is deliberately visible as a test backdoor,
/// never something that looks like ordinary code.
struct JobHandle {
    u32 index      = kInvalidIndex;
    u32 generation = 0;

    [[nodiscard]] constexpr bool IsValid() const { return index != kInvalidIndex; }

    /// Constructs a handle naming slot `index` at `generation` directly, bypassing
    /// JobSystem entirely. For tests that need handles JobSystem never actually returned --
    /// see the class comment above.
    [[nodiscard]] static constexpr JobHandle ForTesting(u32 index, u32 generation) {
        return JobHandle{index, generation};
    }

private:
    /// No real slot ever reaches this index -- see JobSystem::Config::maxJobs, which a
    /// submitter cannot exceed without Submit already failing.
    static constexpr u32 kInvalidIndex = static_cast<u32>(-1);
};

// Free functions in JobHandle's own namespace, not friends: HashMap<JobHandle, V>::FindIndex
// compares keys with == from inside a template defined elsewhere, which finds this operator
// only through argument-dependent lookup -- and ADL looks at the operands' namespace,
// Monarc, not at whichever class happened to declare a friend. A plain free function here
// is what makes that lookup succeed. It also sidesteps String.h's friend/[[nodiscard]]
// portability trap (MSVC accepts [[nodiscard]] on a friend declaration, clang-cl does not):
// with no friend declaration at all, [[nodiscard]] below is unremarkable on both compilers.
[[nodiscard]] constexpr bool operator==(const JobHandle& a, const JobHandle& b) {
    return a.index == b.index && a.generation == b.generation;
}

[[nodiscard]] constexpr bool operator!=(const JobHandle& a, const JobHandle& b) {
    return !(a == b);
}

template <>
struct Hasher<JobHandle> {
    // JobHandle is trivially copyable with no padding (two consecutive u32 members), so
    // hashing its raw bytes is equivalent to hashing (index, generation) directly --
    // mirrors Hasher<Guid> in Platform/Guid.h for the same reason.
    [[nodiscard]] u64 operator()(const JobHandle& handle) const {
        return HashBytes(&handle, sizeof(handle));
    }
};

}  // namespace Monarc
