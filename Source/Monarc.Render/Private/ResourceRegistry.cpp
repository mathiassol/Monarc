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

    // **Exact duplicates are refused, and the reason is Task 3's rather than tidiness.** With
    // this rule, the accesses one pass declares to one resource are a set, so the derivation
    // can walk consecutive accesses in execution order and emit one barrier per real
    // transition without first deduplicating a list that might hold the same access twice. A
    // duplicate is also nothing a caller can have meant.
    //
    // Two *different* accesses to one resource in one pass are legal and are not this case:
    // a read and a write is a read-modify-write attachment, which the plan states is legal.
    // Two accesses whose layouts disagree is a third thing, and is deliberately still
    // representable here -- see `ResourceAccess::DepthStencilAttachmentWrite`.
    for (const AccessInspection& existing : m_accesses) {
        if (existing.pass == pass && existing.resource == texture && existing.access == access) {
            return std::unexpected(Refuse(DiagnosticKind::DuplicateAccess,
                                          ErrorCode::AlreadyExists,
                                          "PassBuilder::Read/Write: this pass already declared "
                                          "that access to that resource",
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
