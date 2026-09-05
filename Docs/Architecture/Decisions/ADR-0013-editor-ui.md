# ADR-0013: Editor UI — ImGui scaffolding behind thin panels

**Status:** Accepted — 2026-09-05

## Context

The Editor is not a development aid; it is the product. Its UI technology is a long-lived
commitment, and the vision's measures of success include how quickly content appears in the
editor and how little friction there is in iteration.

Three directions were considered seriously, and the trade is between time-to-first-proof and
quality ceiling.

## Decision

**The Editor is native C++ and renders through Monarc's own renderer.** Dear ImGui provides the
UI scaffolding initially, with a first-party retained-mode UI as the intended destination.

The decision that makes the migration survivable is not about ImGui at all. It is this:

> **Editor panels are views over engine data, not owners of logic.**

Concretely, a panel may not hold authoritative state. Every panel reads through
[`Monarc.Reflect`](ADR-0010-reflection.md) and every mutation goes through the command stack
that also implements undo/redo. A property inspector does not know what a `Transform` is; it
walks properties. An outliner does not own the scene hierarchy; it displays it.

The consequence: replacing the UI toolkit rewrites *presentation*, not *behaviour*. Undo/redo,
property editing, selection and scene manipulation live in the editor's model layer, which is
toolkit-agnostic.

We deliberately do **not** abstract over ImGui's API. Wrapping an immediate-mode toolkit in a
toolkit-neutral facade produces a bad version of both models. The seam is at the panel/model
boundary, not at the widget call.

**The Hub** is also C++ with ImGui, sharing the Editor's scaffolding. It manages engine
versions and projects and has no engine needs, so it is the most replaceable component here —
a web or native shell could take over later without touching anything else.

## Consequences

**Good.** The Editor dogfoods the renderer, which is how renderer bugs get found by the person
best placed to fix them. No IPC boundary through the viewport, gizmos, or drag-and-drop —
which is where a web-based editor's latency and integration problems concentrate. ImGui is fast
to build panels in, so the first end-to-end proof arrives sooner. No new toolchain.

**Costs.** ImGui's default aesthetics are not a shippable product look, and it resists deep
visual customisation — so the migration to a first-party UI is a *when*, not an *if*.
Accessibility is poor and largely cannot be retrofitted onto ImGui, which matters given the
vision lists accessibility explicitly. Retained-mode layout, text input and internationalisation
will all need to be built eventually.

**The discipline that must hold.** The moment a panel starts holding authoritative state, the
migration cost stops being bounded. This is the thing to watch in review, because it is much
easier to notice early than to undo later.

## Alternatives considered

**A first-party retained-mode UI from day one.** Highest ceiling, no migration. Rejected: it
delays the first end-to-end proof substantially, and we would be designing a UI framework
before knowing what the Editor actually needs from one. Building panels in ImGui first is also
research — it tells us what the real UI must support.

**Editor UI in a web stack (Tauri or Electron) with the engine viewport embedded.** Excellent UI
velocity, polish and accessibility, and a large ecosystem. Rejected on two counts: the viewport,
gizmos, drag-and-drop and iteration loop all cross an IPC boundary, and the Editor stops
dogfooding the renderer — removing the main mechanism by which engine bugs surface.

**A native platform toolkit (Qt, WinUI).** Good widgets, poor fit for a GPU viewport with custom
interaction, and Qt's licensing is a poor match. Rejected.
