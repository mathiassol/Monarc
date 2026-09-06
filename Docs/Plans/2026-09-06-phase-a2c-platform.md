# Phase A2c — The platform layer

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The boundary between Monarc and the operating system — files, paths, time, threads, dynamic libraries, and GUIDs — behind platform-neutral interfaces, with Win32 the only implementation for now.

**Architecture:** Per-platform directories selected by the build, no platform `#ifdef` anywhere ([ADR-0016](../Architecture/Decisions/ADR-0016-platform-code-selection.md)). Public headers carry opaque fixed-size handles rather than conditional members, so their layout does not depend on which platform is building. Fallible operations return `Result<T>`/`Status`; there is no `errno`-style out-of-band error channel.

**Tech Stack:** C++23, MSVC + Clang (`clang-cl`), CMake + Ninja, doctest.

**Plan style:** headers and complete tests, not implementation bodies — as A2a and A2b. See A2a's plan for why.

---

## The discipline that makes this phase safe

[ADR-0016](../Architecture/Decisions/ADR-0016-platform-code-selection.md) records a cost it
cannot enforce: two platform implementations can diverge in *behaviour* while both compile,
and only a shared test suite run on both platforms catches it. There is no second platform
yet.

So: **every test in this phase is written against the interface, never against Win32
specifics.** No test may assert a backslash path separator, a Windows error code, a drive
letter, or a `HANDLE` value. When macOS arrives, this suite must run unchanged and pass. A
test that bakes in Windows behaviour is worse than no test, because it will have to be
rewritten by whoever is already struggling to bring up a new platform.

Two consequences worth stating:

- `Path` functions are tested with forward slashes, which Win32 accepts throughout.
- Where behaviour genuinely differs across platforms (case sensitivity, path length limits),
  the interface documents it as unspecified and the tests do not probe it.

---

## File Structure

```
Source/Monarc.Core/
  Include/Monarc/Core/Platform/
    Time.h        monotonic and wall clocks
    File.h        open, read, write, size, existence, deletion
    Path.h        join, split, normalise -- pure string work, no platform calls
    Thread.h      Thread, Mutex, ScopedLock, ConditionVariable
    Library.h     dynamic library load and symbol lookup
    Guid.h        128-bit identity
  Private/Platform/
    Path.cpp                 platform-neutral: pure string manipulation
    Windows/Time.cpp  Windows/File.cpp  Windows/Thread.cpp
    Windows/Library.cpp  Windows/Guid.cpp
  Tests/
    TestTime.cpp  TestFile.cpp  TestPath.cpp  TestThread.cpp
    TestLibrary.cpp  TestGuid.cpp
```

`Path` is deliberately **not** under `Windows/`: joining and splitting strings needs no
platform call, and the one place platforms differ — the preferred separator — is a constant.
Putting it in the platform-neutral directory keeps it testable everywhere and shrinks what
each new platform must implement.

---

## Task 1: Platform-aware module globbing, and `Time`

Time is the simplest platform concern, so it proves the structure before anything depends on it.

**Files:** modify `CMake/MonarcModule.cmake`; create `Platform/Time.h`, `Private/Platform/Windows/Time.cpp`, `Tests/TestTime.cpp`

- [ ] **Step 1: Teach `monarc_module` to select platform sources**

In `CMake/MonarcModule.cmake`, before the existing `file(GLOB_RECURSE _sources ...)`, add:

```cmake
    # ADR-0016: platform code lives in per-platform directories and is selected here, so
    # that a file which cannot compile on this platform is never handed to the compiler.
    # Any directory under Private/Platform/ that is not the current platform is excluded.
    if(WIN32)
        set(_monarc_platform "Windows")
    elseif(APPLE)
        set(_monarc_platform "Mac")
    elseif(UNIX)
        set(_monarc_platform "Linux")
    else()
        message(FATAL_ERROR "monarc_module(${ARG_NAME}): unrecognised target platform")
    endif()
    set(MONARC_PLATFORM_DIR "${_monarc_platform}" CACHE INTERNAL "")
```

Then, immediately after the glob, filter out the other platforms' directories:

```cmake
    # Drop sources under any Private/Platform/<other> directory.
    set(_filtered "")
    foreach(_source IN LISTS _sources)
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_source}")
        if(_rel MATCHES "^Private/Platform/([^/]+)/" AND
           NOT CMAKE_MATCH_1 STREQUAL _monarc_platform)
            continue()
        endif()
        list(APPEND _filtered "${_source}")
    endforeach()
    set(_sources ${_filtered})
```

- [ ] **Step 2: Write the failing test**

`Tests/TestTime.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Platform/Time.h>

using Monarc::f64;
using Monarc::u64;
namespace Time = Monarc::Platform::Time;

TEST_CASE("the monotonic clock advances and never goes backwards") {
    const u64 first = Time::Ticks();
    u64 last = first;
    for (int i = 0; i < 1000; ++i) {
        const u64 now = Time::Ticks();
        CHECK(now >= last);
        last = now;
    }
    CHECK(last >= first);
}

TEST_CASE("the tick frequency is a sane positive value") {
    const u64 frequency = Time::TicksPerSecond();
    CHECK(frequency > 0);
    // A clock coarser than a millisecond is unusable for frame pacing.
    CHECK(frequency >= 1000);
}

TEST_CASE("ticks convert to seconds consistently") {
    const u64 frequency = Time::TicksPerSecond();
    CHECK(Time::TicksToSeconds(0) == 0.0);
    CHECK(Time::TicksToSeconds(frequency) == doctest::Approx(1.0));
    CHECK(Time::TicksToSeconds(frequency / 2) == doctest::Approx(0.5));
}

TEST_CASE("a measured interval is positive and not absurd") {
    // Deliberately loose: this asserts the clock is wired up, not that the machine is fast.
    // A tight bound here would fail on a loaded CI runner, which is the classic flaky test.
    const u64 start = Time::Ticks();
    volatile u64 sink = 0;
    for (u64 i = 0; i < 2'000'000; ++i) { sink += i; }
    const f64 elapsed = Time::TicksToSeconds(Time::Ticks() - start);
    CHECK(elapsed > 0.0);
    CHECK(elapsed < 60.0);
}

TEST_CASE("the wall clock returns a plausible present-day value") {
    // Seconds since the Unix epoch. Bounded loosely at both ends so it neither passes on a
    // zeroed clock nor fails when this code is still running years from now.
    const u64 now = Time::UnixSeconds();
    CHECK(now > 1'700'000'000ull);   // late 2023
    CHECK(now < 4'000'000'000ull);   // 2096
}
```

- [ ] **Step 3: Write `Platform/Time.h`**

In `namespace Monarc::Platform::Time`:

```cpp
/// Monotonic tick count from an unspecified origin. Never decreases, unaffected by wall
/// clock changes. This is the clock for frame pacing and profiling; UnixSeconds is not.
[[nodiscard]] u64 Ticks();

/// Ticks per second for the monotonic clock. Constant for the life of the process.
[[nodiscard]] u64 TicksPerSecond();

/// Converts a tick *interval* to seconds. Meaningless applied to an absolute Ticks()
/// value, whose origin is unspecified.
[[nodiscard]] f64 TicksToSeconds(u64 ticks);

/// Seconds since the Unix epoch. For logs and file timestamps -- not for measuring
/// intervals, because it can jump when the system clock is corrected.
[[nodiscard]] u64 UnixSeconds();
```

Write the doc comments; they are part of the specification. Implement in
`Private/Platform/Windows/Time.cpp`.

- [ ] **Step 4: Verify the globbing actually excludes other platforms**

Create an empty placeholder that must **not** be compiled:
`Private/Platform/Mac/Time.cpp` containing:

```cpp
// Placeholder proving ADR-0016's directory selection works: this file references nothing
// that exists on Windows, so if the build ever compiles it the exclusion has broken.
#error "Mac platform sources must not be compiled on a non-Mac target"
```

Build. It must succeed — the `#error` never fires, because the file is never handed to the
compiler. Then confirm the file really is excluded rather than merely tolerated:

```
python -c "import json;d=json.load(open('Build/msvc-debug/module-graph.json'));print(d['modules'][0]['name'])"
```
and grep the generated Ninja file:
```
findstr /C:"Platform\Mac" Build\msvc-debug\build.ninja
```
Expected: **no match**. A match means the exclusion silently failed and the `#error` was
skipped for some other reason.

- [ ] **Step 5: Verify on all six presets, then commit**

```bash
git add CMake/MonarcModule.cmake Source/Monarc.Core
git commit -m "core: platform source selection by directory, and the monotonic clock"
```

---

## Task 2: `Path` and `File`

**Files:** create `Platform/Path.h`, `Private/Platform/Path.cpp`, `Platform/File.h`, `Private/Platform/Windows/File.cpp`, `Tests/TestPath.cpp`, `Tests/TestFile.cpp`

- [ ] **Step 1: Write the failing tests**

`Tests/TestPath.cpp` — note every case uses forward slashes, which Win32 accepts and which
keeps the suite portable:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/Path.h>

using Monarc::String;
using Monarc::StringView;
using Monarc::SystemAllocator;
namespace Path = Monarc::Platform::Path;

TEST_CASE("FileName returns the last component") {
    CHECK(Path::FileName("a/b/c.txt") == "c.txt");
    CHECK(Path::FileName("c.txt") == "c.txt");
    CHECK(Path::FileName("a/b/") == "");
    CHECK(Path::FileName("") == "");
}

TEST_CASE("Extension includes no dot and is empty when absent") {
    CHECK(Path::Extension("a/b/c.txt") == "txt");
    CHECK(Path::Extension("archive.tar.gz") == "gz");
    CHECK(Path::Extension("noext") == "");
    CHECK(Path::Extension("a.b/noext") == "");   // the dot is in a directory, not the name
    CHECK(Path::Extension(".hidden") == "");     // a leading dot is not an extension
}

TEST_CASE("Stem is the file name without its extension") {
    CHECK(Path::Stem("a/b/c.txt") == "c");
    CHECK(Path::Stem("archive.tar.gz") == "archive.tar");
    CHECK(Path::Stem("noext") == "noext");
    CHECK(Path::Stem(".hidden") == ".hidden");
}

TEST_CASE("Parent drops the last component and keeps no trailing separator") {
    CHECK(Path::Parent("a/b/c.txt") == "a/b");
    CHECK(Path::Parent("a/b") == "a");
    CHECK(Path::Parent("a") == "");
    CHECK(Path::Parent("") == "");
}

TEST_CASE("Join inserts exactly one separator") {
    SystemAllocator allocator;
    CHECK(Path::Join(allocator, "a", "b").View() == "a/b");
    CHECK(Path::Join(allocator, "a/", "b").View() == "a/b");
    CHECK(Path::Join(allocator, "a", "/b").View() == "a/b");
    CHECK(Path::Join(allocator, "a/", "/b").View() == "a/b");
    CHECK(Path::Join(allocator, "", "b").View() == "b");
    CHECK(Path::Join(allocator, "a", "").View() == "a");
}

TEST_CASE("Normalize resolves . and .. and collapses separators") {
    SystemAllocator allocator;
    CHECK(Path::Normalize(allocator, "a/./b").View() == "a/b");
    CHECK(Path::Normalize(allocator, "a//b").View() == "a/b");
    CHECK(Path::Normalize(allocator, "a/b/../c").View() == "a/c");
    CHECK(Path::Normalize(allocator, "a/b/../..").View() == "");
    CHECK(Path::Normalize(allocator, "./a").View() == "a");
}

TEST_CASE("Normalize keeps a leading .. that cannot be resolved") {
    // Dropping it would silently change which directory a relative path refers to.
    SystemAllocator allocator;
    CHECK(Path::Normalize(allocator, "../a").View() == "../a");
    CHECK(Path::Normalize(allocator, "a/../../b").View() == "../b");
}

TEST_CASE("IsAbsolute distinguishes rooted paths without asserting a platform's form") {
    // Only checks that a clearly-relative path is not absolute, and that the platform's own
    // notion of its root is. Asserting "C:" here would break on every other platform.
    CHECK_FALSE(Path::IsAbsolute("a/b"));
    CHECK_FALSE(Path::IsAbsolute(""));
    CHECK_FALSE(Path::IsAbsolute("./a"));
    CHECK(Path::IsAbsolute(Path::PreferredSeparator() == '\\' ? "C:/x" : "/x"));
}
```

`Tests/TestFile.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/File.h>
#include <Monarc/Core/Platform/Path.h>

#include <cstring>

using Monarc::Array;
using Monarc::String;
using Monarc::SystemAllocator;
using Monarc::u8;
using Monarc::usize;
namespace File = Monarc::Platform::File;
namespace Path = Monarc::Platform::Path;

namespace {

/// A file in the platform's temporary directory, removed on scope exit. Named per-test so
/// a leftover from a crashed run cannot make a later run pass or fail spuriously.
class ScopedTempFile {
public:
    explicit ScopedTempFile(SystemAllocator& allocator, Monarc::StringView label)
        : m_path(Path::Join(allocator, File::TemporaryDirectory(allocator).View(), label)) {
        (void)File::Delete(m_path.View());
    }
    ~ScopedTempFile() { (void)File::Delete(m_path.View()); }
    [[nodiscard]] Monarc::StringView Path() const { return m_path.View(); }

private:
    String m_path;
};

}  // namespace

TEST_CASE("a written file exists, reports its size, and reads back byte-identical") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_write.bin");

    const u8 written[] = {0x00, 0x01, 0xFE, 0xFF, 'a', 'b', 0x00, 'c'};
    REQUIRE(File::WriteAll(temp.Path(), written, sizeof(written)).has_value());

    CHECK(File::Exists(temp.Path()));
    const auto size = File::Size(temp.Path());
    REQUIRE(size.has_value());
    CHECK(*size == sizeof(written));

    Array<u8> read(allocator);
    REQUIRE(File::ReadAll(temp.Path(), read).has_value());
    REQUIRE(read.Size() == sizeof(written));
    CHECK(std::memcmp(read.Data(), written, sizeof(written)) == 0);
}

TEST_CASE("an empty file round-trips") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_empty.bin");
    REQUIRE(File::WriteAll(temp.Path(), nullptr, 0).has_value());
    CHECK(File::Exists(temp.Path()));
    const auto size = File::Size(temp.Path());
    REQUIRE(size.has_value());
    CHECK(*size == 0);

    Array<u8> read(allocator);
    REQUIRE(File::ReadAll(temp.Path(), read).has_value());
    CHECK(read.Size() == 0);
}

TEST_CASE("reading a missing file fails with NotFound rather than crashing") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_absent.bin");
    CHECK_FALSE(File::Exists(temp.Path()));

    Array<u8> read(allocator);
    const auto result = File::ReadAll(temp.Path(), read);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Monarc::ErrorCode::NotFound);
}

TEST_CASE("Size of a missing file fails rather than reporting zero") {
    // Reporting 0 would be indistinguishable from an empty file, and a caller checking
    // "size == 0" would treat a missing asset as an empty one.
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_absent_size.bin");
    const auto size = File::Size(temp.Path());
    REQUIRE_FALSE(size.has_value());
    CHECK(size.error().code == Monarc::ErrorCode::NotFound);
}

TEST_CASE("writing replaces rather than appends") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_replace.bin");
    const u8 first[]  = {1, 2, 3, 4};
    const u8 second[] = {9, 9};
    REQUIRE(File::WriteAll(temp.Path(), first, sizeof(first)).has_value());
    REQUIRE(File::WriteAll(temp.Path(), second, sizeof(second)).has_value());

    Array<u8> read(allocator);
    REQUIRE(File::ReadAll(temp.Path(), read).has_value());
    REQUIRE(read.Size() == 2);
    CHECK(read[0] == 9);
}

TEST_CASE("Delete reports whether something was removed") {
    SystemAllocator allocator;
    ScopedTempFile temp(allocator, "monarc_delete.bin");
    const u8 byte = 7;
    REQUIRE(File::WriteAll(temp.Path(), &byte, 1).has_value());
    CHECK(File::Delete(temp.Path()).has_value());
    CHECK_FALSE(File::Exists(temp.Path()));
    CHECK_FALSE(File::Delete(temp.Path()).has_value());   // already gone
}

TEST_CASE("Exists is false for a directory path, not an error") {
    SystemAllocator allocator;
    const String dir = File::TemporaryDirectory(allocator);
    CHECK_FALSE(File::Exists(dir.View()));
}

TEST_CASE("the temporary directory is usable") {
    SystemAllocator allocator;
    const String dir = File::TemporaryDirectory(allocator);
    CHECK(dir.Size() > 0);
    CHECK(Path::IsAbsolute(dir.View()));
}
```

- [ ] **Step 2: Build, confirm both fail for the right reason**

- [ ] **Step 3: Write the headers**

`Platform/Path.h` in `namespace Monarc::Platform::Path` — pure string work, no platform
calls, so it lives in the platform-neutral directory:

- `[[nodiscard]] StringView FileName(StringView)`, `Extension`, `Stem`, `Parent` — all
  returning views into the input, so no allocation
- `[[nodiscard]] String Join(IAllocator&, StringView a, StringView b)`
- `[[nodiscard]] String Normalize(IAllocator&, StringView)`
- `[[nodiscard]] bool IsAbsolute(StringView)`
- `[[nodiscard]] constexpr char PreferredSeparator()`

`Join` and `Normalize` emit `/` as the separator throughout, because Win32 accepts it and it
keeps paths identical across platforms — which matters for the content-addressed cook cache
in Phase C, where a path is part of a hash.

`Platform/File.h` in `namespace Monarc::Platform::File`:

- `[[nodiscard]] bool Exists(StringView path)` — false for a directory, not an error
- `[[nodiscard]] Result<u64> Size(StringView path)`
- `[[nodiscard]] Status ReadAll(StringView path, Array<u8>& out)`
- `[[nodiscard]] Status WriteAll(StringView path, const void* data, usize size)`
- `[[nodiscard]] Status Delete(StringView path)`
- `[[nodiscard]] String TemporaryDirectory(IAllocator&)`

Missing files must return `ErrorCode::NotFound`, never a zero size — a caller checking
`size == 0` would otherwise treat a missing asset as an empty one.

Streaming reads, seeking and an open-handle type are **deferred**: nothing needs them until
asset streaming, and `ReadAll`/`WriteAll` covers every current caller. Note it in the
header rather than leaving the omission to be guessed at.

- [ ] **Step 4: Verify on all six presets, then commit**

```bash
git add Source/Monarc.Core
git commit -m "core: platform path manipulation and whole-file IO"
```

---

## Task 3: Threads and synchronisation

**Files:** create `Platform/Thread.h`, `Private/Platform/Windows/Thread.cpp`, `Tests/TestThread.cpp`

`Monarc.Jobs` (A2d) is built directly on this, and [ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md) puts the threading model on the *own it* side — because an engine needs thread naming, affinity and stack size that `std::thread` does not expose.

**No ThreadSanitizer exists on Windows** (see [Status.md](../Status.md)), so races here are caught by design and review alone. Keep the surface small and the ownership rules explicit.

- [ ] **Step 1: Write the failing test**

`Tests/TestThread.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Platform/Thread.h>

#include <atomic>

using Monarc::u32;
namespace Platform = Monarc::Platform;

TEST_CASE("HardwareThreadCount reports something plausible") {
    const u32 count = Platform::HardwareThreadCount();
    CHECK(count >= 1);
    CHECK(count <= 4096);
}

TEST_CASE("a thread runs its entry point and Join waits for it") {
    std::atomic<int> ran{0};
    Platform::Thread thread;
    REQUIRE(thread.Start("monarc-test", [&ran] { ran.store(1, std::memory_order_release); })
                .has_value());
    thread.Join();
    CHECK(ran.load(std::memory_order_acquire) == 1);
    CHECK_FALSE(thread.IsRunning());
}

TEST_CASE("many threads all run to completion") {
    constexpr int kThreads = 8;
    std::atomic<int> completed{0};
    Platform::Thread threads[kThreads];
    for (auto& thread : threads) {
        REQUIRE(thread.Start("monarc-many", [&completed] {
                    completed.fetch_add(1, std::memory_order_acq_rel);
                }).has_value());
    }
    for (auto& thread : threads) { thread.Join(); }
    CHECK(completed.load(std::memory_order_acquire) == kThreads);
}

TEST_CASE("a mutex serialises access so a non-atomic counter stays correct") {
    // A plain int deliberately: if the mutex does not exclude, this loses increments and
    // the test fails. An atomic would pass even with a broken mutex.
    constexpr int kThreads = 4;
    constexpr int kPerThread = 20'000;
    Platform::Mutex mutex;
    int guarded = 0;

    Platform::Thread threads[kThreads];
    for (auto& thread : threads) {
        REQUIRE(thread.Start("monarc-lock", [&] {
                    for (int i = 0; i < kPerThread; ++i) {
                        Platform::ScopedLock lock(mutex);
                        ++guarded;
                    }
                }).has_value());
    }
    for (auto& thread : threads) { thread.Join(); }
    CHECK(guarded == kThreads * kPerThread);
}

TEST_CASE("a condition variable wakes a waiter") {
    Platform::Mutex mutex;
    Platform::ConditionVariable signal;
    bool ready = false;
    int  received = 0;

    Platform::Thread waiter;
    REQUIRE(waiter.Start("monarc-wait", [&] {
                Platform::ScopedLock lock(mutex);
                while (!ready) { signal.Wait(mutex); }
                received = 42;
            }).has_value());

    {
        Platform::ScopedLock lock(mutex);
        ready = true;
    }
    signal.NotifyAll();
    waiter.Join();
    CHECK(received == 42);
}

TEST_CASE("a condition variable tolerates a notify that arrives before the wait") {
    // The predicate loop must handle this; a Wait that assumes a notify is still pending
    // deadlocks. Notifying with no waiter must simply do nothing.
    Platform::Mutex mutex;
    Platform::ConditionVariable signal;
    signal.NotifyAll();   // nobody waiting

    bool ready = false;
    {
        Platform::ScopedLock lock(mutex);
        ready = true;     // predicate already true before any wait
    }

    Platform::Thread waiter;
    REQUIRE(waiter.Start("monarc-prewait", [&] {
                Platform::ScopedLock lock(mutex);
                while (!ready) { signal.Wait(mutex); }
            }).has_value());
    waiter.Join();
    CHECK(ready);
}

TEST_CASE("a default-constructed thread is safe to destroy and reports not running") {
    Platform::Thread thread;
    CHECK_FALSE(thread.IsRunning());
}
```

- [ ] **Step 2: Build, confirm it fails**

- [ ] **Step 3: Write `Platform/Thread.h`**

In `namespace Monarc::Platform`:

- `u32 HardwareThreadCount()`
- `class Mutex` — non-copyable, non-movable, `Lock()`, `Unlock()`, `TryLock()`. An opaque
  fixed-size storage member, not a conditional one.
- `class ScopedLock` — RAII over `Mutex`, non-copyable, non-movable
- `class ConditionVariable` — `Wait(Mutex&)`, `NotifyOne()`, `NotifyAll()`. Document that
  spurious wakeups are permitted, so callers must loop on a predicate.
- `class Thread` — non-copyable, movable; `Status Start(StringView name, Callable&&)`,
  `Join()`, `IsRunning()`. The destructor must **not** silently detach: a thread outliving
  its `Thread` object is a use-after-free waiting to happen, so document and assert that
  it must be joined first.

The thread name is passed at `Start` because Win32 requires setting it from within the
thread or via a handle, and because a nameless thread in a profiler is nearly useless once
there are more than three.

- [ ] **Step 4: Verify on all six presets — including `clang-asan`, which will exercise the
      threaded tests under instrumentation — then commit**

```bash
git add Source/Monarc.Core
git commit -m "core: threads, mutexes and condition variables"
```

---

## Task 4: `Library` and `Guid`

**Files:** create `Platform/Library.h`, `Private/Platform/Windows/Library.cpp`, `Platform/Guid.h`, `Private/Platform/Windows/Guid.cpp`, `Tests/TestLibrary.cpp`, `Tests/TestGuid.cpp`

- [ ] **Step 1: Write the failing tests**

`Tests/TestGuid.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/HashMap.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/Guid.h>

using Monarc::Guid;
using Monarc::HashMap;
using Monarc::SystemAllocator;

TEST_CASE("a default Guid is nil and distinguishable from a generated one") {
    const Guid nil;
    CHECK(nil.IsNil());
    CHECK(nil == Guid{});
    CHECK_FALSE(Guid::Generate().IsNil());
}

TEST_CASE("generated Guids do not collide") {
    // Not a statistical test of the entropy source -- just that Generate is not returning a
    // constant or a low-entropy counter, which is the realistic failure mode.
    SystemAllocator allocator;
    HashMap<Guid, int> seen(allocator);
    constexpr int kCount = 4096;
    for (int i = 0; i < kCount; ++i) { seen.Insert(Guid::Generate(), i); }
    CHECK(seen.Size() == kCount);
}

TEST_CASE("a Guid round-trips through its string form") {
    const Guid original = Guid::Generate();
    char text[Guid::kStringLength + 1] = {};
    original.Format(text);
    const auto parsed = Guid::Parse(text);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == original);
}

TEST_CASE("the string form is the canonical 36-character hyphenated shape") {
    const Guid nil;
    char text[Guid::kStringLength + 1] = {};
    nil.Format(text);
    CHECK(Monarc::StringView(text) == "00000000-0000-0000-0000-000000000000");
    CHECK(Guid::kStringLength == 36);
}

TEST_CASE("Parse rejects malformed input rather than guessing") {
    CHECK_FALSE(Guid::Parse("").has_value());
    CHECK_FALSE(Guid::Parse("not-a-guid").has_value());
    CHECK_FALSE(Guid::Parse("00000000-0000-0000-0000-00000000000").has_value());   // short
    CHECK_FALSE(Guid::Parse("00000000-0000-0000-0000-0000000000000").has_value()); // long
    CHECK_FALSE(Guid::Parse("0000000000000000000000000000000000000").has_value()); // no dashes
    CHECK_FALSE(Guid::Parse("gggggggg-0000-0000-0000-000000000000").has_value());  // non-hex
}

TEST_CASE("Parse accepts either case and produces the same value") {
    const auto a = Guid::Parse("01234567-89ab-cdef-0123-456789abcdef");
    const auto b = Guid::Parse("01234567-89AB-CDEF-0123-456789ABCDEF");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == *b);
}

TEST_CASE("Guids order consistently so they can be sorted") {
    const auto low  = Guid::Parse("00000000-0000-0000-0000-000000000001");
    const auto high = Guid::Parse("ffffffff-0000-0000-0000-000000000000");
    REQUIRE(low.has_value());
    REQUIRE(high.has_value());
    CHECK(*low < *high);
    CHECK_FALSE(*high < *low);
    CHECK_FALSE(*low < *low);
}

TEST_CASE("a Guid is usable as a HashMap key") {
    SystemAllocator allocator;
    HashMap<Guid, int> map(allocator);
    const Guid key = Guid::Generate();
    map.Insert(key, 7);
    REQUIRE(map.Find(key) != nullptr);
    CHECK(*map.Find(key) == 7);
    CHECK(map.Find(Guid::Generate()) == nullptr);
}
```

`Tests/TestLibrary.cpp` — the tricky part is having a library to load that exists on every
platform. Load the process's own C runtime by a platform-provided name:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Platform/Library.h>

using Monarc::SystemAllocator;
namespace Platform = Monarc::Platform;

TEST_CASE("loading a nonexistent library fails rather than crashing") {
    auto library = Platform::Library::Open("monarc_no_such_library_exists");
    REQUIRE_FALSE(library.has_value());
    CHECK(library.error().code == Monarc::ErrorCode::NotFound);
}

TEST_CASE("a known system library loads and yields a symbol") {
    // Named by the platform rather than hardcoded, so this test survives macOS.
    auto library = Platform::Library::Open(Platform::Library::SystemLibraryName());
    REQUIRE(library.has_value());
    CHECK(library->IsOpen());

    void* symbol = library->Symbol(Platform::Library::SystemSymbolName());
    CHECK(symbol != nullptr);

    CHECK(library->Symbol("monarc_no_such_symbol") == nullptr);
}

TEST_CASE("a library closes on destruction and reports closed after an explicit close") {
    auto library = Platform::Library::Open(Platform::Library::SystemLibraryName());
    REQUIRE(library.has_value());
    library->Close();
    CHECK_FALSE(library->IsOpen());
    CHECK(library->Symbol(Platform::Library::SystemSymbolName()) == nullptr);
}

TEST_CASE("a default-constructed library is closed and safe to query") {
    Platform::Library library;
    CHECK_FALSE(library.IsOpen());
    CHECK(library.Symbol("anything") == nullptr);
}
```

- [ ] **Step 2: Build, confirm both fail**

- [ ] **Step 3: Write the headers**

`Platform/Guid.h` — `Guid` in `namespace Monarc` (not `Platform`: it is a value type the
whole engine uses, and only its *generation* is platform-specific):

- 128 bits of storage, trivially copyable, `constexpr` default construction to nil
- `static Guid Generate()` — from the platform's cryptographic entropy source, not `rand()`
- `bool IsNil() const`, `operator==`, `operator<=>` or `operator<`
- `static constexpr usize kStringLength = 36`
- `void Format(char* out) const` — writes exactly `kStringLength` characters plus a
  terminator, lowercase hyphenated
- `static Result<Guid> Parse(StringView)` — rejects anything malformed
- `template <> struct Hasher<Guid>`

[ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md) makes this asset identity,
so it must be stable across runs and machines: no process-local counters, no time-based
generation that could collide when two machines cook simultaneously.

`Platform/Library.h` in `namespace Monarc::Platform`:

- `class Library` — non-copyable, movable; `static Result<Library> Open(StringView)`,
  `void* Symbol(StringView)`, `Close()`, `IsOpen()`, destructor closes
- `static StringView SystemLibraryName()` and `SystemSymbolName()` — a library and symbol
  known to exist on this platform, so tests need not hardcode Windows names

- [ ] **Step 4: Verify on all six presets, then commit**

```bash
git add Source/Monarc.Core
git commit -m "core: dynamic library loading and 128-bit Guid identity"
```

---

## Task 5: Close out A2c

**Files:** modify `Docs/Status.md`

- [ ] **Step 1: Clean verification on all six presets**

Record doctest totals. Confirm the four architecture gates and the 30 gate tests still pass.

- [ ] **Step 2: Confirm gate 10 is now doing real work**

A2c is the first module with a platform layer. Verify the gate is genuinely exercised:

1. Confirm it still passes on the real tree.
2. Temporarily add `#ifdef _WIN32` to a file **outside** `Private/Platform/` — for instance
   `Private/Platform/Path.cpp`, which is platform-neutral by design — and confirm gate 10
   fails, naming that file. Then remove it.

Per [ADR-0016](../Architecture/Decisions/ADR-0016-platform-code-selection.md), the
`Platform/` exemption should be **unused**: if no file under `Private/Platform/Windows/`
contains a platform `#ifdef`, say so in the report, because that is the intended outcome.

- [ ] **Step 3: Update `Docs/Status.md`**

Mark A2c complete, add an `### A2c delivered` subsection, and note whether gate 10's
exemption turned out to be needed.

- [ ] **Step 4: Commit**

```bash
python Tools/check_doc_links.py Docs README.md
git add Docs/Status.md
git commit -m "docs: record Phase A2c complete"
```

---

## Definition of done

1. All six presets configure, build clean, and pass.
2. Every build is warning-free (`/WX`).
3. `Private/Platform/Mac/` exists, contains a file that would fail to compile, and is
   verifiably absent from the generated build.
4. No test asserts a Windows-specific path separator, error code, or library name.
5. Gate 10 has been observed failing for a platform `#ifdef` outside the platform layer.
6. `Docs/Status.md` reflects reality.

## What A2c deliberately excludes

- **No streaming file IO, seeking, or open-handle type.** `ReadAll`/`WriteAll` covers every
  current caller; streaming arrives with asset streaming, which M0 defers.
- **No directory enumeration or file watching.** The cooker needs both in Phase C; adding
  them now would be untested speculation about what it wants.
- **No thread affinity or priority.** `Monarc.Jobs` (A2d) is the first thing with an opinion
  about either, so they belong there, driven by a real scheduling need.
- **No process spawning.** The Hub and the cooker will want it; neither exists.
- **No `Path` type.** Free functions over `StringView` and `String` are enough, and a
  dedicated type is easier to add once there is evidence of what it should carry.
