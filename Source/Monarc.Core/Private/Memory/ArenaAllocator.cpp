#include <Monarc/Core/Memory/ArenaAllocator.h>

#include <Monarc/Core/Assert.h>

namespace Monarc {

ArenaAllocator::ArenaAllocator(void* buffer, usize capacity, const char* name)
    : m_base(static_cast<std::byte*>(buffer)),
      m_capacity(capacity),
      m_name(name != nullptr ? name : "Arena") {
    MONARC_CHECK(buffer != nullptr || capacity == 0, "arena needs a backing buffer");
}

void* ArenaAllocator::Allocate(usize size, usize alignment) {
    MONARC_CHECK(IsPowerOfTwo(alignment), "alignment must be a power of two");
    if (size == 0) {
        return nullptr;
    }

    const uptr  base    = reinterpret_cast<uptr>(m_base);
    const uptr  current = base + m_offset;
    const uptr  aligned = static_cast<uptr>(AlignUp(static_cast<usize>(current), alignment));
    const usize padding = static_cast<usize>(aligned - current);

    // Checked before advancing, so an exhausted arena leaves its cursor untouched.
    if (padding > m_capacity - m_offset || size > m_capacity - m_offset - padding) {
        return nullptr;
    }

    m_offset += padding + size;
    if (m_offset > m_highWaterMark) {
        m_highWaterMark = m_offset;
    }
    return reinterpret_cast<void*>(aligned);
}

void ArenaAllocator::Deallocate(void* /*pointer*/, usize /*size*/, usize /*alignment*/) {
    // Intentionally empty: arenas reclaim through Reset.
}

}  // namespace Monarc
