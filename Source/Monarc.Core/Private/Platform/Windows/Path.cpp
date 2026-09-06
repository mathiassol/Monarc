#include <Monarc/Core/Platform/Path.h>

namespace Monarc::Platform::Path {
namespace {

[[nodiscard]] bool IsAsciiLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

}  // namespace

bool IsAbsolute(StringView path) {
    // On Windows a path is unambiguously rooted only when it names a drive: a letter, a
    // colon, then a separator. Win32 accepts '/' here as readily as '\', and Join and
    // Normalize always emit '/', so both are recognised.
    //
    // A bare leading separator ("/x") is deliberately NOT absolute. It resolves against
    // whichever drive is current, which is exactly the ambiguity an asset system cannot
    // afford to call "absolute" -- ADR-0008 makes paths part of a content hash.
    return path.size() >= 3 && IsAsciiLetter(path[0]) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
}

char PreferredSeparator() { return '\\'; }

}  // namespace Monarc::Platform::Path
