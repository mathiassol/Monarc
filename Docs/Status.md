# Status

**What is actually true right now.** Intent lives in the other documents; this file is the
honest account. Update it when reality changes, not when a plan is written.

_Last updated: 2026-09-08_

## Summary

The architecture is designed and recorded, and **phases A1 through A2d are complete**:
the build mechanically enforces the module graph, `Monarc.Core` has its memory,
diagnostics, container, math and platform foundations under test on two compilers and two
sanitizers, and `Monarc.Jobs` — the first module beyond `Monarc.Core` — adds a thread
pool, dependency graph, priorities and instrumentation on top of it.

**A3 is under way: Tasks 1 through 4 are complete.** The module graph has six modules,
`Monarc.RHI.Vulkan` opens `vulkan-1.dll` itself and brings up a real Vulkan 1.3 instance with a
fatal validation messenger, adapter enumeration reports the two GPUs on this machine where raw
enumeration reports five, and there is a logical device on each of them. **`Monarc.FirstLight`
now opens a window, clears it, and exits zero when it is closed.** Task 5 is close-out. See
[M0 — First Light](Milestones/M0-First-Light.md) for how the phases fit together.

**First light is lit, and it is proved rather than looked at — twice.** A `R8G8B8A8_UNORM`
texture cleared through dynamic rendering reads back as `(64, 128, 192, 255)` on both the RTX
3070 Ti and the Intel UHD 730 (Task 3), and the *swapchain image that gets presented* reads back
as `(192, 128, 64, 255)` — the same colour, in the swapchain's own `B8G8R8A8_UNORM` byte order —
on both adapters, with the window's own pixels read off the screen agreeing to the byte. No
tolerance anywhere. See [A3 Task 3 delivered](#a3-task-3-delivered) and
[A3 Task 4 delivered](#a3-task-4-delivered).

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
- **Two monitors, both 1920x1080 and both at 96 DPI**: `\\.\DISPLAY5` at `(-1920, 0)` and
  `\\.\DISPLAY1`, the primary, at `(0, 0)`. Equal DPI is the reason dragging a window between
  them does not exercise the `WM_DPICHANGED` path — see
  [A3 Task 4 delivered](#a3-task-4-delivered)
- **Both adapters can present to a window on either monitor**, measured with
  `vkGetPhysicalDeviceSurfaceSupportKHR` in A3 Task 4 rather than assumed. The integrated Intel
  part is not restricted to the display it drives, which was the open question on a machine with
  two GPUs and two screens

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
- **`AdapterInfo`, `DeduplicateAdapters` and a four-value `CapabilityTier`** in `Monarc.RHI` —
  `Unsupported`, `Baseline`, `Bindless`, `Advanced`: three attainable rungs above an
  unsupported floor every device meets. All pure and all tested with no device, which is what
  makes CI cover them
- **`Monarc.FirstLight --adapters`**, which is also the `Monarc.RHI.Vulkan.Probe` CTest entry

**The headline measurement.** `Monarc.FirstLight --adapters` on this machine, `msvc-debug`,
as the program actually printed it — five lines per adapter, log prefixes and the implicit
layers' own warnings included. Two elisions are marked inline and nothing else is altered:
the three raw Intel blocks that repeat `raw [1]` verbatim, and the two absolute paths inside
the Medal duplicate warning. An earlier revision of this section carried the same numbers as
one line per adapter — a summary in the shape of a capture, which is the one thing a file
whose whole job is being the honest account cannot be.

```
[Info   ] FirstLight: probe: validation requested true
[Warning] VulkanValidation: Loader Message | Removing layer VK_LAYER_MEDAL_HOOK ([... path elided ...]\medal-vulkan32.json) because it is a duplicate of VK_LAYER_MEDAL_HOOK ([... path elided ...]\medal-vulkan64.json)
[Warning] VulkanValidation: Loader Message | Layer VK_LAYER_OW_OVERLAY uses API version 1.2 which is older than the application specified API version of 1.3. May cause issues.
[Warning] VulkanValidation: Loader Message | Layer VK_LAYER_OW_OBS_HOOK uses API version 1.2 which is older than the application specified API version of 1.3. May cause issues.
[Warning] VulkanValidation: Loader Message | Layer VK_LAYER_MEDAL_HOOK uses API version 1.2 which is older than the application specified API version of 1.3. May cause issues.
[Info   ] Vulkan: Vulkan instance created | loader 1.4.357 | requested 1.3.0 | validation on | messenger installed
[Info   ] FirstLight: probe: loader open | instance Vulkan 1.4.357 | validation layer enabled | debug messenger installed
[Info   ] FirstLight: probe: raw enumeration -- 5 physical device(s)
[Info   ] FirstLight:   raw [0] NVIDIA GeForce RTX 3070 Ti
[Info   ] FirstLight:         uuid 759c8156-7b91-7099-6511-5bd91de66f76 | DiscreteGpu | vendor 0x10de device 0x2482
[Info   ] FirstLight:         Vulkan 1.4.351 | tier Advanced | queue families 6 (1 graphics)
[Info   ] FirstLight:         timeline true | dynamic rendering true | sync2 true | bindless images 1048576
[Info   ] FirstLight:         non-uniform indexing true | runtime array true | partially bound true | mesh true | ray tracing true
[Info   ] FirstLight:   raw [1] Intel(R) UHD Graphics 730
[Info   ] FirstLight:         uuid 86808b4c-0400-0000-0002-000000000000 | IntegratedGpu | vendor 0x8086 device 0x4c8b
[Info   ] FirstLight:         Vulkan 1.3.275 | tier Bindless | queue families 2 (1 graphics)
[Info   ] FirstLight:         timeline true | dynamic rendering true | sync2 true | bindless images 1048576
[Info   ] FirstLight:         non-uniform indexing true | runtime array true | partially bound true | mesh false | ray tracing false
[... elided: raw [2], raw [3] and raw [4] -- three more five-line blocks, each identical to raw [1] above ...]
[Info   ] FirstLight: probe: after deduplication on deviceUUID -- 2 adapter(s)
[Info   ] FirstLight:   adapter [0] NVIDIA GeForce RTX 3070 Ti
[Info   ] FirstLight:         uuid 759c8156-7b91-7099-6511-5bd91de66f76 | DiscreteGpu | vendor 0x10de device 0x2482
[Info   ] FirstLight:         Vulkan 1.4.351 | tier Advanced | queue families 6 (1 graphics)
[Info   ] FirstLight:         timeline true | dynamic rendering true | sync2 true | bindless images 1048576
[Info   ] FirstLight:         non-uniform indexing true | runtime array true | partially bound true | mesh true | ray tracing true
[Info   ] FirstLight:   adapter [1] Intel(R) UHD Graphics 730
[Info   ] FirstLight:         uuid 86808b4c-0400-0000-0002-000000000000 | IntegratedGpu | vendor 0x8086 device 0x4c8b
[Info   ] FirstLight:         Vulkan 1.3.275 | tier Bindless | queue families 2 (1 graphics)
[Info   ] FirstLight:         timeline true | dynamic rendering true | sync2 true | bindless images 1048576
[Info   ] FirstLight:         non-uniform indexing true | runtime array true | partially bound true | mesh false | ray tracing false
[Info   ] FirstLight: probe: 5 raw entries collapsed to 2
```

**Three test outcomes, because a suite that silently runs nothing while showing green is the
failure mode this phase is shaped to avoid.** `ctest` now reports nine entries under four
labels: `unit` (five, device-free, run everywhere), `gpu` (one, `SKIP_RETURN_CODE 77`),
`probe` (one, always runs and passes on any finding) and `architecture` (two).

"Passes on any finding" rather than "always passes", and the distinction is real: the probe
exits zero whatever it observes about the machine, including no Vulkan at all, but in Debug it
runs with validation on and the fatal messenger installed — so a VALIDATION-type error stops
it at `0x80000003` and CTest reports the entry **Failed**. What it declines to gate on is the
machine, not Monarc's use of Vulkan while looking at it.

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

**Every new guard was violated on purpose and watched fail.** Fourteen produced test failures:
adjacent-only deduplication (caught by the A-B-A-B-A case and *not* by the four-identical-Intel
case); the in-place compaction deleted (caught *only* by the A-A-B case, which is the one whose
survivors do not start at their final indices — the other cases' do, so all of them pass with
the assignment gone while a real GPU would be dropped in favour of a duplicate); UUID
comparison stopping at the first zero byte (caught at byte 6 of the real Intel UUID — that
mutation is no longer expressible, since `AdapterUuid::operator==` is now defaulted; see the
review pass below); two tiers given distinct but descending values; one bindless requirement
dropped; a failed
`VulkanBackend::Create` that did not deallocate its state (`CHECK( 152 == 0 )`); both formats
mapped to one `VkFormat`; the version decoded with the pre-Vulkan-SC open-coded shifts (caught
*only* by the variant-bits case, since for variant 0 the old layout and the macros agree);
`DeduplicateAdapters`' returned count ignored; a move-assignment that adopted without
releasing; one accessor's null check removed (a SIGSEGV in the moved-from case); the
validation-default generator expression's polarity flipped — which turns exactly the four
non-Debug legs red and leaves both Debug legs green, because in Debug the correct answer and a
hardcoded `true` are the same answer; `Loader`'s two move operators defaulted again, which
turns `CHECK( AllTablesEmpty(*source) )` red in both loader-move cases and in neither of the
`IsOpen()` assertions beside them; and `Loader::operator=`'s `this != &other` guard removed,
which turns both checks in the self-assignment case red — `x = std::move(x)` closes the
library and then adopts the nulls `Close()` had just written.

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

**One header comment was wrong about the code it sat on, and the code is what moved to meet
it.** `Loader`'s move was defaulted, and the class comment claimed a moved-from Loader was a
closed one with every table null. It was not: `Platform::Library`'s move nulls the source's
module handle, but the three entry-point tables are trivially copyable, so a defaulted move
*copies* them and the source keeps the pointers — `IsOpen()` false while
`Global().vkCreateInstance` still non-null, observed rather than reasoned about. The first
answer was to document that, on the grounds that nothing in Monarc reads a table off a Loader
it has moved from; but that is a claim about today's callers rather than about the type, and
Tasks 3 and 4 add callers. So both move operators are now written out and both clear the
source, which makes a moved-from Loader indistinguishable from a default-constructed one.

The hazard being closed is a real one and not tidiness: the source's copied pointers address a
module it no longer owns, so a call through one after the destination is destroyed is a
use-after-unload — and the ASan finding above is the evidence that nothing mechanical in this
codebase would report it. `VulkanBackend::State::BringUp` is the caller that produces such a
moved-from Loader in shipped code, at `loader = std::move(*opened)`.

- Green on all six presets: 9 CTest entries each, including the device tests actually running
  against this machine's two adapters
- 67 device-free doctest cases and 247 assertions across `Monarc.RHI.Tests` (36 cases, 128
  assertions) and `Monarc.RHI.Vulkan.Tests` (31 / 119), plus 15 device-required cases and 80
  assertions — that last number scales with how many adapters a machine has. Ten more
  device-free cases than the task first delivered and 57 fewer assertions, which is the
  review pass below: the new cases test things that were untestable, and the removed
  assertions could not fail
- **Where those numbers moved, and why.** `VulkanBackend` is a factory, so a backend exists
  only if it came up: move construction, move assignment, `Shutdown` and the accessors'
  behaviour on a moved-from backend cannot be reached without a Vulkan implementation, and
  those four cases are in `TestsDevice/` rather than `Tests/`. That is a real cost — the
  moved-from accessor guard is one of the fourteen above, and it no longer runs where there is
  no device. It bought a state that cannot exist: a backend whose allocation failed used to be
  reachable as `Initialize` returning `OutOfMemory`, and now `Create` returns that error with
  no object attached at all
- **`Loader`'s three move cases are device-only for the same shape of reason**, and it is not
  the module handle: a default-constructed `Loader` is closed with no Vulkan present, so the
  device-free suite can build one, but telling a move that *clears* the source's tables from
  one that copies them needs a source whose tables were populated — and `Open` is the only
  writer of those private members. With every table already null, the hand-written move, a
  defaulted one and a `memcpy` produce identical objects, so an assertion placed in `Tests/`
  could not fail. `Tests/TestVulkanLoader.cpp` says so at the foot of the file rather than
  carrying a case that always passes

**What CI will report is not yet known and is deliberately not claimed here.** The probe entry
exists so that the runners' answer becomes an observed fact; reading it and writing it down is
A3 Task 5's checkbox.

### A3 Task 2's code-quality review, and the three defects it found

A mutation-based review of the delivered task caught 13 of 15 behavioural mutations. It also
found three real defects, and every fix below was demonstrated by breaking the thing it guards
and watching the failure.

**A hand-sized stack array that the next edit overran.** `enabledExtensions[3]` in
`VulkanBackend::State::BringUp` was sized by hand to match a `requiredExtensions` declared two
lines *later*, and both `enabledExtensionCount++` sites are unchecked. Adding one required
instance extension — Task 4 wants `VK_KHR_get_surface_capabilities2`, which this machine has —
compiled clean on all six presets at `/W4 /WX` and produced, under `clang-asan`:

```
==16392==ERROR: AddressSanitizer: stack-buffer-overflow ... WRITE of size 8
    [304, 328) 'enabledExtensions' (line 332) <== Memory access at offset 328 overflows this variable
SUMMARY: ... VulkanBackend.cpp:370 in Monarc::RHI::VulkanBackend::State::BringUp
```

The size is now derived from the list, so the same edit grows the array. **No `static_assert`
beside it**: with the size derived, an assertion that the list fits cannot fail, which is the
category this same review pass deleted twenty of. `enabledLayers[1]` had the same shape and
became an array that *is* its initialiser, with a count of 1 or 0 to select it — no counter to
increment at all.

**A half-populated entry-point table, called into unguarded.**
`Loader::LoadInstanceFunctions` returned on the first name that would not resolve without
clearing `m_instance`, on an object whose `IsOpen()` stays true —`Loader::Open`'s own comment
promises that "nothing partially resolved leaves this function". And `vkDestroyInstance` is
*first* in `MONARC_VK_INSTANCE_FUNCTIONS`, while `BringUp` creates the instance before
resolving the table, so the likeliest half-populated table is the one missing the entry point
its own teardown needs — which `State::Shutdown` then called with no null check. With
`vkDestroyInstance` forced null on a machine that has Vulkan, `FirstLight --adapters` exited
`0xC0000005`. Fixed on both sides; with both guards and the same forced null it exits 0,
reports the leak at Error and returns `NotFound` naming the function.

**Three `VkResult`s meaning "the capability is absent" were reported as `BackendFailure`.**
`FailVk` funnelled every non-success result to it, including `VK_ERROR_INCOMPATIBLE_DRIVER`,
`VK_ERROR_LAYER_NOT_PRESENT` and `VK_ERROR_EXTENSION_NOT_PRESENT`. The first is the standard
outcome of `vkCreateInstance` on a machine with `vulkan-1.dll` and no registered ICD — very
plausibly what CI is — so a caller written to fall back on `Unsupported` was not firing on the
result that most deserves it. `Detail::ToErrorCode` now maps those three, tested; mapping them
back turns 3 of 3 assertions red. Both out-of-memory results deliberately stay
`BackendFailure`, because Monarc's `OutOfMemory` means *Monarc's* allocator returned nothing.

**Two documented behaviours had no test at all, proven by mutation passing both suites.**
Deleting `out.Clear()` from `EnumerateAdaptersRaw`, whose header states the contract, and
moving `CopyName`'s bound from `kMaxAdapterNameLength - 1` to `kMaxAdapterNameLength` — a
one-byte overrun of a fixed array. Both are now caught: the first turns 6 assertions red across
2 cases (`7 == 5`, `3 == 2`), the second turns 4 red (`256 == 255`, `97 == 0`, and a clobbered
guard). Four functions moved out of anonymous namespaces to make that possible — `CopyName`
became `Monarc::RHI::CopyAdapterName` beside the field whose contract it implements, and
`ContainsExtension`, `ContainsLayer` and `SeverityToLogLevel` moved into `Private/Translate.h`.

The second of those needed a test *shape* as well as a location. With the destination as a bare
local, the mutation is stack corruption and MSVC Debug stops at a runtime-check dialog before
doctest reports anything — a hang, not a red assertion. Behind a run of guard bytes the stray
byte lands in memory the test owns and the assertions simply fail.

**Forty-five assertions were deleted for being unable to fail, each measured rather than
argued.** The whole "MeetsTier is monotone" case reduced to `(reached >= tier) == (tier <=
reached)`: making `DetermineTier` return `Advanced` where `Baseline` is right left all 16 of
its assertions green while five in the knock-out cases went red. Twenty of the "total order"
case's twenty-six were trichotomy and transitivity of `<` over `u32`-backed enumerators — with
`Bindless = 5, Advanced = 3`, exactly 1 of 26 failed, and it was `kTiers[i-1] < kTiers[i]`,
which is what survives. Six more went from `TestVulkanTranslate.cpp`, where a loop over every
`VkPhysicalDeviceType` could only fail after five direct assertions above it had already
failed. And `AdapterUuid`'s sixteen-position case, 64 assertions, became one at the byte
position that matters: `operator==` is now defaulted, so the two bugs the comment cited as the
reason for hand-writing it are not expressible, and byte 15 of the real Intel UUID is the zero
byte a comparison in `DeduplicateAdapters` could still stop at.

One instruction in the review was not followed, on evidence. It asked to keep a single
assertion of the `>=`-not-`>` fact from the monotonicity case; measuring the `>` mutation showed
it already turns three assertions red in other cases, so a fourth copy was the redundancy the
rest of the pass was removing. The case that carries it now says so, to keep it from reading
like a restatement of the line above it.

### A3 Task 3 delivered

The logical device, its queue and timeline, its resource pools, command recording, the barrier
model, and a colour that is proved rather than looked at. No window, no surface, no swapchain,
no present, no binary semaphores: those are Task 4's.

- **`IDevice`, `IQueue` and `ICommandList`** in `Monarc.RHI/Include/Monarc/RHI/Device.h` —
  abstract interfaces, and the ones `Monarc.Render` consumes in Phase B rather than a step
  towards them. `VulkanDevice` implements all three (the queue and the command lists are
  device-owned objects behind `IQueue&` and `ICommandList*`)
- **No `IBackend`, deliberately, and Task 2's own comment predicting one was wrong.**
  `VulkanBackend::CreateDevice` returns a concrete `Result<VulkanDevice>`: polymorphic *use*
  without polymorphic *ownership*. An `IBackend`'s only reason to be virtual is runtime backend
  selection and there is one backend; and `Monarc.Core` still has no owning-pointer type, which
  is its own design — `IAllocator::Deallocate` needs the size and alignment of what it frees,
  and for a polymorphic type those are the derived class's, so the deleter has to carry values
  captured where the object was made. Both arrive with the second backend
  ([ADR-0012](Architecture/Decisions/ADR-0012-backend-rollout.md) schedules D3D12 immediately
  after M0), and the three interfaces do not change when they do
- **`Barrier.h`: the whole of [ADR-0005](Architecture/Decisions/ADR-0005-rhi-sync-model.md)'s
  model** — `syncBefore`/`syncAfter`, `accessBefore`/`accessAfter`, `layoutBefore`/`layoutAfter`,
  with global, buffer and texture variants as three distinct types. Fifteen pipeline stages,
  eighteen accesses and eight layouts, every one of them translated and tested, though A3
  records three barriers. The membership rule is stated in the header and is not "what A3 uses":
  Vulkan 1.3 *core* only, and only stages Monarc has a plan for — which is why
  `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` arrives with the swapchain in Task 4 and the ray-tracing
  and mesh-shading stages arrive with a phase that schedules them
- **A texture barrier without its layout pair is a compile error**, which the plan asked for at
  the type level. It has to be: `TextureLayout::Undefined` is a legitimate before-layout, so a
  defaulted pair is byte-for-byte identical to one someone meant. All four ways of omitting it
  were tried on both compilers — see below
- **Vulkan 1.3 dynamic rendering, and `grep -rni "renderpass\|framebuffer\|render pass"
  Source/` matches four lines, all of them comments and none of them code.** `BeginRendering`
  names image views at record time; nothing is created ahead of a frame and there is nothing to
  invalidate when a texture is recreated. `VK_NO_PROTOTYPES` is the other half: a call could
  only reach `vkCreateRenderPass` through `Private/Loader.h`'s tables, which name it nowhere
- **Resources are handles from device-owned, generation-checked, fixed-capacity pools** —
  `JobSystem`'s discipline, sized from a `DeviceConfig` at device creation and never grown, so
  no creation call can reallocate a pool and move a slot. A full pool reports
  `ErrorCode::OutOfMemory`. A stale handle resolves to a failure and not to the slot's new
  occupant ([ADR-0002](Architecture/Decisions/ADR-0002-handles-not-pointers.md))
- **One `VkDeviceMemory` per resource, and it is a stated placeholder.** Honest for a task that
  creates one texture and one buffer, and it will not survive the first scene with real assets:
  drivers cap `maxMemoryAllocationCount` (4096 on much hardware), each allocation has real cost,
  and nothing sub-allocates or pools. What makes it safe to ship is the *interface* rather than
  the strategy — `CreateTexture` takes a `TextureDescription` and returns a handle, so a
  sub-allocating allocator replaces one function body without touching a call site.
  [ADR-0014](Architecture/Decisions/ADR-0014-dependency-policy.md)'s table has no VMA row and
  adding one was not this task's call
- **A timeline semaphore and no binary semaphores.** `IQueue::Submit` returns the value the
  submission will signal; `IDevice::BeginFrame` waits on the value its frame slot's previous
  submission signalled before resetting that slot's command pool, which is what makes the reset
  legal. Two frames in flight, two command pools. Binary semaphores arrive in Task 4, because
  `vkQueuePresentKHR` accepts only those
- **The device entry-point table is the loader's fourth, and it is not a member of `Loader`.**
  Task 2's comment anticipated one that was, and that part of it was wrong: a device-dispatched
  function pointer is valid only for the device it was resolved against, and this machine has
  two devices at once. So each `VulkanDevice` owns its own `DeviceFunctions` and
  `Loader::LoadDeviceFunctions` fills one the caller supplies. Thirty-two entries, every one of
  them Vulkan 1.3 core, so **Monarc enables no device extension at all**

**The headline measurement, verbatim, on both adapters.** `Monarc.RHI.Vulkan.DeviceTests`,
`msvc-debug`, as the program printed it:

```
[Info   ] VulkanDeviceTest: readback on "NVIDIA GeForce RTX 3070 Ti": first pixel = (64, 128, 192, 255), expected (64, 128, 192, 255)
[Info   ] VulkanDeviceTest: readback on "NVIDIA GeForce RTX 3070 Ti": 16 of 16 pixel(s) exact
[Info   ] VulkanDeviceTest: readback on "Intel(R) UHD Graphics 730": first pixel = (64, 128, 192, 255), expected (64, 128, 192, 255)
[Info   ] VulkanDeviceTest: readback on "Intel(R) UHD Graphics 730": 16 of 16 pixel(s) exact
```

**The two vendors agree exactly, which was the finding this test was written to be able to
contradict.** Sixteen pixels of a 4×4 texture, each asserted individually — one pixel would
pass with a wrong row pitch — and no tolerance anywhere, because a tolerance is precisely what
would hide the two bugs the test exists to catch: 64 and 192 are far apart, so a red/blue swap
is visible, and an sRGB encode of 0.251 lands near 137 rather than 64. The values are
representable without rounding in both directions, which is why exactness is available at all.
The same four bytes come back on `clang-release` (no validation layer, no messenger) and under
`clang-asan`.

**The clear goes through `BeginRendering` with a `LoadOp::Clear` and not through
`vkCmdClearColorImage`,** and that is the point of the case rather than a detail of it. Task 4's
swapchain clear and A4's render graph both clear through a dynamic-rendering load-op, so a
readback that proved a clear-image path would be coverage for a code path nothing in the engine
uses — which reads as coverage while the shipped path stays unexercised.

**Three barriers, not the two the plan listed, and the third is a correctness fix rather than a
flourish.** The plan's flow is: barrier undefined → colour attachment, begin rendering with a
clear, end, barrier colour attachment → transfer source, copy. Waiting on the timeline after
that makes the copy's writes *available*; it does not make them visible to the host. So there is
a third barrier, a `BufferBarrier` from `Copy`/`TransferWrite` to `Host`/`HostRead`, before the
buffer is mapped. Without it a mapped read is reading memory whose visibility nothing
established — a bug that surfaces as intermittently stale bytes on some driver rather than as a
failure here. It also gives `BufferBarrier` a caller in shipped code instead of only in a
pure-function test.

**Two real defects, both found by the fatal validation messenger during this task.**

- **A texture created without `TextureUsage::ColorAttachment` cannot have an image view, and
  `CreateTexture` was making one unconditionally.** Dynamic rendering names a view rather than
  an image, so an attachment needs one — but `vkCreateImageView` requires the image to carry at
  least one view-compatible usage bit, and a transfer-only texture carries none. The first test
  to create a `TransferSource`-only texture stopped the process at
  `VUID-VkImageViewCreateInfo-image-04441`: "was created with
  `VK_IMAGE_USAGE_2_TRANSFER_SRC_BIT_KHR` but requires … `COLOR_ATTACHMENT` …". A texture with
  no view is now a legitimate texture, and `BeginRendering` refuses an attachment without one
  by name. This is a fair test of the messenger's premise: the mistake was mine, it was in a
  path the readback itself never took, and it was caught the first time anything walked it.
- **A destroyed-but-not-yet-reclaimed handle was caught only by accident.** `Resolve` checks
  both the slot's `live` flag and its generation, and the generation was originally bumped on
  *claim* only — so immediately after a destroy the old handle's generation still matched and
  only the `live` flag refused it. Every stale-handle assertion in the suite happened to create
  a replacement first, which bumps the generation; dropping the `live` check therefore passed
  all of them. Found by mutation. The suite now destroys a texture and a buffer and uses both
  handles with no intervening creation — and the design changed as well: the generation is
  bumped on release too, so it no longer depends on a flag. See the note on that below.

**The compile error for a missing layout pair, on both compilers.** Four ways to omit it, all
tried:

| Attempt | MSVC 19.51 | clang-cl 22.1.8 |
|---|---|---|
| Five parenthesised arguments | `error C2440: '<function-style-cast>': cannot convert from 'initializer list' to 'Monarc::RHI::TextureBarrier'` + `note: … function does not take 5 arguments` | `error: no matching constructor for initialization of 'TextureBarrier'` + `note: candidate constructor not viable: requires 7 arguments, but 5 were provided` |
| Five *braced* arguments | the same pair | the same pair |
| `TextureBarrier barrier;` | `error C2512: 'Monarc::RHI::TextureBarrier': no appropriate default constructor available` | `error: no matching constructor …` + `requires 7 arguments, but 0 were provided` |
| Seven arguments, stages shifted into the layouts' places | `note: … cannot convert argument 2 from 'Monarc::RHI::PipelineStage' to 'Monarc::RHI::TextureLayout'` | `note: … no known conversion from 'Monarc::RHI::PipelineStage' to 'TextureLayout' for 2nd argument` |

The fourth is the one a count-based check would miss, and distinct `enum class` types with no
implicit conversion between them are what buys it. `Tests/TestBarrier.cpp` pins all four as
`static_assert`s over `std::is_default_constructible_v`, `std::is_aggregate_v` and
`std::is_constructible_v`, plus the positive form so the negatives cannot be satisfied by a
type nobody can build.

**Thirty-six behavioural mutations, thirty-five caught and one that would not compile.**
Twenty-seven were caught as red assertions and eight by the fatal messenger stopping the
process at a named VUID, which is a different and stronger signal — the code was wrong in a way
Vulkan itself objects to. An earlier version of this line added "No survivors", which was true
of the thirty-six listed and not of the module: the set was chosen from the translation tables,
the pool and generation machinery, the frame/pool/timeline cycle and the readback, and five
things outside it did survive mutation. The review below found them, and what the set covered is
now stated rather than implied.

The thirty-six:

| Mutation | How it was caught |
|---|---|
| `PipelineStage::Copy` given `VK_PIPELINE_STAGE_2_BLIT_BIT` | 12 assertions red |
| A row dropped from `FromVulkanStages` / `FromVulkanAccess` | 4 red each |
| `operator|=` replaces instead of accumulating | 2 red |
| An unnamed stage bit translating to `NONE` rather than `ALL_COMMANDS` | 2 red |
| An unnamed layout translating to `UNDEFINED` rather than `MAX_ENUM` | 1 red |
| `VkMemoryBarrier2::sType` never set | 2 red |
| `VkImageMemoryBarrier2::image` never assigned | 1 red |
| A barrier's `src`/`dst` stage masks swapped | 3 red |
| `TextureBarrier` storing `layoutBefore` into both layouts | 1 red |
| `FindMemoryType` ignoring `memoryTypeBits`, or requiring exact property equality | 3 red / 1 red |
| `ResizeTo` appending; `ShrinkTo` popping once | 3 red / 1 red |
| `LoadOp::Clear` translating to `LOAD` | 1 red |
| Host-visible memory no longer asking for `HOST_COHERENT` | 1 red |
| **The clear's red and blue channels swapped** | 6 red |
| A pool slot's generation never bumped on claim | 3 red |
| A pool slot's generation never bumped on release (texture pool; buffer pool) | 1 red each |
| `Resolve` ignoring the generation | 1 red |
| `Resolve` ignoring the `live` flag | 1 red — see the generation note below |
| `Resolve` ignoring both | `vkCmdCopyImageToBuffer2` VUID → process stopped |
| A barrier's stale-handle log naming `layoutAfter` twice | 1 red (in the device suite then; in `Monarc.RHI.Tests` now — see the fatal-refusal note below) |
| The queue reusing its last timeline value | `vkQueueSubmit2(): pSubmits[0].pSignalSemaphoreInfos` VUID → stopped |
| Any command list treated as this device's own | `vkQueueSubmit2(): pSubmits[0].pCommandBufferInfos` VUID → stopped |
| `BeginFrame` never waiting; `Submit` never stamping the frame slot | `vkResetCommandPool(): (VkCommandBuffer …)` VUID → stopped |
| A full pool wrapping to slot 0 | 1 red |
| The copy's destination-size check removed | `vkCmdCopyImageToBuffer2` VUID → stopped |
| A mapped span reporting the allocation's size | 1 red |
| A device-local buffer mapped; an already-mapped buffer mapped again | `vkMapMemory` VUID → stopped |
| An attachment with no image view rendered into | 1 red |
| `CreateDevice` accepting a UUID nothing reports | 3 red |
| `vkQueueSubmit2` removed from the device table | `error C2039: 'vkQueueSubmit2': is not a member of 'Monarc::RHI::Detail::DeviceFunctions'` |

That last one is the entry-point table's whole purpose as a compile-time property: the X-macro
list and the call sites cannot drift apart. Three further mutations were tried and turned out
not to be *expressible* at `/W4 /WX` — dropping `out.image = image` is
`warning C4100: 'image': unreferenced parameter`, duplicating `layoutBefore` is the same for
`layoutAfter`, and making `FindOwnList` match unconditionally is `warning C4702: unreachable
code`. Each was re-run in a form that sidesteps the warning, and the numbers above are those
runs.

**One process note, because it invalidated a first set of results.** The mutation harness
restored each file with `shutil.copy2`, which preserves the original mtime — so the clean source
was *older* than the object file built from the mutated one, ninja saw no work to do, and the
next mutation's binary still contained the previous one. Two "caught" results were being
credited to the wrong mutation, and a stale binary made the unmutated suite look red. The
harness now stamps the restored file with the current time, and the numbers above come from a
run that starts from a scratch `Build/msvc-debug`.

**A pool slot's generation is now bumped on release as well as on claim, and the `live` flag is
no longer the only thing refusing a destroyed handle.** With the claim-only bump there was a
window — destroyed, not yet reclaimed — in which a stale handle's generation still matched its
slot, so `live` alone stood between a caller and a `VK_NULL_HANDLE` image. That was not
hypothetical. Dropping the `live` check from `Resolve` under the old design handed
`vkCmdCopyImageToBuffer2` a null `srcImage` and stopped the process with **204 assertions green
and none red**; the same mutation with the release bump in place produces no Vulkan call at all
and one red assertion. Neutering the generation check as well brings the crash straight back,
which is what says the generation — and not something else — is doing the refusing.

`JobSystem::ClaimSlotLocked` bumps on claim only and was cited as the precedent. The mechanics
match and the safety argument does not: there `done` is both the free/occupied flag and the
semantic answer, a stale job handle's correct answer is "complete", and nothing is
dereferenced — a wrong flag yields a benign default. For a resource the stale answer must be
"refuse" and a wrong flag yields a null image. What the one-bump version bought was tidiness:
a slot's generation changed exactly once per occupant. What it cost was the invariant the
counter exists for. The flag stays in `Resolve` for the one case the generation cannot answer —
a forged handle at generation zero naming a slot no device has ever claimed — and that case now
has its own assertion, which is what turns the dropped flag red instead of silent.

**`ICommandList::Barrier`'s stale-handle refusal is now fatal, and the test that used to watch
it happen has been replaced by one that runs in CI.** The refusal was `MONARC_CHECK(false, …)`
followed by `return`. `MONARC_CHECK` reports and optionally breaks and never alters control
flow, so under any handler that declines to break — Shipping, or a test harness — that `return`
*skipped the barrier* and the rest of the frame recorded as though it had been asked for. A
dropped barrier is not a refused operation: it is a synchronisation hole whose symptom is wrong
pixels or a GPU hang on some driver, with nothing in the capture pointing back to the call. It
is `JobSystem::Wait`'s fall-through again, and it takes the same house pattern —
`MONARC_DEBUG_BREAK(); std::abort();`, unconditionally.

**Measured both ways, out of process, on the RTX 3070 Ti.** A program that brings up a real
device, destroys a texture, installs a handler returning `false` and issues a barrier against
the stale handle: with the fix, both overloads print the refusal and the process is gone with
`0x80000003` — the debug break, which is what ends it before `std::abort()` is reached — and
the line after the call never runs. With the `return` restored, the same program printed
`*** BARRIER RETURNED ***` and exited zero, for both the buffer and the texture overload. That
control is what says the abort is doing the stopping.

**Because nothing survives the call, the device case that pinned the refusal could not stay.**
It installed a declining handler, read the log line and then asserted the list still submitted
— every one of which now requires a process that is already dead. So the composition of that
log line was extracted into `Describe(const BufferBarrier&)` and `Describe(const
TextureBarrier&)` in **`Monarc.RHI`**, filled through `std::format_to_n` into a fixed
`BarrierDescription` (`AdapterUuidString`'s shape, and ADR-0003's condition on `<format>`), and
four cases in `Monarc.RHI/Tests/TestBarrier.cpp` pin it with no Vulkan linked at all. It lives
in `Monarc.RHI` rather than in `Monarc.RHI.Vulkan/Private/Translate.h`, whose membership rule it
satisfies, because it names no Vulkan type in either direction — a second backend would
otherwise duplicate it or include a header it has no business seeing.

**That trade is a gain on two counts and a loss on one, and the loss is named in the file where
the case used to be.** Gained: the check moved from a GPU-only suite into CI, and it pins more
than the old one did — the exact text rather than four `find()`s, both overloads rather than
one, a mask's hex, and the buffer's capacity. `ToString(PipelineStage)`, `ToString(Access)` and
`ToString(TextureLayout)` still have a shipped caller, one hop further away: `Barrier.cpp`'s two
`Describe` overloads call all three, and `VulkanCommandList`'s two handle-resolving `Barrier`
overloads call `Describe`. *(Named rather than cited by line: the review below moved those two
out of `VulkanDevice.cpp` and the line numbers this sentence used to carry went with them.)*
Lost: that the refusal fires *on a device* against a handle a real `DestroyTexture` made stale,
and that the `MONARC_CHECK` message names a stale handle. Neither is replaced. What softens the
first is that `Barrier` resolves through the same `Resolve` that `BeginRendering` and
`CopyTextureToBuffer` use, and the stale-handle case above still exercises those two on a real
device, so what is now unobserved is `Barrier`'s call to `Resolve` and not `Resolve` itself.

Five mutations of the new cases were run to check they can fail, each rebuilt from a touched
source rather than a restored one: the layout pair swapped (the texture case's comparison,
alone); `syncAfter` dropped from the buffer form (the buffer case and the mask case); the hex
dropped from the buffer form's `syncBefore` (the same two, and the mask case's failure text —
`sync <not a single PipelineStage> -> AllCommands` — is the argument for the hex existing);
`kBarrierDescriptionLength` cut to 240 (the texture capacity assertion alone, `239 < 239`) and
to 220 (both). A sixth, a `find("layout") == npos` guard on the buffer description, was written,
measured, and **deleted**: it goes red on the copy-paste it was written for, but so does the
comparison above it, in the same run, so it detected nothing new.

**A mapped span's length is asserted against a 250-byte buffer and not a 256-byte one**, because
Vulkan rounds an allocation up to the memory type's alignment: at 256 the buffer's size and the
allocation's size are the same number and the assertion could not tell them apart.

**The gpu suite requires a Vulkan SDK in Debug, deliberately.** Task 3's plan asks to "assert
the messenger was installed, so a build that quietly failed to load the layer cannot pass as
clean" — so `ValidationDefault()` being true is asserted to imply both
`ValidationLayerEnabled()` and `DebugMessengerInstalled()`, with no "if the layer happens to be
present" escape. Weakening it to that would make the assertion unable to fail at all, which is
the whole category this phase's review pass has been deleting. The Release legs assert the
converse — no layer, no messenger.

**The skip machinery still reports Skipped and not Passed.** Demonstrated again by pointing the
registered `gpu` command at a library that cannot exist:

```
7/9 Test #7: Monarc.RHI.Vulkan.DeviceTests ....***Skipped   0.03 sec
...
The following tests did not run:
	  7 - Monarc.RHI.Vulkan.DeviceTests (Skipped)
```

- Green on all six presets, zero warnings, 9 CTest entries each
- **108 device-free cases** — `Monarc.RHI.Tests` 51 and `Monarc.RHI.Vulkan.Tests` 57 — plus
  **31 device-required cases**, and Task 2 left 67 device-free, so Task 3 adds 41 device-free
  cases and 31 that need a GPU. *(The review below takes these to 109 and 36.)*

  **Four of those device-free cases and one of the missing device-required ones are the same
  change**: `Barrier`'s fatal refusal, above, moved the barrier-description check out of the
  device suite and into `Monarc.RHI`'s, where CI runs it. The device suite went from 32 cases
  to 31 and from 309 assertions to 296.

  **Cases and not assertions, and the change of unit is the point.** The assertion totals are
  498 device-free (174 + 324) and 296 device-required, and an earlier draft of this section
  quoted 1061 and 290 — a number that was 572 higher because four `O(n²)` loops each contributed
  one assertion per *pair* of enumerators. Measured, those loops did not detect 572 things: a
  mutation duplicating a Vulkan access bit turned 3 of 153 red, and one duplicating a `ToString`
  case turned 1 of 286. Each is now a single assertion that names the colliding pair
  (`IndexRead[2] and UniformRead[8] collide`), with the same detection power — 3 reds and 1 red
  respectively — and the layout loop's 28 comparisons were deleted outright rather than
  collapsed, because the eight spot checks above them are exhaustive and the round-trip case
  catches a duplicate a second time. An assertion count nobody can interpret is a number that
  gets quoted as coverage, so this section quotes cases.
- **What only runs with a device, and is therefore invisible in CI**: everything through
  `VulkanBackend::CreateDevice` — device creation on each adapter, the readback itself, the
  frame/pool/timeline cycle, every resource-pool and stale-handle assertion, the cross-device
  submission refusal, and the mapping rules. That is 31 cases. What CI *does* cover of Task 3 is
  the whole barrier model's translation in both directions, the text a barrier refusal logs,
  the resource-description and rendering enums, `FindMemoryType` against memory layouts this
  machine does not have, the `Array` operations the device's pools are built on, and every
  type-level property of `TextureBarrier`
- Two functions became testable rather than staying unreachable: `ResizeTo` and `ShrinkTo` moved
  out of `VulkanBackend.cpp`'s anonymous namespace into `Private/ArrayOps.h`, which is where the
  second caller (the device's pools) made them worth sharing. Task 2's review found mutations
  surviving in exactly that category
- The resolve macro is now one body with four call sites, which Task 2's review said a third
  copy would justify — and **optionality is per entry rather than per table**. That is the shape
  that survives what comes next: mesh-shader, ray-tracing and swapchain entry points vary
  *per adapter*, so on this machine the same `DeviceFunctions` table would be fully populated
  for the RTX 3070 Ti and partly populated for the Intel UHD 730, and a table whose identity is
  "everything in me was found" cannot describe that. Task 3's device table is entirely
  `Required` and the only `Optional` entries are the two debug-utils ones that already were, so
  no machinery was added for extensions that do not exist yet
- The messenger block came out of `BringUp` as `State::InstallMessenger`, as Task 2's review
  asked. What stayed is everything whose storage `vkCreateInstance` reads —`messengerInfo`,
  `enabledExtensions` and `applicationInfo` all have to outlive that call

**Three things the self-review changed, and one of them was a false claim.**
`kMaxColorAttachments`' comment said eight was "Vulkan's own guaranteed floor" for
`maxColorAttachments`. It is not — the spec's required minimum is lower, and eight is
D3D12's fixed `D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT`. Both local adapters do report eight,
measured with `vulkaninfo`, and the constant now says what it is: a bound on the array
`BeginRendering` builds, not a capability claim. Second, `ICommandList`'s comment on its
protected move operations claimed nothing derived from it was movable, which is not true of a
defaulted protected move — the reason those operations exist is that deleting copy
suppresses the implicit moves, and `VulkanDevice` needs one. Third,
`TextureUsage::TransferDestination` had no caller: nothing uploads to a texture yet, so the
enumerator was removed and the header says when it comes back. That last one is the rule the
same header states about itself, applied to the header.

### A3 Task 3's code-quality review, and what it found

A second mutation-based pass over the delivered task, from outside the set the thirty-six above
were drawn from. It caught twelve of fifteen mutations, most by a single named assertion — and
it found **two paths that ended the process where every neighbour returned a `Status`, five
things that survived mutation, and one comment asserting a test case that did not exist**. Every
finding below was measured, and each fix was demonstrated by breaking the thing it guards.

**Two caller mistakes ended the process at a named VUID.** `IQueue::Submit` checked that a list
was not still recording and never that it had recorded anything, and `BeginFrame` resets the
pool — which returns the command buffer to Vulkan's *initial* state, where `IsRecording()` is
false for a second reason. A frame loop with an early-out between `BeginFrame` and `Begin` hit
`VUID-vkQueueSubmit2-commandBuffer-03874`, "is unrecorded and contains no commands", at exit
`0xC0000409`. And `VulkanDeviceState::Shutdown` destroyed the command pools without detaching
the lists, so a list a caller still held kept its recording flag and its `VkCommandBuffer`
across the shutdown. With the detach loop removed, the case that pins it exits `0xC0000409` and
prints nothing at all — the process is gone before doctest's output is flushed — while the rest
of the device suite, that case excluded by name, is green at 35 cases and 338 assertions. Both
are now returned `Status`es, which is what the rest of the module already did for `Begin` twice,
`End` outside a pass, `End` inside one, a copy inside a pass, a nested pass, and a list from
another device; the three `Barrier` overloads and `EndRendering` have no `Status` to carry a
refusal, so they report through the assertion handler instead, and the case captures that with a
handler that declines to break.

**`CopyTextureToBuffer` documented one usage precondition it did not check and never mentioned
the other.** The destination's `BufferUsage::TransferDestination` was in the comment and not in
the code (`VUID-VkCopyImageToBufferInfo2-dstBuffer-00191`); the source's
`TextureUsage::TransferSource` was in neither (`…-srcImage-00186`). Both stopped the process,
both are now one `HasAny` against a description the slot already holds, and both are pinned by a
case that also submits the pair that does carry them — so the refusals are about the bits and
not about the call. The sibling case, an attachment created without
`TextureUsage::ColorAttachment`, was already a named `Status` with a test.

**Five things survived mutation, and all five are now pinned.** Each was measured green before
and red after:

| What survived | The mutation | What it turns red now |
|---|---|---|
| `ToVulkan(MemoryLocation)`'s `MAX_ENUM` fallback | returns `0` | 2 assertions |
| `ToVulkanBit(TextureUsage)`'s zero fallback | returns `VK_IMAGE_USAGE_STORAGE_BIT` | 2 assertions |
| `ToVulkanBit(BufferUsage)`'s zero fallback | returns `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` | 1 assertion |
| `Shutdown`'s two live-slot loops | both deleted | `VUID-vkDestroyDevice-device-05137`, "has 5 leaked objects" |
| `ReleaseBufferSlot`'s unmap-on-release branch | deleted | 1 assertion |

The three translation mutations were applied together and turned 5 assertions red across 2
cases, attributed above by the function each assertion names. Every one of those 5 is an
assertion this pass added, which is what "it survived" means: with the mutation in and the new
assertions out, the suites were green. The two device-side rows were measured the other way
round, with the mutation in and the new case *excluded* by name — 35 cases and 344 assertions
green in both, so nothing already in the suite reached either.

The memory-location fallback is asserted as its *consequence* rather than as the constant, which
is what its five-line argument is actually about: the fallback matches no memory type, where
zero is a subset of every type's properties and so matches index 0 — device-local memory for a
`HostVisible` request, failing at `vkMapMemory` a long way from the value that caused it.

**A comment asserted a test case that did not exist.** The readback said `Shutdown`'s sweep over
live pool slots was "covered by the case above, which destroys a device with live resources
still in its pools"; the case above created no textures and no buffers, and no case among the 31
left a live resource in a pool at teardown. There is now a case that leaves a `ColorAttachment`
texture and a mapped `HostVisible` buffer live and lets the device go — and **the validation
layer enforces it rather than an assertion**, which the case says outright: a leaked `VkImage`
is invisible through `IDevice`, because `BytesAllocated()` reaches zero whether or not the pool
arrays' contents were destroyed.

**One finding in that review was wrong, and measurement is what said so.** It held that the same
teardown case covers `ReleaseBufferSlot`'s unmap-on-release branch, because the buffer is left
mapped. It does not: freeing memory that is still mapped is legal Vulkan and draws no validation
error, and with the branch deleted the case is green at 6 of 6. What the branch actually protects
is the *slot* — it clears `mapped` as well as calling `vkUnmapMemory`, so without it the next
buffer to claim that slot is refused by `MapBufferForRead` as "already mapped". A pool of one
buffer, a map, a `DestroyBuffer` with no unmap, and a second map is what pins it, and that
assertion goes red where the teardown case does not.

**`Barrier.h`'s cost claim was true for two of its three enum sets, and the count was worse than
the review said.** "An unused enumerator costs one row in one switch" is exact for
`PipelineStage`'s fifteen and `Access`'s eighteen — inert data, translated both ways, nothing
downstream reads them. It is not true of `TextureLayout`, because a layout is only valid for an
image whose *usage* permits it and `TextureUsage` has two bits. Probed on the RTX 3070 Ti,
barriering a `ColorAttachment | TransferSource` texture into each of the eight in turn:

| Layout | Result |
|---|---|
| `Undefined`, `General`, `ColorAttachment`, `TransferSource` | no validation error |
| `DepthStencilAttachment` | `VUID-VkImageMemoryBarrier2-oldLayout-01209` |
| `DepthStencilReadOnly` | `…-oldLayout-01210` |
| `ShaderReadOnly` | `…-oldLayout-01211` |
| `TransferDestination` | `…-oldLayout-01213` |

**Four of eight and not three** — the review named the two depth layouts and
`TransferDestination` and missed `ShaderReadOnly`, which wants `VK_IMAGE_USAGE_SAMPLED_BIT` or
`VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` and gets neither. The enumerators stay, because
ADR-0005's model arriving whole is the plan's decision and `DepthStencilAttachment` is needed
the moment a depth pass exists; the claim is what changed. `TextureLayout`'s own note now counts
them and connects the two disclosures that already existed on either side — `TextureUsage`'s
deliberately absent transfer-destination bit in `Device.h`, and `ToVulkan(const TextureBarrier&,
VkImage)`'s unconditional `VK_IMAGE_ASPECT_COLOR_BIT` in `Translate.h`, which is the second
thing a depth pass has to fix and would be silently wrong rather than loudly refused.

**`VulkanCommandList.cpp` is the file the plan named, and now it exists.** The A3 plan's listing
names `VulkanBackend.cpp  VulkanDevice.cpp  VulkanCommandList.cpp`; the third did not exist and
nothing recorded the deviation, which made it the only departure from that listing on this
branch that was not argued anywhere. `VulkanDevice.cpp` was 1533 lines holding the device, the
command list and the queue at once, with Task 4's swapchain to come. The three pool-slot structs
and the three class declarations moved to `Private/VulkanDeviceState.h` and the command list's
implementation to `Private/VulkanCommandList.cpp`: **1533 → 1030, plus 402 and 295**, with
nothing duplicated and no behaviour changed. `VulkanQueue` is declared on the new header and
still implemented beside the device, because `Submit` stamps `FrameSlot::timelineValue` and
`BeginFrame` is what waits on it and clears it — the only two functions that write that field.
`VulkanDeviceState.h` is itself a departure from the plan's listing in the other direction, and
the header records that at the top.

**The one failure path that leaked a Vulkan object no longer does.**
`Loader::LoadDeviceFunctions` clears the whole device table when a Required entry is missing —
its promise, and worth keeping — so the freshly created `VkDevice` had nothing left able to
destroy it and `Shutdown` logged "it is leaked until the process exits". Measured by forcing
that failure: without the fix the layer then stops the process at
`VUID-vkDestroyInstance-instance-00629`, "object VkDevice 0x254806800d0 has not been destroyed",
exit `0x80000003`; with `Loader::ResolveDeviceDestroyer` resolving that one entry back, the same
forced failure produces neither the log line nor a validation error.

**Smaller corrections, each measured or argued:**

- `BeginRendering` reads the attachment's `TextureUsage::ColorAttachment` rather than inferring
  it from a null image view. The two are observationally identical today — with the null-view
  test back in, the device suite is green at 36 cases and 350 assertions, and that is stated
  rather than dressed up as a caught bug — but `CreateTexture` already records that `Sampled`
  and `Storage` will make a view too, at which point a `Sampled`-only texture passes a null
  check and becomes the validation error the refusal exists to prevent
- `kNoSlot` documented one of its two meanings: it was also returned by
  `FindGraphicsQueueFamily` and compared against a queue-family index, an unrelated domain. The
  family search has `kNoQueueFamily` of its own
- `FrameSlot::timelineValue` is cleared by the `BeginFrame` whose wait on it returned, so "zero
  means nothing outstanding" stays true of a slot that was begun and never submitted. Nothing
  observable through the interface changes — the re-wait it removes was on an already-signalled
  value — and no test claims otherwise; what changed is that the field means what it says
- `ICommandList`'s and `IQueue`'s protected move operations are gone. Nothing derived from
  either is ever moved: the one implementation of each is a member of a heap-allocated
  `VulkanDeviceState` whose *pointer* is what moves. `IDevice` keeps its own, which
  `VulkanDevice`'s move constructor names, and the header now says which of the three needs
  them and why. Same rule this diff already applied to `TextureUsage::TransferDestination`,
  `Describe(const GlobalBarrier&)` and `AllDeviceFunctionsResolved`
- `TestBarrier.cpp` said "this file's total went from 453 to 169", and neither figure was that
  file's total — both were the whole binary's, at a commit several behind. The file is **15
  cases and 46 assertions**, measured with `--source-file=*TestBarrier.cpp`. The 286 in the same
  sentence is right and is now shown as its parts: 105 pairs for fifteen stages, 153 for
  eighteen accesses, 28 for eight layouts. Which makes the mistake the mistake that paragraph
  warns about, made inside the warning

**Counts after the review: 109 device-free cases** — `Monarc.RHI.Tests` 51 and
`Monarc.RHI.Vulkan.Tests` 58 — **and 36 device-required**, up from 108 and 31. The five new
device cases are the never-recorded submission (with the wrap that proves the bit is cleared and
not merely set), recording after a shutdown, the teardown sweep, the slot-reuse unmap, and the
copy's two usages. The one new device-free case is the memory-location fallback. Assertion
totals, for whoever wants them, are 504 device-free and 350 device-required — but this section
quotes cases, for the reason the section above it gives.

### A3 Task 3's command list state machine, enumerated

The review above closed four caller mistakes that stopped the process. Two of them had the same
shape — a list in a state a method did not check — and closing them one at a time was going to
keep working until Task 4's frame loop found the rest. So the states were written out instead:
`VulkanCommandList` is in exactly one of **reset, recording, recorded, submitted**, `m_rendering`
is a sub-state of recording, and detachment is orthogonal to all four. The table, the five
transitions, and a cell-by-cell account of what each of the seven public methods refuses in each
state are in `VulkanCommandList`'s class comment in `Private/VulkanDeviceState.h`.

**The enumeration found three holes, not one.** Each was reachable through the public interface
and each ended the process at a validation error instead of returning a `Status`:

| sequence | Debug, before | Release, before |
|---|---|---|
| `BeginFrame → Begin → End → Begin` | `VUID-vkBeginCommandBuffer-commandBuffer-00050`, exit 3221226505 | exited 0, silent, `Begin` returned success |
| `… → End → Submit → Begin` | `VUID-vkBeginCommandBuffer-commandBuffer-00049`, exit 3221226505 | exited 0, silent, `Begin` returned success |
| `… → End → Submit → Submit` | `VUID-vkQueueSubmit2-commandBuffer-03875`, exit 3221226505 | exited 0, silent, a timeline value returned |
| `… → Submit → WaitIdle → Submit` | `UNASSIGNED-DrawState-CommandBufferSingleSubmitViolation`, exit 3221226505 | exited 0, silent, a timeline value returned |
| `Begin → BeginRendering → Barrier(GlobalBarrier{})` | `VUID-vkCmdPipelineBarrier2-None-09553`, exit 3221226505 | exited 0, silent, the frame submitted and completed |

**The Release column is why these are guards rather than notes.** `ValidationDefault()` is false
outside Debug, so every one of these was silent undefined behaviour in the configuration that
ships — the fatal messenger catching them is not a mechanism a Release build has. Each row was
run on the RTX 3070 Ti, before and after, and the "before" figures above are the runs.

What each hole cost, and what it is now:

- **`Begin` on a recorded list** is a `Status`. `End` leaves the command buffer *executable*, and
  `vkBeginCommandBuffer` on one of those is an implicit reset, which needs
  `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` — a bit `BringUpDevice` deliberately does not
  set, because `BeginFrame` resets the whole pool and that is the only reset this design
  performs. Once the list is also submitted the buffer is *pending* and the same call is a
  different VUID; `m_recorded` outlives `Submit`, so one guard refuses both. **This is the frame
  loop that records, submits and comes round again without a fresh `BeginFrame`** — which is
  what Task 4 is about to write.
- **`IQueue::Submit` on a submitted list** is a `Status`, carried by a new flag set in `Submit`
  and cleared in `Reset`. Neither existing check could express it: a submitted list *is*
  recorded and *is not* recording, so it looks exactly like one that is ready to go. The flag is
  on the list rather than a test of the frame slot's `timelineValue`, and the `WaitIdle` row
  above is why — after a wait nothing is outstanding, yet the buffer is *invalid* rather than
  executable, `Begin` having recorded it with `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`.
- **`Barrier` inside a rendering pass** ends the process, with `MONARC_CHECK` and an
  unconditional `std::abort()`. `End`, `BeginRendering` and `CopyTextureToBuffer` all refuse
  mid-pass with a `Status`; the three `Barrier` overloads have no `Status`, so this follows their
  stale-handle guard instead. Fatal rather than a plain return because `MONARC_CHECK` alters no
  control flow, and a barrier *skipped* under a handler that declines to break is the
  synchronisation hole `Monarc/RHI/Device.h` argues about at length. There is no legal call to
  record in its place: barriers are not permitted inside a dynamic-rendering instance at all,
  and it was the empty `GlobalBarrier{}` that tripped the VUID — the rule is about the call, not
  the contents. A pass that needs to read what it wrote is what
  `VK_KHR_dynamic_rendering_local_read` is for, and this device does not enable it. After the
  fix the process stops with Monarc's own message and, in Debug, no VUID at all — the layer is
  never handed the illegal call.

**The fourth combination of flags is unreachable, which is what makes four states the whole
table rather than a selection from eight.** `m_recording && m_recorded` cannot occur: `End` is
the only writer that sets `m_recorded` and it clears `m_recording` in the same breath, and
`Begin` is the only writer that sets `m_recording` and now refuses when `m_recorded`.

**Two new device cases, and five mutations run against them** — each rebuilt from a touched
source, with `ninja` confirmed not to have said "no work to do":

| mutation | caught by |
|---|---|
| `Begin`'s recorded guard removed | "a list that has already been recorded is refused rather than begun again" CRASHED at 00050, exit 3221226505 |
| `Submit`'s submitted guard removed | "a list already submitted is refused rather than submitted a second time" CRASHED at 03875, exit 3221226505 |
| `list->MarkSubmitted()` removed | the same case, the same VUID |
| `Reset` no longer clearing the submitted flag | 2 assertions red — `REQUIRE( reused.has_value() )` in the new case, and `REQUIRE( submitted.has_value() )` in the pre-existing wrap case |
| `Reset` no longer clearing `m_recorded` | 1 red — `REQUIRE( (*commands)->Begin().has_value() )` in the wrap case, which the new `Begin` guard now catches earlier than the old submit refusal did |

The first three are crashes rather than red assertions, and that is the shape of the finding
rather than a weakness of the cases: the process is gone at the guarded call, before the
assertion on its result runs. It is how the four refusals in the review above were measured too.

**The barrier guard has no case and cannot have one**, for the reason the stale-handle guard has
none: nothing survives the call, so there is no handler under which a test could assert. It
joins that guard as a fatal path with no in-process case, and
`TestsDevice/TestVulkanDevice.cpp` records it beside it rather than leaving the gap to be
discovered.

**Counts: 109 device-free cases and 38 device-required**, up from 109 and 36 — both new cases
are device-required, since a command buffer's state is only observable against a real device.
Assertion totals are 504 device-free and 384 device-required.

**Not done in Task 3, and recommended for Task 4 rather than smuggled in there: make the four
states an `enum class` and switch on it exhaustively.** With `/w44062` on, a fifth state would
then fail to compile in every method that has to decide about it, which is the only mechanism
that actually stops a fourth patch — a comment cannot. It was left out of Task 3 because no test
can distinguish it from the flags, and a refactor no test can see does not belong in a task
being closed. **Task 4 did it**, and the fifth state was not the one the plan predicted — see
[A3 Task 4 delivered](#a3-task-4-delivered).

### A3 Task 4 delivered

A window, a surface, a swapchain, and first light. `Monarc.FirstLight` with no arguments opens a
1280x720 window, clears every frame to `(64, 128, 192)`, and exits **zero** when the window is
closed; `--frames=N` runs N presented frames and exits, and `--adapter=<n|name>` picks the GPU.

**The headline: the clear reaches the image that gets presented, and it is asserted to the
byte on both adapters.** The swapchain readback barriers the acquired image to transfer-source
*before* presenting it, copies it into a host-visible buffer, and compares every pixel. Verbatim
from `Monarc.Host.Windowed.DeviceTests`:

```
swapchain readback on "NVIDIA GeForce RTX 3070 Ti": first pixel = (192, 128, 64, 255) as blue, green, red, alpha; expected (192, 128, 64, 255)
swapchain readback on "NVIDIA GeForce RTX 3070 Ti": 230400 of 230400 pixel(s) exact
swapchain readback on "Intel(R) UHD Graphics 730": first pixel = (192, 128, 64, 255) as blue, green, red, alpha; expected (192, 128, 64, 255)
swapchain readback on "Intel(R) UHD Graphics 730": 230400 of 230400 pixel(s) exact
screen capture on "NVIDIA GeForce RTX 3070 Ti": first pixel = (192, 128, 64, 255) as blue, green, red, alpha; expected (192, 128, 64, 255)
```

The byte order is the finding as much as the values are. Task 3's `R8G8B8A8_UNORM` texture reads
back `(64, 128, 192, 255)`; the swapchain is `B8G8R8A8_UNORM` and reads back `(192, 128, 64,
255)` from the same clear, because Vulkan's clear value is specified per *component* and the
format decides where each lands in memory. That makes the assertion a channel-order test rather
than a repeat, and a mutation confirms it: with `ToVulkan(Format::B8G8R8A8_UNORM)` returning
`VK_FORMAT_R8G8B8A8_UNORM`, both adapters read `(64, 128, 192, 255)` and **0 of 230400 pixels
matched**.

**The screen capture is real, and the desktop nearly made it lie.** It fronts the window,
presents in batches until two consecutive readings agree, and `BitBlt`s one pixel from the
*screen's* device context — the window's own DC returns black for a Vulkan window, because the
swapchain's contents never enter its GDI surface. For an afternoon it read a stable
`(106, 71, 35)`: the clear at 55%. The cause was not Monarc. `WindowFromPoint` at the capture
point named a layered, topmost `Shell_SystemDim` — the overlay Windows puts over the monitor a
system security dialog is on, and one was open. The case now asks who is on top before it
asserts, tries the virtual screen's origin as a second position (which on this machine is the
other display, undimmed), and **reports rather than asserts** when the answer is somebody else.
The bytes it does assert are exact.

**Both adapters can present to a window on either monitor.** Measured with
`vkGetPhysicalDeviceSurfaceSupportKHR` on the graphics queue family device creation would pick,
per adapter and per surface, and not assumed:

| adapter | graphics family | can present to a window on the primary | on `\\.\DISPLAY5` |
|---|---|---|---|
| NVIDIA GeForce RTX 3070 Ti | 0 | yes | yes |
| Intel(R) UHD Graphics 730 | 0 | yes | yes |

So the Intel iGPU *can* present to the display the NVIDIA card drives on this machine, which was
the open question. `VK_KHR_swapchain` is offered and enabled on both, so the Optional-entry
branch of the loader's device table is not exercised here.

**Swapchain parameters, as negotiated:** `B8G8R8A8_UNORM` + `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`,
`VK_PRESENT_MODE_FIFO_KHR`, opaque composition, exclusive sharing. `minImageCount` is 2 on both
surfaces, so `minImageCount + 1` gives **3 images against 2 frames in flight** — the two counts
differ, which is the point of keeping them unrelated. `maxImageCount` differs by vendor: **8 on
the NVIDIA surface and 64 on the Intel one**. Both surfaces offer
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, so the readback ran on both; both also offer
`R8G8B8A8_UNORM`, which is why the "a format the surface does not offer is refused" half of one
case reports that it did not run rather than asserting.

**The command list's states are one `enum class` now, and the plan's premise about the fifth was
wrong.** The plan deferred the refactor on the grounds that "the swapchain adds the fifth state —
a list holding an acquired image". It does not: an acquired image is a `TextureHandle` in the
device's own texture pool, so a list rendering into one is in exactly the state it is in for any
other texture, and the swapchain adds no list state at all. The fifth state was already there —
`Rendering`, the sub-state Task 3 tracked in a fourth bool — and folding it in turns the "+ pass"
column of Task 3's table into a row of the enum. That column is where one of Task 3's three holes
was.

Sixteen boolean combinations collapse to the five that were ever real, and three unreachability
arguments Task 3 had to make by hand stop needing to be made. Adding a sixth enumerator is a
compile error in **eight** switch sites — seven in `VulkanCommandList.cpp`, one in
`VulkanDevice.cpp`'s `ValidateForSubmit` — on both compilers. Verified by adding one:

- MSVC: `warning C4062: enumerator 'Monarc::RHI::Detail::VulkanCommandList::State::Suspended' in
  switch of enum 'Monarc::RHI::Detail::VulkanCommandList::State' is not handled`, fatal through
  `error C2220`.
- clang-cl: `error: enumeration value 'Suspended' not handled in switch [-Werror,-Wswitch]`.

**RenderDoc captures, and what was machine-verified from them.** One per adapter, in
`Build/Captures/` (outside version control — `/Build/` is git-ignored):

- `monarc-firstlight-nvidia-rtx3070ti_frame345.rdc`
- `monarc-firstlight-intel-uhd730_frame331.rdc`

`renderdoccmd capture` cannot trigger a capture on its own — RenderDoc's hook polls the capture
key inside `vkQueuePresentKHR` and requires the target's window to be the foreground one — so
each was produced by launching `Monarc.FirstLight --frames=900 --adapter=<name>` under
`renderdoccmd capture`, fronting the window and injecting F12 with `keybd_event`. **The contents
were machine-verified, not eyeballed**: `renderdoccmd convert -c xml` produces an XML dump of
every chunk, and both captures report

- exactly **one** `vkCmdBeginRendering`, with `colorAttachmentCount = 1`,
  `loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR`, `storeOp = VK_ATTACHMENT_STORE_OP_STORE`,
  `imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`, `renderArea = (0,0) 1280x720`,
  `layerCount = 1`, and
  `clearValue.color.float32 = [0.25098040699958801, 0.50196081399917603, 0.75294119119644165, 1]`
  — which is exactly `(64, 128, 192, 255) / 255`;
- exactly **two** `vkCmdPipelineBarrier2`, `VK_IMAGE_LAYOUT_UNDEFINED` →
  `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` and `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` →
  `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`;
- one `vkQueueSubmit2`, one `vkQueuePresentKHR`, and **no** `vkCreateRenderPass` or
  `vkCreateFramebuffer` chunk anywhere;
- `vkCreateSwapchainKHR` with the parameters listed above.

One caveat worth stating: the captures show
`imageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`, and
`Monarc.FirstLight` asks for colour-attachment only. RenderDoc patches the transfer-source bit in
so that it can save the backbuffer; that value is RenderDoc's, not Monarc's.

**Mutation experiments.** Each was rebuilt from a touched source, with `ninja` confirmed to have
recompiled rather than reporting "no work to do", and reverted afterwards:

| mutation | result |
|---|---|
| `QueryClientSize` reads `GetWindowRect` instead of `GetClientRect` | 7 of 18 window cases red, 9 assertions — including the maximise case's "no wider than the monitor" and the minimise case's `IsEmpty()` |
| `Rebind` removed from `Window`'s move constructor | the resize case's `REQUIRE` red and the move case **CRASHED** with an SEH exception: `Create` returns by move, so without the rebind the platform writes through a dangling `Window*` immediately |
| `ReleaseClassIfUnused` removed from `Destroy` | 13 of 18 window cases red, 15 assertions, with a cascade of `RegisterClassExW failed with Win32 error 1410` |
| `WM_CLOSE` falls through to `DefWindowProcW` | "closing is a request" red on `IsOpen()` — **and a later case's class-registration assertion red, which was a real drift rather than the mutation**; see the commit that fixed it |
| `ChooseSwapchainImageCount` returns `minImageCount` | `ImageCount() > kFramesInFlight` red on both adapters, and the screen capture red as well: 2 images against 2 frames in flight starves the loop |
| `Acquire`'s "already acquired" refusal disabled | "acquiring twice without presenting is refused" red |
| `AdoptImage` claims it owns the swapchain image | `VUID-vkDestroyImage-image-04882` ("is a presentable image controlled by the implementation"), case CRASHED, exit `0xC0000409` |
| `ToVulkan(Format::B8G8R8A8_UNORM)` returns `VK_FORMAT_R8G8B8A8_UNORM` | both readbacks `(64, 128, 192, 255)`, **0 of 230400 pixels exact** |
| the test's clear colour changed to `(200, 30, 10)` | 11 assertions red across both readbacks and the screen capture, every reading `(10, 30, 200, 255)` |
| the acquire-semaphore reuse wait disabled | **no validation error and no red assertion that detects it** — see below |

**One guard has no observable failure, and saying so is the extent of the claim made for it.**
Removing the wait that retires an acquire semaphore before it is reused produced no validation
output and no assertion that caught it: in the lockstep steady state `IDevice::BeginFrame` has
already waited on the same timeline value, so the semaphore was retired anyway. The guard is for
the drifted case — an out-of-date acquire skips a submission and the two counters stop agreeing —
which this machine does not produce on demand. It stays, and it is written down as reasoned
rather than measured.

**Two other things Task 4 did not exercise, stated rather than implied.** Dragging a window
between this machine's monitors does **not** exercise `WM_DPICHANGED`: both are 1920x1080 and
`GetDpiForWindow` reports 96 on each, which the window tests read and log. And the
`VK_KHR_swapchain`-absent path in `VulkanSwapchainFactory::Create` cannot be reached here,
because both adapters offer the extension.

**Departures from the plan's file listing, recorded.** The plan names
`Private/Platform/Windows/VulkanSurface.cpp`, and that is the file added — separate from Task 2's
`VulkanPlatform.cpp`, so that "exactly one file in the backend includes `<Windows.h>`" names a
file whose whole job is the surface. Three private headers the plan does not name came with it:
`Private/VulkanSurface.h` (because a header that declares surface creation must include
`<vulkan/vulkan.h>`, and the platform .cpp has to define `VK_USE_PLATFORM_WIN32_KHR` before that
header is first seen — a constraint worth confining to one file rather than to everything that
reaches `Loader.h`), `Private/VulkanSwapchainFactory.h` (`VulkanDeviceFactory.h`'s shape, for the
same reason), and `Private/WindowPlatform.h` in `Monarc.Host.Windowed` (which is what lets
`Private/Window.cpp` be the platform-neutral half the plan names). The swapchain device tests
live in `Monarc.Host.Windowed/TestsDevice/` rather than beside the swapchain, because they need
both the backend and a window they can drive, and a tier-3 test reaching down to tier 2 is the
direction the module graph allows.

**Checkable claims.**
`grep -rniE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<windows\.h>' Source/` matches seven
lines: five in `Monarc.Core`'s platform layer, one in `Monarc.Host.Windowed`, one in
`Monarc.RHI.Vulkan`. No test in the repository includes it, which is what
`Detail::WindowTestHooks` is for. `grep -rniE "renderpass|framebuffer|render pass" Source/`
matches four lines, all comments — two in `Monarc/RHI/Device.h` and two in
`VulkanCommandList.cpp`.

**Counts, by suite, because the comparison with Task 3 is otherwise misleading.** Task 3's "109
device-free cases and 504 assertions" was `Monarc.RHI` (51 / 174) plus `Monarc.RHI.Vulkan`
(58 / 330) and did not include `Monarc.Host.Windowed`, which had three cases at the time. Now:

| suite | cases | assertions |
|---|---|---|
| `Monarc.RHI.Tests` | 55 | 189 |
| `Monarc.RHI.Vulkan.Tests` | 58 | 330 |
| `Monarc.Host.Windowed.Tests` | 18 | 98 |
| `Monarc.RHI.Vulkan.DeviceTests` | 38 | 384 |
| `Monarc.Host.Windowed.DeviceTests` | 15 | 884 |

So **131 device-free cases / 617 assertions** and **53 device-required / 1268** across the three
A3 modules. All ten CTest entries pass on all six presets with zero warnings, and both `gpu`
entries report **Skipped** when `--vulkan-library=` is pointed at a name that cannot resolve.

## Verification gates

Five of M0's thirteen gates are implemented and running under CTest as
`Architecture.Gates`: acyclicity (2), renderer package boundary (3), platform containment
(10), apps are graph leaves (12), and module layout (13).

**Gate 3 stopped passing vacuously in A3 Task 1.** It had nothing real to forbid until then:
every include prefix in its list named a module Monarc had not written. `Monarc.RHI` and
`Monarc.RHI.Vulkan` are now tier 2, `Monarc.Host.Windowed` is the project's first tier 3
module, and `Monarc/Host/` is in the list — so the gate polices a boundary that two existing
modules sit either side of, and it was made to fail on purpose before being trusted.

The layout gate was numbered 7 from A1 until A3 Task 1, which collided with M0's own gate 7
(world-kind parity), so `ctest` printed one rule's name under another's number. M0's table is
the authority for these numbers and had no row for layout at all; it is 13 in both places now.

The remaining gates need modules that do not exist yet and are added against this same
harness: export purity (1), cook determinism (4), cook incrementality (5), asset identity
across rename (6), world-kind parity (7), export explainability (8), headless purity (9), and
schema migration (11).
