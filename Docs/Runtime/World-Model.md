# World Model

`Monarc.World` provides the gameplay model: how developers author and think about a running
game. Actors provide identity and gameplay presence; Components provide capabilities.

## Actors and Components

```
Actor                      identity, presence, transform, children
  +-- Component            a capability
  +-- Component
```

An **Actor** has an identity, a transform, an optional parent, and children. It exists in the
world and can be found, referenced, saved, and destroyed.

A **Component** adds a capability to an Actor: a mesh to render, a camera to view through, a
sound to play, a collider to collide with. Components do not exist independently of an Actor.

### Transform lives on the Actor

This departs from Unreal, which puts transform on a root `SceneComponent`. Monarc puts it
directly on the Actor.

The reasoning: the vision defines Actors as providing *presence*, and transform **is** presence.
Unreal's root-component indirection is largely historical, and it costs a layer of lookup and a
category of confusion (which component's transform is authoritative?) in exchange for
flexibility rarely used. Parent-child relationships are Actor-to-Actor.

## Relationships are designed, not incidental

The vision notes that relationships matter as much as isolated objects. The following are
deliberate parts of the model rather than things that emerge:

| Relationship | Mechanism |
|---|---|
| Hierarchy | Actor parent and children, with transform composition |
| References | [Handles](../Architecture/Decisions/ADR-0002-handles-not-pointers.md), resolved through the world |
| Ownership | An Actor owns its Components; a World owns its Actors |
| Attachment | Distinct from parenting: attaching follows a transform without implying ownership |
| Queries | By type, by tag, by hierarchy, by spatial region |
| Communication | Explicit — direct calls through resolved handles, or events. Never by walking the hierarchy hoping to find something |

## Authoring model, not storage model

Actor and Component describe **how developers author and reason about gameplay**. They do not
dictate how runtime systems store hot data.

A system is free to keep a dense, data-oriented array of exactly the fields it iterates —
transforms for hierarchy updates, bounds for visibility — while the authored model remains
Actors with Components. This is possible only because nothing holds a pointer into storage:
handles resolve through the world, so storage can be reorganised freely.

M0 uses straightforward storage. The important thing is that the *interface* does not prevent
better storage later, and it does not.

## World kinds

A `World` has a kind, and all kinds run the same code:

| Kind | Purpose |
|---|---|
| `Editor` | The scene being authored. Not simulating |
| `Play` | Play-in-editor. Simulating, inside the Editor process |
| `Standalone` | A shipped game |
| `Headless` | Dedicated server or tooling. No rendering — see [Module-Graph](../Architecture/Module-Graph.md) |

This uniformity is a requirement, not a convenience. Play-in-editor that runs different code
from standalone is play-in-editor that lies, and it drifts further apart over time. A
[verification gate](../Milestones/M0-First-Light.md#verification-gates) asserts that the same
scene produces identical actor and component state across all four kinds.

## Networking is a boundary before it is a feature

The vision asks that networking be an engine boundary even before multiplayer exists, so
gameplay does not come to depend on assumptions that only hold when one local machine owns the
whole truth.

Concretely, in M0:

- A `World` does not assume it is authoritative. Authority is a property, even though it is
  always local for now.
- State changes intended to be observable go through explicit mutation rather than direct field
  writes from arbitrary code, so replication has something to hook.
- `Standalone` and `Headless` are already distinct kinds, so a server world is not a new concept
  later.

No replication, prediction, or transport is implemented. The point is that adding them should
not require rewriting the world model.

## Tick phases

Systems run in declared phases rather than an implicit order, so the frame is comprehensible.
See [Frame-Model.md](Frame-Model.md).

## Scope in M0

Actor with identity, transform, and hierarchy. Component base type. `StaticMeshComponent` and
`CameraComponent`. World storage with spawn and destroy. Tick phases. All four world kinds.
Scene save and load through [serialization](../Content/Serialization.md).

Deferred: physics, animation, audio components, spatial queries and acceleration structures,
attachment sockets, replication, prefabs and instancing.
