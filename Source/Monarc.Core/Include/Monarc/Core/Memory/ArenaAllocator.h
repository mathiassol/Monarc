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
