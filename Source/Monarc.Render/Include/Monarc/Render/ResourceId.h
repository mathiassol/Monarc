#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc::Render {

namespace Detail {
// Tags exist only to keep otherwise identical id types apart, so they are declared and never
// defined -- exactly as Monarc/RHI/Handles.h does it, and for the same reason: nothing
// constructs one, and an incomplete type cannot be used by accident.
struct TextureTag;
}  // namespace Detail

/// Names one resource **declared in one graph build**: an index into that build's resource
/// list, paired with the generation of the build it was declared in.
///
/// **This is not a GPU resource, and keeping the two apart is the reason this type exists
/// rather than reusing `RHI::TextureHandle`.** An `RHI::TextureHandle` names a live image the
/// device has allocated. A `ResourceId` names a *declaration*: something a pass said it needs,
/// which the graph may back with a physical resource, may back with memory it shares with
/// another declaration, or may decide nothing needs at all once culling has run. Conflating
/// them makes culling and aliasing unrepresentable -- a culled resource never gets a physical
/// resource, so an id that *was* a handle would have to be a handle to nothing.
///
/// The two are therefore not interconvertible, and that is asserted rather than asserted-about:
/// see Tests/TestResourceId.cpp, which pins it with the same four `static_assert`s
/// Tests/TestHandles.cpp uses on `RHI::Handle`, and records the diagnostic each way round.
/// They are separate class templates, so no instantiation of one is ever an instantiation of
/// the other; `Tag` is what will keep *two graph id types* apart when the second arrives.
///
/// **`generation` is the graph build's, not a resource pool's, and that difference is the
/// whole of what it buys.** `RenderGraph` is constructed once and reset every frame (see
/// `RenderGraph::Reset`), because a graph that allocated per frame would defeat the fixed
/// pools it is built on. Its resource indices therefore start again from zero on every build,
/// so an id held across a `Reset` would name a *different* declaration rather than a missing
/// one -- silently, and with no bad pointer for a sanitizer to find. `Reset` bumps the build
/// generation, and every id the graph resolves is checked against the current one, so the
/// stale id is refused instead. ADR-0002's argument for `RHI::Handle`'s generation counter,
/// applied to a different kind of recycling.
///
/// Generation zero is a real generation -- the first build's -- for the reason
/// `RHI::Handle`'s own comment gives: only the index says whether an id names anything.
///
/// Trivially copyable. Default-constructs invalid (an index no declaration ever has), and
/// nothing but the graph should construct a valid one. `ForTesting` is how a test says it is
/// doing that on purpose; it is a convention rather than a barrier, since `ResourceId` is an
/// aggregate and `TextureId{7, 0}` therefore compiles anywhere. What it buys is a name worth
/// grepping for -- construction that bypassed a graph is visible as such at the call site.
/// `RHI::Handle::ForTesting` and `JobHandle::ForTesting` exist for the same reason and with
/// the same limit.
template <typename Tag>
struct ResourceId {
    u32 index      = kInvalidIndex;
    u32 generation = 0;

    [[nodiscard]] constexpr bool IsValid() const { return index != kInvalidIndex; }

    /// Constructs an id naming declaration `index` of build `generation` directly, bypassing
    /// the graph entirely. For tests that need ids no graph handed out -- including the stale
    /// and out-of-range ones whose refusal is the point.
    [[nodiscard]] static constexpr ResourceId ForTesting(u32 index, u32 generation) {
        return ResourceId{index, generation};
    }

private:
    /// No real declaration ever reaches this index: a graph configured with that many
    /// resources could not allocate its pools, and `AddPass` would have refused long before.
    static constexpr u32 kInvalidIndex = static_cast<u32>(-1);
};

// Free function templates in ResourceId's own namespace, which is the house form -- see the
// note on Handle's comparisons in Monarc/RHI/Handles.h for why the choice between this and a
// hidden friend buys nothing either way.
//
// The single Tag parameter is what buys something, and it is the same mechanism Handles.h
// relies on: deducing Tag from both operands of a mixed comparison yields conflicting types,
// so the only candidate is discarded and comparing two different id types does not compile.
// Written over two independent tag parameters it would compile instead. Nothing exercises that
// today -- there is one id type -- so what the tests pin is the property that matters now:
// a `TextureId` and an `RHI::TextureHandle` are not comparable, which holds because they are
// different templates.
template <typename Tag>
[[nodiscard]] constexpr bool operator==(const ResourceId<Tag>& a, const ResourceId<Tag>& b) {
    return a.index == b.index && a.generation == b.generation;
}

template <typename Tag>
[[nodiscard]] constexpr bool operator!=(const ResourceId<Tag>& a, const ResourceId<Tag>& b) {
    return !(a == b);
}

/// A texture declared in a graph build: a render target, a depth buffer, or an imported image
/// the graph does not own.
using TextureId = ResourceId<Detail::TextureTag>;

// **There is deliberately no `BufferId`, and the rule that decides it is not the one
// `Access.h` follows.** `Monarc/RHI/Barrier.h`'s rule -- the barrier model arrives whole,
// because an unused stage or access bit costs one row in one switch -- is what `ResourceAccess`
// in Access.h follows, which is why that enum names buffer accesses (`IndirectRead`) that no
// resource can carry yet. A resource *kind* is not one switch row: it is a declaration call,
// an inspection row, a lifetime, an alias group and a barrier type, so it follows the other
// rule -- the one `Handles.h` states for its own absent handle types, that a resource type
// arrives with something that creates one.
//
// Nothing in Phase A4 declares a buffer: `Monarc.FirstLight` imports one swapchain image.
// `BufferId` arrives with the first pass that needs a buffer -- an indirect draw, or a
// readback -- alongside the `Read`/`Write` overloads, the `RHI::BufferBarrier` derivation and
// the inspection rows that go with it.

}  // namespace Monarc::Render
