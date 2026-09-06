#pragma once

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Error.h>

#include <cstddef>

namespace Monarc::Platform {

/// A dynamically loaded library (a DLL on Windows).
///
/// Non-copyable, movable. Unlike Thread, there is no concurrency hazard in one of these
/// going out of scope while open: the destructor simply closes it, like any other RAII
/// handle -- nothing is running on another thread that could be left referencing memory
/// the way a detached Thread would be.
class Library {
public:
    Library() noexcept;
    ~Library();

    Library(const Library&)            = delete;
    Library& operator=(const Library&) = delete;

    Library(Library&& other) noexcept;
    Library& operator=(Library&& other) noexcept;

    /// Loads the library named by path -- a bare name such as "kernel32.dll" is resolved
    /// through the platform's normal library search, exactly as path is given; Monarc
    /// applies no search logic of its own. Fails with ErrorCode::NotFound if it cannot be
    /// located.
    [[nodiscard]] static Result<Library> Open(StringView path);

    /// Looks up an exported symbol by name. Returns nullptr if this Library is not open,
    /// or if name is not exported by it -- the two are indistinguishable to a caller,
    /// which matches every real use of the result: either way, there is nothing to call.
    [[nodiscard]] void* Symbol(StringView name) const;

    /// Releases the library if it is open. Safe to call unconditionally, and safe to call
    /// more than once.
    void Close();

    [[nodiscard]] bool IsOpen() const;

    /// A library name and an exported symbol within it, both known to exist on this
    /// platform, so a test can load a real library and resolve a real symbol without
    /// hardcoding a platform-specific name itself.
    [[nodiscard]] static StringView SystemLibraryName();
    [[nodiscard]] static StringView SystemSymbolName();

private:
    // Opaque storage for a platform library handle (a Win32 HMODULE). Raw bytes rather
    // than a typed member, so this header names no platform type and its layout does not
    // depend on which platform is building it (ADR-0016; see also Platform::Mutex's own
    // storage member in Thread.h). Sized for exactly one native handle-shaped value; the
    // platform .cpp static_asserts that whatever it actually stores here fits.
    alignas(alignof(void*)) std::byte m_storage[sizeof(void*)];
};

}  // namespace Monarc::Platform
