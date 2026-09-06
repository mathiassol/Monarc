#include <Monarc/Core/Platform/File.h>

#include <Monarc/Core/Assert.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace Monarc::Platform::File {

namespace {

// Generous enough for any path this engine constructs; CreateFileW itself refuses
// anything longer than this without the \\?\ prefix, which Monarc does not use.
constexpr int kMaxPathWide = 4096;

[[nodiscard]] ErrorCode MapLastError(DWORD lastError) {
    switch (lastError) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return ErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
            return ErrorCode::PermissionDenied;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return ErrorCode::AlreadyExists;
        default:
            return ErrorCode::IoFailure;
    }
}

/// Converts a UTF-8 path to UTF-16 into a fixed buffer, returning the number of wchar_t
/// written (excluding the terminator this always adds). Returns 0 -- and an
/// empty, terminated buffer -- if utf8 is empty or does not fit; every caller then makes a
/// Win32 call against an empty/truncated path, which fails with a reportable error rather
/// than silently operating on the wrong file.
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

}  // namespace

bool Exists(StringView path) {
    wchar_t wide[kMaxPathWide];
    ToWide(path, wide, kMaxPathWide);

    const DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

Result<u64> Size(StringView path) {
    wchar_t wide[kMaxPathWide];
    ToWide(path, wide, kMaxPathWide);

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &info)) {
        return Err(MapLastError(GetLastError()), "could not query file");
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        // Deliberately NotFound, not a distinct code: File::Size names a specific concept
        // (the size of a file), and a directory does not have one -- from this API's
        // caller's point of view that is indistinguishable from the path not existing.
        return Err(ErrorCode::NotFound, "path names a directory, not a file");
    }

    ULARGE_INTEGER size;
    size.HighPart = info.nFileSizeHigh;
    size.LowPart  = info.nFileSizeLow;
    return static_cast<u64>(size.QuadPart);
}

Status ReadAll(StringView path, Array<u8>& out) {
    wchar_t wide[kMaxPathWide];
    ToWide(path, wide, kMaxPathWide);

    HANDLE handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Err(MapLastError(GetLastError()), "could not open file");
    }

    out.Clear();

    // Queried from the open handle rather than a separate path-based call beforehand, so
    // it at least reflects this file object rather than whatever last happened to be at
    // this path. Even so, treat it only as a capacity hint: nothing here locks the file
    // against another process, so it may still grow or shrink before the reads below
    // reach the end of it. Reading in a loop until ReadFile itself reports end-of-file --
    // rather than trusting this number of bytes exist -- is correct regardless of which
    // way the file moved; the query only avoids reallocating out on the common case.
    LARGE_INTEGER queriedSize;
    if (GetFileSizeEx(handle, &queriedSize) && queriedSize.QuadPart > 0) {
        out.Reserve(static_cast<usize>(queriedSize.QuadPart));
    }

    constexpr DWORD kChunkSize = 64 * 1024;
    u8 chunk[kChunkSize];
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(handle, chunk, kChunkSize, &bytesRead, nullptr)) {
            const DWORD lastError = GetLastError();
            CloseHandle(handle);
            return Err(MapLastError(lastError), "read failed");
        }
        if (bytesRead == 0) {
            break;   // End of file.
        }
        for (DWORD i = 0; i < bytesRead; ++i) {
            out.Push(chunk[i]);
        }
    }

    CloseHandle(handle);
    return {};
}

Status WriteAll(StringView path, const void* data, usize size) {
    MONARC_CHECK(data != nullptr || size == 0, "WriteAll needs data unless size is 0");

    wchar_t wide[kMaxPathWide];
    const int wideLength = ToWide(path, wide, kMaxPathWide);

    // ".monarc_tmp" is appended in place rather than kept in a second buffer, so the
    // temporary file sits beside the destination -- guaranteeing MoveFileExW below is a
    // same-volume rename rather than a cross-volume copy, which is what makes it atomic.
    constexpr wchar_t kTempSuffix[] = L".monarc_tmp";
    constexpr int     kTempSuffixLength = 11;   // excludes the terminator
    if (wideLength <= 0 || wideLength + kTempSuffixLength >= kMaxPathWide) {
        return Err(ErrorCode::InvalidArgument, "path is empty or too long");
    }
    wchar_t tempWide[kMaxPathWide];
    for (int i = 0; i < wideLength; ++i) {
        tempWide[i] = wide[i];
    }
    for (int i = 0; i < kTempSuffixLength; ++i) {
        tempWide[wideLength + i] = kTempSuffix[i];
    }
    tempWide[wideLength + kTempSuffixLength] = L'\0';

    // Written to the temporary path first and moved into place only once it is complete,
    // so a write that fails partway -- or a crash mid-write -- never leaves a truncated
    // file at `path`: the destination is either replaced whole or not touched at all.
    HANDLE handle = CreateFileW(tempWide, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Err(MapLastError(GetLastError()), "could not create temporary file");
    }

    const u8* bytes   = static_cast<const u8*>(data);
    usize     written = 0;
    ErrorCode failure  = ErrorCode::Unknown;
    bool      ok       = true;
    while (written < size) {
        const usize remaining = size - written;
        const DWORD toWrite =
            remaining > 0xFFFF'FFFFull ? 0xFFFF'FFFFu : static_cast<DWORD>(remaining);
        DWORD bytesWritten = 0;
        if (!WriteFile(handle, bytes + written, toWrite, &bytesWritten, nullptr)) {
            failure = MapLastError(GetLastError());
            ok      = false;
            break;
        }
        if (bytesWritten == 0) {
            // No progress and no error reported: treat as a failure rather than loop
            // forever.
            failure = ErrorCode::IoFailure;
            ok      = false;
            break;
        }
        written += bytesWritten;
    }

    CloseHandle(handle);

    if (!ok) {
        DeleteFileW(tempWide);   // Best-effort: the destination was never touched either way.
        return Err(failure, "write failed");
    }

    if (!MoveFileExW(tempWide, wide, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const ErrorCode code = MapLastError(GetLastError());
        DeleteFileW(tempWide);
        return Err(code, "could not replace destination file");
    }

    return {};
}

Status Delete(StringView path) {
    wchar_t wide[kMaxPathWide];
    ToWide(path, wide, kMaxPathWide);

    if (!DeleteFileW(wide)) {
        return Err(MapLastError(GetLastError()), "could not delete file");
    }
    return {};
}

String TemporaryDirectory(IAllocator& allocator) {
    wchar_t wide[kMaxPathWide];
    DWORD   length = GetTempPathW(kMaxPathWide, wide);

    if (length == 0 || length >= kMaxPathWide) {
        // GetTempPathW consults %TMP%, %TEMP%, %USERPROFILE%, then the Windows directory
        // itself, in that order -- it fails only in a genuinely broken environment. This
        // function returns a plain String rather than a Result, so it cannot report that
        // failure; falling back to the Windows directory (which the OS itself cannot run
        // without) keeps the contract that the result is always non-empty and absolute,
        // rather than handing every caller a value that silently fails to be either.
        length = GetWindowsDirectoryW(wide, kMaxPathWide);
        if (length == 0 || length + 5 >= kMaxPathWide) {
            return String(allocator, "C:/Windows/Temp");
        }
        const wchar_t kSuffix[] = L"\\Temp";
        for (int i = 0; i < 5; ++i) {
            wide[length + i] = kSuffix[i];
        }
        length += 5;
    }

    // Drop the trailing separator GetTempPathW always includes; Path::Join always adds
    // its own.
    while (length > 0 && (wide[length - 1] == L'\\' || wide[length - 1] == L'/')) {
        --length;
    }

    char      utf8[kMaxPathWide * 3];   // Worst case for UTF-8 is 3 bytes per UTF-16 unit here.
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length),
                                                utf8, static_cast<int>(sizeof(utf8)),
                                                nullptr, nullptr);
    if (utf8Length <= 0) {
        return String(allocator, "C:/Windows/Temp");
    }

    String result(allocator, StringView(utf8, static_cast<usize>(utf8Length)));

    // Monarc paths use '/' throughout; GetTempPathW/GetWindowsDirectoryW return '\\'.
    for (usize i = 0; i < result.Size(); ++i) {
        if (result[i] == '\\') {
            result[i] = '/';
        }
    }
    return result;
}

}  // namespace Monarc::Platform::File
