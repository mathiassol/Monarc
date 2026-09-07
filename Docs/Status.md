# Status

**What is actually true right now.** Intent lives in the other documents; this file is the
honest account. Update it when reality changes, not when a plan is written.

_Last updated: 2026-09-07_

## Summary

The architecture is designed and recorded, and **phases A1 through A2d are complete**:
the build mechanically enforces the module graph, `Monarc.Core` has its memory,
diagnostics, container, math and platform foundations under test on two compilers and two
sanitizers, and `Monarc.Jobs` — the first module beyond `Monarc.Core` — adds a thread
pool, dependency graph, priorities and instrumentation on top of it.

**A3 is under way: Tasks 1 and 2 are complete.** The module graph has six modules,
`Monarc.RHI.Vulkan` opens `vulkan-1.dll` itself and brings up a real Vulkan 1.3 instance with a
fatal validation messenger, and adapter enumeration reports the two GPUs on this machine where
raw enumeration reports five. Tasks 3 (device, command lists, colour readback) and 4 (window,
surface, swapchain) are next. See [M0 — First Light](Milestones/M0-First-Light.md) for how the
phases fit together.

Nothing renders yet — there is no device and no window. That is Tasks 3 and 4.

## Verified environment

Confirmed by direct testing on the development machine, not assumed:

| Component | Version | Notes |
|---|---|---|
| Compiler | MSVC 19.51 (toolset 14.51, VS 2026 Community) | Compiles C++23 language features cleanly at `/W4` — verified: deducing `this`, `static operator()`, multidimensional `operator[]`, `if consteval`, `auto(x)`, `[[assume]]`, `std::expected` |
| Second compiler | Clang 22.1.8 (`clang-cl`, standalone LLVM) | Builds the whole project and test suite warning-free at `/WX`, output identical to MSVC. Runs in CI on every push, which is what [ADR-0003](Architecture/Decisions/ADR-0003-cpp23-baseline.md)'s condition required |
| Build | CMake 4.2.1 + Ninja 1.13.2 (standalone, on `PATH`) | Presets pin no absolute tool paths, so one set serves this machine, CI, and macOS later |
| Vulkan | SDK 1.4.357.0, loader reports instance 1.4.357 | Validation layers, gfxreconstruct, SPIRV-Tools present. Device support verified with `vulkaninfo` — see [Hardware](#hardware). **The build does not use `find_package(Vulkan)` and does not need an SDK**: headers come from `FetchContent` at the pinned tag `vulkan-sdk-1.4.357.0`, and the runtime is opened at run time through `Platform::Library` — see [RHI.md](Rendering/RHI.md#how-the-vulkan-runtime-is-loaded) |
| Graphics debugger | RenderDoc 1.46 | Vendor-neutral, so it can capture on the Intel UHD 730 as well as the NVIDIA card. Nsight is also installed but is NVIDIA-only |
| Shaders | Slang 2026.13.1 (in the Vulkan SDK), DXC 1.9, glslang | |
| Windows SDK | 10.0.26100.0 | D3D12 headers present |
| Other | Python (see quirks — three interpreters), Node 22.15, .NET 9 + 10 | |

### Continuous integration

`.github/workflows/ci.yml` builds and tests six configurations on every push and pull
request — MSVC and Clang in Debug and Release, plus **`clang-asan`** and **`clang-ubsan`**
under Address and UndefinedBehavior sanitizers — and checks documentation links. `fail-fast` is disabled so that when
the compilers disagree, both results are visible. First run green, 2026-09-06.

The sanitizer legs exist because this codebase has produced three aliasing
use-after-free hazards in three containers — `Array<T>`, `String`, `HashMap` — each caught
by careful review rather than by a tool. **ASan** catches that class mechanically;
**UBSan** catches a different one — signed overflow, invalid casts, misaligned loads —
which matters for the math that landed in A2b. Both were verified to actually report
rather than merely to build: a deliberate use-after-free gives
`ERROR: AddressSanitizer: heap-use-after-free`, and a deliberate signed overflow gives
`runtime error: signed integer overflow`, both with line numbers and a non-zero exit.

UBSan needs `-fno-sanitize-recover=undefined`, without which it prints and continues and
the process still exits zero — a leg that reports nothing is not a gate.

**There is no ThreadSanitizer leg, and cannot be one here.** `clang-cl` rejects
`-fsanitize=thread` for the MSVC target and LLVM ships no TSan runtime for Windows. Data
races in `Monarc.Jobs` (A2d) will need a Linux CI leg or macOS, not a preset — worth
knowing before that phase rather than during it.

Sanitizer CRT choice is per-sanitizer and getting it wrong fails the link with an
uninformative `/failifmismatch: mismatch detected for 'RuntimeLibrary'`: ASan's runtime is
a DLL and needs the dynamic CRT, UBSan ships only a static standalone runtime and needs the
static one.

ASan on Windows needs the dynamic CRT and its runtime linked explicitly — CMake drives
`lld-link` directly, so `-fsanitize=address` never becomes a runtime library and the link
fails on undefined `__asan_*` symbols. `MonarcTargetOptions.cmake` queries
`clang-cl -print-resource-dir` rather than hardcoding a path containing the LLVM major
version, and copies the runtime DLL beside each test executable.

The runner's toolchain differs from this machine usefully: the same MSVC family
(19.51.36256 vs 19.51.36244 here) but **Clang 20.1.8 against 22.1.8 locally**, so the
pairing spans two Clang major versions. That is better coverage than the MSVC spread that
was expected and did not materialise.

### Hardware

- **NVIDIA RTX 3070 Ti** (Ampere) — `Advanced` tier. Reports Vulkan **1.4.351**, driver
  616.56, `DRIVER_ID_NVIDIA_PROPRIETARY`, discrete, six queue families (one graphics),
  `deviceUUID` `759c8156-7b91-7099-6511-5bd91de66f76`. Has `VK_EXT_mesh_shader`,
  `VK_KHR_ray_tracing_pipeline` and `VK_KHR_acceleration_structure`
- **Intel UHD 730** (Xe-LP, integrated) — `Bindless` tier, a genuinely useful *second vendor
  and lower tier* on the same machine, for keeping capability tiers honest rather than
  theoretical. Reports Vulkan **1.3.275**, driver 101.5334,
  `DRIVER_ID_INTEL_PROPRIETARY_WINDOWS`, two queue families (one graphics), `deviceUUID`
  `86808b4c-0400-0000-0002-000000000000`. No mesh shading and no ray tracing
- Intel i5-11400, 6 cores / 12 threads, 32 GB RAM — modest, so **compile-time discipline is
  a design constraint, not a virtue**

Two facts from this constrained `Monarc.RHI.Vulkan` before a line of it was written, and
both held up once it was:

- **Vulkan 1.3 is the ceiling, not 1.4.** The Intel part caps at 1.3.275, so anything
  requiring 1.4 silently drops the lower tier out of the test matrix — which would defeat
  the whole reason for having it. [M0](Milestones/M0-First-Light.md) already targets 1.3;
  this confirms it empirically rather than by intent.
- **`vkEnumeratePhysicalDevices` returns the Intel GPU four times here.** All four report an
  identical `deviceUUID` (`86808b4c-0400-…`) and `DRIVER_ID_INTEL_PROPRIETARY_WINDOWS`; the
  duplicates come from the virtual display adapters on this machine (Parsec, USB Mobile
  Monitor, LuminonCore IDDCX), each of which causes the Intel ICD to be registered again.
  So adapter enumeration must **dedupe on `VkPhysicalDeviceIDProperties::deviceUUID`**, or
  the adapter list a user sees will have four identical entries in it. This is exactly the
  class of bug that normally surfaces only on someone else's machine.
  **Closed in A3 Task 2** — `Monarc.FirstLight --adapters` now prints five raw entries and two
  deduplicated ones; the output is in [A3 Task 2 delivered](#a3-task-2-delivered).

Two further measurements from A3 Task 2, both of which changed the design rather than
confirming it:

- **Descriptor indexing does not distinguish the two devices.** Both report
  `shaderSampledImageArrayNonUniformIndexing`, `runtimeDescriptorArray` and
  `descriptorBindingPartiallyBound`, and both report
  `maxDescriptorSetUpdateAfterBindSampledImages = 1048576`. The only descriptor-related number
  that differs is `maxUpdateAfterBindDescriptorsInAllPools` (4294967295 against 1048576), which
  nothing asks about. So a bindless tier alone leaves the two devices indistinguishable, which
  is why the tier ladder gained an `Advanced` rung keyed on mesh shading and ray tracing — and
  why device *type* is not a tier input at all, despite the phase plan listing it as one.
- **Five implicit Vulkan layers are inserted into every instance this machine creates**: NVIDIA
  Optimus and Present, Overwolf's overlay and OBS hook, and Medal's capture hook. Three of them
  (`VK_LAYER_OW_OVERLAY`, `VK_LAYER_OW_OBS_HOOK`, `VK_LAYER_MEDAL_HOOK`) are built against
  Vulkan 1.2 and the loader warns about each on an instance created at 1.3; Medal registers
  itself twice and the loader drops the duplicate. Four warnings per process start, none of
  them Monarc's, and none of them fatal — which is precisely why the fatal messenger keys off
  VALIDATION-type messages rather than on ERROR severity alone.

### Known gaps

- **No macOS machine.** Metal is designed for but unimplemented and unproven. Expected
  within a year — see [ADR-0012](Architecture/Decisions/ADR-0012-backend-rollout.md).
- ~~**No graphics debugger.**~~ **Closed 2026-09-06.** RenderDoc 1.46 is installed, and
  being vendor-neutral it can capture on the Intel UHD 730 as well as the NVIDIA card.
- ~~**Windows long paths disabled.**~~ **Closed 2026-09-06.** `LongPathsEnabled` is 1, so
  deep generated paths under `Build/` will not truncate.
- **No compiler cache.** Neither `sccache` nor `ccache` is present. On six cores this is
  worth having before the module count grows.
- **No ThreadSanitizer.** Not a gap that can be closed on this platform — see
  [Continuous integration](#continuous-integration) above. `Monarc.Jobs` is verified by
  invariant-shaped tests and repetition instead, which is corroboration and not proof.

## Toolchain quirks worth remembering

- MSVC reports `__cplusplus == 199711` unless built with `/Zc:__cplusplus`. Set it globally.
- **MSVC 19.51 does not accept `/std:c++23` at all.** Passed explicitly it warns
  `D9002: ignoring unknown option` and silently drops to C++14. CMake's `CXX_STANDARD 23`
  therefore falls back to `/std:c++latest`, under which `__cplusplus` reports `202400L` —
  a value *beyond* C++23. `clang-cl` reports exactly `202302L`.

  The consequence is worth stating plainly: **a successful MSVC build is not evidence that
  code is C++23-conforming.** MSVC will happily accept post-C++23 features that Clang
  rejects. This is precisely the divergence
  [ADR-0003](Architecture/Decisions/ADR-0003-cpp23-baseline.md) makes the Clang build
  conditional on catching, and it means that build is load-bearing rather than belt-and-braces.
- `/MP` is inert under Ninja and is deliberately absent from the compiler flags. It
  parallelises multiple sources within a single `cl.exe` invocation, and Ninja invokes
  `cl.exe` once per file. Build parallelism comes from Ninja's scheduler.
- CMake 4.2 enables C++20 module dependency scanning by default at C++23. We do not use
  modules ([ADR-0003](Architecture/Decisions/ADR-0003-cpp23-baseline.md)), so
  `CXX_SCAN_FOR_MODULES OFF` is free build time.
- Git Bash puts MSYS2's `g++` ahead of MSVC on `PATH`. Builds must run from a VS developer
  environment (`vcvars64.bat`) or via CMake presets that pin the toolchain.

  The same `PATH` entry bites the Clang presets differently and more confusingly: MSYS2 also
  ships `ld.exe` and `ar.exe`, which CMake picks up as the linker and archiver during
  compiler detection, producing a link failure in the sanity check that looks nothing like a
  `PATH` problem. `vcvars64.bat` alone does not cause this — it appears when composing a
  `PATH` by hand from the Machine and User variables. **Filter `msys64` out when doing
  that**, or configure from a plain developer prompt.
- **`clang-cl` does not accept `/std:c++23`** — it silently ignores the flag (emitting only
  an "argument unused" warning) and falls back to C++17, which then fails with a wall of
  confusing errors. Use `/std:c++latest`, or let CMake's `CXX_STANDARD 23` pick the flag.
- `core.autocrlf` is `true` on this machine. `.gitattributes` overrides it with
  `text=auto eol=lf` so line-ending behaviour does not depend on developer config.
- **Three Python installations exist; `python`, `pip` and the build now agree on one.**
  Until 2026-09-06 they did not: `python` resolved to MSYS2's 3.14.7 (because
  `C:\msys64\mingw64\bin` preceded WindowsApps on `PATH`), `pip` resolved to the Microsoft
  Store 3.11 shim (because MSYS2 ships no `pip.exe`, so the name fell through), and CMake's
  `find_package(Python3)` picked CPython 3.12. `pip install X` followed by
  `python -c "import X"` therefore failed, and the architecture gates ran under a third
  interpreter again.

  Fixed by prepending `%LOCALAPPDATA%\Programs\Python\Python312` and its `Scripts` to the
  user `PATH` — chosen because CMake had already settled on that interpreter, so the shell
  now matches the build rather than the other way round. All of `python`, `pip`, `pip3` and
  `find_package(Python3)` resolve to **3.12.10**.

  Two residues worth knowing. `python3` still resolves to MSYS2's 3.14.7, because CPython
  on Windows installs no `python3.exe`; nothing in this project invokes it, but it is the
  same class of trap. And MSYS2's `bin` remains on `PATH`, which is separately why
  `g++` shadows MSVC in Git Bash.

  Independently of all that, the scripts in `Tools/` are written against the standard
  library alone and should stay that way: CI runs them under a different interpreter again,
  so depending on a package would mean depending on which Python happened to win.
- **A `FetchContent` dependency can enable a language the project never declared.**
  `Monarc`'s own `project()` says `LANGUAGES CXX`, but Vulkan-Headers says
  `project(VULKAN_HEADERS LANGUAGES C CXX)`, so adding it enabled C for the whole build. The
  Clang presets then failed *compiler detection* — `CMAKE_CXX_COMPILER` was pinned to
  `clang-cl` and `CMAKE_C_COMPILER` was not, so CMake picked plain `clang`, which rejects the
  MSVC-style flags CMake hands it: `clang: error: no such file or directory: '/DWIN32'`. The
  four Clang presets now pin `CMAKE_C_COMPILER` to `clang-cl` as well. Worth knowing because
  the failure is a wall of unrecognised-flag errors from a compiler nobody chose.
- Ninja 1.13.2 and LLVM's `bin` are on `PATH` as of 2026-09-06, which is what lets
  `CMakePresets.json` pin no absolute tool paths.
- Windows long paths are **not** enabled (`LongPathsEnabled=0`, `core.longpaths` unset).
  Deep module trees plus Ninja build directories approach the 260-character limit.

## Implementation progress

| Phase | Contents | State |
|---|---|---|
| A1 | Build system, module gates, Core memory + diagnostics | **Complete** |
| A2a | Hash, String, HashMap | **Complete** |
| A2b | Math — vectors, matrices, quaternions, transforms | **Complete** |
| A2c | Platform — files, paths, time, threads, dynamic libs, GUID | **Complete** |
| A2d | Monarc.Jobs — thread pool, dependency graph, priorities | **Complete** |
| A3 | RHI, Vulkan backend, Host.Windowed | **Tasks 1–2 complete**; 3–5 not started |
| A4 | Minimal render graph | Not started |
| B | ShaderCompiler, Shaders, Render | Not started |
| C | Reflect, Serialize, Assets, Cook | Not started |
| D | World, Engine | Not started |
| E | Editor | Not started |
| F | Hub, Build and export | Not started |

### A1 delivered

- `monarc_module()` enforcing kind and tier rules at configure time, and rejecting
  unrecognised arguments so a typo cannot silently drop a dependency edge
- `module-graph.json` emitted as a build artifact
- Four architecture gates running under CTest, each verified to fail when violated
- `Monarc.Core`: `Types`, `Assert` (replaceable handler), `Error`/`Result`/`Status`,
  `IAllocator`, `SystemAllocator`, `ArenaAllocator`, `Array<T>`, categorised logging
- Verified under MSVC Debug, MSVC RelWithDebInfo, and Clang Debug, warnings-as-errors
  — 43 doctest cases (126 assertions) plus the 4 architecture gates, 100% passing on a
  clean rebuild of all three presets

### A2a delivered

- `Hash.h`: 64-bit FNV-1a over a byte range, a splitmix64-style integer finaliser so
  sequential ids do not collapse into adjacent buckets, and a `Hasher<T>` customisation
  point with built-in specialisations for `std::string_view`, every integral type, `bool`,
  and pointers
- `String`: owning, null-terminated, growable string over an explicit allocator;
  `StringView` is an alias for `std::string_view` rather than a new type
- `HashMap<Key, Value>`: open-addressed, power-of-two capacity, linear probing, and
  backward-shift deletion, so no tombstones accumulate in a long-lived map
- Verified under MSVC Debug, MSVC Release, Clang Debug, and Clang Release, warnings-as-errors
  — 80 doctest cases (2373 assertions) plus the 4 architecture gates, 100% passing on a
  clean rebuild of all four presets

### A2b delivered

- `Scalar.h`: `kPi`/`kTwoPi`/`kHalfPi`/`kEpsilon`, `Radians`/`Degrees`, `Clamp`, `Lerp`,
  `ApproxEqual` — `constexpr` wherever `sqrt` and trig are not involved
- `Vec.h`: `Vec2`, `Vec3`, `Vec4` with the full arithmetic vocabulary, `Dot`, `Cross`,
  `Length`, `Normalize`; `Vec3` stays an unpadded 12 bytes for GPU vertex layout, `Vec4` is
  16-byte aligned for a future SIMD backend
- `Mat.h`: `Mat3`, `Mat4` (column-major, `columns[3]` the translation), `Perspective` and
  `Orthographic` both mapping depth to 0..1, `LookAt`, and an `Inverse` for a general affine
  matrix, not only a rigid one
- `Quat.h`: Hamilton quaternions matching `Mat4`'s composition order (`a * b` applies `b`
  first), and a `Slerp` that takes the shorter path and falls back to a normalized lerp when
  its inputs are nearly parallel rather than dividing by a near-zero `sin(theta)`
- `Transform.h`: translation/rotation/scale composition and inverse, the latter checked
  with `MONARC_CHECK` rather than silently wrong under non-uniform scale
- `Bounds.h`: `AABB` with `Expand`, `Contains`, `Intersects`, and a `Transformed` that
  re-fits around all eight transformed corners rather than just `min`/`max`, which is wrong
  under any rotation
- Every convention in [ADR-0015](Architecture/Decisions/ADR-0015-math-conventions.md) —
  handedness, storage order, composition order, depth range — is the record of what this
  phase decided, and is locked in by a test built to fail if any of it is ever quietly
  changed, not merely stated in a comment
- Verified under MSVC Debug, MSVC Release, Clang Debug, Clang Release, and Clang +
  AddressSanitizer, warnings-as-errors — 132 doctest cases (2517 assertions) plus the 4
  architecture gates, 100% passing on a clean rebuild of all five presets

### Known gaps in A1, carried into A2

A cross-cutting review at the end of A1 found no live defects, but four things worth
carrying forward rather than rediscovering:

- **`Array<T>`'s abort-on-allocation-failure contract is untested.** Its most important
  safety property is the one thing no test exercises, because triggering it kills the test
  process. Making it testable means routing through a replaceable fatal handler, the way
  `Assert.h` already does — worth doing when a second container needs the same treatment.
- **`MONARC_ASSERT` has no production call sites yet.** Everything real uses `MONARC_CHECK`.
  Not a misuse — A1 has no expensive invariant to check — but the distinction will quietly
  erode unless A2's hash tables and job graphs actually use it.
- **No small-string optimisation in `String`.** Deferred deliberately: it is an
  implementation detail behind an unchanged interface, and nothing serializes a `String`
  yet. Revisit when there is profiling data rather than intuition.

### A2c delivered

- Platform code selected by **directory**, not `#ifdef` — `monarc_module()` globs the current
  platform and excludes the others. Verified concretely: a `Private/Platform/Mac/` file
  containing `#error` is present, the build succeeds, and `Platform\Mac` appears nowhere in
  the generated Ninja file.
- `Time` (monotonic and wall clocks), `Path`, `File` (whole-file IO), `Thread`/`Mutex`/
  `ConditionVariable`, `Library`, and 128-bit `Guid` from the platform's cryptographic RNG
- **Zero platform `#ifdef`s anywhere in `Monarc.Core`**, which is
  [ADR-0016](Architecture/Decisions/ADR-0016-platform-code-selection.md)'s intended outcome:
  gate 10's exemption exists as a pressure valve and is currently unused
- 177 doctest cases, 3666 assertions, green on all six presets

**Gate 10 now does real work.** It had been passing vacuously since A1 for want of any
platform code. Verifying it during closeout found it was still too permissive: the exemption
covered the whole `Private/Platform/` tree, including `Path.cpp`, which ADR-0016 makes
platform-*neutral*. A Windows conditional could have sat unnoticed in a file explicitly
labelled neutral. The exemption now covers only per-platform subdirectories, and three
tests pin that scope.

### A2d delivered

- `JobHandle`: a generation-checked (index, generation) pair, invalid by default, so a
  recycled pool slot can never be mistaken for the job that previously occupied it
- `JobSystem`: a thread pool (`Config::workerCount` OS threads) over a fixed-capacity pool
  of job slots (`Config::maxJobs`, sized at construction) — `Submit`/`SubmitAfter` fail
  rather than allocate or block once the pool is full, and store each job's callable
  inline, never on the heap, with a `static_assert` turning an over-large capture into a
  compile error instead of a silent allocation
- Dependencies as a counted graph, not a traversal: each job holds a count of its own
  unfinished dependencies, and a dependency that is already finished or stale by the time
  `SubmitAfter` is called is satisfied immediately rather than waited on
- Three strict priorities (`JobPriority::High`/`Normal`/`Low`), deliberately without
  ageing, and completed-job instrumentation (`JobProfileRecord`/`CollectProfile`) behind a
  ring buffer that costs nothing — no allocation, no timestamp read — when
  `Config::profileCapacity` is left at its default of zero
- **One mutex guards the entire scheduler**, with no atomics and no per-queue or per-slot
  locking — see [Threading.md](Runtime/Threading.md) for why that is the right trade with
  no ThreadSanitizer available on this platform to verify anything cleverer
- `Wait` rejects being called from one of the pool's own worker threads via `MONARC_CHECK`
  **backed by an unconditional abort**. The check alone was not enough: verified under a
  non-breaking assert handler, it reported the violation and then fell through into the
  blocking wait loop, deadlocking the pool for real. Re-verified after the fix — the
  handler fires and the process stops at the guard (`0x80000003`), never reaching the wait
  loop; see [Threading.md](Runtime/Threading.md#wait-is-for-the-owning-thread-not-a-worker)
- 21 doctest cases, 8939 assertions, green on all six presets, plus the job test binary
  run 200+ consecutive times standalone with zero failures

**Gate 2 now sees a real edge.** The module graph has had exactly one node since A1, so
gate 2's acyclicity check had only ever run against scratch fixtures in the gate's own
tests, never a real second module. `module-graph.json` now records two modules —
`Monarc.Core` and `Monarc.Jobs` — and the `Monarc.Jobs → Monarc.Core` edge Task 1 of the
A2d plan introduced, and the gate continues to pass against a graph that could, for the
first time, actually contain a cycle.

### A3 Task 1 delivered

- `monarc_app()` in `CMake/MonarcModule.cmake`: executables that are nodes in the module graph
  rather than exceptions to it, with the leaf rule — nothing may depend on an app — enforced at
  configure time and again by gate 12 against the emitted `module-graph.json`
- `Monarc.RHI` (tier 2), `Monarc.RHI.Vulkan` (tier 2), `Monarc.Host.Windowed` (tier 3) and the
  `Monarc.FirstLight` app (tier 3), as honest stubs: `module-graph.json` records six modules and
  the first Tier 3 → Tier 2 edges in the project
- Gate 13 (module layout) renumbered from 7, which had collided with M0's own gate 7

### A3 Task 2 delivered

The loader, the instance, the debug messenger, and adapter enumeration — `Monarc.RHI.Vulkan`
is real from `vkCreateInstance` down. No device, no command buffers, no window: those are
Tasks 3 and 4.

- **The Vulkan runtime is opened by us**, `vulkan-1.dll` through `Platform::Library`, with every
  entry point resolved through `vkGetInstanceProcAddr` into X-macro-generated tables. Nothing
  links `vulkan-1.lib`, and `VK_NO_PROTOTYPES` makes a direct call a compile error rather than
  a discipline. Three tables — global, instance, and the `VK_EXT_debug_utils` pair kept apart
  because those two are the only entry points in the module whose absence is not an error. A
  device table would be the fourth and arrives with Task 3's `VkDevice`; `Loader.h` says so
  rather than shipping an empty one now
- **Vulkan headers come from `FetchContent` at `vulkan-sdk-1.4.357.0`**, not
  `find_package(Vulkan)`, so a machine with no SDK — which is every CI runner — still
  configures and builds. Recorded as a row in
  [ADR-0014](Architecture/Decisions/ADR-0014-dependency-policy.md) with the linking rule that
  goes with it
- **A Vulkan 1.3 instance** with `VK_KHR_surface` + `VK_KHR_win32_surface` required, and
  `VK_LAYER_KHRONOS_validation` + `VK_EXT_debug_utils` requested in Debug. A missing layer is a
  warning naming which half is absent and a degraded instance, not a failure
- **Validation errors stop the process**, and the guard was provoked rather than assumed — see
  below
- **`ErrorCode::BackendFailure`** in `Monarc.Core`, whose message is the `VkResult`'s own
  spelling; the operation that returned it and its numeric value are on the `MONARC_LOG` line
  beside it
- **Factory construction for both classes.** `Detail::Loader::Open` and
  `VulkanBackend::Create` return a `Result<T>`, the shape `Platform::Library::Open` and
  `Host::Window::Create` already use. Every failure message is a string literal, because
  `Error::message` is a non-owning view and a factory that returns no object has nowhere else
  to point; the composed detail — which library, which operation, which numeric `VkResult`,
  which version was reported — goes to the log at the failure site. `VulkanBackend` allocates
  its state inside `Create`, so a backend with no state cannot be handed to a caller.
  **This departs from the plan's wording** for the loader's device-free test, which asks for
  "`ErrorCode::NotFound` with a message naming it". The code and the test name the *missing
  entry point* — a literal — and not the library, whose name is caller-supplied. The same
  checkbox also asks for "the same shape as `Platform::Library`'s own tests", and
  `Platform::Library::Open` returns the bare literal `"could not load library"`; the two halves
  of that requirement pull in opposite directions, and this is the half that was kept
- **`AdapterInfo`, `DeduplicateAdapters` and a three-rung capability tier** in `Monarc.RHI`, all
  pure and all tested with no device — which is what makes CI cover them
- **`Monarc.FirstLight --adapters`**, which is also the `Monarc.RHI.Vulkan.Probe` CTest entry

**The headline measurement.** `--adapters` on this machine:

```
probe: loader open | instance Vulkan 1.4.357 | validation layer enabled | debug messenger installed
probe: raw enumeration -- 5 physical device(s)
  raw [0] NVIDIA GeForce RTX 3070 Ti   759c8156-7b91-7099-6511-5bd91de66f76  DiscreteGpu    1.4.351  Advanced
  raw [1] Intel(R) UHD Graphics 730    86808b4c-0400-0000-0002-000000000000  IntegratedGpu  1.3.275  Bindless
  raw [2] Intel(R) UHD Graphics 730    86808b4c-0400-0000-0002-000000000000  IntegratedGpu  1.3.275  Bindless
  raw [3] Intel(R) UHD Graphics 730    86808b4c-0400-0000-0002-000000000000  IntegratedGpu  1.3.275  Bindless
  raw [4] Intel(R) UHD Graphics 730    86808b4c-0400-0000-0002-000000000000  IntegratedGpu  1.3.275  Bindless
probe: after deduplication on deviceUUID -- 2 adapter(s)
  adapter [0] NVIDIA GeForce RTX 3070 Ti   759c8156-7b91-7099-6511-5bd91de66f76  DiscreteGpu    1.4.351  Advanced
  adapter [1] Intel(R) UHD Graphics 730    86808b4c-0400-0000-0002-000000000000  IntegratedGpu  1.3.275  Bindless
probe: 5 raw entries collapsed to 2
```

**Three test outcomes, because a suite that silently runs nothing while showing green is the
failure mode this phase is shaped to avoid.** `ctest` now reports nine entries under four
labels: `unit` (five, device-free, run everywhere), `gpu` (one, `SKIP_RETURN_CODE 77`),
`probe` (one, always runs and always passes) and `architecture` (two).

The skip machinery was demonstrated in both directions rather than trusted. Pointing the
registered `gpu` command at a library that cannot exist:

```
7/9 Test #7: Monarc.RHI.Vulkan.DeviceTests ....***Skipped   0.02 sec
...
The following tests did not run:
	  7 - Monarc.RHI.Vulkan.DeviceTests (Skipped)
```

and then changing the binary's skip code from 77 to 0 and running the same no-device path:

```
1/1 Test #7: Monarc.RHI.Vulkan.DeviceTests ....   Passed    0.02 sec
```

— the fake-green outcome, on demand. `SKIP_RETURN_CODE` is what separates them, observed
rather than assumed.

**The fatal validation messenger was provoked.** With a deliberately wrong
`VkApplicationInfo::sType`, the validation layer's `VUID-VkApplicationInfo-sType-sType` error
reached the callback during `vkCreateInstance` and the process stopped at `0x80000003` — the
same signature `JobSystem::Wait`'s worker guard produces. Verified twice over:

- Under an assert handler that **reports and declines to break** — a test harness, or
  Shipping — the handler ran, printed, returned false, and the process *still* stopped at
  `0x80000003` from the unconditional break, never re-entering Vulkan. This is the case
  `MONARC_CHECK` alone would have got wrong, and it is why the house pattern has the extra two
  lines.
- **Removing the messenger from `VkInstanceCreateInfo::pNext`**, with the same invalid `sType`,
  changed the outcome completely: the layer printed the same error to stderr on its own,
  `vkCreateInstance` **succeeded**, enumeration ran, and the process **exited zero**. So the
  chained create-info is load-bearing rather than good practice — without it, an
  instance-creation validation error is exactly the output that scrolls past.

**Every new guard was violated on purpose and watched fail.** Twelve produced test failures:
adjacent-only deduplication (caught by the A-B-A-B-A case and *not* by the four-identical-Intel
case); the in-place compaction deleted (caught *only* by the A-A-B case, which is the one whose
survivors do not start at their final indices — the other cases' do, so all of them pass with
the assignment gone while a real GPU would be dropped in favour of a duplicate); UUID
comparison stopping at the first zero byte (caught at byte 6 of the real Intel UUID); two tiers
given distinct but descending values; one bindless requirement dropped; a failed
`VulkanBackend::Create` that did not deallocate its state (`CHECK( 152 == 0 )`); both formats
mapped to one `VkFormat`; the version decoded with the pre-Vulkan-SC open-coded shifts (caught
*only* by the variant-bits case, since for variant 0 the old layout and the macros agree);
`DeduplicateAdapters`' returned count ignored; a move-assignment that adopted without
releasing; one accessor's null check removed (a SIGSEGV in the moved-from case); and the
validation-default generator expression's polarity flipped — which turns exactly the four
non-Debug legs red and leaves both Debug legs green, because in Debug the correct answer and a
hardcoded `true` are the same answer.

**One expected mechanism turned out not to exist.** `Error::message` is a non-owning view, and
the factory shape depends on every message being a string literal, so a message re-pointed at
a buffer inside `VulkanBackend::State` was read after that state had been destroyed and
deallocated. **`clang-asan` reported nothing at all** — the freed storage still held its bytes
and the read went unnoticed. What failed was the test's exact-text comparison. So those two
cases compare the whole message rather than searching it for a substring; a `find()` or a
non-empty check would let the dangling case through on all six presets.

Three produced **compile errors rather than test failures**, and the comments now say that
rather than claiming a test covers them: two tiers given the same value — `error C2196: case
value 'Monarc::RHI::CapabilityTier::Bindless' already used`, from the `default`-less switch in
`ToString`; a direct call to a global entry point under `VK_NO_PROTOTYPES`, which is
`error C3861: 'vkCreateInstance': identifier not found`, and is what makes the loader decision
a property of the build rather than a discipline; and `VulkanBackend backend;`, which is
`error C2512: 'Monarc::RHI::VulkanBackend': no appropriate default constructor available` — the
factory is the only way to come by one.

**One header comment was wrong about the code it sat on, and measurement is what caught it.**
`Loader`'s move is defaulted, and the class comment was written to say that a moved-from Loader
is a closed one with every table null. It is not: `Platform::Library`'s move nulls the source's
module handle, but the three entry-point tables are trivially copyable, so the defaulted move
*copies* them and the source keeps the pointers. Observed — `IsOpen()` false while
`Global().vkCreateInstance` is still non-null — and the comment now says that, with an
assertion in the device suite pinning it. `IsOpen()` is the query that separates the two
states; nothing in Monarc reads a table off a Loader it has moved from, so the move is left
defaulted rather than hand-written to clear them.

- Green on all six presets: 9 CTest entries each, including the device tests actually running
  against this machine's two adapters
- 57 device-free doctest cases and 304 assertions across `Monarc.RHI.Tests` (34 cases, 217
  assertions) and `Monarc.RHI.Vulkan.Tests` (23 / 87), plus 12 device-required cases and 57
  assertions — that last number scales with how many adapters a machine has
- **Where those numbers moved, and why.** `VulkanBackend` is a factory, so a backend exists
  only if it came up: move construction, move assignment, `Shutdown` and the accessors'
  behaviour on a moved-from backend cannot be reached without a Vulkan implementation, and
  those four cases are in `TestsDevice/` rather than `Tests/`. That is a real cost — the
  moved-from accessor guard is one of the twelve above, and it no longer runs where there is no
  device. It bought a state that cannot exist: a backend whose allocation failed used to be
  reachable as `Initialize` returning `OutOfMemory`, and now `Create` returns that error with
  no object attached at all

**What CI will report is not yet known and is deliberately not claimed here.** The probe entry
exists so that the runners' answer becomes an observed fact; reading it and writing it down is
A3 Task 5's checkbox.

## Verification gates

Four of M0's eleven gates are implemented and running under CTest as
`Architecture.Gates`: acyclicity (2), renderer package boundary (3), module layout (7), and
platform containment (10). Gate 3 currently passes vacuously — no tier 2 module exists yet —
which is the intended state: it will go red the first time the boundary is crossed.

The remaining gates need modules that do not exist yet and are added against this same
harness: export purity (1), cook determinism (4), cook incrementality (5), asset identity
across rename (6), world-kind parity, export explainability (8), headless purity (9), and
schema migration (11).
