#include <Monarc/Render/RenderGraph.h>

// The declared resources and their descriptions: what a `TextureId` names, and every refusal
// that can happen while naming one.
//
// **Every id this file hands out is `{index, m_buildGeneration}`, and both halves are
// load-bearing.** The index is dense and starts again from zero on every build, which is what
// makes resolution a bounds check rather than a search; the generation is what stops an id
// from a previous build resolving to whatever declaration now occupies its index. See
// `ResourceId`'s class comment for why that recycling is ADR-0002's problem in a different
// costume.

namespace Monarc::Render {

Result<TextureId> RenderGraph::DeclareTransient(u32 pass, u32 generation, std::string_view name,
                                                const RHI::TextureDescription& description) {
    if (!IsCurrentPass(pass, generation)) {
        return std::unexpected(Refuse(DiagnosticKind::UnknownPass, ErrorCode::NotFound,
                                      "PassBuilder::CreateTexture: this builder names no pass in "
                                      "the current build",
                                      pass, TextureId{}));
    }
    if (m_resources.Size() >= m_config.maxResources) {
        return std::unexpected(Refuse(DiagnosticKind::ResourcePoolExhausted,
                                      ErrorCode::OutOfMemory,
                                      "PassBuilder::CreateTexture: resource pool exhausted", pass,
                                      TextureId{}));
    }

    const TextureId    id{static_cast<u32>(m_resources.Size()), m_buildGeneration};
    ResourceInspection resource{};
    resource.name        = name;
    resource.id          = id;
    resource.origin      = ResourceOrigin::Transient;
    resource.description = description;
    // No physical texture and no import states: a transient has neither. `importedTexture`
    // stays invalid and both states stay at their defaults, and `origin` is what says so --
    // see `ResourceInspection`.
    m_resources.Push(resource);
    return id;
}

Result<TextureId> RenderGraph::DeclareImport(u32 pass, u32 generation, std::string_view name,
                                             const TextureImport& import) {
    if (!IsCurrentPass(pass, generation)) {
        return std::unexpected(Refuse(DiagnosticKind::UnknownPass, ErrorCode::NotFound,
                                      "PassBuilder::ImportTexture: this builder names no pass in "
                                      "the current build",
                                      pass, TextureId{}));
    }
    if (!import.Texture().IsValid()) {
        return std::unexpected(Refuse(DiagnosticKind::InvalidImport, ErrorCode::InvalidArgument,
                                      "PassBuilder::ImportTexture: the imported texture handle "
                                      "is invalid",
                                      pass, TextureId{}));
    }

    // One physical resource, one identity. Two imports of the same handle would give the
    // derivation two independent chains of accesses over the same image, and it would produce
    // two independent sets of barriers for it -- each correct in isolation and wrong together.
    // Linear because imports are few; a build with enough of them for this to matter has other
    // problems.
    for (const ResourceInspection& existing : m_resources) {
        if (existing.origin == ResourceOrigin::Imported &&
            existing.importedTexture == import.Texture()) {
            return std::unexpected(Refuse(DiagnosticKind::DuplicateImport,
                                          ErrorCode::AlreadyExists,
                                          "PassBuilder::ImportTexture: this build already "
                                          "imported that texture",
                                          pass, existing.id));
        }
    }

    // Checked after the duplicate scan, the same way `DeclareAccess` orders its two: a full
    // pool and a duplicate at once is better reported as the duplicate, because the duplicate
    // is the mistake and the full pool is a consequence of it. Pinned by *"a duplicate import
    // is reported as the duplicate even with the resource pool full"* in
    // Tests/TestPassDeclaration.cpp -- an ordering nothing asserts is an ordering that reverses
    // on the next edit.
    if (m_resources.Size() >= m_config.maxResources) {
        return std::unexpected(Refuse(DiagnosticKind::ResourcePoolExhausted,
                                      ErrorCode::OutOfMemory,
                                      "PassBuilder::ImportTexture: resource pool exhausted", pass,
                                      TextureId{}));
    }

    const TextureId    id{static_cast<u32>(m_resources.Size()), m_buildGeneration};
    ResourceInspection resource{};
    resource.name            = name;
    resource.id              = id;
    resource.origin          = ResourceOrigin::Imported;
    resource.description     = import.Description();
    resource.importedTexture = import.Texture();
    resource.incoming        = import.Incoming();
    resource.outgoing        = import.Outgoing();
    m_resources.Push(resource);
    return id;
}

Status RenderGraph::DeclareAccess(u32 pass, u32 generation, TextureId texture,
                                  ResourceAccess access, bool writing) {
    if (!IsCurrentPass(pass, generation)) {
        return std::unexpected(Refuse(DiagnosticKind::UnknownPass, ErrorCode::NotFound,
                                      "PassBuilder::Read/Write: this builder names no pass in "
                                      "the current build",
                                      pass, TextureId{}));
    }

    const AccessRequirement requirement = RequirementOf(access);

    // A buffer-only access -- `IndirectRead` -- has no layout, so it cannot name a texture.
    // This is also the answer for a value cast into the enum from outside its set:
    // `RequirementOf` returns a zeroed requirement for one, whose `namesTexture` is false.
    if (!requirement.namesTexture) {
        return std::unexpected(Refuse(DiagnosticKind::AccessNamesNoTexture,
                                      ErrorCode::InvalidArgument,
                                      "PassBuilder::Read/Write: that access cannot name a "
                                      "texture",
                                      pass, texture));
    }
    if (requirement.writes != writing) {
        return std::unexpected(Refuse(DiagnosticKind::AccessDirectionMismatch,
                                      ErrorCode::InvalidArgument,
                                      "PassBuilder::Read/Write: the access disagrees with the "
                                      "direction it was declared through",
                                      pass, texture));
    }

    if (FindResource(texture) == m_resources.Size()) {
        return std::unexpected(Refuse(DiagnosticKind::UnknownResource, ErrorCode::NotFound,
                                      "PassBuilder::Read/Write: that id names no resource in the "
                                      "current build",
                                      pass, texture));
    }

    // **One sweep, two refusals, and they are the two ways a second access to one resource from
    // one pass can be wrong.**
    //
    // **Exact duplicates, and the reason is the derivation's rather than tidiness.** With this
    // rule the accesses one pass declares to one resource are a set, so the derivation can
    // combine them into one required state without first deduplicating a list that might hold
    // the same access twice. A duplicate is also nothing a caller can have meant.
    //
    // **Disagreeing layouts, which is the open question Task 1 raised and Task 3 answered.** A
    // pass's accesses to one resource are combined into a single state -- one layout, the union
    // of the stages, the union of the accesses -- because a texture is in exactly one layout at
    // a time and a barrier cannot be recorded inside a rendering instance. Two accesses asking
    // for two layouts have no such combination, so the derivation would have to pick one
    // arbitrarily and emit a barrier into a layout the other access cannot use. That is refused
    // here instead: `RequirementOf` is `constexpr` and the conflict is visible in the
    // declarations alone, so no compilation state is needed to see it. See
    // `DiagnosticKind::AccessLayoutConflict`, which carries the argument at length.
    //
    // A read and a write whose layouts *agree* are still legal and are neither of these: that is
    // the read-modify-write attachment the plan names, and both `ColorAttachmentRead` and
    // `ColorAttachmentWrite` ask for `TextureLayout::ColorAttachment`.
    //
    // **Checked in the order the mistakes are distinct in, and they cannot both fire for one
    // pair**: an exact duplicate has the same access and therefore the same layout, so a pair
    // that is a duplicate is never a conflict and a pair that is a conflict is never a
    // duplicate.
    for (const AccessInspection& existing : m_accesses) {
        if (existing.pass != pass || existing.resource != texture) {
            continue;
        }
        if (existing.access == access) {
            return std::unexpected(Refuse(DiagnosticKind::DuplicateAccess,
                                          ErrorCode::AlreadyExists,
                                          "PassBuilder::Read/Write: this pass already declared "
                                          "that access to that resource",
                                          pass, texture));
        }
        if (RequirementOf(existing.access).layout != requirement.layout) {
            return std::unexpected(Refuse(DiagnosticKind::AccessLayoutConflict,
                                          ErrorCode::InvalidArgument,
                                          "PassBuilder::Read/Write: this pass already declared "
                                          "an access to that resource needing a different "
                                          "layout",
                                          pass, texture));
        }
    }

    // Checked after the duplicate scan on purpose: a full pool and a duplicate at once is
    // better reported as the duplicate, which is the mistake. Pinned by *"a duplicate access is
    // reported as the duplicate even with the access pool full"* in
    // Tests/TestPassDeclaration.cpp, which is the only shape of case that can tell the two
    // orderings apart -- both conditions in force at once.
    if (m_accesses.Size() >= m_config.maxAccesses) {
        return std::unexpected(Refuse(DiagnosticKind::AccessPoolExhausted, ErrorCode::OutOfMemory,
                                      "PassBuilder::Read/Write: access pool exhausted", pass,
                                      texture));
    }

    AccessInspection declared{};
    declared.pass     = pass;
    declared.resource = texture;
    declared.access   = access;
    m_accesses.Push(declared);
    return {};
}

Status RenderGraph::DeclareColorAttachment(u32 pass, u32 generation, TextureId texture,
                                           RHI::LoadOp loadOp, RHI::StoreOp storeOp,
                                           const RHI::ClearColor& clearValue) {
    if (!IsCurrentPass(pass, generation)) {
        return std::unexpected(Refuse(DiagnosticKind::UnknownPass, ErrorCode::NotFound,
                                      "PassBuilder::ColorAttachment: this builder names no pass "
                                      "in the current build",
                                      pass, TextureId{}));
    }

    const usize resource = FindResource(texture);
    if (resource == m_resources.Size()) {
        return std::unexpected(Refuse(DiagnosticKind::UnknownResource, ErrorCode::NotFound,
                                      "PassBuilder::ColorAttachment: that id names no resource "
                                      "in the current build",
                                      pass, texture));
    }

    // **The description is what makes both of the next two checkable here rather than at record
    // time**, and the graph has one for every resource -- supplied by the creating pass for a
    // transient and by the importing pass for an import. `ICommandList::BeginRendering` refuses
    // both conditions too, and arriving there means finding out with a frame already half
    // recorded; see `DiagnosticKind::TooManyAttachments` for the argument in full.
    const RHI::TextureDescription& description = m_resources[resource].description;
    if (!RHI::HasAny(description.usage, RHI::TextureUsage::ColorAttachment)) {
        return std::unexpected(Refuse(DiagnosticKind::AttachmentNotRenderable,
                                      ErrorCode::InvalidArgument,
                                      "PassBuilder::ColorAttachment: that resource was not "
                                      "described with TextureUsage::ColorAttachment",
                                      pass, texture));
    }
    if (description.extent.width == 0 || description.extent.height == 0) {
        return std::unexpected(Refuse(DiagnosticKind::AttachmentExtentEmpty,
                                      ErrorCode::InvalidArgument,
                                      "PassBuilder::ColorAttachment: that resource's extent has "
                                      "a zero dimension",
                                      pass, texture));
    }

    // **One sweep over this pass's attachments, and it answers all three of the remaining
    // questions**: whether this resource is already one, how many slots are taken, and what
    // extent the pass's first attachment fixed. Linear over a list bounded by
    // `RHI::kMaxColorAttachments` per pass, which is the same shape `DeclareAccess`' sweep has.
    u32       slot        = 0;
    TextureId firstOfPass = {};
    for (const AttachmentInspection& existing : m_attachments) {
        if (existing.pass != pass) {
            continue;
        }
        if (existing.resource == texture) {
            return std::unexpected(Refuse(DiagnosticKind::DuplicateAttachment,
                                          ErrorCode::InvalidArgument,
                                          "PassBuilder::ColorAttachment: this pass already "
                                          "declared that resource as a colour attachment",
                                          pass, texture));
        }
        if (slot == 0) {
            firstOfPass = existing.resource;
        }
        ++slot;
    }

    if (slot >= static_cast<u32>(RHI::kMaxColorAttachments)) {
        return std::unexpected(Refuse(DiagnosticKind::TooManyAttachments,
                                      ErrorCode::InvalidArgument,
                                      "PassBuilder::ColorAttachment: this pass already has "
                                      "RHI::kMaxColorAttachments colour attachments",
                                      pass, texture));
    }

    // **The extent is read from the resources and never from the caller, which is why a
    // disagreement is a refusal and not an arbitration.** `RHI::RenderingDescription` has
    // exactly one extent, so a pass whose attachments disagree has no rendering description at
    // all, and there is no third value for the graph to prefer.
    if (firstOfPass.IsValid() &&
        m_resources[firstOfPass.index].description.extent != description.extent) {
        return std::unexpected(Refuse(DiagnosticKind::AttachmentExtentConflict,
                                      ErrorCode::InvalidArgument,
                                      "PassBuilder::ColorAttachment: that resource's extent "
                                      "disagrees with this pass's first colour attachment",
                                      pass, texture));
    }

    // Checked after the sweep, which is the order `DeclareAccess` and `DeclareImport` both use
    // and for their reason: a full pool and a duplicate at once is better reported as the
    // duplicate, because the duplicate is the mistake.
    if (m_attachments.Size() >= m_config.maxAttachments) {
        return std::unexpected(Refuse(DiagnosticKind::AttachmentPoolExhausted,
                                      ErrorCode::OutOfMemory,
                                      "PassBuilder::ColorAttachment: attachment pool exhausted",
                                      pass, texture));
    }

    // **The accesses the load-op implies, declared before the attachment is recorded.** A
    // `default`-less switch, so a new `RHI::LoadOp` is a compile error here rather than an
    // attachment whose access set was guessed -- the rule `RequirementOf` in Access.h follows
    // for its own map.
    //
    // The read comes first for `LoadOp::Load`, because that is the order a read-modify-write
    // attachment happens in and the order `GraphInspection::accesses` will show. Neither the
    // derivation nor the ordering depends on it: a pass's accesses to one resource are combined
    // into one state.
    //
    // `StoreOp` contributes nothing. `StoreOp::DontCare` discards the result rather than
    // declining to produce it, so it writes exactly as `StoreOp::Store` does.
    bool loads = false;
    switch (loadOp) {
        case RHI::LoadOp::Load:     loads = true; break;
        case RHI::LoadOp::Clear:    loads = false; break;
        case RHI::LoadOp::DontCare: loads = false; break;
    }

    if (loads) {
        if (Status read = DeclareAccess(pass, generation, texture,
                                        ResourceAccess::ColorAttachmentRead, false);
            !read) {
            return read;
        }
    }
    if (Status written =
            DeclareAccess(pass, generation, texture, ResourceAccess::ColorAttachmentWrite, true);
        !written) {
        return written;
    }

    AttachmentInspection declared{};
    declared.pass       = pass;
    declared.resource   = texture;
    declared.slot       = slot;
    declared.loadOp     = loadOp;
    declared.storeOp    = storeOp;
    declared.clearValue = clearValue;
    m_attachments.Push(declared);
    return {};
}

usize RenderGraph::FindResource(TextureId texture) const {
    // `m_resources.Size()` is the not-found answer, which is also why an index equal to it is
    // out of range: the two coincide rather than one being a special case of the other.
    if (!texture.IsValid() || texture.generation != m_buildGeneration) {
        return m_resources.Size();
    }
    if (texture.index >= m_resources.Size()) {
        return m_resources.Size();
    }
    return texture.index;
}

}  // namespace Monarc::Render
