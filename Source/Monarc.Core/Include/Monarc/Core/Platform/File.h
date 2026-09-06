#pragma once

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Error.h>

namespace Monarc::Platform::File {

/// True if path names an existing regular file. False both for a path that does not exist
/// and for one that names a directory -- neither is an error.
[[nodiscard]] bool Exists(StringView path);

/// The size in bytes of the file at path.
///
/// Fails with ErrorCode::NotFound if path does not exist or names a directory, rather than
/// returning 0: a caller checking `size == 0` must be able to tell a missing file from an
/// empty one.
[[nodiscard]] Result<u64> Size(StringView path);

/// Reads the entire file at path into out, replacing out's previous contents.
///
/// Fails with ErrorCode::NotFound if path does not exist. The amount actually read is
/// however much the file holds at the time of reading, not a size queried beforehand --
/// safe regardless of whether another process grows or shrinks the file while this runs.
[[nodiscard]] Status ReadAll(StringView path, Array<u8>& out);

/// Writes size bytes from data to path, creating the file if it does not exist and
/// replacing its entire previous contents if it does -- never appending. data may be
/// nullptr only if size is 0.
///
/// The replacement is all-or-nothing: a write that fails partway, or a crash mid-write,
/// never leaves a truncated file at path. Any previous content stays exactly as it was
/// until the new content is complete and ready to take its place.
[[nodiscard]] Status WriteAll(StringView path, const void* data, usize size);

/// Removes the file at path. Fails if path does not exist or names a directory.
[[nodiscard]] Status Delete(StringView path);

/// A directory suitable for temporary files, as an absolute path with no trailing
/// separator. Callers choose their own file names within it (unlikely to collide, and
/// theirs to delete); nothing here creates or cleans the directory itself.
///
/// Always returns a non-empty, absolute path, even if the platform's own environment for
/// locating one is unset or broken -- callers can rely on IsAbsolute(TemporaryDirectory())
/// unconditionally rather than checking for a degenerate result.
[[nodiscard]] String TemporaryDirectory(IAllocator& allocator);

// Deliberately deferred: streaming reads, seeking, and an open-handle type. Nothing needs
// them until asset streaming (see the A2c plan); ReadAll/WriteAll covers every current
// caller.

}  // namespace Monarc::Platform::File
