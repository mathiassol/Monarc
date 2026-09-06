#include <Monarc/Core/Platform/Library.h>

#include <Monarc/Core/Assert.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstring>

namespace Monarc::Platform {

namespace {

constexpr int kMaxPathWide = 4096;

/// Converts a UTF-8 path/name to UTF-16 into a fixed buffer. Duplicated from (rather than
/// shared with) Windows/File.cpp's identical helper: both are anonymous-namespace
/// internals of their own translation unit, and ADR-0016 leaves a shared home for small
/// cross-file helpers like this one as a deliberate step nothing has needed to take yet.
int ToWide(StringView utf8, wchar_t* buffer, int bufferCount) {
    MONARC_CHECK(bufferCount > 0, "ToWide needs a non-empty buffer");
    if (utf8.empty()) {
        buffer[0] = L'\0';
        return 0;
    }
    const int written = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()), buffer,
                                             bufferCount - 1);
    if (written <= 0) {
        buffer[0] = L'\0';
        return 0;
    }
    buffer[written] = L'\0';
    return written;
}

[[nodiscard]] ErrorCode MapLastError(DWORD lastError) {
    switch (lastError) {
        case ERROR_MOD_NOT_FOUND:
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
            return ErrorCode::PermissionDenied;
        default:
            return ErrorCode::IoFailure;
    }
}

// Library's storage holds a plain HMODULE (a scalar pointer value), read and written by
// byte-copy for the same reason Platform::Thread's HANDLE storage is -- see
// Windows/Thread.cpp -- rather than needing any placement-new/Initialize ceremony.
[[nodiscard]] HMODULE LoadModule(const std::byte* storage) {
    HMODULE module;
    std::memcpy(&module, storage, sizeof(module));
    return module;
}

void StoreModule(std::byte* storage, HMODULE module) {
    std::memcpy(storage, &module, sizeof(module));
}

}  // namespace

static_assert(sizeof(HMODULE) <= sizeof(Library) && alignof(HMODULE) <= alignof(Library),
              "HMODULE does not fit Library's opaque storage");

Library::Library() noexcept { StoreModule(m_storage, nullptr); }

Library::~Library() { Close(); }

Library::Library(Library&& other) noexcept {
    StoreModule(m_storage, LoadModule(other.m_storage));
    StoreModule(other.m_storage, nullptr);
}

Library& Library::operator=(Library&& other) noexcept {
    if (this != &other) {
        Close();   // Release whatever this Library currently holds before overwriting it.
        StoreModule(m_storage, LoadModule(other.m_storage));
        StoreModule(other.m_storage, nullptr);
    }
    return *this;
}

Result<Library> Library::Open(StringView path) {
    wchar_t wide[kMaxPathWide];
    ToWide(path, wide, kMaxPathWide);

    const HMODULE module = LoadLibraryW(wide);
    if (module == nullptr) {
        return Err(MapLastError(GetLastError()), "could not load library");
    }

    Library result;
    StoreModule(result.m_storage, module);
    return result;
}

void* Library::Symbol(StringView name) const {
    const HMODULE module = LoadModule(m_storage);
    if (module == nullptr) {
        return nullptr;
    }

    // GetProcAddress needs a null-terminated ANSI string; name (a StringView) carries no
    // such guarantee, so it is copied into a small fixed buffer first. Symbol names are
    // short identifiers, so truncation here only ever affects a name nobody would
    // realistically export, and a truncated lookup simply, safely, fails to find one.
    char buffer[512];
    const usize length = name.size() < sizeof(buffer) - 1 ? name.size() : sizeof(buffer) - 1;
    std::memcpy(buffer, name.data(), length);
    buffer[length] = '\0';

    return reinterpret_cast<void*>(GetProcAddress(module, buffer));
}

void Library::Close() {
    const HMODULE module = LoadModule(m_storage);
    if (module != nullptr) {
        FreeLibrary(module);
        StoreModule(m_storage, nullptr);
    }
}

bool Library::IsOpen() const { return LoadModule(m_storage) != nullptr; }

StringView Library::SystemLibraryName() { return "kernel32.dll"; }
StringView Library::SystemSymbolName() { return "CreateFileW"; }

}  // namespace Monarc::Platform
