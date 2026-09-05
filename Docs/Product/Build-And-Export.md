# Build and Export

`Monarc.Build` assembles a shipped game: only the code a target needs, only the assets it
references, and an account of why each is present.

## What export produces

```
MyGame-Windows/
  MyGame.exe            game module + runtime engine modules only
  Content.mpk           cooked assets actually referenced
  manifest.json         what is here, and why
```

No editor code. No cooking code. No source assets. No unreferenced cooked assets.

## How code is stripped

Not by stripping. By never linking.

The [module graph](../Architecture/Module-Graph.md) declares each module's `KIND`. A game target
links `Runtime` modules and the game module; `Tool` and `Editor` modules are absent from the
link, not excluded from a superset. This is why
[ADR-0001](../Architecture/Decisions/ADR-0001-layered-modules.md)'s kind rule is enforced by the
build rather than by convention — export purity is a consequence of the module graph being real.

A [verification gate](../Milestones/M0-First-Light.md#verification-gates) asserts that no
`Monarc::Editor` or `Monarc::Cook` symbols appear in a shipped binary. A second asserts that a
headless build contains no graphics-API symbols at all.

## How assets are selected

By reachability. `Monarc.Build` starts from root references — the scenes and assets a project
declares as entry points — and walks the dependency records that
[cooking produced](../Content/Asset-Pipeline.md#dependencies-and-explaining-an-export). Anything
unreachable is not included.

This is only possible because dependencies are recorded as data at cook time. An engine that
discovers dependencies at load time cannot know, before shipping, what a game will need.

## Explaining an export

The vision requires that exporting be able to explain why an asset was included and what the
final game actually contains. Since dependencies are data, this is a graph query:

```
$ monarc explain Content/Textures/Barrel_Albedo.png

Content/Textures/Barrel_Albedo.png  [GUID 7f3a...c210]  412 KB cooked
  <- Content/Materials/Barrel.mmat        (base colour)
     <- Content/Models/Barrel.gltf        (material slot 0)
        <- Content/Scenes/Warehouse.mscene (actor "Barrel_03")
           <- ROOT: project start-up scene
```

The same query answers the more common and more useful question: *why is my game 4 GB?* A
manifest ordered by cooked size, each entry explainable, turns that from an investigation into a
lookup.

## Reproducible builds

An export is reproducible because its inputs are:

- **Cooked assets** are content-addressed and deterministic, so two clean cooks produce
  identical bytes ([ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md)).
- **Code** builds through pinned toolchains via CMake presets, not ambient `PATH`
  ([ADR-0011](../Architecture/Decisions/ADR-0011-build-system.md)).
- **The manifest** records engine version, cooker version, target platform, and the module graph
  used.

Two exports of the same commit should be byte-identical. Where they are not, the manifest is
what makes the difference diagnosable.

## Scope in M0

Export a Windows standalone from the M0 project: game executable, cooked assets reachable from
the start-up scene, and a manifest. `monarc explain` for any included asset. The export runs and
displays the same scene as the Editor and play-in-editor.

Deferred: multiple target platforms, patches and delta updates, DLC and additional content
packages, installer generation, code signing, asset bundling strategies beyond a single package,
compression tuning.

## Things that belong here but arrive later

Noted so later work has somewhere natural to belong: platform SDK management, continuous
integration and performance regression checks, backend conformance testing, crash diagnostics
and symbol servers, and safe handling of plugins and imported files.
