#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Memory/Allocator.h>

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

        T* newData = static_cast<T*>(m_allocator->Allocate(newCapacity * sizeof(T), alignof(T)));
        MONARC_CHECK(newData != nullptr, "Array allocation failed");
        if (newData == nullptr) {
            return;
        }

        for (usize i = 0; i < m_size; ++i) {
            std::construct_at(newData + i, std::move(m_data[i]));
            std::destroy_at(m_data + i);
        }

        Release();
        m_data     = newData;
        m_capacity = newCapacity;
    }

    T& Push(const T& value) { return Emplace(value); }
    T& Push(T&& value) { return Emplace(std::move(value)); }

    template <typename... Args>
    T& Emplace(Args&&... args) {
        if (m_size == m_capacity) {
            Reserve(m_capacity == 0 ? kInitialCapacity : m_capacity * 2);
        }
        T* slot = std::construct_at(m_data + m_size, std::forward<Args>(args)...);
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

    [[nodiscard]] T*       Data() { return m_data; }
    [[nodiscard]] const T* Data() const { return m_data; }

    [[nodiscard]] T*       begin() { return m_data; }
    [[nodiscard]] T*       end() { return m_data + m_size; }
    [[nodiscard]] const T* begin() const { return m_data; }
    [[nodiscard]] const T* end() const { return m_data + m_size; }

private:
    static constexpr usize kInitialCapacity = 8;

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
