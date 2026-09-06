# ADR-0016: Platform code is selected by directory, not by `#ifdef`

**Status:** Accepted — 2026-09-06

## Context

`Monarc.Core`'s platform layer is the boundary between the engine and the operating system:
files, paths, time, threads, dynamic libraries, and entropy for GUIDs. Every one of those
needs a different implementation per platform, and macOS is expected within a year
([ADR-0012](ADR-0012-backend-rollout.md)).

There are two ways to arrange that, and the choice is hard to reverse once a dozen files
exist.

[Gate 10](../../Milestones/M0-First-Light.md#verification-gates) already forbids
platform-conditional compilation outside the platform layer. This decision is about what
happens *inside* it.

## Decision

**Per-platform directories, selected by the build. No `#ifdef` on platform macros anywhere,
including inside the platform layer.**

```
Source/Monarc.Core/
  Include/Monarc/Core/Platform/     platform-neutral interfaces only
    File.h  Path.h  Time.h  Thread.h  Library.h  Guid.h
  Private/Platform/
    Windows/                        Win32 implementations
    Mac/                            (when macOS arrives)
    Linux/                          (if it ever does)
```

`monarc_module()` globs `Private/Platform/<CurrentPlatform>/` and **excludes every other
platform directory**, so a file that cannot compile here is never handed to the compiler.

Public headers stay platform-neutral. Where a type must carry platform state — a file
descriptor, a thread handle — it holds an opaque fixed-size field rather than a conditional
member, so the header's layout does not depend on which platform is building.

Consequently, gate 10's exemption for `Platform/` should end up **unused**: there is no
legitimate reason for a platform macro to appear anywhere, and the exemption exists only as
a pressure valve.

## Consequences

**Good.** "What does Monarc do on Windows?" is answered by listing a directory. Adding macOS
means adding files, not editing every existing one and re-testing Windows. A Win32
implementation cannot accidentally acquire a Mac-only branch, because there is nowhere to
put one. Each file reads as ordinary code for one platform rather than as three interleaved
programs — which matters most in exactly the files that are hardest to test, since only one
platform's branch is ever exercised on a given machine.

The subtler benefit: with `#ifdef`s, *unbuilt* branches rot silently — they are not compiled,
so they are not even syntax-checked, and the rot is discovered only when someone tries that
platform. Directory selection makes that visible: the Mac directory is plainly not built
here, rather than appearing to be covered.

**Costs.** Interface changes must be applied to every platform directory, and a signature
changed in only one of them fails to build on a machine nobody has yet — the same rot,
relocated to a place where it is at least obvious. Small shared helpers between platforms
need a deliberate home rather than falling out of a shared file. `monarc_module()` grows
platform-aware globbing, a little more build logic in exchange for none in the source.

**And a real one:** two implementations of the same interface can silently diverge in
*behaviour* while both compiling. Only a shared test suite run on both platforms catches
that, and there is no second platform yet. So the platform tests must be written against the
interface, never against Windows specifics — that discipline is the actual guarantee here,
and it is worth stating because it is not enforced by anything.

## Alternatives considered

**`#ifdef` blocks inside one file per concern.** Fewer files, and a reader sees all
platforms' behaviour side by side, which genuinely helps when they must stay in step.
Rejected because the unbuilt branches are not even syntax-checked, so they decay
invisibly — and because a platform layer is precisely where the temptation to add "just one
more" conditional is strongest, until each file is three programs interleaved.

**A pimpl or virtual `IPlatform` interface with runtime dispatch.** Would allow more than one
platform backend in a single binary, which is useful for nothing Monarc does — the platform
is fixed at compile time. Rejected as paying indirection for a capability with no consumer.

**Rely on the compiler's own target macros with no directory structure**, i.e. one big
`Platform.cpp`. Rejected for the same reason as the first alternative, more so.
