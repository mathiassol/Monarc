#pragma once

#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Handles.h>

#include <concepts>
#include <string_view>

namespace Monarc::RHI {

/// A pipeline stage, as one bit of a synchronisation scope.
///
/// This is the `syncBefore`/`syncAfter` half of ADR-0005's barrier: the stages being
/// synchronised. A *set* of them, because both APIs the model is shaped from take a set --
/// Vulkan's `VkPipelineStageFlags2` and D3D12's `D3D12_BARRIER_SYNC` are masks, and a barrier
/// that had to name a single stage could not express "after both the colour write and the
/// depth write". `operator|` below is what builds one.
///
/// **The membership rule for this list, and it is not "what A3 uses".** A3 uses four of these.
/// The plan is explicit that the barrier model arrives whole and that translation covers all
/// of it, with the unused part carried by the pure-function tests -- which is the opposite of
/// `Format`'s "a format arrives with its first user" in Types.h, and deliberately so: a format
/// costs a size claim nothing verifies, where a stage costs one row in one switch. So the line
/// drawn here is instead:
///
/// - **Vulkan 1.3 core only.** Nothing that needs an extension. `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
///   is the case that makes this a rule worth stating: presentation is a layout the swapchain
///   needs and `VK_KHR_swapchain` defines, so it arrives with `ISwapchain` in Task 4 rather
///   than here.
/// - **Stages Monarc has a plan for.** Tessellation, geometry, transform feedback, video and
///   the ray-tracing and mesh-shading stages are absent because no phase of M0 schedules them;
///   ADR-0005 is about the shape of a barrier, not about naming every stage Vulkan has.
///
/// Adding one is a row in `ToString` (Private/Barrier.cpp) and a row in `ToVulkan`
/// (Monarc.RHI.Vulkan/Private/Translate.cpp), and both switches are `default`-less, so
/// forgetting either is a compile error rather than a stage that silently synchronises
/// nothing.
enum class PipelineStage : u32 {
    /// No stage. A legitimate value on either side of a barrier, and the reason it is a
    /// named enumerator rather than an empty mask by accident: `syncBefore = None` is how a
    /// barrier says "nothing before this needs waiting for", which is exactly what the first
    /// transition of a freshly created texture means.
    None = 0,

    DrawIndirect          = 1u << 0,
    VertexShader          = 1u << 1,
    FragmentShader        = 1u << 2,
    EarlyFragmentTests    = 1u << 3,
    LateFragmentTests     = 1u << 4,
    ColorAttachmentOutput = 1u << 5,
    ComputeShader         = 1u << 6,

    // The four transfer stages Vulkan 1.3 separates. Kept separate here for the same reason
    // Vulkan separates them: a barrier that named "transfer" would synchronise a blit against
    // a clear that never touched the same memory.
    Copy    = 1u << 7,
    Blit    = 1u << 8,
    Resolve = 1u << 9,
    Clear   = 1u << 10,

    /// The host's own accesses. Needed by anything the CPU reads back -- the readback path in
    /// Task 3's device tests is its first caller, on the buffer barrier between the copy and
    /// the map.
    Host = 1u << 11,

    AllGraphics = 1u << 12,
    AllCommands = 1u << 13,
};

/// How memory was, or will be, accessed -- ADR-0005's `accessBefore`/`accessAfter`.
///
/// A set, for the reason `PipelineStage` is one, and governed by the same membership rule:
/// Vulkan 1.3 core, and nothing for a stage Monarc has no plan for.
enum class Access : u32 {
    /// No access. Paired with a stage of `None` this is the "execution dependency only" half
    /// of the model -- ordering with no cache maintenance -- and paired with a real stage it
    /// is what the discard side of an undefined-layout transition wants.
    None = 0,

    IndirectCommandRead = 1u << 0,
    IndexRead           = 1u << 1,
    VertexAttributeRead = 1u << 2,
    UniformRead         = 1u << 3,

    ShaderSampledRead  = 1u << 4,
    ShaderStorageRead  = 1u << 5,
    ShaderStorageWrite = 1u << 6,

    ColorAttachmentRead         = 1u << 7,
    ColorAttachmentWrite        = 1u << 8,
    DepthStencilAttachmentRead  = 1u << 9,
    DepthStencilAttachmentWrite = 1u << 10,

    TransferRead  = 1u << 11,
    TransferWrite = 1u << 12,

    HostRead  = 1u << 13,
    HostWrite = 1u << 14,

    /// Any read, or any write, of any kind. The catch-alls a render graph reaches for when it
    /// cannot narrow further, and what an unrecognised value translates to -- see
    /// `ToVulkan(Access)` in Monarc.RHI.Vulkan/Private/Translate.h.
    MemoryRead  = 1u << 15,
    MemoryWrite = 1u << 16,
};

/// How a texture's memory is arranged for a particular use -- ADR-0005's
/// `layoutBefore`/`layoutAfter`, and the one part of the model that applies to textures only.
///
/// A single value rather than a set: a texture is in exactly one layout at a time, which is
/// the whole reason a transition needs naming.
enum class TextureLayout : u32 {
    /// The contents are not defined. Every texture starts here, and a transition *out* of it
    /// discards whatever the memory held rather than preserving it.
    ///
    /// **This is a legitimate before-layout, and that is why a missing layout pair cannot be
    /// caught at run time.** A `TextureBarrier` whose `layoutBefore` was left to a default
    /// would land on exactly this value, indistinguishable from a caller who meant it -- so
    /// the pair is required by the constructor instead. See `TextureBarrier` below.
    Undefined = 0,

    /// Usable for anything the device supports, at the cost of being optimal for nothing.
    General,

    ColorAttachment,
    DepthStencilAttachment,

    /// Depth and stencil, readable but not writable. Distinct from `DepthStencilAttachment`
    /// because a read-only depth attachment can be sampled at the same time.
    DepthStencilReadOnly,

    ShaderReadOnly,

    TransferSource,
    TransferDestination,
};

/// The enumerator's own spelling, for logs and test failures. Never nullptr.
///
/// **The shipped caller of all three is `Describe` at the foot of this header, and the shipped
/// caller of `Describe` is a backend's barrier refusal.** When `ICommandList::Barrier` is
/// handed a resource handle it cannot resolve it has no return value to report through, so
/// the `MONARC_LOG` beside its `MONARC_CHECK` is where the barrier gets identified -- and
/// identifying a barrier means naming its layout pair and its two synchronisation scopes.
/// Monarc.RHI.Vulkan/Private/VulkanDevice.cpp's two handle-resolving `Barrier` overloads are
/// where that log line is emitted; a `MONARC_CHECK` message is a string literal by house rule,
/// so the composed half has to live in a log line, and `Describe` is what composes it.
///
/// The chain matters because the far end of it is fatal and therefore untestable in process:
/// the refusal ends with `std::abort()` (see `ICommandList::Barrier` in Device.h), so no test
/// can watch that log line being emitted and then go on to read it. `Describe` is the half
/// that can be tested, and `Tests/TestBarrier.cpp` tests it with no device anywhere in sight.
///
/// **A mask of several stages is not an enumerator and does not have a spelling here.**
/// `ToString(Copy | Blit)` reports the not-a-stage name, exactly as a value no enumerator
/// names does, because that is what it is. That is why the log lines print each mask's hex
/// beside its name: the fallback name is honest but not decodable on its own. A mask-walking
/// formatter is what would replace them, and it arrives with a caller that needs one.
[[nodiscard]] const char* ToString(PipelineStage stage);

/// The enumerator's own spelling. `ToString(PipelineStage)`'s rule about masks, and its note
/// on the shipped caller, apply here too.
[[nodiscard]] const char* ToString(Access access);

/// The enumerator's own spelling. Never nullptr; a value outside the enumerator set gets a
/// name of its own rather than "Undefined" -- see `ToString(Format)` in Types.h for why
/// conflating a named state with an invalid one misdirects whoever reads the log.
///
/// This one is never a mask -- a texture is in exactly one layout -- so in the barrier log
/// line above it is the pair that always reads as two real spellings.
[[nodiscard]] const char* ToString(TextureLayout layout);

namespace Detail {

/// Opt-in marker for the enums above whose values are bits rather than alternatives.
///
/// Opt-in rather than a blanket `template <typename E> requires std::is_enum_v<E>`, which
/// would give `operator|` to `TextureLayout` and to `Format` -- and `ColorAttachment |
/// TransferSource` is not a layout, it is a bug that would compile. Adding a third flag set is
/// one specialisation.
template <typename E>
inline constexpr bool kIsFlagSet = false;

template <>
inline constexpr bool kIsFlagSet<PipelineStage> = true;

template <>
inline constexpr bool kIsFlagSet<Access> = true;

template <typename E>
concept FlagSet = kIsFlagSet<E>;

}  // namespace Detail

// Free function templates in the enums' own namespace, which is the house form -- see the
// note on Handle's comparisons in Handles.h for why the choice between this and a hidden
// friend buys nothing either way. What it does buy here is that both flag sets are served by
// one definition each instead of eight one-line overloads.

template <Detail::FlagSet E>
[[nodiscard]] constexpr E operator|(E a, E b) {
    return static_cast<E>(static_cast<u32>(a) | static_cast<u32>(b));
}

template <Detail::FlagSet E>
[[nodiscard]] constexpr E operator&(E a, E b) {
    return static_cast<E>(static_cast<u32>(a) & static_cast<u32>(b));
}

template <Detail::FlagSet E>
constexpr E& operator|=(E& a, E b) {
    a = a | b;
    return a;
}

/// Whether any bit of `bits` is set in `value`. `HasAny(x, E::None)` is false: an empty set
/// intersects nothing, which is the answer a caller asking "does this barrier touch the
/// transfer stages" wants.
template <Detail::FlagSet E>
[[nodiscard]] constexpr bool HasAny(E value, E bits) {
    return static_cast<u32>(value & bits) != 0;
}

/// A barrier over every resource at once: ordering and cache maintenance with no particular
/// buffer or texture named.
///
/// An aggregate whose every field defaults to `None`, so `GlobalBarrier{}` is the barrier that
/// changes nothing. That value is representable on purpose -- a render graph deriving barriers
/// will produce it for a pass with no state to change, and a model in which "no change" had to
/// be spelled as "no barrier" would push that decision into every caller.
struct GlobalBarrier {
    PipelineStage syncBefore   = PipelineStage::None;
    PipelineStage syncAfter    = PipelineStage::None;
    Access        accessBefore = Access::None;
    Access        accessAfter  = Access::None;

    constexpr bool operator==(const GlobalBarrier&) const = default;
};

/// A barrier over one buffer.
///
/// An aggregate for `GlobalBarrier`'s reason, and it has no layout pair at all -- buffers do
/// not have layouts in either API the model is shaped from. Three distinct barrier types
/// rather than one type with fields that apply to some of them: a `layoutBefore` on a buffer
/// barrier would be a field with no meaning that every backend would have to remember to
/// ignore.
struct BufferBarrier {
    BufferHandle buffer = {};

    PipelineStage syncBefore   = PipelineStage::None;
    PipelineStage syncAfter    = PipelineStage::None;
    Access        accessBefore = Access::None;
    Access        accessAfter  = Access::None;

    constexpr bool operator==(const BufferBarrier&) const = default;
};

/// A barrier over one texture, including its layout transition.
///
/// **Not an aggregate, and every field is required by the constructor. That is the whole
/// design of this type.** The A3 plan asks that a texture barrier without a layout pair be
/// "rejected at the type level rather than at runtime", and a run-time check cannot do it:
/// `TextureLayout::Undefined` is a legitimate `layoutBefore` -- it is what a freshly created
/// texture is in -- so a barrier that had defaulted its layouts is byte-for-byte identical to
/// one whose author meant `Undefined`. There is nothing left for a check to look at.
///
/// So there is no default constructor and no aggregate initialisation, and the constructor
/// takes all six ADR-0005 fields plus the texture with no defaults on any of them. Omitting
/// the layout pair does not produce a barrier with a plausible default; it fails to compile.
/// **All four ways of omitting it were tried on both compilers**, and each is an error:
///
/// - Five parenthesised arguments -- MSVC `error C2512`'s sibling, `error C2440:
///   '<function-style-cast>': cannot convert from 'initializer list' to
///   'Monarc::RHI::TextureBarrier'` with `note: ... function does not take 5 arguments`;
///   clang-cl `error: no matching constructor for initialization of 'TextureBarrier'` with
///   `note: candidate constructor not viable: requires 7 arguments, but 5 were provided`.
/// - Five *braced* arguments, which is what would have worked on an aggregate: the same two
///   diagnostics.
/// - `TextureBarrier barrier;` -- MSVC `error C2512: 'Monarc::RHI::TextureBarrier': no
///   appropriate default constructor available`; clang-cl the same "no matching constructor"
///   with `requires 7 arguments, but 0 were provided`.
/// - Seven arguments with the stages shifted up into the layouts' places, which is the one a
///   count-based check would miss -- MSVC `note: ... cannot convert argument 2 from
///   'Monarc::RHI::PipelineStage' to 'Monarc::RHI::TextureLayout'`, clang-cl `note: ... no
///   known conversion from 'Monarc::RHI::PipelineStage' to 'TextureLayout' for 2nd argument`.
///   Distinct enum classes with no implicit conversion between them are what buys that one.
///
/// The cost is a seven-argument call. It is paid deliberately: the arguments are in the order
/// ADR-0005 and Docs/Rendering/RHI.md list the fields, before and after paired, and an
/// explicit barrier model is a place where naming every field at the call site is the point
/// rather than a burden. Batching, and the render graph that will do the naming instead of a
/// human, are A4's.
///
/// A no-change barrier is representable here too -- equal layouts, `None` everywhere -- and
/// the constructor stores what it was given rather than folding such a pair away. Tests
/// pin it: see Tests/TestBarrier.cpp.
class TextureBarrier {
public:
    constexpr TextureBarrier(TextureHandle texture, TextureLayout layoutBefore,
                             TextureLayout layoutAfter, PipelineStage syncBefore,
                             PipelineStage syncAfter, Access accessBefore, Access accessAfter)
        : m_texture(texture),
          m_layoutBefore(layoutBefore),
          m_layoutAfter(layoutAfter),
          m_syncBefore(syncBefore),
          m_syncAfter(syncAfter),
          m_accessBefore(accessBefore),
          m_accessAfter(accessAfter) {}

    [[nodiscard]] constexpr TextureHandle Texture() const { return m_texture; }
    [[nodiscard]] constexpr TextureLayout LayoutBefore() const { return m_layoutBefore; }
    [[nodiscard]] constexpr TextureLayout LayoutAfter() const { return m_layoutAfter; }
    [[nodiscard]] constexpr PipelineStage SyncBefore() const { return m_syncBefore; }
    [[nodiscard]] constexpr PipelineStage SyncAfter() const { return m_syncAfter; }
    [[nodiscard]] constexpr Access AccessBefore() const { return m_accessBefore; }
    [[nodiscard]] constexpr Access AccessAfter() const { return m_accessAfter; }

    constexpr bool operator==(const TextureBarrier&) const = default;

private:
    TextureHandle m_texture;
    TextureLayout m_layoutBefore;
    TextureLayout m_layoutAfter;
    PipelineStage m_syncBefore;
    PipelineStage m_syncAfter;
    Access        m_accessBefore;
    Access        m_accessAfter;
};

// Split barriers -- ADR-0005's last clause -- are not here. Both APIs express one as a pair of
// halves sharing an identity, and nothing in Monarc issues the first half yet: A3 records
// commands directly and A4's render graph is what will have a reason to overlap a transition
// with unrelated work. The shape above is what a split barrier is built from either way, so
// this is a field or a pair of calls added later rather than a different model.

// ---------------------------------------------------------------------------------------
// Describing a barrier in words.
// ---------------------------------------------------------------------------------------
//
// **Why this is in Monarc.RHI and not in the backend that needs it.** Its only shipped caller
// is Monarc.RHI.Vulkan's barrier refusal, and the obvious home was that module's
// Private/Translate.h, whose stated membership rule -- total, allocation-free, touching no
// Vulkan *state* -- these two functions satisfy. They are here instead for a reason that rule
// does not cover: every other function in Translate.h names a Vulkan type in its signature,
// and these name none. A `BufferBarrier` in and text out involves nothing Vulkan-shaped at
// any point, so putting it behind a Vulkan module's private header would mean a second
// backend either duplicates the composition or includes a header it has no business seeing.
//
// The narrower reason is that this is `ToString(PipelineStage)`'s own kind of function one
// level up -- the spelling of a whole barrier rather than of one enumerator -- and those live
// in Private/Barrier.cpp beside it. Keeping them together makes the shipped-caller chain a
// single hop within one module: shipped code calls `Describe`, `Describe` calls all three
// `ToString`s.
//
// The practical consequence is the one that decided it: Monarc.RHI's own test suite links no
// Vulkan at all, so the composition is checked in CI on a machine with no GPU. That was not
// true of the device-suite case this replaced.
//
// There is deliberately no `Describe(const GlobalBarrier&)`. A global barrier names no
// resource, so it has no handle to resolve and no refusal path, and a third overload would be
// one nothing calls -- which is the rule this module already applies to itself elsewhere (see
// `TextureUsage` in Device.h, whose absent `TransferDestination` says the same thing).

/// Chars a `BarrierDescription` holds, including the terminator.
///
/// Sized so that no description can be truncated, and the arithmetic is worth writing down
/// because the widest description is not the one anyone would guess. The texture form is the
/// longer of the two: 85 chars of fixed text, two `u32`s in decimal (10 each), two layout
/// names, and four synchronisation fields that each carry a name *and* a hex mask. The longest
/// name in each set belongs to a not-a-value marker rather than to any real enumerator --
/// `<not a single PipelineStage>` is 28 chars where the longest stage, `ColorAttachmentOutput`,
/// is 21 -- so a bound read off the enumerator lists would be too small for exactly the
/// barrier this description exists to report. 85 + 20 + 2*23 + 2*(28 + 8) + 2*(27 + 8) + 1 is
/// 294, and 320 is that with room to add a field without recomputing.
///
/// That is an *upper* bound and not the length of anything: it treats each field's longest
/// name and its widest hex as independent, and they are not -- a name and a number are two
/// views of one value. Measured, the widest description that actually exists is 283 chars for
/// a texture barrier and 223 for a buffer one. Tests/TestBarrier.cpp builds both and asserts
/// neither was clipped, so the constant is pinned rather than trusted.
inline constexpr usize kBarrierDescriptionLength = 320;

/// One barrier's identity and content, in words.
///
/// A returned struct rather than a `std::span<char>` out-parameter, which is
/// `AdapterUuidString`'s shape in Adapter.h and is chosen for its reason: there is no buffer
/// size to get wrong at a call site and nothing to document about one. It is filled through
/// `std::format_to_n` and never allocates, which is ADR-0003's condition on `<format>` in
/// runtime code.
struct BarrierDescription {
    char text[kBarrierDescriptionLength] = {};

    [[nodiscard]] std::string_view View() const { return std::string_view(text); }
};

/// Describes `barrier`: which buffer, and both synchronisation scopes by name with each mask's
/// hex beside it.
///
/// The hex is not decoration. A `syncBefore` of several stages is a mask and not an
/// enumerator, so `ToString` reports its not-a-single-value name for one -- see the note on
/// `ToString(PipelineStage)` above -- and the number is what keeps the line decodable when
/// that happens.
[[nodiscard]] BarrierDescription Describe(const BufferBarrier& barrier);

/// Describes `barrier`: which texture, its layout pair, and both synchronisation scopes by
/// name with each mask's hex.
///
/// The layout pair leads, because it is the half that is never a mask -- a texture is in
/// exactly one layout -- so those two words are always real spellings. "Undefined ->
/// ColorAttachment on a stale texture" names the transition the caller meant unambiguously,
/// where a refusal that said only "a texture handle was stale" would leave whoever reads it
/// to guess which of a frame's barriers it was. The layouts carry no hex for the same reason
/// they lead: there is no mask to decode.
[[nodiscard]] BarrierDescription Describe(const TextureBarrier& barrier);

}  // namespace Monarc::RHI
