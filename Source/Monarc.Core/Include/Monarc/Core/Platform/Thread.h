#pragma once

#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Error.h>
#include <Monarc/Core/Types.h>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace Monarc::Platform {

/// Logical processor count, for sizing a thread pool. Includes hyperthreads/SMT lanes;
/// this is not a count of physical cores.
[[nodiscard]] u32 HardwareThreadCount();

/// A non-recursive mutual-exclusion lock.
///
/// Non-copyable and non-movable: a lock is identified by its address -- every ScopedLock
/// and every ConditionVariable::Wait call referencing it does so by reference, so moving
/// it would leave them pointing at a stale one.
///
/// Locking it twice from the same thread without an intervening Unlock is undefined,
/// exactly as with std::mutex. This is not a recursive mutex.
class Mutex {
public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex&)            = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&)                 = delete;
    Mutex& operator=(Mutex&&)      = delete;

    void Lock();
    void Unlock();

    /// Acquires the lock without blocking. Returns false immediately if it is already
    /// held, rather than waiting.
    [[nodiscard]] bool TryLock();

private:
    friend class ConditionVariable;

    // Opaque storage for the platform native primitive (an SRWLOCK on Windows). Raw bytes
    // rather than a typed member, so this header names no platform type and its layout
    // does not depend on which platform is building it (ADR-0016). Sized for exactly one
    // native handle-shaped value; the platform .cpp static_asserts that whatever it
    // actually stores here fits.
    alignas(alignof(void*)) std::byte m_storage[sizeof(void*)];
};

/// RAII lock guard over a Mutex: locks on construction, unlocks on destruction.
///
/// Non-copyable and non-movable, like the standard library own guards -- a lock guard
/// exists entirely for a destructor tied to one specific scope.
class ScopedLock {
public:
    explicit ScopedLock(Mutex& mutex) noexcept : m_mutex(mutex) { m_mutex.Lock(); }
    ~ScopedLock() { m_mutex.Unlock(); }

    ScopedLock(const ScopedLock&)            = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
    ScopedLock(ScopedLock&&)                 = delete;
    ScopedLock& operator=(ScopedLock&&)      = delete;

private:
    Mutex& m_mutex;
};

/// A condition variable paired with a caller-supplied Mutex, in the usual monitor pattern.
///
/// Wait may return spuriously -- with no corresponding NotifyOne/NotifyAll having been
/// called -- exactly as Win32 documents for its own condition variables. Every caller
/// MUST loop on a predicate:
///
///     Platform::ScopedLock lock(mutex);
///     while (!ConditionHolds()) { conditionVariable.Wait(mutex); }
///
/// and never call Wait as a bare "if". A Notify that arrives before anybody is waiting is
/// not queued or remembered -- it simply does nothing -- which is exactly why the
/// predicate, and not the notification itself, is what a waiter is actually waiting for:
/// a waiter checks the predicate before it ever calls Wait, so a notify that arrived early
/// is not lost, only irrelevant, because the condition it announced is already visible.
class ConditionVariable {
public:
    ConditionVariable();
    ~ConditionVariable();

    ConditionVariable(const ConditionVariable&)            = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&)                 = delete;
    ConditionVariable& operator=(ConditionVariable&&)      = delete;

    /// mutex must be held by the calling thread on entry. It is released while this call
    /// blocks and reacquired before returning -- including when returning spuriously, per
    /// the class comment above.
    void Wait(Mutex& mutex);

    /// Wakes at least one waiter, if any are currently blocked in Wait. Does nothing if
    /// nobody is waiting.
    void NotifyOne();

    /// Wakes every waiter currently blocked in Wait. Does nothing if nobody is waiting.
    void NotifyAll();

private:
    // See Mutex::m_storage for why this is raw bytes. Windows stores a CONDITION_VARIABLE.
    alignas(alignof(void*)) std::byte m_storage[sizeof(void*)];
};

namespace Detail {

/// Type-erased thread entry point. Not part of the public interface: Thread::Start wraps
/// whatever callable it is given in a ThreadTask<Callable> and hands the platform
/// implementation only this base pointer, so the platform .cpp needs no template code and
/// no knowledge of any callable type a caller uses.
struct IThreadTask {
    virtual ~IThreadTask() = default;
    virtual void Invoke() = 0;
};

template <typename Callable>
struct ThreadTask final : IThreadTask {
    template <typename Forwarded>
    explicit ThreadTask(Forwarded&& callable) : m_callable(std::forward<Forwarded>(callable)) {}
    void Invoke() override { m_callable(); }

    Callable m_callable;
};

}  // namespace Detail

/// An OS thread running one callable to completion.
///
/// Non-copyable, movable. The destructor must never run while the thread is still
/// running: silently detaching would let the OS thread keep executing against whatever
/// its callable captured by reference, after the scope that owns that state -- possibly
/// this very Thread object -- has already gone away. MONARC_CHECK enforces this as a
/// programming error; Join() then still runs unconditionally as a last-resort safety net
/// in case that check does not stop the program, so the thread is never left silently
/// detached. A correctly-used Thread has already been joined by the time it is destroyed,
/// at which point this Join() call is a no-op.
class Thread {
public:
    Thread() noexcept;
    ~Thread();

    Thread(const Thread&)            = delete;
    Thread& operator=(const Thread&) = delete;

    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;

    /// Starts callable running on a new OS thread named name. name is taken here, rather
    /// than read from the thread later, because Win32 can only name a thread from within
    /// it or through a handle -- never in advance -- and an unnamed thread in a profiler
    /// is nearly useless once there are more than a handful.
    ///
    /// callable is moved or copied into storage owned by the running thread itself, and is
    /// invoked exactly once and destroyed there, immediately after it returns -- so
    /// whatever it captures by reference (or by pointer) must stay valid until Join()
    /// returns. Failure to allocate that storage is treated the same way it is elsewhere
    /// in Monarc.Core (Array<T>, HashMap<K,V>): fatal rather than recoverable, since
    /// Monarc has no fallible allocation story yet.
    ///
    /// Fails with ErrorCode::AlreadyExists if this Thread is already running -- Join() it
    /// first. Fails with ErrorCode::OutOfMemory if the OS refuses to create a thread,
    /// which in practice is always a resource limit rather than a bad argument, since
    /// every argument Start itself passes to the OS is valid.
    template <typename Callable>
    [[nodiscard]] Status Start(StringView name, Callable&& callable) {
        using Stored = std::decay_t<Callable>;
        return StartTask(name, new Detail::ThreadTask<Stored>(std::forward<Callable>(callable)));
    }

    /// Blocks until the thread entry point returns, then releases the OS thread handle.
    /// Safe to call unconditionally: a no-op if this Thread was never started, or has
    /// already been joined.
    void Join();

    /// True from a successful Start() until Join() returns. This reflects whether the
    /// Thread object currently owns a live, not-yet-joined handle -- not whether the
    /// entry point has actually finished executing. The two can differ if the callable
    /// finishes before Join() is called; that is fine, Join() still returns immediately
    /// once it is.
    [[nodiscard]] bool IsRunning() const;

private:
    [[nodiscard]] Status StartTask(StringView name, Detail::IThreadTask* task);

    // Opaque storage for a platform thread handle (a Win32 HANDLE). See Mutex::m_storage
    // for why this is raw bytes rather than a typed member.
    alignas(alignof(void*)) std::byte m_storage[sizeof(void*)];
};

}  // namespace Monarc::Platform
