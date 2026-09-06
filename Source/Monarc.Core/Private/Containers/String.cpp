#include <Monarc/Core/Containers/String.h>

#include <Monarc/Core/Assert.h>

#include <cstdlib>
#include <cstring>

namespace Monarc {

namespace {
constexpr usize kInitialCapacity = 15;
}  // namespace

String::String(IAllocator& allocator) : m_allocator(&allocator) {}

String::String(IAllocator& allocator, StringView text) : m_allocator(&allocator) {
    Append(text);
}

String::~String() {
    ReleaseBuffer();
}

String::String(String&& other) noexcept
    : m_allocator(other.m_allocator),
      m_data(other.m_data),
      m_size(other.m_size),
      m_capacity(other.m_capacity) {
    other.m_data     = nullptr;
    other.m_size     = 0;
    other.m_capacity = 0;
}

String& String::operator=(String&& other) noexcept {
    if (this != &other) {
        ReleaseBuffer();
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

void String::Reserve(usize newCapacity) {
    if (newCapacity <= m_capacity) {
        return;
    }
    char* newData = AllocateBuffer(newCapacity);
    if (m_size > 0) {
        std::memcpy(newData, m_data, m_size);
    }
    newData[m_size] = '\0';
    AdoptBuffer(newData, newCapacity);
}

void String::Append(StringView text) {
    if (text.empty()) {
        return;
    }

    const usize addedSize = text.size();
    // m_size <= kMaxCapacity always holds, so this subtraction cannot underflow; it is the
    // standard "check before adding" idiom that avoids computing the (possibly overflowing)
    // sum first.
    MONARC_CHECK(addedSize <= kMaxCapacity - m_size, "String size would overflow");
    if (addedSize > kMaxCapacity - m_size) {
        OnAllocationFailed();
    }
    const usize newSize = m_size + addedSize;

    if (newSize > m_capacity) {
        const usize newCapacity = NextCapacityFor(newSize);
        char* const newData     = AllocateBuffer(newCapacity);

        // Copy the existing content and the new text into the new buffer BEFORE the old
        // buffer is released. `text` may alias the current (about-to-be-freed) buffer --
        // s.Append(s.View()) is legal -- and reading from it here is still safe because
        // the old buffer has not been touched yet. Releasing first and copying after would
        // read freed memory whenever `text` aliased it.
        if (m_size > 0) {
            std::memcpy(newData, m_data, m_size);
        }
        std::memcpy(newData + m_size, text.data(), addedSize);
        newData[newSize] = '\0';

        AdoptBuffer(newData, newCapacity);
    } else {
        // Enough room already. `text` can only alias bytes within [0, m_size) -- the
        // string's current contents -- which never overlaps the destination range
        // [m_size, newSize), so a plain memcpy (rather than memmove) is safe here too.
        std::memcpy(m_data + m_size, text.data(), addedSize);
        m_data[newSize] = '\0';
    }

    m_size = newSize;
}

void String::PushBack(char c) {
    Append(StringView(&c, 1));
}

void String::Clear() {
    m_size = 0;
    if (m_data != nullptr) {
        m_data[0] = '\0';
    }
}

char& String::operator[](usize index) {
    MONARC_CHECK(index < m_size, "String index out of range");
    return m_data[index];
}

const char& String::operator[](usize index) const {
    MONARC_CHECK(index < m_size, "String index out of range");
    return m_data[index];
}

usize String::Size() const {
    return m_size;
}

usize String::Capacity() const {
    return m_capacity;
}

bool String::IsEmpty() const {
    return m_size == 0;
}

const char* String::CStr() const {
    // m_data is nullptr only when nothing has ever been allocated (a default-constructed
    // or moved-from string). Fall back to a string literal rather than a shared mutable
    // buffer: it is read-only storage, so there is no instance to corrupt another with.
    return m_data != nullptr ? m_data : "";
}

StringView String::View() const {
    return StringView(CStr(), m_size);
}

char* String::Data() {
    return m_data;
}

const char* String::Data() const {
    return m_data;
}

bool operator==(const String& a, const String& b) {
    return a.View() == b.View();
}

bool operator==(const String& a, StringView b) {
    return a.View() == b;
}

u64 Hasher<String>::operator()(const String& s) const {
    return Hash(s.View());
}

void String::OnAllocationFailed() {
    MONARC_DEBUG_BREAK();
    std::abort();
}

usize String::NextCapacityFor(usize minCapacity) const {
    usize doubled;
    if (m_capacity == 0) {
        doubled = kInitialCapacity;
    } else if (m_capacity > kMaxCapacity / 2) {
        // Saturate rather than let capacity * 2 wrap past kMaxCapacity.
        doubled = kMaxCapacity;
    } else {
        doubled = m_capacity * 2;
    }
    return doubled > minCapacity ? doubled : minCapacity;
}

char* String::AllocateBuffer(usize capacity) const {
    MONARC_CHECK(capacity <= kMaxCapacity, "String capacity would overflow usize");
    if (capacity > kMaxCapacity) {
        OnAllocationFailed();
    }
    // capacity <= kMaxCapacity == SIZE_MAX - 1, so capacity + 1 cannot overflow.
    char* buffer = static_cast<char*>(m_allocator->Allocate(capacity + 1, alignof(char)));
    MONARC_CHECK(buffer != nullptr, "String allocation failed");
    if (buffer == nullptr) {
        OnAllocationFailed();
    }
    return buffer;
}

void String::AdoptBuffer(char* buffer, usize capacity) {
    ReleaseBuffer();
    m_data     = buffer;
    m_capacity = capacity;
}

void String::ReleaseBuffer() {
    if (m_data != nullptr) {
        m_allocator->Deallocate(m_data, m_capacity + 1, alignof(char));
        m_data     = nullptr;
        m_capacity = 0;
    }
}

}  // namespace Monarc
