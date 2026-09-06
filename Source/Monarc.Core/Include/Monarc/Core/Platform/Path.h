#pragma once

#include <Monarc/Core/Containers/String.h>

namespace Monarc::Platform::Path {

/// The last path component: the file or directory name after the final '/', or the whole
/// path if it contains none. Empty if path ends with a separator (there is no name after
/// it) or is itself empty. Returns a view into path -- no allocation.
[[nodiscard]] StringView FileName(StringView path);

/// The characters after the last '.' in FileName(path), excluding the dot itself. Empty if
/// FileName(path) has no dot, or if its only dot is the first character -- a name like
/// ".hidden" is not treated as an extension-only name. Returns a view into path.
[[nodiscard]] StringView Extension(StringView path);

/// FileName(path) with its Extension, and the dot separating them, removed. Returns a view
/// into path.
[[nodiscard]] StringView Stem(StringView path);

/// path with its last component removed, and no trailing separator. Empty if path has no
/// separator. Returns a view into path.
[[nodiscard]] StringView Parent(StringView path);

/// Joins two path segments with exactly one '/' between them, regardless of how many
/// separators either side already carries at the join point. If either side is empty, the
/// other is returned unchanged -- in particular, a leading separator on b is preserved
/// rather than stripped when a is empty, since that separator may be the only thing marking
/// b as absolute.
[[nodiscard]] String Join(IAllocator& allocator, StringView a, StringView b);

/// Resolves "." and ".." components and collapses repeated separators, emitting '/'
/// throughout regardless of what path used.
///
/// A leading ".." that cannot be resolved against anything earlier in the path is kept
/// rather than dropped: dropping it would silently change which directory a relative path
/// refers to. A ".." that would climb above a rooted path's root is dropped, since there is
/// nowhere higher for a rooted path to refer to.
[[nodiscard]] String Normalize(IAllocator& allocator, StringView path);

/// True if path is rooted rather than resolved relative to a current directory -- this
/// platform's own notion of an absolute path. Deliberately unspecified beyond that; see the
/// A2c plan for why no test probes the exact form.
[[nodiscard]] bool IsAbsolute(StringView path);

/// The separator this platform's own APIs and paths prefer. Join and Normalize always emit
/// '/' regardless of this value -- see their docs -- so this exists only for a caller that
/// specifically wants the platform's native convention, such as formatting a path for
/// display in a platform-native tool.
[[nodiscard]] constexpr char PreferredSeparator() { return '\\'; }

}  // namespace Monarc::Platform::Path
