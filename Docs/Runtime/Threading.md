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

`Monarc.Jobs` makes that concrete with one further choice, worth stating because it is a
cost as much as a design: **a single mutex guards every piece of mutable scheduling
state** — each job's generation, done flag, priority, and dependency bookkeeping; all
three per-priority ready queues; the completed-job profile buffer; the outstanding-job
count. Nothing in the scheduler is lock-free, and no piece of that state gets a lock of
its own. That means every submit, every completion, and every worker's dequeue serialises
on the same lock, so fine-grained jobs spread across many workers will contend more than a
cleverer design would. It is the right trade regardless, for a reason specific to this
codebase: there is no ThreadSanitizer for this platform (see [Status.md](../Status.md)),
so nothing mechanical would catch a mistake in anything more elaborate, and a design small
enough to enumerate in a paragraph is one that can actually be checked by reading it. This
is a performance limitation to revisit once profiling shows real contention, not a
correctness concern to fix pre-emptively.

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

## `Wait` is for the owning thread, not a worker

`JobSystem::Wait` blocks the calling thread until one specific job finishes. Calling it
from one of the job system's own worker threads is not merely discouraged — it is a
`MONARC_CHECK` violation, naming the reason in its own message: a worker that blocks
inside `Wait` has removed itself from the pool without telling anyone, and stays removed
until whatever it is waiting on finishes. It takes as few as one worker doing this to
itself to deadlock the whole pool: job A, running on the only worker able to run job B,
calls `Wait` on B — B sits in the ready queue forever, because the one worker that could
run it is the one now blocked waiting for it.

That guard is diagnostic, not preventive, in exactly the configuration where it matters
most: Shipping, or any other build running a non-breaking assert handler.
`MONARC_CHECK`'s contract is to report and optionally break, never to alter control flow
on its own, so when the handler declines to break, the check has already done everything
it is ever going to do — the next line runs regardless of whether the condition held.
Verified directly for A2d: with a non-breaking handler installed, a worker calling `Wait`
fires the handler (confirming the violation is detected and reported) and then falls
straight through into the blocking wait loop exactly as if nothing had checked anything,
deadlocking the pool for real, with no timeout and no recovery. `Wait`'s implementation
contains no code path that inspects the check's own outcome — there is nothing to catch
the case where the handler chose not to break. The discipline this depends on is
absolute: **express ordering with `SubmitAfter`'s dependencies; never wait inside a job.**

The capable alternative is helping-while-waiting: a worker that blocks runs other ready
jobs instead of merely sleeping, so one self-wait cannot starve the pool. It is deferred,
not rejected — it needs re-entrant job bodies (a job that is itself mid-callable when it
starts running another job), which is a real complication to accept before anything
actually needs it. `Wait`'s interface does not change if it arrives later; only its
insides would.

## Where the job system is actually used in M0

`Monarc.Jobs` itself is built and tested as of A2d — a thread pool, counted dependencies,
strict priorities, and completed-job instrumentation, all under CI on two compilers and
two sanitizers. What is *not* built yet is its first real consumer: `Monarc.Cook` is
Phase C, so nothing in the engine actually submits a job outside the test suite today.

Cooking many assets is embarrassingly parallel, obviously valuable, and will exercise
jobs, dependencies, and cancellation on real work the moment `Monarc.Cook` exists. That is
the right first consumer — it proves the API without requiring the whole engine to be
parallel on day one.

The frame loop's phases are structured so that parallelising them later is a scheduling change,
but M0 runs them largely synchronously and makes no performance claims.

## Scope in M0

Delivered in A2d: a thread pool sized at construction (`Config::workerCount`); job
submission through generation-checked handles from a fixed-capacity pool
(`Config::maxJobs`, never grown, so the hot path never touches the allocator); dependencies
as a counted graph rather than a traversal, satisfied immediately if already complete or
stale at submission time; `Wait` and `IsComplete`; three strict priorities (`High` /
`Normal` / `Low`, deliberately without ageing — a saturated high queue can starve low
work, acceptable while the cooker is the only planned producer); and completed-job
instrumentation (`JobProfileRecord` / `CollectProfile`) that costs nothing — no
allocation, no timestamp read — when `Config::profileCapacity` is left at its default of
zero.

Deferred: fibers, work-stealing tuning, helping-while-waiting (see above), cancellation,
thread affinity, priority ageing, and a lock-free container library. None of these are
needed until something other than tests — eventually `Monarc.Cook` — actually submits work.

Verification here is necessarily corroboration, not proof: there is no ThreadSanitizer for
this platform (see [Status.md](../Status.md)), so A2d's tests assert exact invariants a
race would violate — a strict ordering, an exact completion count — rather than merely "it
didn't crash," and every concurrency test repeats its scenario many times within one case.
The job test binary has additionally been run 200+ consecutive times standalone with zero
failures. That is corroboration, not proof: a race improbable enough is invisible to any
finite number of runs, and this record is exactly what it is — evidence gathered in the
absence of a sanitizer, not a substitute for one.
