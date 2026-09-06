# Architecture Decision Records

One record per load-bearing decision. Each states what we chose, why, what it costs, and
what we rejected. When a decision changes, **amend the record and note the change** rather
than deleting it — the vision asks that we preserve an account of why direction moved.

| ADR | Decision | Status |
|---|---|---|
| [0001](ADR-0001-layered-modules.md) | Layered modules with build-enforced kinds | Accepted |
| [0002](ADR-0002-handles-not-pointers.md) | Handles, not pointers, for all engine identity | Accepted |
| [0003](ADR-0003-cpp23-baseline.md) | C++23 baseline, library restricted by module kind | Accepted (conditional) |
| [0004](ADR-0004-slang-shading-language.md) | Slang as the shading language | Accepted |
| [0005](ADR-0005-rhi-sync-model.md) | RHI barrier model from the D3D12/Vulkan convergence | Accepted |
| [0006](ADR-0006-render-graph.md) | The render graph owns all synchronisation | Accepted |
| [0007](ADR-0007-renderer-package-boundary.md) | The renderer may not depend on world or assets | Accepted |
| [0008](ADR-0008-asset-identity.md) | GUID identity, content-addressed cook cache | Accepted |
| [0009](ADR-0009-render-extraction.md) | An explicit render extraction boundary | Accepted |
| [0010](ADR-0010-reflection.md) | Reflection by explicit registration, codegen-compatible | Accepted |
| [0011](ADR-0011-build-system.md) | CMake backend with declarative module metadata | Accepted |
| [0012](ADR-0012-backend-rollout.md) | Backend order: Vulkan, then D3D12, then Metal | Accepted |
| [0013](ADR-0013-editor-ui.md) | Editor UI: ImGui scaffolding behind thin panels | Accepted |
| [0014](ADR-0014-dependency-policy.md) | Own the core, license the specialists | Accepted |
| [0015](ADR-0015-math-conventions.md) | Right-handed Y-up, column vectors, 0..1 depth | Accepted |
| [0016](ADR-0016-platform-code-selection.md) | Platform code selected by directory, not `#ifdef` | Accepted |
