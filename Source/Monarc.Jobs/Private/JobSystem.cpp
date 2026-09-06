#include <Monarc/Jobs/JobSystem.h>

#include <Monarc/Core/Assert.h>

#include <cstdlib>
#include <format>
#include <memory>
#include <new>

namespace Monarc {

namespace {

/// Allocation and worker-thread creation are both treated as fatal here, for the same
/// reason Array<T> and HashMap<K,V> treat allocation failure as fatal: Monarc has no
/// fallible construction story yet, and continuing with a half-built pool would corrupt
/// memory the first time anything touched it rather than fail cleanly.
[[noreturn]] void OnResourceExhausted() {
    MONARC_DEBUG_BREAK();
    std::abort();
}

/// True only on a thread currently running JobSystem::WorkerLoop -- for any JobSystem, not
/// just one instance's own workers, since the rule Wait() enforces ("never from a worker")
/// is about what kind of thread this is, not which pool it belongs to. Thread-local, so it
/// needs no synchronisation of its own; see JobSystem.h's class comment.
thread_local bool t_isJobWorkerThread = false;

}  // namespace

JobSystem::JobSystem(IAllocator& allocator, const Config& config)
    : m_allocator(&allocator), m_config(config) {
    MONARC_CHECK(config.workerCount > 0, "JobSystem: workerCount must be at least 1");
    MONARC_CHECK(config.maxJobs > 0, "JobSystem: maxJobs must be at least 1");

    m_slots = static_cast<JobSlot*>(
        m_allocator->Allocate(sizeof(JobSlot) * config.maxJobs, alignof(JobSlot)));
    MONARC_CHECK(m_slots != nullptr, "JobSystem: failed to allocate the job pool");
    if (m_slots == nullptr) {
        OnResourceExhausted();
    }
    for (u32 i = 0; i < config.maxJobs; ++i) {
        std::construct_at(&m_slots[i], allocator);
    }

    m_readyQueue = static_cast<u32*>(
        m_allocator->Allocate(sizeof(u32) * config.maxJobs, alignof(u32)));
    MONARC_CHECK(m_readyQueue != nullptr, "JobSystem: failed to allocate the ready queue");
    if (m_readyQueue == nullptr) {
        OnResourceExhausted();
    }

    m_workers = static_cast<Platform::Thread*>(m_allocator->Allocate(
        sizeof(Platform::Thread) * config.workerCount, alignof(Platform::Thread)));
    MONARC_CHECK(m_workers != nullptr, "JobSystem: failed to allocate worker threads");
    if (m_workers == nullptr) {
        OnResourceExhausted();
    }
    for (u32 i = 0; i < config.workerCount; ++i) {
        std::construct_at(&m_workers[i]);
    }

    // Started only after every slot, the queue and every Thread object already exist:
    // a worker can start running WorkerLoop the instant Start() returns, and WorkerLoop
    // reads m_slots/m_readyQueue/m_config immediately.
    for (u32 i = 0; i < config.workerCount; ++i) {
        char       nameBuffer[32];
        const auto formatted =
            std::format_to_n(nameBuffer, sizeof(nameBuffer) - 1, "Monarc.Jobs.Worker{}", i);
        *formatted.out = '\0';

        const Status started = m_workers[i].Start(StringView(nameBuffer), [this] { WorkerLoop(); });
        MONARC_CHECK(started.has_value(), "JobSystem: failed to start a worker thread");
        if (!started.has_value()) {
            OnResourceExhausted();
        }
    }
}

JobSystem::~JobSystem() {
    // Drain first: wait for every already-submitted job to finish running before a single
    // worker is told to stop. Only once m_outstandingCount is zero do we know the ready
    // queue is empty and nothing is mid-callable -- see CompleteSlot and the class comment
    // on m_outstandingCount -- so setting m_stopping here can never abandon live work.
    {
        Platform::ScopedLock lock(m_mutex);
        while (m_outstandingCount > 0) {
            m_cv.Wait(m_mutex);
        }
        m_stopping = true;
    }
    m_cv.NotifyAll();

    for (u32 i = 0; i < m_config.workerCount; ++i) {
        m_workers[i].Join();
    }
    for (u32 i = 0; i < m_config.workerCount; ++i) {
        std::destroy_at(&m_workers[i]);
    }
    m_allocator->Deallocate(m_workers, sizeof(Platform::Thread) * m_config.workerCount,
                            alignof(Platform::Thread));

    for (u32 i = 0; i < m_config.maxJobs; ++i) {
        std::destroy_at(&m_slots[i]);
    }
    m_allocator->Deallocate(m_slots, sizeof(JobSlot) * m_config.maxJobs, alignof(JobSlot));
    m_allocator->Deallocate(m_readyQueue, sizeof(u32) * m_config.maxJobs, alignof(u32));
}

void JobSystem::Wait(JobHandle handle) {
    MONARC_CHECK(
        !t_isJobWorkerThread,
        "JobSystem::Wait called from one of this pool's own worker threads -- a worker "
        "that blocks here removes itself from the pool, and enough of them doing it "
        "deadlocks the rest. Express ordering between jobs with SubmitAfter's dependencies "
        "instead of waiting inside a job.");

    Platform::ScopedLock lock(m_mutex);
    while (!IsCompleteLocked(handle)) {
        m_cv.Wait(m_mutex);
    }
}

bool JobSystem::IsComplete(JobHandle handle) const {
    Platform::ScopedLock lock(m_mutex);
    return IsCompleteLocked(handle);
}

bool JobSystem::IsCompleteLocked(JobHandle handle) const {
    if (!handle.IsValid() || handle.index >= m_config.maxJobs) {
        return true;
    }
    const JobSlot& slot = m_slots[handle.index];
    if (slot.generation != handle.generation) {
        return true;   // Stale: this generation is long gone. Always reported complete.
    }
    return slot.done;
}

u32 JobSystem::ClaimSlotLocked(StringView name) {
    for (u32 i = 0; i < m_config.maxJobs; ++i) {
        if (m_slots[i].done) {
            JobSlot& slot = m_slots[i];
            ++slot.generation;
            slot.done = false;
            slot.name = name;
            return i;
        }
    }
    return kNoSlot;
}

JobHandle JobSystem::ScheduleAfterDependenciesLocked(u32                         slotIndex,
                                                     std::span<const JobHandle> dependencies) {
    JobSlot& slot = m_slots[slotIndex];

    // Counted, not traversed (see the A2d plan's decisions section): only the count of
    // still-pending dependencies is tracked here, never a graph walk at schedule time. An
    // already-complete or stale dependency is satisfied right now and is never registered
    // as anything -- it will not decrement a count that was never incremented for it,
    // because there is nothing left for it to ever notify.
    u32 pending = 0;
    for (const JobHandle& dependency : dependencies) {
        if (IsCompleteLocked(dependency)) {
            continue;
        }
        ++pending;
        m_slots[dependency.index].dependents.Push(slotIndex);
    }
    slot.pendingDependencies = pending;

    const JobHandle handle{slotIndex, slot.generation};
    if (pending == 0) {
        PushQueueLocked(slotIndex);
        m_cv.NotifyAll();
    }
    return handle;
}

void JobSystem::PushQueueLocked(u32 slotIndex) {
    MONARC_CHECK(m_queueCount < m_config.maxJobs,
                 "JobSystem: ready queue overflow -- more ready jobs than pool capacity, "
                 "which should be impossible since each slot can only be queued once per "
                 "generation");
    const usize tail = (m_queueHead + m_queueCount) % m_config.maxJobs;
    m_readyQueue[tail] = slotIndex;
    ++m_queueCount;
}

u32 JobSystem::PopQueueLocked() {
    MONARC_CHECK(m_queueCount > 0, "JobSystem: PopQueueLocked called on an empty queue");
    const u32 slotIndex = m_readyQueue[m_queueHead];
    m_queueHead         = (m_queueHead + 1) % m_config.maxJobs;
    --m_queueCount;
    return slotIndex;
}

void JobSystem::CompleteSlot(u32 slotIndex) {
    MONARC_CHECK(m_outstandingCount > 0, "JobSystem: outstanding-count underflow");
    JobSlot& slot = m_slots[slotIndex];
    slot.done     = true;
    --m_outstandingCount;

    // The dependency graph's other half: wake everything that was waiting on exactly this
    // slot. Every decrement below happens inside this one m_mutex-held call, so if two
    // dependencies of the same dependent finish on two different workers, their two
    // CompleteSlot calls are still strictly ordered by the mutex -- one strictly before the
    // other -- and only whichever one observes the count reach zero pushes the dependent.
    // The other observes a still-positive count and does nothing. Neither ordering can
    // enqueue it twice, and neither can miss enqueueing it.
    for (const u32 dependentIndex : slot.dependents) {
        JobSlot& dependent = m_slots[dependentIndex];
        MONARC_CHECK(dependent.pendingDependencies > 0,
                     "JobSystem: a dependent's pending-dependency count underflowed");
        --dependent.pendingDependencies;
        if (dependent.pendingDependencies == 0) {
            PushQueueLocked(dependentIndex);
        }
    }
    slot.dependents.Clear();
}

void JobSystem::WorkerLoop() {
    t_isJobWorkerThread = true;

    for (;;) {
        u32 slotIndex;
        {
            Platform::ScopedLock lock(m_mutex);
            while (m_queueCount == 0 && !m_stopping) {
                m_cv.Wait(m_mutex);
            }
            if (m_queueCount == 0) {
                // m_stopping is set only after ~JobSystem has already waited for
                // m_outstandingCount to reach zero, so nothing can ever be queued again
                // once we observe both conditions together here -- see the destructor.
                return;
            }
            slotIndex = PopQueueLocked();
        }

        // Invoked with no lock held: this is the only point in the whole class where that
        // is true, and it is the entire reason a thread pool is faster than one mutex.
        JobSlot& slot     = m_slots[slotIndex];
        auto*    callable = std::launder(reinterpret_cast<Detail::IJobCallable*>(slot.callableStorage));
        callable->Invoke();
        callable->~IJobCallable();

        {
            Platform::ScopedLock lock(m_mutex);
            CompleteSlot(slotIndex);
            m_cv.NotifyAll();
        }
    }
}

}  // namespace Monarc
