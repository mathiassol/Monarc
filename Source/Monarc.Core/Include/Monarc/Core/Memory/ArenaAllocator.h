#pragma once

#include <Monarc/Core/Memory/Allocator.h>

#include <cstddef>

namespace Monarc {

/// Linear bump allocator over a caller-supplied buffer.
///
/// Individual deallocation is not supported: Deallocate is a no-op and Reset reclaims
/// the whole arena at once. This is the right shape for anything with a clear phase
/// boundary — loading, cooking, and the per-frame render snapshot.
///
/// The arena does not own its backing memory, so its lifetime must not exceed the
/// buffer's.
///
/// Two traps worth knowing before reaching for one. Both were measured, not theorised:
///
/// 1. **Reset does not destroy anything.** Placement-constructing a non-trivially
///    destructible object here and then calling Reset leaks whatever that object owned,
///    silently and permanently. That includes an `Array<T>` control block living in an
///    arena while its *elements* live in some other allocator — Reset skips `~Array`, and
///    the elements leak out of the unrelated allocator. `IAllocator` is type-erased, so
///    nothing can detect this for you; it is a contract, not a guarantee.
///
/// 2. **Containers that reallocate waste arena space.** `Deallocate` is a no-op, so every
///    superseded buffer stays consumed. An `Array<int>` pushed one element at a time to
///    10,000 entries occupies 64 KB live but advances the cursor 128 KB — exactly twice.
///    Interleave two growing arrays and only half the consumed space is live. Reserve the
///    final capacity up front, or back the container with a different allocator.
class ArenaAllocator final : public IAllocator {
public:
    ArenaAllocator(void* buffer, usize capacity, const char* name);

    [[nodiscard]] void* Allocate(usize size, usize alignment) override;

    /// No-op. Arenas reclaim through Reset.
    void Deallocate(void* pointer, usize size, usize alignment) override;

    [[nodiscard]] usize       BytesAllocated() const override { return m_offset; }
    [[nodiscard]] const char* Name() const override { return m_name; }

    /// Reclaims the entire arena. Does not run destructors — arena-allocated types must
    /// be trivially destructible, or destroyed explicitly before Reset.
    void Reset() { m_offset = 0; }

    [[nodiscard]] usize Capacity() const { return m_capacity; }

    /// Peak usage since construction, surviving Reset. This is the number budgeting needs.
    [[nodiscard]] usize HighWaterMark() const { return m_highWaterMark; }

private:
    std::byte*  m_base          = nullptr;
    usize       m_capacity      = 0;
    usize       m_offset        = 0;
    usize       m_highWaterMark = 0;
    const char* m_name          = "Arena";
};

}  // namespace Monarc
