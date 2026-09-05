# Hub

The Hub is the entry point to Monarc:

```
Monarc Hub  ->  install or choose an engine version  ->  open the Monarc Editor
            ->  make a game  ->  run it  ->  export a standalone game
```

It manages engine versions and projects. It contains no engine.

## Responsibilities

| Responsibility | Notes |
|---|---|
| List installed engine versions | Which versions exist locally, and which is default |
| Install and remove versions | Deferred past M0 — M0 uses a locally built engine |
| Create a project | From a template: folder layout, project file, a game module that compiles |
| List and open projects | Including which engine version each was last opened with |
| Engine-version compatibility | Warn before opening a project with a different version than it was authored against |
| Launch the Editor | With the correct engine version and project |

## Project layout

A project created by the Hub looks like this:

```
MyGame/
  MyGame.monarcproject      project file: name, engine version, settings
  Content/                  source assets, each with a .meta sidecar
  Source/
    Game.MyGame/            the game module (C++, KIND Runtime)
  Config/                   project configuration
  DerivedData/              cook cache and intermediates  [not version controlled]
  Saved/                    logs, editor state             [not version controlled]
```

`Content/` and `Source/` are the project. `DerivedData/` and `Saved/` are reproducible and
excluded from version control — which is only true because
[cooking is deterministic](../Content/Asset-Pipeline.md#freshness-incrementality-determinism).

## Engine-version compatibility

A project records the engine version it was authored against. Opening it with a different
version is allowed but warned about, because the
[engine version participates in the cook cache key](../Architecture/Decisions/ADR-0008-asset-identity.md)
and in [serialized schema versions](../Content/Serialization.md) — a different engine may
recook everything and may migrate scene data.

Making this visible in the Hub is the point. A silent recook of an entire project is a
surprising experience, and a silent schema migration is worse.

## Technology

C++ with Dear ImGui, sharing the Editor's UI scaffolding
([ADR-0013](../Architecture/Decisions/ADR-0013-editor-ui.md)).

This is a pragmatic choice rather than an ideal one. The Hub is a launcher: it has no engine
needs, no viewport, and no performance requirements, so almost any technology would serve. Using
the Editor's scaffolding means no new toolchain and no new dependency.

**It is also the most replaceable component in Monarc.** A web or native shell could take over
the Hub without touching anything else, and if the Hub ever needs to look like a consumer
product rather than a developer tool, that is the likely path. Recording that here so the choice
does not calcify by default.

## Scope in M0

Create a project from a template, list projects, list locally available engine versions, launch
the Editor with a chosen project. The created project must compile and open without manual
intervention — that is the actual test of this milestone's Hub work.

Deferred: downloading and installing engine versions, multiple concurrent installs, project
upgrade workflows, templates beyond one, plugin and marketplace concerns.
