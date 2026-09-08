#include <doctest/doctest.h>

#include <Monarc/RHI/Handles.h>

#include <type_traits>

using Monarc::RHI::BufferHandle;
using Monarc::RHI::TextureHandle;

namespace {

/// True when `a == b` is well-formed for the two types. Written as a variable template
/// rather than a bare requires-expression inside static_assert so that the requirement is
/// checked in a template context on both compilers.
template <typename A, typename B>
constexpr bool kEqualityComparable = requires(A a, B b) { a == b; };

}  // namespace

TEST_CASE("a default handle is invalid") {
    CHECK_FALSE(TextureHandle{}.IsValid());
    CHECK_FALSE(BufferHandle{}.IsValid());
}

TEST_CASE("ForTesting takes index first, generation second") {
    // Every other case in this file -- and every pool test Tasks 2 and 3 will add -- reads
    // its meaning through this function: `ForTesting(3, 1)` says "slot 3, generation 1" only
    // because the parameters are in that order. Nothing else pins it. A test that compares
    // one handle against another built the same way cannot tell the two arguments apart, so
    // swapping them inside ForTesting was confirmed to leave the rest of the suite green.
    // This is the one place the order is checked against the member names directly.
    const TextureHandle handle = TextureHandle::ForTesting(3, 1);
    CHECK(handle.index == 3u);
    CHECK(handle.generation == 1u);
}

TEST_CASE("handles compare on both index and generation") {
    const TextureHandle a = TextureHandle::ForTesting(3, 1);
    const TextureHandle b = TextureHandle::ForTesting(3, 1);
    const TextureHandle c = TextureHandle::ForTesting(3, 2);   // same slot, later generation
    const TextureHandle d = TextureHandle::ForTesting(4, 1);   // different slot
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.IsValid());
}

TEST_CASE("generation zero is a valid generation") {
    // Only the index says whether a handle names anything; generation 0 is the first
    // occupant of a slot, not a marker for "none". Treating it as invalid would make every
    // resource created before the first recycle unusable.
    CHECK(TextureHandle::ForTesting(0, 0).IsValid());
}

// The tag parameter's entire purpose, and the one property no run-time CHECK can express:
// mixing handle types must not compile. Buffer pool slot 7 and texture pool slot 7 name
// unrelated resources, and nothing about the two handles at run time says so -- both carry
// index 7 and the same generation, so every member-by-member comparison a test could write
// passes whether the tag does its job or not. Only the type distinguishes them, which means
// only the compiler can be asked. A TEST_CASE doing exactly that comparison used to sit
// here; it was deleted because it reduced to 7 == 7. If the tag were dropped -- or the
// comparison operators written over two independent template parameters -- these assertions
// fail and say so at build time, which is when it matters.
static_assert(!std::is_same_v<TextureHandle, BufferHandle>);
static_assert(!std::is_assignable_v<TextureHandle&, BufferHandle>);
static_assert(!std::is_convertible_v<BufferHandle, TextureHandle>);
static_assert(!kEqualityComparable<TextureHandle, BufferHandle>);
static_assert(kEqualityComparable<TextureHandle, TextureHandle>);
static_assert(kEqualityComparable<BufferHandle, BufferHandle>);

// A handle is data, not an object with behaviour: it must survive being memcpy'd into a
// command buffer, a serialized stream, or across a language boundary (ADR-0002).
static_assert(std::is_trivially_copyable_v<TextureHandle>);
static_assert(sizeof(TextureHandle) == sizeof(BufferHandle));
static_assert(sizeof(TextureHandle) == 2 * sizeof(Monarc::u32));

// Constant-evaluable, so a handle can be a compile-time constant and the operators are
// exercised at constant-evaluation time too.
static_assert(!TextureHandle{}.IsValid());
static_assert(TextureHandle::ForTesting(1, 0).IsValid());
static_assert(TextureHandle::ForTesting(1, 0) == TextureHandle::ForTesting(1, 0));
static_assert(TextureHandle::ForTesting(1, 0) != TextureHandle::ForTesting(1, 1));
