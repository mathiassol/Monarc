#pragma once

#include <Monarc/Core/Hash.h>
#include <Monarc/Core/Memory/Allocator.h>

#include <string_view>

namespace Monarc {

/// Non-owning view of characters. An alias rather than a distinct type: std::string_view
/// is header-only, allocation-free and universally available, and a span of characters is
/// not where an engine's character lives. See the plan for the reasoning.
using StringView = std::string_view;

/// Owning, null-terminated, growable string over an explicit allocator.
///
/// Null-terminated because it talks to logging, paths and OS calls, all of which want a
/// `const char*`. `Size()` excludes the terminator; embedded nulls are preserved and are
/// visible through `View()` but will truncate `CStr()` for a C consumer.
///
/// Copying is deleted, as with Array<T>: an allocating copy should be visible at the call
/// site. There is no small-string optimisation yet -- a deliberate deferral, and an
/// implementation detail this interface does not expose.
class String {
public:
    explicit String(IAllocator& allocator);
    String(IAllocator& allocator, StringView text);

    ~String();

    String(const String&)            = delete;
    String& operator=(const String&) = delete;

    String(String&& other) noexcept;
    String& operator=(String&& other) noexcept;

    void Reserve(usize capacity);
    void Append(StringView text);
    void PushBack(char c);

    /// Destroys the contents. Capacity is retained.
    void Clear();

    [[nodiscard]] char&       operator[](usize index);
    [[nodiscard]] const char& operator[](usize index) const;

    [[nodiscard]] usize Size() const;
    [[nodiscard]] usize Capacity() const;
    [[nodiscard]] bool  IsEmpty() const;

    /// Always returns a valid null-terminated pointer, including for an empty or
    /// moved-from string. Never returns nullptr.
    [[nodiscard]] const char* CStr() const;

    [[nodiscard]] StringView View() const;

    [[nodiscard]] char*       Data();
    [[nodiscard]] const char* Data() const;

    // [[nodiscard]] on a friend function is accepted by MSVC but rejected by clang-cl
    // ("an attribute list cannot appear here") in both attribute positions tried, so it is
    // omitted here to keep both compilers building warning-free.
    friend bool operator==(const String& a, const String& b);
    friend bool operator==(const String& a, StringView b);

private:
    /// Largest character count whose allocation size -- capacity plus one, for the
    /// terminator -- still fits in usize. Mirrors Array<T>::kMaxCapacity, adjusted for the
    /// extra byte every buffer here carries.
    static constexpr usize kMaxCapacity = static_cast<usize>(-1) - 1;

    [[noreturn]] static void OnAllocationFailed();

    [[nodiscard]] usize NextCapacityFor(usize minCapacity) const;
    [[nodiscard]] char* AllocateBuffer(usize capacity) const;
    void                AdoptBuffer(char* buffer, usize capacity);
    void                ReleaseBuffer();

    IAllocator* m_allocator = nullptr;
    char*       m_data      = nullptr;
    usize       m_size      = 0;
    usize       m_capacity  = 0;
};

template <>
struct Hasher<String> {
    [[nodiscard]] u64 operator()(const String& s) const;
};

}  // namespace Monarc
