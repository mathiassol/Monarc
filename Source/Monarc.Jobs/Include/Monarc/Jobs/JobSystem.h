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
#include <type_traits>
#include <utility>

namespace Monarc {

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
    explicit JobCallable(Callable&& callable) : m_callable(std::move(callable)) {}
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
///   slot's `generation`, `done` flag and stored name; the ready queue; the
///   outstanding-job count; and the stop flag. There is deliberately no lock-free path
///   anywhere in this scheduler -- Submit, Wait, IsComplete and the workers' own
///   bookkeeping all take the same mutex for the (brief) duration of any state change.
///   That costs some contention under heavy submission, which nothing here has measured
///   as a problem; the payoff is that no field in this class can be read torn or updated
///   out of order relative to another, because nothing outside the lock ever touches them.
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
        PushQueueLocked(slotIndex);
        ++m_outstandingCount;
        m_cv.NotifyAll();
        return JobHandle{slotIndex, m_slots[slotIndex].generation};
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
    };

    /// Finds a free slot (one with `done == true`), recycles it for a new job -- bumping
    /// its generation and clearing `done` -- and returns its index. Returns kNoSlot,
    /// touching nothing, if every slot is currently occupied. Assumes m_mutex is held.
    [[nodiscard]] u32 ClaimSlotLocked(StringView name);

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

    /// Marks slotIndex's job finished and decrements the outstanding-job count. Assumes
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
