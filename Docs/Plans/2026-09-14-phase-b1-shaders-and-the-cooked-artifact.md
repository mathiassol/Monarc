# Phase B1 — Slang, reflection, and the cooked shader artifact

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One `.slang` file is compiled to SPIR-V by `Monarc.ShaderCompiler`, its reflection is extracted into a binding layout, and both are written to a versioned artifact that `Monarc.Shaders` reads back — asserted device-free against what the shader source itself declares. **No pipeline object, no draw call, no GPU.**

**Architecture:** Slang is Monarc's shading language ([ADR-0004](../Architecture/Decisions/ADR-0004-slang-shading-language.md)), split across the Tool/Runtime pair in [Shaders.md](../Rendering/Shaders.md). `Monarc.ShaderCompiler` (Tier 4, Tool) drives Slang at cook time; `Monarc.Shaders` (Tier 2, Runtime) loads the result. **The artifact is the contract between them**, and a shipped game contains only the runtime half.

**Tech Stack:** C++23, MSVC + Clang (`clang-cl`), CMake + Ninja, doctest, Slang pinned by version.

**Plan style:** headers and complete tests, not implementation bodies — as A2a onward.

---

## What makes this phase different from A4, and it is the opposite problem

A4 was the easy case and its plan said so: the render graph's whole value is a pure function over declarations, so 93.5% of its assertions run on a machine with no GPU. **B1 inverts that, for a reason that has nothing to do with GPUs.**

The crown jewel here is a *compiler invocation*, and **the compiler is not on the CI runners.** Slang 2026.13.1 is installed on the development machine only because it ships inside Vulkan SDK 1.4.357, and `.github/workflows/ci.yml` installs no Vulkan SDK — the Vulkan *headers* come from a pinned `FetchContent`, and the loader is whatever the runner image happens to carry. Nothing Slang-dependent would run in CI at all.

That is not a testing inconvenience. It is the difference between a phase whose most important code is covered and a phase that ships its most important code behind a skip, which is exactly what A3 did and A4 spent itself undoing.

### Decision: vendor Slang, pinned by version, the way the Vulkan headers are pinned

`shader-slang/slang` publishes prebuilt release archives — **verified against the GitHub release API, latest `v2026.17.1`**: `slang-<version>-windows-x86_64.zip` and `slang-<version>-linux-x86_64.zip`, plus macOS and aarch64 variants, 38 assets in all. So the same `FetchContent_Declare` shape `CMakeLists.txt:69` already uses for `VulkanHeaders` applies here.

Three things follow, and the third is the one worth noticing:

1. **The whole cook path becomes CI-testable.** Compile, reflect, write, read back — all of it, on a runner with no GPU and no SDK.
2. **The pinned version becomes part of the cook key**, which [ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md) requires anyway. A floating Slang would make the cache key a lie.
3. **It makes the deferred Linux port cheaper rather than more expensive.** The same pinned release has a `linux-x86_64` archive. Had B1 instead depended on a locally-installed SDK, the port would have inherited "get the Vulkan SDK onto the cloud box" as a prerequisite. This is the one place in Phase B where doing the work now actively reduces the port's cost, and it is worth stating because the general instinct runs the other way.

**Watch the download size.** The Windows archive is ~63 MB and the Linux one ~87 MB. Six presets × a cold fetch each would be minutes of CI time per run. Cache it, and make the cache key the pinned version.

---

## The oracle problem, which A4 did not have

A4 had an unusually sharp definition of correct: A3 left two RenderDoc captures with twelve measured barrier values, so "the graph must derive exactly these" was a *measured* reference rather than a plausible one.

**B1 has no such reference, and a test that checks `compile succeeded` asserts nothing.** So the oracle has to be constructed, and there are three honest sources for it:

- **The shader's own source.** The test `.slang` file declares a known set of parameters. Reflection must report **exactly** those — no more, no fewer, in the right stages, at the right bindings. Write the shader so its declarations *are* the assertion, and keep it small enough to read in one screen.
- **SPIR-V is self-describing.** A SPIR-V module carries its magic number, entry point names, execution models and binding decorations in a documented binary layout. A minimal reader can confirm those without a GPU, a driver, or Slang. That is a genuinely independent check on the compiler's output — the same relationship `CapturedFrame.h` has to the derivation, where an independent transcription is what stops an assertion becoming a tautology.
- **Deliberate failure.** A shader that does not compile, a parameter removed, a stage renamed — each must turn something red.

**Anything asserted from Slang's own reflection API checked against Slang's own output is a tautology.** At least one assertion per structural claim must come from outside Slang. Say which, per test.

---

## How Phase B is split, and why the seams are where they are

Phase B in [M0](../Milestones/M0-First-Light.md) is *"a cube renders from in-memory data via `RenderScene` and a Slang shader"*. That is far too much for one plan: **the RHI today has no pipeline, no shader, no sampler, no vertex buffer, no upload path and no draw call** — `Handles.h` declares exactly `BufferHandle` and `TextureHandle`. The synchronisation vocabulary anticipates all of it (`PipelineStage::VertexShader`, `Access::VertexAttributeRead`) but nothing creates or binds anything.

Plans are written one phase ahead ([README](../README.md)), so the sub-phases below are named to fix the seams and **only B1 is specified**. Each later plan is written when the one before it lands, because what it teaches will change the next.

| | Delivers | Device needed? |
|---|---|---|
| **B1** | Slang vendored; `Monarc.Shaders`' artifact format; `Monarc.ShaderCompiler` producing SPIR-V + reflection; the cook cache | **No** |
| **B2** | `Monarc.RHI`'s pipeline and draw vocabulary — pipeline objects, parameter-block binding, vertex/index buffers, an upload path, `Draw` — and the Vulkan backend for it | Descriptions no, execution yes |
| **B3** | `Monarc.Shaders` creating real pipelines from artifacts, cached by hash | Yes |
| **B4** | `Monarc.Render` records a draw: `PassCommandList` forwards draw calls, `ResourceAccess` gains transfer access, culling stops dropping read-only import passes. A triangle through the graph | Partly |
| **B5** | `RenderScene`, materials, views — the cube, from in-memory data | Yes |

**B4 is where A4's three known holes get closed**, and they are recorded in [Status.md](../Status.md#what-a4-does-not-prove) as carried gaps rather than as bugs. They are deliberately not B1's: a draw-forwarding API designed before there is a pipeline to bind would be invented rather than derived, which is the failure this project's "arrives with its first user" rule exists to prevent.

---

## File Structure

```
Source/Monarc.Shaders/            Tier 2, Runtime, deps: Core, RHI
  Include/Monarc/Shaders/
    ShaderArtifact.h            the cooked format: header, entry points, binary, layout
    BindingLayout.h             what reflection produced, in RHI's stage vocabulary
    ArtifactReader.h            read and validate; every refusal named
  Private/
    ArtifactReader.cpp
    BindingLayout.cpp
  Tests/
    TestArtifactFormat.cpp      round-trip, and every refusal the reader can make
    TestBindingLayout.cpp       the layout's own invariants, no Slang anywhere

Source/Monarc.ShaderCompiler/     Tier 4, Tool, deps: Core, Shaders
  Include/Monarc/ShaderCompiler/
    Compiler.h                  source -> artifact, with diagnostics
    CookKey.h                   ADR-0008 identity for a shader
  Private/
    SlangDriver.cpp             the only file that includes slang.h
    Reflect.cpp                 Slang reflection -> BindingLayout
    CookKey.cpp
  Tests/
    TestCompile.cpp             a shader that compiles, and several that must not
    TestReflection.cpp          the declared parameters, and nothing else
    TestCookKey.cpp             five inputs, five separate recook mutations
    TestSpirvIndependently.cpp  a minimal SPIR-V reader that does not link Slang
  Shaders/
    Triangle.slang              the test shader: small enough to read in one screen
```

**`SlangDriver.cpp` is the only file in the repository that may include `slang.h`.** That is the same containment `VulkanSurface.cpp` has for `<Windows.h>` and for the same reason — and it is worth a gate if one is cheap, because the whole point of the Tool/Runtime split is that `Monarc.Shaders` never sees the compiler.

---

## Task 1: Vendor Slang, and prove CI actually has it

- [ ] Pin a Slang release by **version and hash**, in the shape `FetchContent_Declare(VulkanHeaders)` at `CMakeLists.txt:69` already uses. Pin a specific version; `v2026.17.1` is current but check for newer at implementation time and record which you chose and why.
- [ ] **Decide how `Monarc.ShaderCompiler` reaches Slang: link, or open at runtime through `Platform::Library`?** The Vulkan loader is opened at runtime specifically because a *shipped game* must run on a machine with no SDK — see `Monarc.RHI.Vulkan`'s loader. **That argument does not transfer**: `Monarc.ShaderCompiler` is a `Tool` and never ships. My recommendation is to link, and to state the asymmetry where the decision is made so the next reader does not "fix" it into consistency with the Vulkan loader.
- [ ] A probe test reporting the Slang version actually loaded, in the shape `Monarc.RHI.Vulkan.Probe` uses. **Read how CI invokes that probe first** (`ci.yml:140`): CTest discards a passing test's output, so the probe runs a second time verbosely — a mechanism whose entire purpose is to report once reported nothing, and the fix is in the workflow, not the test.
- [ ] CI caches the download, keyed by the pinned version.

**Verification.** Deliberately: pin a version that does not exist and confirm configure fails loudly and names it, rather than silently continuing to a build that cannot find a header. Then corrupt the expected hash and confirm *that* fails too, and says which. **Both, separately** — they are different failures and one passing does not demonstrate the other.

---

## Task 2: `Monarc.Shaders` — the artifact format, and nothing that needs a device

- [ ] `Monarc.Shaders` (Runtime, tier 2, deps `Monarc.Core`, `Monarc.RHI`). Confirm `module-graph.json` records the edges and every gate still passes. The `RHI` dependency is real and not speculative: a binding layout names shader stages, and `RHI::PipelineStage` is where that vocabulary already lives.
- [ ] `ShaderArtifact.h`: the cooked format. Versioned and self-describing. It carries the target binary, the entry points with their stages, the binding layout, and the cook key inputs that produced it.
- [ ] `BindingLayout.h`: parameter blocks, their bindings, each binding's kind and stages. This is what [ADR-0004](../Architecture/Decisions/ADR-0004-slang-shading-language.md) means by *"binding layouts are generated from reflection, never written by hand"* — so the type must be **constructible only from reflection or from a reader**, and a hand-built one should be awkward enough that nobody writes one by accident.
- [ ] `ArtifactReader.h`: read and validate. **Every refusal is a named error, and each one is a test**: wrong magic, unknown version, truncated header, a declared binary size that overruns the buffer, an entry point naming a stage that does not exist, and a binding layout whose bindings collide.

**Device-free tests.**

- [ ] A round-trip: build an artifact, write it, read it back, and assert every field survived. **The write half must come from the compiler in Task 3** — a round-trip through a writer the same test wrote is a tautology, so until then assert the reader against a byte array committed as a fixture, with the bytes' provenance stated.
- [ ] Each refusal above, with the malformed input constructed by mutating one field of a valid artifact — so the test proves the reader rejects *that field* rather than rejecting a blob for unrelated reasons.
- [ ] Version handling: a future version number is refused rather than read optimistically, and the message says which version was found and which was expected.

**Nothing in this module may include `slang.h`**, and nothing in it may need a GPU. Both are properties to check, not to assume.

---

## Task 3: `Monarc.ShaderCompiler` — drive Slang to SPIR-V

- [ ] `Monarc.ShaderCompiler` (Tool, tier 4, deps `Core`, `Shaders`). `Tool` kind means the full C++23 standard library ([ADR-0003](../Architecture/Decisions/ADR-0003-cpp23-baseline.md)) — this module builds only on developer machines and on CI, never into a game.
- [ ] `Compiler.h`: source path in, artifact out, diagnostics either way. One vertex and one fragment entry point from one `.slang` file is the whole of B1's ambition.
- [ ] `Shaders/Triangle.slang`: the test shader. Small enough to read in one screen, with a deliberately **known** parameter set — it is the oracle for Task 4, so its declarations are load-bearing and it should say so in a comment.
- [ ] Diagnostics. A shader that does not compile reports Slang's diagnostic **with file and line**, and **writes no artifact**. A half-written artifact on a failed compile would be the worst possible failure mode, because the next cook would find it and consider the work done.

**Tests.**

- [ ] `Triangle.slang` compiles, and the result's SPIR-V magic number is correct.
- [ ] **A minimal SPIR-V reader that does not link Slang** confirms the entry point names and execution models. This is the independent oracle — see the oracle section — and `TestSpirvIndependently.cpp` exists so that the independence is visible in the file list rather than buried.
- [ ] At least three shaders that must **not** compile: a syntax error, an undeclared identifier, and a missing entry point. Each asserts the failure is reported, names a line, and left no artifact behind.
- [ ] Compiling twice in a row produces byte-identical output, or — if Slang embeds a timestamp or path that makes that false — **find out, say so, and assert the weaker property that is actually true**. Do not assert determinism you have not measured; the cook cache in Task 5 depends on knowing which it is.

---

## Task 4: Reflection to binding layout

This is the task the phase is for, and the one where a tautology is easiest to write.

- [ ] `Reflect.cpp`: Slang's reflection API to `BindingLayout`. Parameter blocks, bindings, kinds, stages.
- [ ] The layout is complete and exact: **every parameter the shader declares appears, and nothing else does.** A reflection that silently drops an unused binding, or invents one, must fail a test.

**Tests, and each must name where its oracle comes from.**

- [ ] `Triangle.slang`'s declared parameters, asserted **field by field against the source**, not against a string dump. The shader declares them; the test restates them independently; a change to either without the other turns it red.
- [ ] A shader with **two** parameter blocks, to prove set indices are not hardcoded to zero.
- [ ] A shader with **no** parameters at all — an empty layout is a legitimate value and must not be confused with a failure, which is the same distinction `ResourceLifetime::IsUnused()` had to make in A4.
- [ ] A parameter used only in the fragment stage reports fragment and **not** vertex. Stage masks are the field most likely to be filled in with a plausible default.
- [ ] Delete one parameter from a copy of the test shader and confirm exactly one binding disappears. **This is the mutation that proves the test reads reflection rather than reciting the fixture.**

---

## Task 5: The cook key, and a cache that does no work twice

[ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md) and [Shaders.md](../Rendering/Shaders.md) both specify the key: **source content, compile options, Slang version, target, engine version.**

- [ ] `CookKey.h`: the identity, and what goes into it.
- [ ] Recooking with nothing changed does no work.

**Tests.**

- [ ] **Five inputs, five separate mutations.** Change each key input in turn and confirm each one forces a recook, independently. Four passing and one untested is a cache that silently serves stale output for one input, which is the single worst bug this subsystem can have and is invisible until someone edits a shader and sees no change.
- [ ] The no-change case does no work, asserted by **observation rather than by timing** — a recorded did-work flag, or the artifact's mtime. A timing assertion is a flake with extra steps.
- [ ] Two different shaders do not collide.

---

## Task 6: Close-out

- [ ] Update [Shaders.md](../Rendering/Shaders.md) where B1 turned an intention into a fact, and state plainly what does not exist yet — pipeline creation, the pipeline cache, variants, hot reload. Keep its intent-describing voice; [Status.md](../Status.md) is where reality goes.
- [ ] Update [Status.md](../Status.md) with a "B1 delivered" section in the shape A3's and A4's use: what runs, what CI covers, and **what B1 does not prove** — nothing about a pipeline being creatable from the layout, nothing about the shader actually drawing, nothing about any target but SPIR-V.
- [ ] Update [M0](../Milestones/M0-First-Light.md). **Its Phase B row reads `ShaderCompiler`, `Shaders`, `Render`, which reads as "create `Monarc.Render`" — but A4 created that module already**, with the render-graph slice of what [Module-Graph.md](../Architecture/Module-Graph.md) always intended it to hold. Correct the row to say what B actually does, and add the B1–B5 split.
- [ ] Confirm a gate polices `slang.h` containment if one was added, and make it fail on purpose once.
- [ ] Read CI's output on the merge and record what fraction of B1 runs without a GPU. Vendoring Slang is the decision that makes that number high; if it is not high, the decision did not pay and that is a finding.

---

## Open questions, to answer where they become answerable

- **Does `Monarc.Shaders` need the `RHI` dependency in B1, or only from B3?** The binding layout names shader stages, and `RHI::PipelineStage` is the existing vocabulary — but if B1's layout turns out to need nothing else from RHI, a dependency justified by one enum is worth re-examining rather than inheriting from the module graph unread.
- **Is the artifact one file or two?** Binary and metadata together is simpler; separate lets the runtime memory-map the binary without parsing. Decide when there is a loader with a reason to care — which is B3, not now.
- **Where do cooked artifacts live on disk, and who owns that directory?** This is the first cooked output in the project, so whatever B1 chooses becomes the precedent [ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md) and `Monarc.Cook` inherit. Choose deliberately and say so, or explicitly defer it and keep B1's output in a test-scoped location.
- **Does the Vulkan SDK's Slang and the pinned Slang ever disagree?** The development machine has 2026.13.1 installed via the SDK and the pin will be newer. Nothing should reach the SDK's copy — but "should" is not "does", and a probe reporting the version actually loaded is what turns that into an observation.

---

## Definition of done

- One `.slang` file compiles to SPIR-V through a **pinned, vendored** Slang, on the development machine and on CI.
- Its reflection produces a binding layout asserted **field by field against the shader source**, with at least one structural claim confirmed by a reader that does not link Slang.
- The artifact round-trips, and every refusal the reader can make has a test that constructs exactly that malformation.
- Each of the five cook-key inputs is shown, separately, to force a recook.
- No file outside `SlangDriver.cpp` includes `slang.h`; `Monarc.Shaders` needs no GPU and no compiler.
- Six presets green, zero warnings, and the CI-covered fraction recorded rather than assumed.
