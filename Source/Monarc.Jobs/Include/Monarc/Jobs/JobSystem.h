#pragma once

#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/Core/Platform/Thread.h>
#include <Monarc/Core/Types.h>
#include <Monarc/Jobs/JobHandle.h>

#include <cstddef>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Monarc {

// SubmitAfter's multi-dependency overload takes std::span<const JobHandle> rather than a
// Monarc::Span: M0-First-Light.md lists Span among Monarc.Core's containers, but it does
// not exist yet (only Array, String and HashMap have landed so far), and adding it is out
// of scope for this module. <span> is in ADR-0003's allowed subset for Runtime code, and
// Monarc::Array<T>'s pointer iterators already satisfy std::span's contiguous-range
// constructor, so `SubmitAfter(name, someArray, callable)` below works with no change to
// Array itself -- see TestJobDependencies.cpp.
namespace Detail {

/// Type-erased job body, invoked exactly once then destroyed. Mirrors
/// Platform::Detail::IThreadTask, but JobSystem placement-constructs the derived
/// JobCallable<Callable> inline inside a pool slot's own storage instead of heap-allocating
/// it -- see JobSystem's class comment on why submission must not touch the allocator.
class IJobCallable {
public:
    virtual ~IJobCallable() = default;
    virtual void Invoke()   = 0;
};

template <typename Callable>
class JobCallable final : public IJobCallable {
public:
    // A constructor template with its own forwarding-reference parameter, distinct from
    // the class's own (already-fixed) Callable -- exactly Platform::Detail::ThreadTask's
    // own shape, and for the same reason: Submit/SubmitAfter's Callable&& is a forwarding
    // reference and may legitimately be handed an lvalue (TestJobDependencies.cpp's chain
    // test reuses one local lambda across several Submit/SubmitAfter calls in a ternary,
    // so it is never an rvalue at the call site). A plain `Callable&& callable` here would
    // be an ordinary rvalue-reference constructor once Callable is fixed by JobCallable's
    // own instantiation, and would reject exactly that lvalue.
    template <typename Forwarded>
    explicit JobCallable(Forwarded&& callable) : m_callable(std::forward<Forwarded>(callable)) {}
    void Invoke() override { m_callable(); }

private:
    Callable m_callable;
};

}  // namespace Detail

/// A thread pool that runs submitted jobs to completion. See Docs/Runtime/Threading.md for
/// the design this implements, and ADR-0002 for why jobs are named by handle rather than by
/// pointer.
///
/// **Concurrency design, enumerated in full because there is no ThreadSanitizer on this
/// platform to catch a mistake here (see the A2d plan's opening note):**
///
/// - `m_mutex` guards every piece of mutable scheduling state this class has: each job
///   slot's `generation`, `done` flag, stored name, `pendingDependencies` count and
///   `dependents` list; the ready queue; the outstanding-job count; and the stop flag.
///   There is deliberately no lock-free path anywhere in this scheduler -- Submit,
///   SubmitAfter, Wait, IsComplete and the workers' own bookkeeping all take the same
///   mutex for the (brief) duration of any state change. That costs some contention under
///   heavy submission, which nothing here has measured as a problem; the payoff is that no
///   field in this class can be read torn or updated out of order relative to another,
///   because nothing outside the lock ever touches them. In particular: a dependency's
///   completion and a dependent's registration can never interleave -- one job's
///   `SubmitAfter` deciding whether a given dependency already finished, and that
///   dependency's own completion deciding who to wake, both happen only while holding this
///   one mutex, so a job can never be enqueued twice from two dependencies finishing
///   "simultaneously": the mutex forces their bookkeeping into some definite order, and
///   only the decrement that observes the count reach zero ever enqueues anything.
/// - `m_cv` is the one condition variable, shared by every waiter -- idle workers waiting
///   for the queue to become non-empty, and `Wait()` waiting for one specific job to
///   finish. Every waiter loops on its own predicate under `m_mutex`, per
///   `ConditionVariable`'s own contract, so one condition variable serving two different
///   predicates costs a spurious wake-and-recheck at worst, never a missed one.
/// - The **only** state that is not behind `m_mutex` is the job's own callable, which runs
///   with no lock held at all -- holding `m_mutex` across a job body would serialise every
///   job in the pool and defeat the entire point of having one.
/// - A `thread_local` flag records whether the current thread is one of this pool's
///   workers. It is thread-local storage, not shared state, so it needs no synchronisation
///   of its own; see the private `Wait` guard below for what it is for.
///
/// Non-copyable and non-movable: workers capture `this` for the lifetime of the pool, so a
/// JobSystem cannot be relocated out from under them.
class JobSystem {
public:
    struct Config {
        u32 workerCount;
        u32 maxJobs;
    };

    JobSystem(IAllocator& allocator, const Config& config);

    /// Waits for every outstanding job to finish running, then stops and joins every
    /// worker. Nothing submitted before this call is ever abandoned mid-flight or
    /// destroyed while still running -- see CompleteSlot's use of m_outstandingCount.
    ~JobSystem();

    JobSystem(const JobSystem&)            = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&)                 = delete;
    JobSystem& operator=(JobSystem&&)      = delete;

    /// Submits callable to run on the pool, with no dependencies -- it becomes runnable
    /// immediately. Fails with ErrorCode::OutOfMemory, without allocating or blocking, if
    /// every one of Config::maxJobs slots is currently occupied by unfinished work.
    ///
    /// callable is moved or copied into storage inline inside the claimed slot -- never
    /// heap-allocated -- so it must fit within kInlineCallableSize at kInlineCallableAlign;
    /// the static_asserts below turn an over-large capture into a compile error rather
    /// than a silent allocation on what is meant to be an allocation-free path.
    template <typename Callable>
    [[nodiscard]] Result<JobHandle> Submit(StringView name, Callable&& callable) {
        return SubmitAfter(name, std::span<const JobHandle>{}, std::forward<Callable>(callable));
    }

    /// Submits callable to run once `dependency` has finished. If `dependency` has already
    /// finished -- including if it is stale, per JobHandle's class comment -- callable
    /// becomes runnable immediately rather than waiting for a completion that will never
    /// arrive for that generation again. See the span-taking overload below for the
    /// general case and for what this means for submission order.
    template <typename Callable>
    [[nodiscard]] Result<JobHandle> SubmitAfter(StringView name, JobHandle dependency,
                                                Callable&& callable) {
        const JobHandle dependencies[1] = {dependency};
        return SubmitAfter(name, std::span<const JobHandle>(dependencies, 1),
                           std::forward<Callable>(callable));
    }

    /// Submits callable to run once every handle in dependencies has finished. Each one
    /// that has *already* finished by the time this call takes the lock -- including a
    /// stale one -- counts as satisfied immediately rather than being waited on: a
    /// dependency that finished before SubmitAfter was even called must not leave the new
    /// job waiting on a completion event that already happened and will not happen again,
    /// or Wait on it would hang forever. If every dependency is already satisfied (an empty
    /// span included -- this is what Submit itself calls through to), callable becomes
    /// runnable immediately, exactly as a plain Submit.
    ///
    /// dependencies is read only for the duration of this call; nothing about it is stored.
    /// callable is stored inline exactly as Submit's -- see its doc comment.
    template <typename Callable>
    [[nodiscard]] Result<JobHandle> SubmitAfter(StringView name,
                                                std::span<const JobHandle> dependencies,
                                                Callable&& callable) {
        using Stored = std::decay_t<Callable>;
        static_assert(sizeof(Detail::JobCallable<Stored>) <= kInlineCallableSize,
                      "job callable does not fit JobSystem's inline storage -- capture less, "
                      "or capture a handle/pointer to shared state instead of the state "
                      "itself");
        static_assert(alignof(Detail::JobCallable<Stored>) <= kInlineCallableAlign,
                      "job callable's alignment exceeds JobSystem's inline storage alignment");

        Platform::ScopedLock lock(m_mutex);
        const u32 slotIndex = ClaimSlotLocked(name);
        if (slotIndex == kNoSlot) {
            return Err(ErrorCode::OutOfMemory, "JobSystem: job pool exhausted");
        }
        ::new (static_cast<void*>(m_slots[slotIndex].callableStorage))
            Detail::JobCallable<Stored>(std::forward<Callable>(callable));
        ++m_outstandingCount;
        return ScheduleAfterDependenciesLocked(slotIndex, dependencies);
    }

    /// Blocks the calling thread until handle's job has finished running. Returns
    /// immediately, without blocking, if the job already finished or if handle is stale --
    /// see JobHandle's class comment.
    ///
    /// MONARC_CHECKs that the caller is not one of this pool's own worker threads. A
    /// worker that blocks here removes itself from the pool until whatever it is waiting
    /// on finishes; with no other worker doing anything different, that is a deadlock
    /// waiting to happen rather than a hypothetical one. Express ordering between jobs
    /// with SubmitAfter's dependencies instead -- see Docs/Runtime/Threading.md.
    void Wait(JobHandle handle);

    /// True if handle's job has finished running, or if handle is stale -- naming a slot
    /// that has since been recycled for other work. A default-constructed (invalid) handle
    /// reports complete: there is nothing outstanding it could be waiting on.
    [[nodiscard]] bool IsComplete(JobHandle handle) const;

    [[nodiscard]] u32 WorkerCount() const { return m_config.workerCount; }

private:
    /// No slot ever reaches this index; returned by ClaimSlotLocked when the pool is full.
    static constexpr u32 kNoSlot = static_cast<u32>(-1);

    /// Largest callable Submit/SubmitAfter will store inline, and its alignment. Declared
    /// before JobSlot deliberately: a data member's array bound and alignas are resolved
    /// immediately at the point they are parsed, unlike a member function body -- so
    /// JobSlot::callableStorage below needs these to already be visible, not merely
    /// declared somewhere else in the class. Comfortably covers every capture-by-reference
    /// lambda in this codebase's own job tests (a handful of pointers plus one vtable
    /// pointer); a lambda that needs more should capture a pointer to shared state rather
    /// than the state itself.
    static constexpr usize kInlineCallableSize  = 64;
    static constexpr usize kInlineCallableAlign = alignof(std::max_align_t);

    /// One entry in the fixed-capacity job pool. Every field here is read or written only
    /// while JobSystem::m_mutex is held -- see the class comment above.
    struct JobSlot {
        /// dependents needs an allocator to grow into -- see its own comment -- so this
        /// slot cannot simply be value-initialized the way Task 1's version could.
        explicit JobSlot(IAllocator& allocator) : dependents(allocator) {}

        /// Storage for the type-erased callable, placement-constructed by Submit and
        /// destroyed immediately after JobSystem invokes it. Only ever holds a live object
        /// while `done` is false; a slot with `done == true` holds no callable at all, and
        /// nothing may read callableStorage until the next Submit placement-constructs a
        /// new one into it.
        alignas(kInlineCallableAlign) std::byte callableStorage[kInlineCallableSize];

        /// Bumped by ClaimSlotLocked every time this slot is recycled for a new job. Read
        /// by IsCompleteLocked to recognise a handle from a previous occupant as stale.
        u32 generation = 0;

        /// True from construction, and again once the current occupant's callable has
        /// returned, until ClaimSlotLocked recycles this slot for the next job. A slot is
        /// eligible for reuse precisely when this is true.
        bool done = true;

        /// Non-owning: Submit's `name` parameter is expected to reference a string literal
        /// or otherwise long-lived storage, exactly like Error::message.
        StringView name;

        /// Count of this job's own dependencies that have not yet finished. Set once, by
        /// ScheduleAfterDependenciesLocked, to the number of entries in the span it was
        /// given that were not already complete; decremented by CompleteSlot every time one
        /// of them finishes. This slot is pushed onto the ready queue the instant the count
        /// reaches zero -- either immediately, if it started at zero, or from inside
        /// whichever dependency's completion is the one that ticks it down to zero.
        u32 pendingDependencies = 0;

        /// Indices of jobs that named *this* slot as a dependency and were not already
        /// finished when they did -- who to notify when this job completes. Cleared by
        /// CompleteSlot once notified, so a new occupant of this slot starts with none.
        /// Grows on demand through JobSystem's own allocator; unlike the pool itself, its
        /// size is bounded by how much real fan-out a job actually accumulates; not by
        /// Config::maxJobs, so it is the one place in this class that can allocate after
        /// construction -- see the A2d plan's note on why the pool itself must not.
        Array<u32> dependents;
    };

    /// Finds a free slot (one with `done == true`), recycles it for a new job -- bumping
    /// its generation and clearing `done` -- and returns its index. Returns kNoSlot,
    /// touching nothing, if every slot is currently occupied. Assumes m_mutex is held.
    [[nodiscard]] u32 ClaimSlotLocked(StringView name);

    /// Second half of SubmitAfter, after slotIndex's slot has already been claimed and its
    /// callable already placement-constructed into it: counts how many of dependencies are
    /// not yet complete, registers slotIndex as a dependent of each of those (and of only
    /// those -- an already-complete or stale dependency is satisfied and is not registered
    /// anywhere), and pushes slotIndex onto the ready queue immediately if that count comes
    /// to zero. Always succeeds; the fallible part of submission is claiming the slot,
    /// already done by the caller. Assumes m_mutex is held.
    [[nodiscard]] JobHandle ScheduleAfterDependenciesLocked(u32 slotIndex,
                                                            std::span<const JobHandle> dependencies);

    /// True if handle names a slot that has finished running, or no longer exists under
    /// that generation at all. Assumes m_mutex is held.
    [[nodiscard]] bool IsCompleteLocked(JobHandle handle) const;

    /// Appends slotIndex to the ready queue. The queue's capacity is exactly
    /// Config::maxJobs: at most one entry per slot can ever be queued at a time, since a
    /// slot is only queued once per generation and only after Submit has already claimed
    /// it, so the queue can never need more room than the pool itself. Assumes m_mutex is
    /// held.
    void PushQueueLocked(u32 slotIndex);

    /// Removes and returns the slot index at the front of the ready queue. Caller must
    /// have already confirmed the queue is non-empty. Assumes m_mutex is held.
    [[nodiscard]] u32 PopQueueLocked();

    /// Marks slotIndex's job finished, decrements the outstanding-job count, and then -- the
    /// dependency graph's other half -- decrements pendingDependencies on every slot in
    /// slotIndex's own `dependents`, pushing any that reach zero onto the ready queue,
    /// before clearing `dependents` so the next occupant of this slot starts with none.
    /// Every one of those decrements happens here, inside this one call while m_mutex is
    /// held, which is what makes a job's completion indivisible with respect to any other
    /// slot's completion -- see the class comment's note on double-enqueueing. Assumes
    /// m_mutex is held; called by a worker immediately after the job's callable returns.
    void CompleteSlot(u32 slotIndex);

    /// Entry point run by every worker thread for the pool's entire lifetime.
    void WorkerLoop();

    IAllocator* m_allocator;
    Config      m_config;

    JobSlot* m_slots = nullptr;   // m_config.maxJobs entries, allocated once in the constructor.

    u32*  m_readyQueue = nullptr;   // Ring buffer, m_config.maxJobs entries. Guarded by m_mutex.
    usize m_queueHead   = 0;
    usize m_queueCount  = 0;

    Platform::Thread* m_workers = nullptr;   // m_config.workerCount entries.

    mutable Platform::Mutex     m_mutex;
    Platform::ConditionVariable m_cv;

    /// Jobs submitted but not yet finished running -- queued, or currently executing.
    /// The destructor waits for this to reach zero before it stops any worker, which is
    /// what guarantees every submitted job runs to completion even if nobody ever calls
    /// Wait on it. Guarded by m_mutex.
    u32 m_outstandingCount = 0;

    /// Set once, by the destructor, after m_outstandingCount has already reached zero.
    /// Guarded by m_mutex.
    bool m_stopping = false;
};

}  // namespace Monarc
