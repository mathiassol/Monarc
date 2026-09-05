# Threading

The vision asks that threading be a foundation rather than an optimisation added later. This is
one of the clearest cases where the vision is right for a structural reason: retrofitting
parallelism means auditing every system for shared mutable state, and that audit is far more
expensive than designing for it.

## The job system

`Monarc.Jobs` is where expensive work belongs. It provides:

| Capability | Notes |
|---|---|
| Jobs | A unit of work, submitted and awaited by handle |
| Dependency graph | A job may depend on others; the scheduler orders them |
| Priorities | So latency-sensitive work is not queued behind bulk work |
| Thread affinity | For work that must run on a specific thread (main-thread-only APIs) |
| Cancellation | Where useful — long cooks, abandoned loads |
| Instrumentation | What is running, on which thread, for how long. Visibility is a requirement |

The scheduler is a **thread pool with a dependency graph**, not fibers. Fibers give cleaner
blocking semantics and cost significant platform-specific complexity; a pool with explicit
continuations is sufficient, portable, and can be replaced behind the same API if it proves
limiting.

## Ownership rules

Parallelism is safe because ownership is stated, not because of locking discipline. The rules:

1. **One writer.** For any piece of state during any phase, exactly one job may write it.
2. **Read-only sharing.** Data read by multiple jobs must be immutable for that phase.
3. **No hidden global mutable state.** Subsystems hold their state explicitly.
4. **Phase boundaries are synchronisation points.** State written in one
   [phase](Frame-Model.md) is readable in the next without further coordination.
5. **Handles, not pointers**, across job boundaries — see
   [ADR-0002](../Architecture/Decisions/ADR-0002-handles-not-pointers.md). A job holding a
   pointer to something another job may relocate is a bug waiting for a schedule change.

The [extraction boundary](../Architecture/Decisions/ADR-0009-render-extraction.md) is the
largest application of rule 2: the render frame is immutable, so any number of jobs may read it.

## The main thread's role is deliberate

Defined in [Frame-Model.md](Frame-Model.md#the-main-thread-has-a-job). Restated here because it
is a threading rule: the main thread pumps OS messages, collects input, presents, and sequences
phases. It schedules and joins; it does not perform simulation, extraction, recording, or asset
loading.

## Where the job system is actually used in M0

Honestly: mostly the cooker.

Cooking many assets is embarrassingly parallel, obviously valuable, and exercises jobs,
dependencies, and cancellation on real work. That is the right first consumer — it proves the
API without requiring the whole engine to be parallel on day one.

The frame loop's phases are structured so that parallelising them later is a scheduling change,
but M0 runs them largely synchronously and makes no performance claims.

## Scope in M0

Thread pool sized to hardware. Job submission with dependencies and wait handles. Priorities.
Instrumentation sufficient to see what ran where. Used by `Monarc.Cook` for parallel imports.

Deferred: fibers, work-stealing tuning, cancellation beyond cooking, parallel extraction and
command recording, lock-free container library.
