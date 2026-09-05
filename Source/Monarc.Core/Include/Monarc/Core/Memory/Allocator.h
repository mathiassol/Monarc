#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc {

/// Interface every Monarc allocator implements.
///
/// Alignment is always explicit. There is no default argument, because default
/// arguments on virtual functions bind to the static type and silently disagree
/// across an override.
class IAllocator {
public:
    virtual ~IAllocator() = default;

    IAllocator(const IAllocator&)            = delete;
    IAllocator& operator=(const IAllocator&) = delete;

    /// Returns nullptr on failure. `alignment` must be a power of two.
    ///
    /// A zero-size request also returns nullptr. Callers cannot therefore distinguish
    /// "asked for nothing" from "out of memory" by the return value alone — check the
    /// size first if that distinction matters. Pairing that nullptr with
    /// `Deallocate(nullptr, 0, alignment)` is safe.
    [[nodiscard]] virtual void* Allocate(usize size, usize alignment) = 0;

    /// `size` and `alignment` must match the original call. Passing nullptr is a no-op.
    virtual void Deallocate(void* pointer, usize size, usize alignment) = 0;

    /// Bytes currently outstanding. Every allocator reports usage so that budgets and
    /// per-subsystem reports need no extra machinery — see Docs/Runtime/Memory.md.
    [[nodiscard]] virtual usize BytesAllocated() const = 0;

    [[nodiscard]] virtual const char* Name() const = 0;

protected:
    IAllocator() = default;
};

/// True when `value` is a power of two, which every alignment must be.
[[nodiscard]] constexpr bool IsPowerOfTwo(usize value) {
    return value != 0 && (value & (value - 1)) == 0;
}

/// Rounds `value` up to the next multiple of `alignment`.
[[nodiscard]] constexpr usize AlignUp(usize value, usize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace Monarc
