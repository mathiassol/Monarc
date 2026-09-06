#include <Monarc/Core/Platform/Path.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Types.h>

namespace Monarc::Platform::Path {

namespace {

constexpr char kSeparator = '/';

[[nodiscard]] bool IsSeparator(char c) {
    return c == kSeparator;
}

[[nodiscard]] bool IsAsciiLetter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

}  // namespace

StringView FileName(StringView path) {
    const usize lastSeparator = path.find_last_of(kSeparator);
    if (lastSeparator == StringView::npos) {
        return path;
    }
    return path.substr(lastSeparator + 1);
}

StringView Extension(StringView path) {
    const StringView name = FileName(path);
    const usize lastDot = name.find_last_of('.');
    // A dot at index 0 (".hidden") is a hidden-file convention, not an extension separator.
    if (lastDot == StringView::npos || lastDot == 0) {
        return StringView{};
    }
    return name.substr(lastDot + 1);
}

StringView Stem(StringView path) {
    const StringView name = FileName(path);
    const usize lastDot = name.find_last_of('.');
    if (lastDot == StringView::npos || lastDot == 0) {
        return name;
    }
    return name.substr(0, lastDot);
}

StringView Parent(StringView path) {
    const usize lastSeparator = path.find_last_of(kSeparator);
    if (lastSeparator == StringView::npos) {
        return StringView{};
    }
    // A separator at index 0 is a root; keep it rather than collapsing "/a" to "".
    if (lastSeparator == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, lastSeparator);
}

String Join(IAllocator& allocator, StringView a, StringView b) {
    if (a.empty()) {
        return String(allocator, b);
    }
    if (b.empty()) {
        return String(allocator, a);
    }

    usize aEnd = a.size();
    while (aEnd > 0 && IsSeparator(a[aEnd - 1])) {
        --aEnd;
    }
    usize bStart = 0;
    while (bStart < b.size() && IsSeparator(b[bStart])) {
        ++bStart;
    }

    String result(allocator, a.substr(0, aEnd));
    result.PushBack(kSeparator);
    result.Append(b.substr(bStart));
    return result;
}

String Normalize(IAllocator& allocator, StringView path) {
    const bool rooted = !path.empty() && IsSeparator(path.front());

    // A stack of the components resolved so far. Kept as views into `path` -- nothing here
    // outlives this function's own local storage above `path` itself.
    Array<StringView> stack(allocator);

    usize i = 0;
    while (i < path.size()) {
        while (i < path.size() && IsSeparator(path[i])) {
            ++i;
        }
        const usize start = i;
        while (i < path.size() && !IsSeparator(path[i])) {
            ++i;
        }
        const StringView component = path.substr(start, i - start);

        if (component.empty() || component == ".") {
            continue;
        }
        if (component == "..") {
            if (!stack.IsEmpty() && stack[stack.Size() - 1] != "..") {
                stack.Pop();
            } else if (!rooted) {
                // Nothing earlier to resolve against: keep it rather than silently
                // changing which directory a relative path refers to.
                stack.Push(component);
            }
            // Rooted with nothing to pop: ".." above the root stays at the root, so it is
            // dropped -- there is nowhere higher for a rooted path to refer to.
            continue;
        }
        stack.Push(component);
    }

    String result(allocator);
    if (rooted) {
        result.PushBack(kSeparator);
    }
    for (usize idx = 0; idx < stack.Size(); ++idx) {
        if (idx > 0) {
            result.PushBack(kSeparator);
        }
        result.Append(stack[idx]);
    }
    return result;
}

bool IsAbsolute(StringView path) {
    // The one platform-specific fact in this file: on Windows, a path is unambiguously
    // rooted only when it names a drive -- a letter, a colon, then a separator. Win32
    // accepts '/' here as readily as '\\', and Join/Normalize always emit '/', so both are
    // recognised. A bare leading separator ("/x") is deliberately NOT treated as absolute:
    // it is resolved against whichever drive happens to be current, which is exactly the
    // ambiguity an asset system cannot afford to call "absolute".
    //
    // This is the one function in this file that will need attention when a second
    // platform arrives -- see the A2c plan's note on why Path.cpp has no per-platform
    // sibling the way Time.cpp and File.cpp do.
    return path.size() >= 3 && IsAsciiLetter(path[0]) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
}

}  // namespace Monarc::Platform::Path
