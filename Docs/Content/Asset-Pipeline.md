# Asset Pipeline

Monarc's runtime never loads a source art file. Content travels a deliberate path from what an
artist authors to what a game ships.

Design decision: [ADR-0008](../Architecture/Decisions/ADR-0008-asset-identity.md).

## The journey

```
source asset  ->  importer  ->  intermediate representation  ->  processing
              ->  platform-specific cooked asset  ->  runtime loading
```

| Stage | Module | What happens |
|---|---|---|
| Source | — | What the artist authored. Never read by the runtime |
| Importer | `Monarc.Cook` | Parses a format into Monarc's intermediate representation |
| Intermediate | `Monarc.Cook` | Format-agnostic, lossless-enough, platform-neutral |
| Processing | `Monarc.Cook` | Optimisation, validation, derived data |
| Cooked | `Monarc.Cook` -> disk | Platform-specific, runtime-ready bytes |
| Runtime | `Monarc.Assets` | Loads cooked bytes through handles |

The intermediate representation is what makes this more than a converter. Importers only need
to reach the IR; processing and cooking are written once regardless of source format.

## Identity

Every asset has a **GUID**, assigned on first import and stored in a `.meta` sidecar beside the
source file:

```
Content/Models/Barrel.gltf
Content/Models/Barrel.gltf.meta      <- GUID + import settings
```

References — between assets, and from scenes to assets — store the GUID, never the path.
Renaming or moving the source carries its `.meta` along and breaks nothing.

**`.meta` files are project data and must be committed.** Losing one orphans its GUID and breaks
every reference to that asset.

## Freshness, incrementality, determinism

A cooked artifact is addressed by a hash of:

```
source content + import settings + cooker version + target platform + engine version
```

This gives incrementality and determinism through one mechanism rather than trading them
against each other:

- Nothing changed means the key is identical, so the cook is a **no-op**.
- Two clean cooks on different machines produce **identical bytes**, so output can be cached,
  shared, and compared.

Timestamps are never authoritative. They may be used as a *hint* to avoid rehashing a large
file, but content decides.

Bumping the cooker or engine version invalidates everything. That is deliberate, and the reason
those versions must be meaningful rather than incidental.

## Dependencies, and explaining an export

Each cook records what the asset referenced and what it was built from. Those records are data,
which is what makes two otherwise hard things easy:

- **Export contains only what is reachable.** `Monarc.Build` walks the dependency graph from
  root references and includes nothing else.
- **The export can justify itself.** `monarc explain <asset>` prints the chain of references
  that pulled an asset in. See [Build-And-Export.md](../Product/Build-And-Export.md).

The same records later drive streaming, which is why they are recorded from the start even
though M0 does not stream.

## Failure, redirects, and change

The pipeline must behave well when things go wrong, because they will:

- **Failed imports** are recorded, not silently skipped. A failed asset keeps its identity, so
  fixing the source and reimporting restores every reference to it.
- **Redirects** let an asset be replaced or merged while old references continue to resolve.
- **Import settings** live in `.meta` and participate in the cache key, so changing a setting
  recooks exactly what it affects.
- **Tool and engine version changes** are part of the key, so upgrading never leaves stale
  artifacts behind.

## Scope in M0

Importers for glTF (static mesh) and common image formats. The IR, processing, and cooking path
for meshes and textures. Content-addressed incremental cache. Dependency records. `.meta`
sidecars with GUIDs and import settings. Runtime loading through
[handles](../Architecture/Decisions/ADR-0002-handles-not-pointers.md) with explicit load states.

Textures cook to uncompressed RGBA8 with mips in M0. **The cooked format supports compressed
formats from the start**, so adding BC7 later is a cooker change, not a format change — this is
the kind of thing that is cheap to allow for now and expensive to retrofit.

Deferred: streaming, asset bundles, audio and animation importers, LOD generation, texture
compression, reimport-on-change file watching.
