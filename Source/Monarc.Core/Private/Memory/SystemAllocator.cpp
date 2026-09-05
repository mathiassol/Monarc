#include <Monarc/Core/Memory/SystemAllocator.h>

#include <Monarc/Core/Assert.h>

#include <cstdlib>
#include <new>

namespace Monarc {

void* SystemAllocator::Allocate(usize size, usize alignment) {
    MONARC_CHECK(IsPowerOfTwo(alignment), "alignment must be a power of two");
    if (size == 0) {
        return nullptr;
    }

    // std::aligned_alloc requires size to be a multiple of alignment; operator new
    // with an alignment argument has no such constraint and is available everywhere
    // we build.
    void* pointer = ::operator new(size, std::align_val_t{alignment}, std::nothrow);
    if (pointer != nullptr) {
        m_bytesAllocated.fetch_add(size, std::memory_order_relaxed);
    }
    return pointer;
}

void SystemAllocator::Deallocate(void* pointer, usize size, usize alignment) {
    if (pointer == nullptr) {
        return;
    }
    ::operator delete(pointer, std::align_val_t{alignment});
    m_bytesAllocated.fetch_sub(size, std::memory_order_relaxed);
}

usize SystemAllocator::BytesAllocated() const {
    return m_bytesAllocated.load(std::memory_order_relaxed);
}

SystemAllocator& SystemAllocator::Get() {
    static SystemAllocator instance;
    return instance;
}

}  // namespace Monarc
