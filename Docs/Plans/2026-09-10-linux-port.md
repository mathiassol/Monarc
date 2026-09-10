# The Linux port

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Monarc builds and tests on Ubuntu, in a Cursor cloud environment, from a clean clone with no hand-configured machine state — and `Monarc.Jobs` runs under ThreadSanitizer for the first time.

**Not the goal:** a window. See "What this port is not" below; it is the only hard part of a Linux port and this machine cannot host it.

**When:** after Phase A4 completes and merges. The reasoning is in "Why after A4".

**Tech Stack:** C++23, Clang and GCC on Ubuntu, CMake + Ninja, doctest, Vulkan 1.3 through pinned headers.

---

## Why this is much smaller than a Linux port sounds

The whole platform-coupled surface of the engine is **1,668 lines across 9 files**, measured:

```
Monarc.Core/Private/Platform/Windows/    820   File, Guid, Library, Path, Thread, Time
Monarc.Host.Windowed/…/Windows/          674   Window.cpp  ← out of scope, see below
Monarc.RHI.Vulkan/…/Windows/             174   VulkanSurface.cpp, VulkanPlatform.cpp
```

Everything else — `Monarc.Jobs`, `Monarc.RHI`'s pure half, **all of `Monarc.Render`**, and most of
`Monarc.Core` — has no platform coupling at all. That is not luck: ADR-0016 confines platform code
to `Private/Platform/<Platform>/` directories, gate 10 has enforced it since A2c, and A3 Task 5
renamed the gate to say so. **There are no platform `#ifdef`s to hunt down**, and a Linux build
begins by simply not compiling those nine files.

Four pieces of the port are already built:

- **`monarc_platform_directory()` already has a `Linux` branch.** The mechanism is proven today —
  `Monarc.Core` carries a `Platform/Mac/Time.cpp` containing `#error`, the build excludes it, and
  A2c verified `Platform\Mac` appears nowhere in the generated Ninja file.
- **`MonarcTargetOptions.cmake` already has the non-MSVC branch**: `-Wall -Wextra -Wpedantic
  -Werror`. `/w44062` needs no counterpart — `-Wswitch` is on with `-Wall` and already fatal.
- **Vulkan headers come from pinned `FetchContent`, not an SDK.** Taken because CI has no SDK; on
  Linux it means no distro-package dance for the headers at all.
- **The Vulkan loader is opened by name at run time** through `Platform::Library`, and the name is
  chosen inside the backend by `Detail::VulkanLibraryName()`. `libvulkan.so.1` is one line.

## Why after A4, and not after Task 3

- **A4 Task 4 rewrites `Monarc.FirstLight` and changes how the window is used.** A Linux stub
  written now is a stub against a moving target.
- **After A4, `Monarc.Render` is complete** — the largest pure-code module in the engine, and the
  most valuable code in the renderer. A Linux leg landing then covers the barrier derivation and
  the compile stages on day one, which is the strongest possible first result.
- **A4 Task 3 is the phase's crown jewel and is entirely device-free.** A port would not help it
  and interrupting for one would cost focus on the task that most needs it.
- A4's own Task 5 merges the phase and writes its Status.md section. A port is naturally the next
  chunk of work, not a detour inside a phase.

## What the cloud environment changes — and it is mostly good news

**It removes the hard part.** A Cursor cloud environment has no display server, so X11 and Wayland
are not on the table — and the 674-line `Window.cpp` was the *only* piece of this port with real
design in it. What remains is mechanical.

**It raises the bar on reproducibility, which this project should welcome.** The environment is
ephemeral and the requirement is "repo to working". Nothing may depend on hand-configured machine
state — which is a strictly higher standard than the Windows setup currently meets:
`Docs/Status.md` records two machine-specific traps there (a build outside `vcvars` reporting
success by saying nothing, and `cmake --preset` poisoning the Clang caches without LLVM on
`PATH`). A Linux environment that is declared in the repo cannot acquire either.

**It may still have a Vulkan device, and that would be a real win.** Mesa's **lavapipe**
(`VK_DRIVER_ID_MESA_LLVMPIPE`) is a software Vulkan implementation that has been 1.3-conformant
since Mesa 23, and Vulkan 1.3 core is exactly what Monarc requires — timeline semaphores, dynamic
rendering, `synchronization2`, descriptor indexing. If it is present, the device suite runs:
device creation, command lists, barriers, and **A3's offscreen colour readback**, which needs no
surface and no display. It would also be a **third vendor** after NVIDIA and Intel, which is
precisely what capability tiers exist to keep honest rather than theoretical.

**Do not assume that.** Task L2 exists to measure it, using the probe A3 already built for this
exact purpose, and everything after L2 is written against what it reports rather than against the
paragraph above.

---

## What this port is not

**A real window.** Xlib or xcb is roughly 500–700 lines to replace `Window.cpp`, and ADR-0014 puts
windowing on the *own it* side, so no SDL or GLFW. That work needs a desktop Linux machine with a
display server and belongs in its own pass — and native Wayland (`wl_registry`, `xdg-shell`,
`libdecor` for decorations) is larger again and should be deferred past that.

Two consequences to state rather than discover:

- **The swapchain, present and resize paths stay Windows-only for now.** `VK_EXT_headless_surface`
  is worth a look in L4 if lavapipe is present, but it is an investigation and not a plan.
- **The on-screen capture test is Windows-only, and on Linux would be X11-only.** It uses `BitBlt`
  from the screen DC; X11 has `XGetImage`, but Wayland needs a desktop portal. That is a statable
  gap, not a blocker — the swapchain readback is the assertion that proves the pixels, and it
  needs no display.

---

## Task L1: `Monarc.Core` on Linux, and everything pure that follows

- [ ] `Source/Monarc.Core/Private/Platform/Linux/` — the six files. The Win32 versions total 820
      lines; the POSIX ones should be smaller: `clock_gettime` for `Time`, `/`-separator rules for
      `Path`, `dlopen`/`dlsym`/`dlclose` for `Library`, pthreads for `Thread`/`Mutex`/
      `ConditionVariable`, `open`/`read`/`write`/`stat` for `File`, and `getrandom()` for `Guid`.
- [ ] `Platform::Library::SystemLibraryName()` and `SystemSymbolName()` need Linux answers — they
      exist so a test can load a real library and resolve a real symbol without naming a
      platform itself, so pick something as unconditionally present as `kernel32.dll` is.
- [ ] Add `GNU` to `MONARC_SUPPORTED_COMPILERS` in `CMakeLists.txt` if GCC is wanted as the second
      compiler; Clang on Linux already reports `Clang` and passes the check.
- [ ] Linux presets. The existing four Clang presets pin `clang-cl` and `CMAKE_C_COMPILER` for
      reasons that are Windows-only; these need their own, and the sanitizer block's Windows half
      (`CMAKE_MSVC_RUNTIME_LIBRARY`, `-print-resource-dir`, copying `clang_rt` DLLs beside each
      binary) needs a Linux branch — which is far simpler: `-fsanitize=address
      -fno-omit-frame-pointer` and done.
- [ ] `Tools/run_death_test.py` asserts a non-zero exit. On Linux `__builtin_trap()` raises
      `SIGILL`, so the harness needs `WIFSIGNALED` rather than an exit code. The eleven guards it
      covers are otherwise platform-neutral.

**Verification.** Everything device-free green on Linux: `Monarc.Core`, `Monarc.Jobs`,
`Monarc.RHI`, **`Monarc.Render`'s whole suite**, the architecture gates, the doc-link checker.
That is the majority of the test suite, and it is the proof that the platform boundary gate 10
polices is real rather than merely clean-looking.

Then deliberately: confirm `Platform\Windows` appears nowhere in the generated Ninja file, the way
A2c confirmed it for `Mac`. **Gate 10 has never been tested by a second platform actually
building** — this is the first time its claim can be checked rather than asserted.

## Task L2: the probe — find out what the machine has, then plan against it

- [ ] `Source/Monarc.RHI.Vulkan/Private/Platform/Linux/VulkanPlatform.cpp` —
      `VulkanLibraryName()` returning `libvulkan.so.1`, and `PlatformSurfaceExtensionName()`.
      **This one file makes the whole Vulkan backend build on Linux**: the loader, instance,
      adapter enumeration, device, command lists, barriers and the readback are all
      platform-neutral already.
- [ ] `Source/Monarc.Host.Windowed/Private/Platform/Linux/Window.cpp` — a stub whose
      `WindowPlatform::Create` returns `ErrorCode::Unsupported` with a message saying so, and the
      other five entry points refusing in kind. Without it the module does not link; with it,
      `Monarc.FirstLight` reports honestly that this platform has no window yet. Task 1 of A3 used
      exactly this shape and it is what "not yet" looks like from a caller's side.
- [ ] A Linux surface stub, likewise refusing, so the backend links.
- [ ] **Run `Monarc.RHI.Vulkan.Probe` and `Monarc.FirstLight --adapters`, and record what the box
      actually has in `Docs/Status.md`: loader present or not, instance version, adapters, driver
      IDs. No guesses.** This is A3 Task 5's method, and A3's own CI answer turned out to be more
      specific than "no Vulkan" — the runners have `vulkan-1.dll` and a loader reporting 1.3.301
      with no ICD registered, which falsified a recorded assumption.

**Everything in L4 is written against this measurement.** If lavapipe is present, say which
version and which driver ID; if it is absent, say what installing `mesa-vulkan-drivers` changes
before deciding whether to.

## Task L3: ThreadSanitizer — the reason this port is worth doing

`Docs/Status.md` records this as a gap that **cannot be closed on Windows**: `clang-cl` rejects
`-fsanitize=thread` and LLVM ships no Windows runtime. So `Monarc.Jobs` is verified by
invariant-shaped tests and 200 consecutive runs, which that file itself calls "corroboration, not
proof" and "evidence gathered in the absence of a sanitizer, not a substitute for one".

- [ ] A `linux-tsan` preset, and `MONARC_SANITIZE` gaining `thread` with the Linux-only
      restriction stated where the option is declared.
- [ ] Run `Monarc.Jobs`' suite under it, including the concurrency cases that repeat their
      scenario many times within one case.
- [ ] **Verify TSan actually reports**, the way ASan and UBSan were verified on Windows: a scratch
      program with a deliberate data race must produce `WARNING: ThreadSanitizer: data race` with
      a non-zero exit. A leg that reports nothing is not a gate — UBSan needed
      `-fno-sanitize-recover=undefined` for exactly this reason.
- [ ] Record the result in `Docs/Status.md` and update `Docs/Runtime/Threading.md`, which says
      three separate times that no ThreadSanitizer exists for this platform and that the
      single-mutex design was chosen partly because *"nothing mechanical would catch a mistake in
      anything more elaborate"*. If TSan comes back clean, that argument weakens and the file
      should say so. If it does not come back clean, that is the most valuable finding of the port.

**Expect this to find something, and treat that as the point rather than as a setback.**

## Task L4: device tests, against what L2 measured

- [ ] If a device exists: run `Monarc.RHI.Vulkan.DeviceTests` — device creation on every
      deduplicated adapter, command lists, the barrier suite, and the offscreen colour readback
      asserting exactly `(64, 128, 192, 255)`. A software rasteriser producing those bytes would
      be a genuinely strong result, and a *different* answer on a third vendor is a finding.
- [ ] Whatever cannot run must **skip** and say so — `SKIP_RETURN_CODE 77` is already the
      mechanism and A3 proved it works on a machine with no device.
- [ ] Investigate `VK_EXT_headless_surface` for the swapchain path only if L2 found a device, and
      only far enough to say whether it is viable. Do not build a headless swapchain on spec.

## Task L5: the environment, declared in the repo

- [ ] Dependencies declared where a clean clone can act on them — a devcontainer, a setup script,
      or both. Whatever a fresh cloud environment needs, in the repo rather than in someone's
      shell history.
- [ ] A Linux CI leg. The existing workflow is six Windows presets plus a docs job; Linux should
      add at least a Clang build, and the TSan leg if L3 lands. Note that `.github/workflows/ci.yml`
      already has an `ubuntu-latest` docs job, so the runner shape is proven.
- [ ] Update `Docs/Status.md`'s "Verified environment" with the Linux toolchain versions, measured;
      close or amend the ThreadSanitizer entry under Known gaps; and add the Linux platform files
      to `Docs/Architecture/Module-Graph.md`'s account of where platform code lives.
- [ ] Update `Docs/Architecture/Decisions/ADR-0012-backend-rollout.md` if the port changes anything
      it says about platform reach — it schedules Metal on macOS and says an abstraction validated
      against one backend is not validated. The same argument applies to one OS.

---

## Definition of done

- A clean clone in a fresh cloud environment builds and passes the device-free suite with one
  documented command.
- `Monarc.Render`, `Monarc.Jobs`, `Monarc.RHI` and `Monarc.Core` are green on a second OS and a
  second standard library.
- Gate 10's platform-boundary claim has been checked by a second platform building, not asserted.
- `Monarc.Jobs` has run under ThreadSanitizer, and TSan has been verified to report.
- What the machine has is recorded as a measurement, and what cannot run is skipped visibly.
- Nothing depends on hand-configured state on either machine.

## Open questions this plan does not settle

- **GCC as a second Linux compiler, or Clang only.** Two compilers on Windows has repeatedly paid
  for itself — ADR-0003 keeps the Clang build for exactly that reason, and A3 found a defaulted
  comparison MSVC accepted and clang-cl rejected. GCC would extend that, and it is also the
  compiler most likely to disagree about something in `Monarc.Core`'s containers. Decide in L1
  once the POSIX layer builds under one of them.
- **Whether `Monarc.Host.Windowed` should be linked at all on a headless platform**, or whether a
  refusing stub is the honest shape. The stub is cheaper and keeps `Monarc.FirstLight` building;
  excluding the module would mean a second app target. L2 decides, with the stub as the default.
- **Whether the Windows machine remains the primary.** A cloud environment that runs the whole
  device-free suite plus TSan is a strong second home, but the swapchain, the window, the
  RenderDoc captures and both real GPUs stay on Windows. This plan assumes Windows remains
  primary and Linux becomes the verification machine; that assumption is worth revisiting once
  L3's result is known.
