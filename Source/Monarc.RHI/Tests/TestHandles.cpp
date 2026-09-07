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

TEST_CASE("two handle types can name the same slot without being the same resource") {
    // Buffer pool slot 7 and texture pool slot 7 are unrelated. Nothing at run time
    // distinguishes them -- the type does, which is the whole point of the tag.
    const BufferHandle  buffer  = BufferHandle::ForTesting(7, 2);
    const TextureHandle texture = TextureHandle::ForTesting(7, 2);
    CHECK(buffer.index == texture.index);
    CHECK(buffer.generation == texture.generation);
}

// The tag parameter's entire purpose, and the one property no run-time CHECK can express:
// mixing handle types must not compile. If the tag were dropped -- or the comparison
// operators written over two independent template parameters -- these assertions fail and
// say so at build time, which is when it matters.
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
