# Phase A2a — Hash, String, and HashMap

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The vocabulary types the rest of Monarc is written in — a hashing facility, an owning `String` over an explicit allocator, and a `HashMap` — each derived test-first.

**Architecture:** Everything continues to take an `IAllocator&`; there is no ambient `new`. `HashMap` is open-addressed with power-of-two capacity, linear probing, and backward-shift deletion, so it carries no tombstones. Hashing is FNV-1a behind a `Hasher<T>` customisation point, so the algorithm can change without touching call sites.

**Tech Stack:** C++23, MSVC + Clang (`clang-cl`), CMake + Ninja, doctest.

**How this plan differs from A1's.** It specifies **headers and complete tests**, not implementation bodies. A1's plan contained hand-written implementations and two shipped real defects — a use-after-free and an integer overflow — that review had to catch. Tests are a more reliable specification than an implementation the plan author wrote without compiling. Implementers write the bodies to satisfy the tests.

**Reference docs:** [Memory](../Runtime/Memory.md), [ADR-0003](../Architecture/Decisions/ADR-0003-cpp23-baseline.md), [ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md), [Module-Graph](../Architecture/Module-Graph.md).

---

## File Structure

```
Source/Monarc.Core/
  Include/Monarc/Core/
    Hash.h                     FNV-1a, Hasher<T> customisation point
    Containers/String.h        owning String; StringView alias
    Containers/HashMap.h       open-addressed map
  Private/
    Hash.cpp                   out-of-line byte hashing
    Containers/String.cpp      out-of-line growth paths
  Tests/
    TestHash.cpp  TestString.cpp  TestHashMap.cpp
```

`HashMap` is header-only because it is a template; `String` is not a template, so its growth
paths live in a `.cpp` to keep the header cheap to include.

---

## Decisions this plan makes, with reasons

**`StringView` is an alias for `std::string_view`, not a new type.** `std::string_view` is
header-only, allocation-free, universally available, and already used by `Error` and `Log`.
[ADR-0014](../Architecture/Decisions/ADR-0014-dependency-policy.md) says Monarc owns what
defines its character and performance — a non-owning span of chars is not that. `String` is
different: its allocator model and debug-build behaviour are exactly why engines write their
own. The alias keeps Monarc's vocabulary consistent and leaves a seam if it ever needs to
become a real type.

**No small-string optimisation yet.** SSO is the main performance reason to own a string
type, and it will be added — but it is an implementation detail behind an unchanged
interface, and nothing serializes a `String` yet, so deferring costs nothing later. A1's
lesson was that hand-written container internals harbour real defects; adding SSO on top of
an unproven `String` compounds that risk rather than managing it. Recorded as deferred, not
forgotten.

**`String` is null-terminated.** It talks to logging, file paths, and OS calls, all of which
want `const char*`. Paying one byte to avoid a copy at every boundary is the right trade.

**Backward-shift deletion, not tombstones.** Tombstones degrade a long-lived map until it is
rehashed, and an engine's maps are long-lived. Backward shifting is slightly more code in one
place and removes a whole failure mode.

---

## Task 1: `Hash`

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Hash.h`
- Create: `Source/Monarc.Core/Private/Hash.cpp`
- Create: `Source/Monarc.Core/Tests/TestHash.cpp`

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestHash.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Hash.h>

#include <string_view>

using Monarc::Hash;
using Monarc::HashBytes;
using Monarc::Hasher;
using Monarc::u32;
using Monarc::u64;

TEST_CASE("HashBytes is deterministic and order-sensitive") {
    const char a[] = "monarc";
    const char b[] = "monrac";
    CHECK(HashBytes(a, 6) == HashBytes(a, 6));
    CHECK(HashBytes(a, 6) != HashBytes(b, 6));
}

TEST_CASE("HashBytes distinguishes length even with a shared prefix") {
    CHECK(HashBytes("ab", 2) != HashBytes("abc", 3));
}

TEST_CASE("hashing an empty range is well defined") {
    CHECK(HashBytes(nullptr, 0) == HashBytes("", 0));
}

TEST_CASE("Hasher handles strings through the generic entry point") {
    CHECK(Hash(std::string_view("engine")) == Hash(std::string_view("engine")));
    CHECK(Hash(std::string_view("engine")) != Hash(std::string_view("Engine")));
}

TEST_CASE("integers hash without collapsing neighbouring values") {
    // A weak integer hash sends 1, 2, 3 to adjacent buckets, which degrades linear probing
    // exactly when keys are sequential ids -- the common case in an engine.
    const u64 h1 = Hash(u64{1});
    const u64 h2 = Hash(u64{2});
    const u64 h3 = Hash(u64{3});
    CHECK(h1 != h2);
    CHECK(h2 != h3);
    // Neighbouring inputs must differ in the high bits, not merely the low ones.
    CHECK((h1 >> 32) != (h2 >> 32));
}

TEST_CASE("different integer widths reach the same entry point") {
    CHECK(Hash(u32{7}) == Hash(u32{7}));
    CHECK(Hash(u64{7}) == Hash(u64{7}));
}

TEST_CASE("pointers are hashable") {
    int  value = 0;
    int* p     = &value;
    CHECK(Hash(p) == Hash(p));
    CHECK(Hash(p) != Hash(static_cast<int*>(nullptr)));
}

TEST_CASE("Hasher can be specialised for a user type") {
    CHECK(Hasher<Key>{}(Key{1, 2}) == Hasher<Key>{}(Key{1, 2}));
    CHECK(Hasher<Key>{}(Key{1, 2}) != Hasher<Key>{}(Key{2, 1}));
}
```

> `Key` must be declared at namespace scope above the test cases — in an anonymous
> namespace, as `TestHashMap.cpp` does with `Collide` — with `template <> struct
> Monarc::Hasher<Key>` beside it. A type local to a function cannot be specialised for at
> namespace scope. That case exists to prove the customisation point is reachable from
> outside `Hash.h`.

- [ ] **Step 2: Build and confirm it fails**

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cd /d C:\Users\mathi\Documents\projects\Monarc && cmake --preset msvc-debug && cmake --build --preset msvc-debug'
```

Expect `Cannot open include file: 'Monarc/Core/Hash.h'`.

- [ ] **Step 3: Write the header**

`Source/Monarc.Core/Include/Monarc/Core/Hash.h` must declare exactly this surface. Doc
comments are part of the specification; write them.

```cpp
#pragma once

#include <Monarc/Core/Types.h>

#include <string_view>
#include <type_traits>

namespace Monarc {

/// 64-bit FNV-1a over a byte range. Deterministic across runs and platforms, which matters
/// because hashes reach the cook cache and asset identity later -- see ADR-0008.
/// A null pointer with a zero length is valid and hashes as the empty range.
[[nodiscard]] u64 HashBytes(const void* data, usize size);

/// Mixes an integer so that neighbouring values differ in their high bits. Sequential ids
/// are the common key in an engine, and a weak integer hash sends them to adjacent buckets,
/// which is precisely the worst case for linear probing.
[[nodiscard]] constexpr u64 HashInteger(u64 value);

/// Customisation point. Specialise for a user type to make it usable as a HashMap key.
template <typename T>
struct Hasher;

/// Generic entry point. Prefer this over Hasher<T>{} at call sites.
template <typename T>
[[nodiscard]] u64 Hash(const T& value);

}  // namespace Monarc
```

Provide `Hasher` specialisations covering, at minimum: `std::string_view`, the integral
types, pointers, and `bool`. Implement `HashInteger` in the header as `constexpr`;
implement `HashBytes` in `Hash.cpp`.

Choose a well-known integer finaliser (splitmix64's, or MurmurHash3's `fmix64`) rather than
inventing one — the test asserts a property that a hand-rolled mixer will usually fail.

- [ ] **Step 4: Build and confirm all eight cases pass**

Run the build command above, then `ctest --preset msvc-debug`.

- [ ] **Step 5: Verify on Clang and Release**

```
cmd /c '"...vcvars64.bat" >nul && cd /d <repo> && cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug'
```
and the same for `msvc-release`. All must be warning-free — `/WX` is on.

- [ ] **Step 6: Commit**

```bash
git add Source/Monarc.Core
git commit -m "core: FNV-1a hashing with a Hasher customisation point"
```

---

## Task 2: `String`

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Containers/String.h`
- Create: `Source/Monarc.Core/Private/Containers/String.cpp`
- Create: `Source/Monarc.Core/Tests/TestString.cpp`

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestString.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <cstring>
#include <utility>

using Monarc::String;
using Monarc::StringView;
using Monarc::SystemAllocator;

TEST_CASE("a default String is empty, allocates nothing, and is still a valid C string") {
    SystemAllocator allocator;
    String s(allocator);
    CHECK(s.Size() == 0);
    CHECK(s.IsEmpty());
    CHECK(allocator.BytesAllocated() == 0);
    REQUIRE(s.CStr() != nullptr);
    CHECK(std::strlen(s.CStr()) == 0);
}

TEST_CASE("a String built from a view carries its contents") {
    SystemAllocator allocator;
    String s(allocator, "monarc");
    CHECK(s.Size() == 6);
    CHECK_FALSE(s.IsEmpty());
    CHECK(s.View() == "monarc");
    CHECK(std::strcmp(s.CStr(), "monarc") == 0);
}

TEST_CASE("Append grows and stays null-terminated") {
    SystemAllocator allocator;
    String s(allocator, "mon");
    s.Append("arc");
    CHECK(s.Size() == 6);
    CHECK(s.View() == "monarc");
    CHECK(std::strcmp(s.CStr(), "monarc") == 0);

    for (int i = 0; i < 100; ++i) {
        s.Append("x");
    }
    CHECK(s.Size() == 106);
    CHECK(std::strlen(s.CStr()) == 106);
}

TEST_CASE("PushBack appends a single character") {
    SystemAllocator allocator;
    String s(allocator);
    s.PushBack('a');
    s.PushBack('b');
    CHECK(s.View() == "ab");
    CHECK(std::strcmp(s.CStr(), "ab") == 0);
}

TEST_CASE("appending a String to itself is safe") {
    // The argument aliases the buffer the append may reallocate -- the same shape that was
    // a use-after-free in Array<T> before it was fixed.
    SystemAllocator allocator;
    String s(allocator, "abc");
    s.Append(s.View());
    CHECK(s.Size() == 6);
    CHECK(s.View() == "abcabc");
}

TEST_CASE("indexing reads and writes characters") {
    SystemAllocator allocator;
    String s(allocator, "cat");
    CHECK(s[0] == 'c');
    s[0] = 'b';
    CHECK(s.View() == "bat");
}

TEST_CASE("Clear empties without releasing capacity") {
    SystemAllocator allocator;
    String s(allocator, "something long enough to allocate");
    const Monarc::usize capacity = s.Capacity();
    REQUIRE(capacity > 0);

    s.Clear();
    CHECK(s.Size() == 0);
    CHECK(s.Capacity() == capacity);
    CHECK(std::strlen(s.CStr()) == 0);
}

TEST_CASE("Reserve allocates without changing size") {
    SystemAllocator allocator;
    String s(allocator, "ab");
    s.Reserve(256);
    CHECK(s.Size() == 2);
    CHECK(s.Capacity() >= 256);
    CHECK(s.View() == "ab");
}

TEST_CASE("moving transfers the buffer and leaves the source usable") {
    SystemAllocator allocator;
    String source(allocator, "moved");
    String moved(std::move(source));
    CHECK(moved.View() == "moved");
    CHECK(source.Size() == 0);
    CHECK(std::strlen(source.CStr()) == 0);   // still a valid empty C string
}

TEST_CASE("move assignment releases what the target held") {
    SystemAllocator allocator;
    String target(allocator, "target contents long enough to allocate");
    String source(allocator, "src");
    target = std::move(source);
    CHECK(target.View() == "src");
}

TEST_CASE("equality compares contents, not identity") {
    SystemAllocator allocator;
    String a(allocator, "same");
    String b(allocator, "same");
    String c(allocator, "different");
    CHECK(a == b);
    CHECK_FALSE(a == c);
    CHECK(a != c);
    CHECK(a == StringView("same"));
}

TEST_CASE("a String releases everything it allocated") {
    SystemAllocator allocator;
    {
        String s(allocator);
        for (int i = 0; i < 500; ++i) {
            s.Append("chunk");
        }
        CHECK(s.Size() == 2500);
    }
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("embedded null bytes are preserved in the view") {
    SystemAllocator allocator;
    String s(allocator, StringView("a\0b", 3));
    CHECK(s.Size() == 3);
    CHECK(s.View().size() == 3);
    CHECK(s.View()[1] == '\0');
}

TEST_CASE("a String is hashable and agrees with its view") {
    SystemAllocator allocator;
    String s(allocator, "key");
    CHECK(Monarc::Hash(s) == Monarc::Hash(StringView("key")));
}
```

- [ ] **Step 2: Build and confirm it fails**

- [ ] **Step 3: Write the header**

`Source/Monarc.Core/Include/Monarc/Core/Containers/String.h` must declare exactly this
surface, with these doc comments:

```cpp
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
/// site. There is no small-string optimisation yet — a deliberate deferral, and an
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

    // No [[nodiscard]] on these two: MSVC accepts it on a friend declaration, clang-cl
    // rejects it as "an attribute list cannot appear here" in either ordering. Found by
    // building both, which is what ADR-0003's second compiler is for.
    friend bool operator==(const String& a, const String& b);
    friend bool operator==(const String& a, StringView b);

private:
    // Implementer's choice, subject to the tests.
};

template <>
struct Hasher<String> {
    [[nodiscard]] u64 operator()(const String& s) const;
};

}  // namespace Monarc
```

Requirements the tests enforce, stated plainly so they are not discovered by failure:

1. `CStr()` never returns `nullptr` — not when empty, not when moved-from.
2. `Append` must tolerate `text` aliasing the string's own buffer.
3. Growth must not overflow when computing byte sizes — apply the same bound `Array<T>`
   uses, and treat allocation failure the same way (fatal; see `Array.h`'s reasoning).
4. `Clear` retains capacity.

- [ ] **Step 4: Build and confirm all fourteen cases pass**

- [ ] **Step 5: Verify on Clang and Release**

- [ ] **Step 6: Commit**

```bash
git add Source/Monarc.Core
git commit -m "core: owning String over an explicit allocator"
```

---

## Task 3: `HashMap`

**Files:**
- Create: `Source/Monarc.Core/Include/Monarc/Core/Containers/HashMap.h`
- Create: `Source/Monarc.Core/Tests/TestHashMap.cpp`

- [ ] **Step 1: Write the failing test**

`Source/Monarc.Core/Tests/TestHashMap.cpp`:

```cpp
#include <doctest/doctest.h>

#include <Monarc/Core/Containers/HashMap.h>
#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Memory/SystemAllocator.h>

#include <utility>

using Monarc::HashMap;
using Monarc::StringView;
using Monarc::SystemAllocator;
using Monarc::u64;
using Monarc::usize;

namespace {

/// Counts lifetimes so the tests can prove values are destroyed exactly once.
struct Tracked {
    static int s_alive;
    int        value;

    explicit Tracked(int v = 0) : value(v) { ++s_alive; }
    Tracked(const Tracked& o) : value(o.value) { ++s_alive; }
    Tracked(Tracked&& o) noexcept : value(o.value) { ++s_alive; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --s_alive; }
};

int Tracked::s_alive = 0;

/// Sends every key to the same bucket, forcing the probe sequence to be exercised.
struct Collide {
    int v;
};

}  // namespace

template <>
struct Monarc::Hasher<Collide> {
    u64 operator()(const Collide&) const { return 0; }
};

static bool operator==(const Collide& a, const Collide& b) { return a.v == b.v; }

TEST_CASE("a new map is empty and holds no memory") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    CHECK(map.Size() == 0);
    CHECK(map.IsEmpty());
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Insert then Find round-trips") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    map.Insert(1, 100);
    map.Insert(2, 200);

    REQUIRE(map.Size() == 2);
    const int* a = map.Find(1);
    REQUIRE(a != nullptr);
    CHECK(*a == 100);
    CHECK(*map.Find(2) == 200);
    CHECK(map.Find(3) == nullptr);
}

TEST_CASE("inserting an existing key replaces its value without growing the map") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    map.Insert(1, 100);
    map.Insert(1, 999);
    CHECK(map.Size() == 1);
    CHECK(*map.Find(1) == 999);
}

TEST_CASE("Contains agrees with Find") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    map.Insert(5, 50);
    CHECK(map.Contains(5));
    CHECK_FALSE(map.Contains(6));
}

TEST_CASE("Remove deletes only the requested key") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    for (int i = 0; i < 10; ++i) {
        map.Insert(i, i * 10);
    }
    CHECK(map.Remove(4));
    CHECK_FALSE(map.Remove(4));      // already gone
    CHECK(map.Size() == 9);
    CHECK(map.Find(4) == nullptr);
    for (int i = 0; i < 10; ++i) {
        if (i != 4) {
            REQUIRE(map.Find(i) != nullptr);
            CHECK(*map.Find(i) == i * 10);
        }
    }
}

TEST_CASE("the map survives growth across many rehashes") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i) {
        map.Insert(i, i * 3);
    }
    CHECK(map.Size() == kCount);
    for (int i = 0; i < kCount; ++i) {
        REQUIRE(map.Find(i) != nullptr);
        CHECK(*map.Find(i) == i * 3);
    }
}

TEST_CASE("colliding keys all remain findable") {
    // Every key hashes to bucket 0, so this exercises the probe sequence directly.
    SystemAllocator allocator;
    HashMap<Collide, int> map(allocator);
    for (int i = 0; i < 50; ++i) {
        map.Insert(Collide{i}, i);
    }
    CHECK(map.Size() == 50);
    for (int i = 0; i < 50; ++i) {
        REQUIRE(map.Find(Collide{i}) != nullptr);
        CHECK(*map.Find(Collide{i}) == i);
    }
}

TEST_CASE("removal from a fully-colliding map does not strand later entries") {
    // This is the case tombstone-free deletion gets wrong if backward shifting is buggy:
    // removing from the middle of a probe chain must not make its tail unreachable.
    SystemAllocator allocator;
    HashMap<Collide, int> map(allocator);
    for (int i = 0; i < 20; ++i) {
        map.Insert(Collide{i}, i);
    }
    for (int i = 0; i < 20; i += 2) {
        CHECK(map.Remove(Collide{i}));
    }
    CHECK(map.Size() == 10);
    for (int i = 1; i < 20; i += 2) {
        REQUIRE(map.Find(Collide{i}) != nullptr);
        CHECK(*map.Find(Collide{i}) == i);
    }
}

TEST_CASE("values are destroyed exactly once") {
    SystemAllocator allocator;
    CHECK(Tracked::s_alive == 0);
    {
        HashMap<int, Tracked> map(allocator);
        for (int i = 0; i < 50; ++i) {
            map.Insert(i, Tracked{i});
        }
        CHECK(Tracked::s_alive == 50);
        CHECK(map.Remove(10));
        CHECK(Tracked::s_alive == 49);
    }
    CHECK(Tracked::s_alive == 0);
    CHECK(allocator.BytesAllocated() == 0);
}

TEST_CASE("Clear destroys values and empties the map") {
    SystemAllocator allocator;
    HashMap<int, Tracked> map(allocator);
    for (int i = 0; i < 8; ++i) {
        map.Insert(i, Tracked{i});
    }
    map.Clear();
    CHECK(map.Size() == 0);
    CHECK(Tracked::s_alive == 0);
    CHECK(map.Find(0) == nullptr);
}

TEST_CASE("String keys work through the Hasher specialisation") {
    SystemAllocator allocator;
    HashMap<StringView, int> map(allocator);
    map.Insert("alpha", 1);
    map.Insert("beta", 2);
    REQUIRE(map.Find("alpha") != nullptr);
    CHECK(*map.Find("alpha") == 1);
    CHECK(*map.Find("beta") == 2);
    CHECK(map.Find("gamma") == nullptr);
}

TEST_CASE("moving a map transfers ownership and leaves the source empty") {
    SystemAllocator allocator;
    HashMap<int, int> source(allocator);
    source.Insert(1, 10);
    HashMap<int, int> moved(std::move(source));
    CHECK(moved.Size() == 1);
    CHECK(*moved.Find(1) == 10);
    CHECK(source.Size() == 0);
    CHECK(source.Find(1) == nullptr);
}

TEST_CASE("iteration visits every entry exactly once") {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    for (int i = 0; i < 20; ++i) {
        map.Insert(i, i);
    }
    int count = 0;
    int sum   = 0;
    for (const auto& entry : map) {
        ++count;
        sum += entry.value;
    }
    CHECK(count == 20);
    CHECK(sum == 190);   // 0..19
}

TEST_CASE("a map releases everything it allocated") {
    SystemAllocator allocator;
    {
        HashMap<int, int> map(allocator);
        for (int i = 0; i < 500; ++i) {
            map.Insert(i, i);
        }
    }
    CHECK(allocator.BytesAllocated() == 0);
}
```

- [ ] **Step 2: Build and confirm it fails**

- [ ] **Step 3: Write the header**

`Source/Monarc.Core/Include/Monarc/Core/Containers/HashMap.h` must provide this surface.
The container is open-addressed with **power-of-two capacity, linear probing, and
backward-shift deletion** — no tombstones, because an engine's maps are long-lived and
tombstones degrade them until rehashed.

```cpp
#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Hash.h>
#include <Monarc/Core/Memory/Allocator.h>

namespace Monarc {

/// Open-addressed hash map over an explicit allocator.
///
/// Linear probing with backward-shift deletion, so no tombstones accumulate. Capacity is
/// always a power of two, which makes the bucket index a mask rather than a modulo.
///
/// Copying is deleted, as with Array<T> and String. Iteration order is unspecified and
/// must not be relied on.
template <typename Key, typename Value>
class HashMap {
public:
    struct Entry {
        Key   key;
        Value value;
    };

    explicit HashMap(IAllocator& allocator);
    ~HashMap();

    HashMap(const HashMap&)            = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& other) noexcept;
    HashMap& operator=(HashMap&& other) noexcept;

    /// Inserts, or replaces the value if the key is already present.
    /// Returns a reference to the stored value.
    Value& Insert(const Key& key, Value value);

    /// Returns nullptr when the key is absent.
    [[nodiscard]] Value*       Find(const Key& key);
    [[nodiscard]] const Value* Find(const Key& key) const;

    [[nodiscard]] bool Contains(const Key& key) const;

    /// Returns true if a key was removed.
    bool Remove(const Key& key);

    /// Destroys every entry. Capacity is retained.
    void Clear();

    [[nodiscard]] usize Size() const;
    [[nodiscard]] usize Capacity() const;
    [[nodiscard]] bool  IsEmpty() const;

    // Forward iteration over occupied entries, in unspecified order.
    class Iterator;
    class ConstIterator;
    [[nodiscard]] Iterator      begin();
    [[nodiscard]] Iterator      end();
    [[nodiscard]] ConstIterator begin() const;
    [[nodiscard]] ConstIterator end() const;

private:
    // Implementer's choice, subject to the tests.
};

}  // namespace Monarc
```

Requirements the tests enforce:

1. Removing from the middle of a probe chain must leave the rest of the chain findable.
   The fully-colliding tests exist precisely to catch a wrong backward shift.
2. Values are destroyed exactly once — on `Remove`, on `Clear`, on destruction, and on
   rehash (moved, not copied, then destroyed).
3. Growth applies the same overflow bound and fatal-on-failure policy as `Array<T>`.
4. A moved-from map is empty and safe to use.

- [ ] **Step 4: Build and confirm all fourteen cases pass**

- [ ] **Step 5: Verify on Clang and Release**

- [ ] **Step 6: Commit**

```bash
git add Source/Monarc.Core
git commit -m "core: open-addressed HashMap with backward-shift deletion"
```

---

## Task 4: Close out A2a

**Files:** modify `Docs/Status.md`

- [ ] **Step 1: Clean verification on all four presets**

For each of `msvc-debug`, `msvc-release`, `clang-debug`, `clang-release`:

```
cmake --preset <p> && cmake --build --preset <p> --clean-first && ctest --preset <p>
```

All must be warning-free and pass. Record the doctest case and assertion totals.

- [ ] **Step 2: Update Status.md**

In the implementation-progress table, split the A2 row into A2a–A2d and mark A2a complete:

```markdown
| A2a | Hash, String, HashMap | **Complete** |
| A2b | Math — vectors, matrices, quaternions, transforms | Not started |
| A2c | Platform — files, paths, time, threads, dynamic libs, GUID | Not started |
| A2d | Monarc.Jobs — thread pool, dependency graph, priorities | Not started |
```

Add an `### A2a delivered` subsection listing what landed and the test totals, in the same
style as `### A1 delivered`.

Add to the A1-gaps subsection, since it now applies to three containers rather than one:

```markdown
- **No small-string optimisation in `String`.** Deferred deliberately: it is an
  implementation detail behind an unchanged interface, and nothing serializes a `String`
  yet. Revisit when there is profiling data rather than intuition.
```

- [ ] **Step 3: Confirm links and commit**

```bash
python Tools/check_doc_links.py Docs README.md
git add Docs/Status.md
git commit -m "docs: record Phase A2a complete"
```

---

## Definition of done

1. All four presets configure, build clean, and pass.
2. Every build is warning-free (`/WX`).
3. The four architecture gates still pass.
4. `Hasher` is demonstrably specialisable from outside `Hash.h`.
5. The aliasing cases and the colliding-removal case pass — those catch the defects this
   design is most likely to have. Note `s.Append(s.View())` on a string that fits inside its
   capacity does **not** reallocate, so it does not exercise the dangerous path on its own;
   a second case fills to capacity first, because growth-plus-aliasing is what was a
   use-after-free in `Array<T>`.
6. `Docs/Status.md` reflects reality.

## What A2a deliberately excludes

- **No small-string optimisation.** See the decisions section.
- **No `HashSet`.** Trivial to add on top of `HashMap` when something needs one.
- **No string formatting or parsing.** `MONARC_LOG` covers formatting; parsing arrives with
  serialization in Phase C.
- **No `Clone` for `String` or containers.** Copying stays deleted until a caller needs it,
  at which point it should be explicit.
- **No stable iteration order.** Unspecified and documented as such.
