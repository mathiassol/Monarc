#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc::RHI {

namespace Detail {
// Tags exist only to keep otherwise identical handle types apart, so they are declared and
// never defined: nothing constructs one, and an incomplete type cannot be used by accident.
struct BufferTag;
struct TextureTag;
}  // namespace Detail

/// Names one GPU resource: an index into the device's pool for that resource type, paired
/// with a generation counter.
///
/// A pool slot is reused once its resource is destroyed, so the index alone cannot tell a
/// resource apart from whatever is created into that same slot next. The generation can: the
/// device bumps it whenever a slot changes hands -- on the destroy *and* on the next claim, so
/// a handle goes stale the moment its resource is destroyed rather than when the slot is next
/// filled -- and a handle whose generation no longer matches its slot resolves to a failure
/// rather than to whatever now occupies the memory. See ADR-0002, which is also why these are
/// handles and not pointers at all, and `IDevice::DestroyTexture` for why the destroy half of
/// that is load-bearing.
///
/// `Tag` carries no data and is never instantiated. Its only job is to make BufferHandle and
/// TextureHandle *different types*, so that passing one where the other is expected is a
/// compile error rather than a resolve that succeeds against the wrong pool.
///
/// Trivially copyable. Default-constructs invalid (an index no real resource ever has), and
/// nothing but the owning device should construct a valid one. ForTesting is how a test says
/// it is doing that on purpose -- it is a convention rather than a barrier, since Handle is
/// an aggregate and `BufferHandle{7, 2}` therefore compiles anywhere. What it buys is a name
/// worth grepping for: construction that bypasses a device is visible as such at the call
/// site instead of reading like ordinary code. JobHandle::ForTesting exists for the same
/// reason and with the same limit.
template <typename Tag>
struct Handle {
    u32 index      = kInvalidIndex;
    u32 generation = 0;

    [[nodiscard]] constexpr bool IsValid() const { return index != kInvalidIndex; }

    /// Constructs a handle naming slot `index` at `generation` directly, bypassing the
    /// device entirely. For tests that need handles no device actually returned -- see the
    /// class comment above.
    [[nodiscard]] static constexpr Handle ForTesting(u32 index, u32 generation) {
        return Handle{index, generation};
    }

private:
    /// No real slot ever reaches this index: a pool that large cannot be allocated, and a
    /// creation call would have failed long before.
    static constexpr u32 kInvalidIndex = static_cast<u32>(-1);
};

// Free function templates in Handle's own namespace rather than hidden friends. That part is
// the house form and not a technical requirement -- Monarc/Jobs/JobHandle.h records why the
// two reasons formerly cited here, an ADL blind spot around friends and a [[nodiscard]]
// portability trap, are not reasons. A hidden friend would forbid the mixed comparison below
// just as this does; the choice between the shapes is not what buys anything.
//
// The single Tag parameter is what buys something, and it is orthogonal to that choice.
// Deducing Tag from both operands of a mixed comparison yields conflicting types, so the
// only candidate is discarded and `texture == buffer` does not compile -- clang says
// "candidate template ignored: deduced conflicting types for parameter 'Tag'". Written over
// two independent tag parameters it would compile instead, which is the difference between
// the tag being a type-safety mechanism and it being decoration. Tests/TestHandles.cpp pins
// the result with `!kEqualityComparable<TextureHandle, BufferHandle>`, an assertion the
// two-parameter form was confirmed to break.
template <typename Tag>
[[nodiscard]] constexpr bool operator==(const Handle<Tag>& a, const Handle<Tag>& b) {
    return a.index == b.index && a.generation == b.generation;
}

template <typename Tag>
[[nodiscard]] constexpr bool operator!=(const Handle<Tag>& a, const Handle<Tag>& b) {
    return !(a == b);
}

/// A GPU buffer. Phase A3 needs one for host-visible readback staging.
using BufferHandle = Handle<Detail::BufferTag>;

/// A GPU texture. Phase A3 needs one to clear and read back.
using TextureHandle = Handle<Detail::TextureTag>;

// Samplers, shader modules and pipelines get their own handle types when there is a shader
// to bind them to. Adding them now would be declaring an interface for resources nothing
// creates, which is the kind of guess a later phase then has to undo.
//
// There is deliberately no Hasher<Handle<Tag>> either. The reason is the second one only:
// nothing keys a map by a resource handle, and a resource pool indexes by handle.index
// directly, so a hasher here would have no caller. JobHandle's own Hasher is not the
// counter-example it looks like -- the sole HashMap<JobHandle, V> in the tree is
// Monarc.Jobs/Tests/TestJobHandle.cpp:30, a test written to exercise that hasher rather
// than a use the engine has. This arrives with its first real caller.

}  // namespace Monarc::RHI
