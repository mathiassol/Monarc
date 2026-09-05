# Editor

The Editor is the authoring environment, and it is the product — not a development aid attached
to one. It runs in-process with the engine and renders through Monarc's own renderer.

Design decision: [ADR-0013](../Architecture/Decisions/ADR-0013-editor-ui.md).

## The rule that governs everything here

> **Panels are views over engine data, not owners of logic.**

A panel may not hold authoritative state. Every panel reads through
[`Monarc.Reflect`](../Architecture/Decisions/ADR-0010-reflection.md), and every mutation goes
through the command stack that implements undo/redo.

Consequences worth being concrete about:

- The property inspector does not know what a `Transform` is. It walks properties from the type
  registry. A new component type appears in the inspector with no inspector code written.
- The outliner does not own the scene hierarchy. It displays the world's.
- Dragging an actor in the viewport issues the same command that typing a number in the
  inspector does. Undo works identically for both because there is one path.

This is what makes replacing the UI toolkit later a rewrite of *presentation* rather than
*behaviour*. It is also the discipline most likely to erode under pressure, because holding
state in a panel is always the shortest path in the moment.

## Structure

```
Monarc.Editor
  Model layer        selection, commands, undo/redo, scene editing   [toolkit-agnostic]
  Panels             viewport, outliner, inspector, content browser, log
  UI backend         Dear ImGui, rendered through Monarc.RHI
```

The model layer is the part intended to survive a toolkit change. The panels are thin. The UI
backend is explicitly transitional.

We do **not** abstract over ImGui's API. Wrapping an immediate-mode toolkit in a
toolkit-neutral facade produces a poor version of both models. The seam is at the panel/model
boundary, not at the widget call.

## Panels in M0

| Panel | Function |
|---|---|
| Viewport | Renders the editor world through `Monarc.Render`; camera navigation; selection |
| Outliner | The actor hierarchy; selection; parenting |
| Inspector | Properties of the selection, driven entirely by the type registry |
| Content browser | Assets in the project, their import state, and failed imports |
| Log | Engine log with categories and filtering |

## Undo/redo

A command stack. Every state-changing operation is a command that can apply and revert itself.
Property edits, actor creation and deletion, reparenting, and component addition all go through
it.

This exists in M0 rather than later for the same reason [versioning](../Content/Serialization.md)
does: retrofitting undo means finding every mutation path already written, and there is no way
to be sure you found them all. Building it first means every mutation path is created correctly.

## Play-in-editor

Play-in-editor creates a `Play` world and runs the same phases as a standalone game — see
[World-Model.md](../Runtime/World-Model.md#world-kinds). It is not a preview mode and not a
separate code path.

A [verification gate](../Milestones/M0-First-Light.md#verification-gates) asserts that the same
scene yields identical actor and component state in editor, play, standalone, and headless
worlds. Play-in-editor that runs different code from standalone is play-in-editor that lies, and
the drift compounds.

## Dogfooding the renderer

The Editor renders through `Monarc.Render` and `Monarc.RHI`. This is deliberate: it means the
person best placed to fix a renderer bug encounters it first, every day, in normal work. An
editor built on a separate UI stack removes the main mechanism by which engine bugs surface.

## Scope in M0

The five panels above, the command stack with undo/redo, scene save and load, asset import
triggering a cook, and play-in-editor.

Deferred: gizmos beyond basic translation, multi-select editing, docking layouts and layout
persistence, a first-party retained-mode UI, accessibility work (which
[ADR-0013](../Architecture/Decisions/ADR-0013-editor-ui.md) notes cannot be retrofitted onto
ImGui and is a real cost of this choice), live iteration beyond asset reimport.
