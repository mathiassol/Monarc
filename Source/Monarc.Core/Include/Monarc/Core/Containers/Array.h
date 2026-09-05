#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Memory/Allocator.h>

#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Monarc {

/// Dynamic array over an explicit allocator.
///
/// Copying is deleted: an allocating copy should be deliberate and visible. Duplicate
/// through an explicit Clone when one is needed.
template <typename T>
class Array {
    /// Largest element count whose byte size still fits in usize. Beyond this,
    /// `count * sizeof(T)` would wrap and we would allocate a fraction of what the
    /// caller believes they have.
    static constexpr usize kMaxCapacity = static_cast<usize>(-1) / sizeof(T);

public:
    explicit Array(IAllocator& allocator) : m_allocator(&allocator) {}

    ~Array() {
        Clear();
        Release();
    }

    Array(const Array&)            = delete;
    Array& operator=(const Array&) = delete;

    Array(Array&& other) noexcept
        : m_allocator(other.m_allocator),
          m_data(other.m_data),
          m_size(other.m_size),
          m_capacity(other.m_capacity) {
        other.m_data     = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }

    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            Clear();
            Release();
            m_allocator      = other.m_allocator;
            m_data           = other.m_data;
            m_size           = other.m_size;
            m_capacity       = other.m_capacity;
            other.m_data     = nullptr;
            other.m_size     = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    void Reserve(usize newCapacity) {
        if (newCapacity <= m_capacity) {
            return;
        }
        T* newData = AllocateBuffer(newCapacity);
        MoveElementsTo(newData);
        AdoptBuffer(newData, newCapacity);
    }

    T& Push(const T& value) { return Emplace(value); }
    T& Push(T&& value) { return Emplace(std::move(value)); }

    template <typename... Args>
    T& Emplace(Args&&... args) {
        if (m_size < m_capacity) {
            T* slot = std::construct_at(m_data + m_size, std::forward<Args>(args)...);
            ++m_size;
            return *slot;
        }

        // Growing. The new element is constructed into the new buffer BEFORE the old
        // elements are moved out and the old buffer released, because the arguments may
        // alias an existing element. `array.Push(array[0])` is legal, and constructing
        // after the move would read freed memory. std::vector orders it the same way.
        const usize newCapacity = NextCapacity();
        T* const    newData     = AllocateBuffer(newCapacity);

        T* slot = std::construct_at(newData + m_size, std::forward<Args>(args)...);
        MoveElementsTo(newData);
        AdoptBuffer(newData, newCapacity);
        ++m_size;
        return *slot;
    }

    void Pop() {
        MONARC_CHECK(m_size > 0, "Pop on an empty Array");
        if (m_size == 0) {
            return;
        }
        --m_size;
        std::destroy_at(m_data + m_size);
    }

    /// Destroys every element. Capacity is retained.
    void Clear() {
        for (usize i = 0; i < m_size; ++i) {
            std::destroy_at(m_data + i);
        }
        m_size = 0;
    }

    [[nodiscard]] T& operator[](usize index) {
        MONARC_CHECK(index < m_size, "Array index out of range");
        return m_data[index];
    }

    [[nodiscard]] const T& operator[](usize index) const {
        MONARC_CHECK(index < m_size, "Array index out of range");
        return m_data[index];
    }

    [[nodiscard]] usize Size() const { return m_size; }
    [[nodiscard]] usize Capacity() const { return m_capacity; }
    [[nodiscard]] bool  IsEmpty() const { return m_size == 0; }

    /// Largest element count this array can hold. Requesting more is fatal rather than
    /// silently wrapping the byte-size computation.
    [[nodiscard]] static constexpr usize MaxCapacity() { return kMaxCapacity; }

    [[nodiscard]] T*       Data() { return m_data; }
    [[nodiscard]] const T* Data() const { return m_data; }

    [[nodiscard]] T*       begin() { return m_data; }
    [[nodiscard]] T*       end() { return m_data + m_size; }
    [[nodiscard]] const T* begin() const { return m_data; }
    [[nodiscard]] const T* end() const { return m_data + m_size; }

private:
    static constexpr usize kInitialCapacity = 8;

    /// Allocation failure is fatal here, deliberately.
    ///
    /// Monarc has no fallible container API yet. The alternative — returning without
    /// growing — leaves `Emplace` constructing past the end of the buffer, which is
    /// silent heap corruption. Stopping is strictly better. When a caller appears that
    /// can genuinely recover from exhaustion, the right answer is a `TryReserve`
    /// returning `Status`, not a quiet failure here.
    [[noreturn]] static void OnAllocationFailed() {
        MONARC_DEBUG_BREAK();
        std::abort();
    }

    [[nodiscard]] usize NextCapacity() const {
        if (m_capacity == 0) {
            return kInitialCapacity;
        }
        // Saturate rather than wrap: doubling near the ceiling would produce a small
        // capacity and a buffer far smaller than callers expect.
        return m_capacity > kMaxCapacity / 2 ? kMaxCapacity : m_capacity * 2;
    }

    [[nodiscard]] T* AllocateBuffer(usize count) {
        MONARC_CHECK(count <= kMaxCapacity, "Array capacity would overflow usize");
        if (count > kMaxCapacity) {
            OnAllocationFailed();
        }
        T* buffer = static_cast<T*>(m_allocator->Allocate(count * sizeof(T), alignof(T)));
        MONARC_CHECK(buffer != nullptr, "Array allocation failed");
        if (buffer == nullptr) {
            OnAllocationFailed();
        }
        return buffer;
    }

    void MoveElementsTo(T* destination) {
        for (usize i = 0; i < m_size; ++i) {
            std::construct_at(destination + i, std::move(m_data[i]));
            std::destroy_at(m_data + i);
        }
    }

    void AdoptBuffer(T* buffer, usize capacity) {
        Release();
        m_data     = buffer;
        m_capacity = capacity;
    }

    void Release() {
        if (m_data != nullptr) {
            m_allocator->Deallocate(m_data, m_capacity * sizeof(T), alignof(T));
            m_data     = nullptr;
            m_capacity = 0;
        }
    }

    IAllocator* m_allocator = nullptr;
    T*          m_data      = nullptr;
    usize       m_size      = 0;
    usize       m_capacity  = 0;
};

}  // namespace Monarc
