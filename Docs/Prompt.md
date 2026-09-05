# Monarc Vision

Monarc is a from-scratch game engine and a complete product, not a library that sits beside an otherwise separate game. The experience should be:

`Monarc Hub → install or choose an engine version → open the Monarc Editor → make a game → run it → export a standalone game`

The long-term ambition is to stand beside Unreal Engine, Unity, and Godot. The more immediate ambition is to make the distance between “I want this in my engine” and “it works” unusually short, while building on architecture that remains stable as the engine, its developers, and the technology around it change.

You are the primary engineering partner for this project. You are a frontier-level model with the ability to reason across a large system, research unfamiliar areas, make sound technical choices, and carry work through. Use that ability. I do not want to specify every implementation detail or turn the project into a checklist of commands. Understand the intent behind this vision, make sensible choices, and bring me the decisions that genuinely belong to me. Challenge an assumption when doing so protects the project or reveals a better path.

## The shape of the engine

Monarc should have clear layers with clear ownership:

- the Hub manages engine versions and projects;
- the Editor is the authoring environment;
- the Engine Runtime provides the systems a game actually needs;
- the Renderer is a reusable C++ package with a deliberate public interface;
- the RHI and graphics backends translate that interface to Vulkan, D3D12, and Metal;
- the asset toolchain turns source content into optimized, platform-specific runtime data;
- the build and export system assembles only the code and data required by a target.

The Editor, Hub, internal tools, and development-only systems should not be pulled into a shipped game simply because they exist in the same repository. The build system and module graph should make this separation real and visible. A dedicated server or headless runtime should be able to exist without graphics when a project needs one.

This separation is about more than organization. It should make the engine replaceable in parts, keep compile and iteration times under control, and allow a project to evolve without repeatedly starting over.

## Rendering

The Renderer should be designed as though it could be consumed as a high-quality C++ package. Its normal interfaces should not expose Vulkan, D3D12, or Metal concepts unnecessarily. Backend code belongs behind the RHI, and the high-level renderer should deal in useful engine concepts rather than scattered API calls.

The RHI should be small, coherent, and honest about the differences between graphics APIs. Monarc should share behavior where the APIs genuinely overlap, use capability tiers where hardware differs, and give advanced features a clear extension path rather than pretending that every platform is identical.

A render graph should be central to rendering. It should understand resource dependencies, lifetimes, transient allocations, aliasing, barriers, synchronization, queue usage, execution order, pass culling, and parallel recording. It should also be inspectable and debuggable. High-level rendering code should describe what it needs; synchronization should not be scattered through every feature as manual bookkeeping.

The rendering foundation also includes shader authoring and compilation, reflection, resource binding conventions, pipeline state and shader caches, device loss, presentation, frame pacing, color and HDR behavior, GPU memory, validation, and capture/debug tooling. Those pieces are part of the renderer’s architecture even if some arrive later than the first triangle.

## Content and assets

Monarc should not be built around the idea that the runtime directly loads artists’ source files. The intended content journey is:

`source asset → importer → intermediate representation → processing → platform-specific cooked asset → runtime loading`

The pipeline should be data-oriented, incremental, deterministic, cacheable, and prepared for streaming. It should understand dependencies, stable identity, import settings, platform differences, tool versions, engine versions, redirects, failed imports, and the difference between source content and generated artifacts. Exporting should be able to explain why an asset was included and what the final game actually contains.

Asset identity should survive renames, moves, reimports, and engine evolution. Runtime code should use stable handles or resource identities with explicit loading, unloading, reload, and failure behavior rather than treating raw pointers as the identity of content.

Serialization and versioning belong here from the beginning. File formats should have intentional schemas, compatibility rules, migrations, corruption checks, and room to change. Asset data, saved-game data, and network data may share foundations, but they should not be accidentally coupled.

## Runtime, world, and gameplay

The initial gameplay model should follow the useful part of the Unreal approach: Actors provide identity and gameplay presence, while Components provide capabilities. Relationships matter just as much as isolated objects, so parent-child structure, references, ownership, attachments, queries, and communication should be designed deliberately.

Actor and Component should describe how developers think about and author gameplay. They do not automatically dictate how every runtime system stores its hot data. Monarc should be free to use data-oriented or hybrid representations where they improve cache behavior, iteration, memory use, or parallelism.

The frame and simulation model deserves the same care as the renderer. Input, fixed-step simulation, variable-step systems, transforms, visibility, animation, physics, gameplay, render extraction, GPU work, and presentation should have a comprehensible relationship. Editor worlds, play-in-editor worlds, network worlds, and headless worlds should not require unrelated assumptions.

Threading should be a foundation rather than an optimization added later. Expensive work belongs in a capable scheduler with jobs, dependencies, priorities, ownership rules, cancellation where useful, thread-affinity handling, and good visibility into what is running. The main thread should have a deliberate role instead of becoming the place where every system eventually waits.

Memory should also be intentional from day one: CPU memory, GPU memory, frame memory, persistent memory, asset memory, and transient memory should have understandable ownership and lifetime. Linear, pool, frame, general, GPU, and transient allocation strategies can evolve, but memory budgets, alignment, reclamation, profiling, and telemetry should not be afterthoughts.

## Extensibility and change

Monarc will initially ship with native C++ game code. At the same time, its public gameplay and engine-facing APIs should be designed so another language can be added later. Reflection, properties, handles, serialization, events, and host boundaries should not assume that C++ will be the only way to create logic forever.

Whether the future language is C#, Lua, Verse-like, or something created specifically for Monarc is a later decision. The important early decision is to create a good boundary so that the choice remains open.

The same principle applies to plugins, modules, platform services, physics, audio, UI, input, animation, navigation, networking, and other engine systems. Build replaceable seams where they are valuable, but do not create abstraction for its own sake. A seam should make a real future change easier.

Networking should be an engine boundary even before multiplayer is implemented. Gameplay should not fundamentally depend on assumptions that only make sense when one local machine owns the whole truth. The eventual architecture should be able to support authoritative simulation, clients, replication, prediction or other appropriate models without forcing a rewrite of the world model.

## The whole product

The engine is more than rendering and runtime code. The vision includes the unglamorous systems that make an engine trustworthy and pleasant to use:

- project creation and engine-version compatibility;
- build configuration, dependency management, platform SDKs, and reproducible builds;
- import and cook tooling, shader tooling, packaging, patches, and distribution;
- logging, assertions, profiling, memory reports, crash diagnostics, and GPU debugging;
- testing, continuous integration, performance regression checks, and backend conformance;
- safe handling of plugins, imported files, packages, scripts, and network data;
- editor workflows such as inspection, undo/redo, live iteration, scene management, and asset discovery;
- audio, input, UI, physics, animation, effects, localization, accessibility, and platform services.

These areas do not all need to be built immediately. They do need to be considered when the early foundations are chosen, so that later work has somewhere natural to belong.

“Compete with the big three” is a direction, not a first milestone. Monarc should eventually be judged by concrete things: how quickly a project can be created, how quickly content appears in the editor, how little friction there is in iteration, how fast and reliable builds are, how small and clean exported games can be, how well the engine performs, and how confidently developers can understand and change it.

## How we should begin

The first session should be generous with discovery and architecture. Start by understanding the repository, its current state, the available toolchain, and the existing documentation and skills. Then help me turn this vision into a coherent system: explore the full problem, research the areas where current practice or platform behavior matters, identify the choices that shape everything else, and recommend a direction with its tradeoffs.

I want a real design conversation rather than a questionnaire. Surface the important questions, but use your judgement about which details can be chosen sensibly or deferred. If there are many questions, organize them around the decisions they affect and work through the highest-impact ones first. Do not rush into a large implementation before the architecture has a clear first proof, and do not keep researching after the remaining uncertainty is no longer changing the direction.

The first useful proof should be end to end: a project can be created, opened in the editor, display or load something through the intended runtime path, run outside the editor, and export to a clean standalone build. It should be small enough to finish and real enough to test the boundaries between the Hub, Editor, toolchain, runtime, renderer, assets, and packaging. Designing for all three graphics backends begins immediately; implementing every backend at once is a rollout choice to make intelligently.

## Documentation and AI collaboration

Keep the project understandable to both people and AI. Important decisions, architecture, research, schemas, workflows, and current status should live in readable files inside `Docs/`, with links between them and a clear path from vision to implementation. Treat generated binaries and visualizations as useful outputs, not the only record of knowledge.

When the architecture map is ready, create an Obsidian Canvas that makes the relationships easy to see. If the repository’s `obsidian-canvas-creator` skill is available, use it. The canvas should support the Markdown documentation rather than replace it.

As the project grows, keep the vision stable while allowing implementation decisions to mature. Record meaningful changes to direction, explain why they happened, and preserve a clear account of what is currently true. The engine should be ambitious, but the work should remain legible.