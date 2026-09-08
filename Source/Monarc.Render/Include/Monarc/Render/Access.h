#pragma once

#include <Monarc/Core/Types.h>
#include <Monarc/RHI/Barrier.h>

namespace Monarc::Render {

/// How one pass touches one resource.
///
/// **This is the input to barrier derivation, and it exists so that derivation has something
/// to derive *from*.** ADR-0006's promise is that features do not write barriers; what they
/// write instead is one of these per resource per pass. ADR-0005's barrier is
/// sync-before/after, access-before/after and (for a texture) layout-before/after, so a
/// declaration that named only "reads this" would not carry enough to produce one. Each
/// enumerator below therefore maps to exactly the three things a barrier side needs -- a
/// pipeline stage, a memory access and, where the resource is a texture, a layout -- and
/// `RequirementOf` below is that map.
///
/// **The membership rule is `Monarc/RHI/Barrier.h`'s, and this set is where it applies.** That
/// rule: the model arrives whole where a value costs one row in one switch, and a value whose
/// backend behaviour nothing implements waits. Every enumerator here costs exactly one row in
/// `RequirementOf` and one in `ToString`, both `default`-less, so a new one is a compile error
/// rather than an access that silently derives nothing. And the *backend* behaviour each one
/// needs already exists: every stage, access and layout on the right-hand side of
/// `RequirementOf` is translated by `ToVulkan` in Monarc.RHI.Vulkan/Private/Translate.cpp
/// today, and `Monarc.RHI.Vulkan`'s own tests cover all three sets exhaustively.
///
/// **What that rule does *not* claim, and `TextureLayout`'s note in Barrier.h is where it is
/// counted.** Three of the layouts these accesses ask for -- `ShaderReadOnly`,
/// `DepthStencilAttachment` and `DepthStencilReadOnly` -- name a transition no texture
/// `IDevice::CreateTexture` can make may legally be put into, because `RHI::TextureUsage` has
/// two bits and neither is `Sampled` nor a depth-stencil attachment. So `SampledRead` and the
/// two depth accesses are derivable and inspectable now, and become *executable* when the
/// usage bits that permit them arrive with the first shader that binds one. That is the same
/// disclosure `TextureLayout` makes about itself, one level up.
///
/// The two storage accesses are a fourth case and a milder one: `General` is a layout a
/// texture Monarc creates today can legally be put into -- Barrier.h records it as measured
/// clean on both local adapters -- so what is missing for `StorageRead` and `StorageWrite` is
/// not the layout but `TextureUsage::Storage`, which no shader needs yet.
///
/// `IndirectRead` is the one enumerator here that no `TextureId` can ever carry -- a buffer
/// has no layout and an indirect read is a buffer read. It is present because it is one
/// switch row and because ADR-0005's model covers it; `PassBuilder::Read` refuses it on a
/// texture, and it becomes usable when `BufferId` arrives. See the note at the foot of
/// ResourceId.h for why that is a different rule from this one.
enum class ResourceAccess : u32 {
    /// A colour attachment whose existing contents the pass reads -- a `LoadOp::Load`
    /// attachment, or a blend.
    ColorAttachmentRead = 0,

    /// A colour attachment the pass writes. The only access `Monarc.FirstLight` will
    /// declare once Task 4 replaces its hand-written frame: one pass writing an imported
    /// swapchain image, cleared on load.
    ColorAttachmentWrite,

    /// Depth or stencil the pass tests but does not write.
    DepthStencilAttachmentRead,

    /// Depth or stencil the pass writes.
    ///
    /// **This is one access and a pass that both tests and writes depth declares two, whose
    /// layouts disagree -- and that is a stated gap rather than a resolved question.** A
    /// depth attachment being written is in `DepthStencilAttachment`; one only being tested
    /// is in `DepthStencilReadOnly`, which is what makes it simultaneously sampleable. A
    /// single pass declaring both accesses on one resource therefore asks for two layouts at
    /// once, which is not representable -- barriers are not permitted inside a rendering
    /// instance at all (see `ICommandList::Barrier`). Nothing in Phase A4 has a depth pass,
    /// because a depth pass needs a shader, so how the two combine is deliberately not
    /// guessed at here: the first depth pass is what decides it. **The phase plan does not
    /// raise this among its open questions, and it should**, which is why it is written down
    /// at the enumerator rather than left to be rediscovered -- a declaration that asks for
    /// two layouts at once is representable today and refused by nothing.
    DepthStencilAttachmentWrite,

    /// Sampled in a shader.
    SampledRead,

    /// Read as a storage image or buffer.
    StorageRead,

    /// Written as a storage image or buffer.
    StorageWrite,

    /// Read by the command processor as indirect draw or dispatch arguments. Buffers only --
    /// see the class comment.
    IndirectRead,
};

/// ADR-0005's two synchronisation-scope halves for one side of a barrier.
///
/// A stage *set* and an access *set*, because both are masks in the two APIs the model is
/// shaped from -- see `RHI::PipelineStage`. `SampledRead`'s stage is three stages for exactly
/// that reason: a graph knows a texture is sampled without knowing by which shader stage, and
/// a scope that had to name one could not express the truth.
struct AccessScope {
    RHI::PipelineStage stage  = RHI::PipelineStage::None;
    RHI::Access        access = RHI::Access::None;

    constexpr bool operator==(const AccessScope&) const = default;
};

/// A texture's complete state in ADR-0005's model: one side of a texture barrier.
///
/// **What an imported resource declares, and the reason the two barriers `Monarc.FirstLight`
/// hand-wrote in Phase A3 are derivable at all.** A swapchain image arrives from `Acquire` in
/// `TextureLayout::Undefined` and must be in `PresentSource` before `Present`, and neither
/// fact is inferable from any pass's declaration -- so an import states both, and the graph's
/// first and last transitions for that resource are the two ends. See
/// `TextureImport` in PassBuilder.h.
///
/// **The stage and access halves are not decoration on the layout, and A3's measured barriers
/// are why.** Its first barrier names `PipelineStage::ColorAttachmentOutput` on the *before*
/// side rather than `None`, because the queue submission waits on the acquire semaphore at
/// that stage and a layout transition is a write that must be ordered after that wait; its
/// second names `None` on the after side, because what reads the image next is the
/// presentation engine by way of a semaphore rather than a command. An import that carried
/// only layouts could express neither.
///
/// **A plain aggregate with defaults, unlike `RHI::TextureBarrier`, and the difference is
/// where the omission would be invisible.** `TextureBarrier` has no default constructor
/// because a barrier with defaulted layouts is byte-identical to one whose author meant
/// `Undefined`, and nothing at the call site shows which. Here the requirement sits one level
/// up: `TextureImport`'s constructor takes both states with no defaults, so an import cannot
/// omit them -- writing `TextureState{}` is possible but is a visible statement at the call
/// site rather than an absence.
struct TextureState {
    RHI::TextureLayout layout = RHI::TextureLayout::Undefined;
    RHI::PipelineStage stage  = RHI::PipelineStage::None;
    RHI::Access        access = RHI::Access::None;

    constexpr bool operator==(const TextureState&) const = default;
};

/// Everything derivation needs about one `ResourceAccess`, in one value.
///
/// One struct from one switch rather than four functions from four switches. That is what
/// makes this header's "an enumerator costs one row in one switch" claim literally true, and
/// it removes the failure mode four parallel switches have: three of them updated for a new
/// enumerator and the fourth left to a stale default.
struct AccessRequirement {
    /// The stage and access scope this access contributes to a barrier side.
    AccessScope scope = {};

    /// Whether this access can name a texture at all.
    ///
    /// **This is the discriminator for `layout`, and the layout value is not.** False only
    /// for `IndirectRead` today. `layout` is then filled with `TextureLayout::Undefined`
    /// because a struct member has to hold something, and `Undefined` is a legitimate layout
    /// that a real transition uses -- so reading `layout` to decide whether there is one
    /// would be exactly the conflation `RHI::TextureLayout`'s comment warns about. Read this
    /// field instead.
    bool namesTexture = false;

    /// The layout a texture must be in for this access. Meaningful only where
    /// `namesTexture`.
    RHI::TextureLayout layout = RHI::TextureLayout::Undefined;

    /// Whether this access writes the resource.
    ///
    /// What `PassBuilder`'s `Read`/`Write` split checks against, and what Task 2's lifetime
    /// computation means by "first write". A read-modify-write attachment is two accesses,
    /// one with this false and one with it true, not one access with a third state.
    bool writes = false;

    constexpr bool operator==(const AccessRequirement&) const = default;
};

/// What `access` requires, in ADR-0005's vocabulary.
///
/// `constexpr` and in the header on purpose: it makes the whole map constant-evaluable, so
/// Tests/TestAccess.cpp pins every row with `static_assert` rather than with a case that has
/// to be run. The switch is `default`-less, so `/w44062` on MSVC and `-Wswitch` on clang-cl
/// make a new `ResourceAccess` a build failure here first.
[[nodiscard]] constexpr AccessRequirement RequirementOf(ResourceAccess access) {
    using RHI::Access;
    using RHI::PipelineStage;
    using RHI::TextureLayout;

    switch (access) {
        case ResourceAccess::ColorAttachmentRead:
            return {{PipelineStage::ColorAttachmentOutput, Access::ColorAttachmentRead},
                    true,
                    TextureLayout::ColorAttachment,
                    false};
        case ResourceAccess::ColorAttachmentWrite:
            return {{PipelineStage::ColorAttachmentOutput, Access::ColorAttachmentWrite},
                    true,
                    TextureLayout::ColorAttachment,
                    true};
        case ResourceAccess::DepthStencilAttachmentRead:
            return {{PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
                     Access::DepthStencilAttachmentRead},
                    true,
                    TextureLayout::DepthStencilReadOnly,
                    false};
        case ResourceAccess::DepthStencilAttachmentWrite:
            return {{PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
                     Access::DepthStencilAttachmentWrite},
                    true,
                    TextureLayout::DepthStencilAttachment,
                    true};
        case ResourceAccess::SampledRead:
            return {{PipelineStage::VertexShader | PipelineStage::FragmentShader |
                         PipelineStage::ComputeShader,
                     Access::ShaderSampledRead},
                    true,
                    TextureLayout::ShaderReadOnly,
                    false};
        case ResourceAccess::StorageRead:
            return {{PipelineStage::VertexShader | PipelineStage::FragmentShader |
                         PipelineStage::ComputeShader,
                     Access::ShaderStorageRead},
                    true,
                    TextureLayout::General,
                    false};
        case ResourceAccess::StorageWrite:
            return {{PipelineStage::VertexShader | PipelineStage::FragmentShader |
                         PipelineStage::ComputeShader,
                     Access::ShaderStorageWrite},
                    true,
                    TextureLayout::General,
                    true};
        case ResourceAccess::IndirectRead:
            // No layout: a buffer does not have one. `namesTexture` is false and the layout
            // field is filler -- see `AccessRequirement::namesTexture`.
            return {{PipelineStage::DrawIndirect, Access::IndirectCommandRead},
                    false,
                    TextureLayout::Undefined,
                    false};
    }

    // Not dead and not optional. `ResourceAccess` has a fixed underlying type and can hold a
    // value outside its enumerator set -- which is exactly what TestAccess.cpp's completeness
    // assertions hand it -- and with every case covered MSVC still asks what those return
    // (`warning C4715`, fatal through `/WX`). The same shape `IsEnumerator` in
    // Monarc.RHI.Vulkan/Tests/TestVulkanBarrierTranslate.cpp uses, for the same reason.
    return {};
}

/// Whether `access` writes.
///
/// A projection of `RequirementOf` rather than a second switch, deliberately: a switch of its
/// own would be a second list to keep true, and the failure mode would be an access that
/// derived a correct barrier and computed a wrong lifetime.
[[nodiscard]] constexpr bool IsWrite(ResourceAccess access) {
    return RequirementOf(access).writes;
}

/// The enumerator's own spelling, for inspection text and test failures. Never nullptr; a
/// value outside the enumerator set gets a name of its own rather than any real access's --
/// see `ToString(Format)` in Monarc/RHI/Types.h for why conflating a named state with an
/// invalid one misdirects whoever reads the line.
[[nodiscard]] const char* ToString(ResourceAccess access);

}  // namespace Monarc::Render
