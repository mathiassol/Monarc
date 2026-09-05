# ADR-0010: Reflection by explicit registration, codegen-compatible

**Status:** Accepted — 2026-09-05

## Context

Reflection is needed by more of the engine than it first appears: serialization, the editor's
property inspector, undo/redo, asset schemas, and eventually binding a scripting language. The
vision asks that reflection, properties, handles, serialization, events and host boundaries not
assume C++ will be the only way to create logic forever.

There are three plausible mechanisms, and they differ mainly in *cost paid where*:

- **Explicit registration** — macros or template calls that populate a runtime registry. No
  build step; the cost is a little authoring boilerplate.
- **Header-parsing codegen** — a libclang or custom parser, Unreal Header Tool style. Powerful
  and invisible to the author; the cost is a build step, a parser to maintain, and build time.
- **C++26 static reflection (P2996)** — the eventual right answer, and unavailable: no
  shipping compiler has usable support.

Build time is a real constraint here. The development machine has six cores, and
[ADR-0003](ADR-0003-cpp23-baseline.md) already treats compile-time discipline as a design
constraint rather than a virtue.

## Decision

**Explicit registration now, into a runtime `TypeRegistry`:**

```cpp
MONARC_TYPE(Transform)
    MONARC_PROPERTY(position)
    MONARC_PROPERTY(rotation)
    MONARC_PROPERTY(scale)
MONARC_TYPE_END()
```

The **`TypeRegistry` API is the stable contract** — not the macros. Serialization, the editor
inspector, and undo/redo are written against the registry and know nothing about how it was
populated.

That is the whole point of the shape: a future header-parsing generator, or a C++26 static
reflection backend, can emit *the same registration calls* without any consumer changing. We
do not need to predict when P2996 becomes usable; we only need the seam.

M0 scope: types, properties (name, type, offset, attributes), and enums. Not methods, not
dynamic invoke.

## Consequences

**Good.** No libclang dependency, no code generation step, no build-time cost — the engine
builds with nothing but a compiler. The registry is debuggable, being ordinary data built by
ordinary code. Consumers are insulated from the mechanism, so the mechanism can change.

**Costs.** Authoring boilerplate, which will be forgotten sometimes — a property that exists in
the type but not in the registry will silently fail to serialize or appear in the inspector.
This is the real weakness of the approach. Mitigation: a test that compares registered property
counts against expectations for core types, and treating "missing from registry" as the first
hypothesis when a value fails to round-trip. Registration is also runtime work at startup,
which is negligible at this scale.

## Alternatives considered

**Header-parsing codegen (UHT-style) now.** Removes the boilerplate and the forgetting. Costs a
parser to build and maintain, a build step in every developer's loop, and build time on a
six-core machine. Deferred rather than rejected — if forgotten registrations become a real
source of bugs, this is the upgrade, and it fits behind the same registry API.

**Wait for C++26 static reflection.** Not available, no reliable date. Rejected as a plan,
adopted as a direction.

**Serialize by hand, no reflection.** Rejected: the editor inspector and undo/redo both need
generic property access, and hand-written serialization for every type is where versioning bugs
breed.
