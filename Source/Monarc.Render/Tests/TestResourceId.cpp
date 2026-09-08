#include <doctest/doctest.h>

#include <Monarc/RHI/Handles.h>
#include <Monarc/Render/ResourceId.h>

#include <type_traits>

using Monarc::Render::TextureId;
using Monarc::RHI::BufferHandle;
using Monarc::RHI::TextureHandle;

namespace {

/// True when `a == b` is well-formed for the two types. Written as a variable template rather
/// than a bare requires-expression inside `static_assert` so that the requirement is checked
/// in a template context on both compilers -- Tests/TestHandles.cpp in Monarc.RHI does the
/// same and says why.
template <typename A, typename B>
constexpr bool kEqualityComparable = requires(A a, B b) { a == b; };

}  // namespace

TEST_CASE("a default id is invalid") { CHECK_FALSE(TextureId{}.IsValid()); }

TEST_CASE("ForTesting takes index first, generation second") {
    // Every other case in this file and in TestPassDeclaration.cpp reads its meaning through
    // the (index, generation) order: `ForTesting(3, 1)` says "declaration 3 of build 1" only
    // because the parameters are in that order, and nothing else pins it. A test comparing one
    // id against another built the same way cannot tell the two arguments apart. This is the
    // one place the order is checked against the member names directly.
    const TextureId id = TextureId::ForTesting(3, 1);
    CHECK(id.index == 3u);
    CHECK(id.generation == 1u);
}

TEST_CASE("ids compare on both index and generation") {
    const TextureId a = TextureId::ForTesting(3, 1);
    const TextureId b = TextureId::ForTesting(3, 1);
    const TextureId c = TextureId::ForTesting(3, 2);  // same declaration index, later build
    const TextureId d = TextureId::ForTesting(4, 1);  // different declaration
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.IsValid());
}

TEST_CASE("generation zero is a real build generation") {
    // A `RenderGraph` starts at build generation 0 and only `Reset` bumps it, so every id the
    // first build hands out carries zero. Treating that as "no build" would make the whole of
    // the first frame's declarations unusable. Only the index says whether an id names
    // anything -- `RHI::Handle` states the same rule about its own generation.
    CHECK(TextureId::ForTesting(0, 0).IsValid());
}

// ---------------------------------------------------------------------------------------
// The property the whole file exists for, and the one no run-time CHECK can express: a graph
// id and an RHI handle must not be interconvertible.
//
// **Nothing at run time distinguishes them.** `TextureId{4, 0}` and `TextureHandle{4, 0}` are
// two `u32`s with the same values; every member-by-member comparison a test could write passes
// whether the types are kept apart or not. Only the compiler can be asked, which is why these
// are `static_assert`s and why there is no TEST_CASE doing the same thing -- it would reduce to
// `4 == 4`. Monarc.RHI/Tests/TestHandles.cpp records that exact deletion.
//
// **What breaks each of these, so that none is a tautology.** `using TextureId =
// RHI::TextureHandle` breaks the first. Giving `ResourceId` a converting constructor from
// `RHI::Handle`, or `RHI::Handle` one to `ResourceId`, breaks the middle three -- each
// direction independently, which is why both are here. And writing `operator==` over an id and
// a handle, or over two independent template parameters wide enough to admit both, breaks the
// last.
//
// **Measured, three ways, on both compilers.** Each of these was compiled as its own
// translation unit under the same flags the module uses, on MSVC 19.51 and clang-cl 22.1, and
// each is an error:
//
// - `id = handle;` -- MSVC `error C2679: binary '=': no operator found which takes a
//   right-hand operand of type 'Monarc::RHI::TextureHandle'`; clang-cl `error: no viable
//   overloaded '='` with `note: candidate function (the implicit copy assignment operator)
//   not viable: no known conversion from 'RHI::TextureHandle' (aka 'Handle<Detail::TextureTag>')
//   to 'const ResourceId<Monarc::Render::Detail::TextureTag>'`.
// - `id == handle` -- MSVC `error C2676: binary '==': 'Monarc::Render::TextureId' does not
//   define this operator or a conversion to a type acceptable to the predefined operator`;
//   clang-cl `error: invalid operands to binary expression ('TextureId' (aka
//   'ResourceId<Detail::TextureTag>') and 'TextureHandle' (aka 'Handle<Detail::TextureTag>'))`.
//   Reversed to `handle == id` it is the same C2676 on MSVC with the other type named, and the
//   same clang-cl error with the operands swapped: the message names whichever operand stands
//   on the left, and neither order produces C2678. **This bullet previously recorded a C2678
//   naming `TextureHandle` as the left-hand operand of `id == handle`, which corresponds to no
//   compilation of either order** -- it was re-measured rather than reasoned about, and the
//   text above is what came back.
//   **Both compilers then name the mechanism in their notes, which is the half worth
//   reading.** clang: `candidate template ignored: could not match 'Handle' against
//   'ResourceId'` and `candidate template ignored: could not match 'ResourceId' against
//   'Handle'`. MSVC says the same thing at more length: `could not deduce template argument
//   for 'const Monarc::RHI::Handle<Tag> &' from 'Monarc::Render::TextureId'`, and the
//   mirrored note for `ResourceId`. Either way it is the two comparison templates each
//   deducing their single `Tag` from both operands and discarding themselves.
// - `Takes(TextureId{})` where `Takes` wants an `RHI::TextureHandle` -- MSVC `error C2664:
//   'void Takes(Monarc::RHI::TextureHandle)': cannot convert argument 1 from
//   'Monarc::Render::TextureId' to 'Monarc::RHI::TextureHandle'`; clang-cl `error: no matching
//   function for call to 'Takes'` with `note: candidate function not viable: no known
//   conversion`. This is the one a graph would meet in practice, at the seam where Task 4's
//   `Execute` resolves an id into the handle it stands for.
// ---------------------------------------------------------------------------------------

static_assert(!std::is_same_v<TextureId, TextureHandle>);
static_assert(!std::is_assignable_v<TextureId&, TextureHandle>);
static_assert(!std::is_assignable_v<TextureHandle&, TextureId>);
static_assert(!std::is_convertible_v<TextureHandle, TextureId>);
static_assert(!std::is_convertible_v<TextureId, TextureHandle>);
static_assert(!kEqualityComparable<TextureId, TextureHandle>);
static_assert(!kEqualityComparable<TextureId, BufferHandle>);
static_assert(kEqualityComparable<TextureId, TextureId>);
static_assert(kEqualityComparable<TextureHandle, TextureHandle>);

// An id is data, not an object with behaviour: it goes in an inspection row, in a barrier's
// cause, and eventually in whatever a debug view reads. ADR-0002's rule for `RHI::Handle`
// applies unchanged.
static_assert(std::is_trivially_copyable_v<TextureId>);
static_assert(sizeof(TextureId) == 2 * sizeof(Monarc::u32));

// Constant-evaluable, so an id can be a compile-time constant and the operators are exercised
// at constant-evaluation time as well as at run time.
static_assert(!TextureId{}.IsValid());
static_assert(TextureId::ForTesting(1, 0).IsValid());
static_assert(TextureId::ForTesting(1, 0) == TextureId::ForTesting(1, 0));
static_assert(TextureId::ForTesting(1, 0) != TextureId::ForTesting(1, 1));
static_assert(TextureId::ForTesting(1, 0) != TextureId::ForTesting(2, 0));
