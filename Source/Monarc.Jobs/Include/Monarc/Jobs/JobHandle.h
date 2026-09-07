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

// Free functions in JobHandle's own namespace rather than hidden friends. For a plain
// non-template type there is no strong technical reason either way; this is the house form,
// and that is the whole of it.
//
// Saying so explicitly, because this comment used to give two reasons and both were wrong --
// and both had already been cited verbatim by two files in Monarc.RHI. What is actually
// true, measured on cl and clang-cl at /W4 /WX:
//
//   * A hidden friend is not hidden from ADL. A friend *defined inside* the class is found
//     by ADL from a template instantiated anywhere, including a template in a wholly
//     unrelated namespace: the class is its own associated entity, and ADL considers
//     friends declared in associated classes. So HashMap<JobHandle, V>::FindIndex comparing
//     keys with == from inside a class template needed nothing from this decision -- and
//     HashMap lives in namespace Monarc alongside JobHandle anyway, so the operands'
//     namespace was never in question to begin with.
//   * [[nodiscard]] on a hidden friend *definition* compiles clean on both compilers.
//     Containers/String.h's portability note is genuine but narrower than it was made out
//     to be: clang-cl rejects the attribute on a friend *declaration* separated from its
//     definition ("an attribute list cannot appear here"), which is the shape String.h uses
//     and this file does not.
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
