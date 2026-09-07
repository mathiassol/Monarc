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
/// device bumps it every time a slot is recycled, so a handle whose generation no longer
/// matches its slot resolves to a failure rather than to whatever now occupies the memory --
/// see ADR-0002, which is also why these are handles and not pointers at all.
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

// Free function templates in Handle's own namespace, not friends. JobHandle.h explains the
// reasoning in full; the short of it is that ADL looks at the operands' namespace rather
// than at whichever class declared a friend, and that MSVC accepts [[nodiscard]] on a friend
// declaration where clang-cl correctly rejects it.
//
// Note what the single Tag parameter buys: deduction fails outright for a comparison between
// two different handle types, so `texture == buffer` does not compile. That is the type
// safety the tag exists for, and Tests/TestHandles.cpp asserts it.
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
// There is deliberately no Hasher<Handle<Tag>> either. JobHandle has one because
// HashMap<JobHandle, V> is a real user of it; nothing keys a map by a resource handle, and a
// resource pool indexes by handle.index directly. It arrives with its first caller.

}  // namespace Monarc::RHI
