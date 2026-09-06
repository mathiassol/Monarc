#include <Monarc/Core/Platform/Thread.h>

#include <Monarc/Core/Assert.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <process.h>
#include <processthreadsapi.h>

#include <cstring>
#include <memory>
#include <new>

namespace Monarc::Platform {

namespace {

constexpr int kMaxThreadNameWide = 256;

/// Converts a UTF-8 thread name into a fixed wide buffer. Mirrors Windows/File.cpp's
/// ToWide, but with a far smaller buffer -- thread names are short labels, not paths.
/// Truncates (by giving up and leaving an empty string) rather than failing outright: a
/// missing profiler label is cosmetic, never a reason to refuse to start the thread.
void ThreadNameToWide(StringView name, wchar_t (&buffer)[kMaxThreadNameWide]) {
    if (name.empty()) {
        buffer[0] = L'\0';
        return;
    }
    const int written = MultiByteToWideChar(CP_UTF8, 0, name.data(),
                                             static_cast<int>(name.size()), buffer,
                                             kMaxThreadNameWide - 1);
    buffer[written > 0 ? written : 0] = L'\0';
}

unsigned __stdcall ThreadMain(void* parameter) {
    // Owns the task from here: invoke exactly once, then destroy it, on this thread. This
    // is the only place a ThreadTask is ever deleted, and StartTask below is the only
    // other place one could be (on the failure path, before a thread ever runs it) -- the
    // two are mutually exclusive, so there is exactly one owner at all times.
    auto* task = static_cast<Detail::IThreadTask*>(parameter);
    task->Invoke();
    delete task;
    return 0;
}

}  // namespace

// -------------------------------------------------------------------------------------------
// HardwareThreadCount
// -------------------------------------------------------------------------------------------

u32 HardwareThreadCount() {
    // GetActiveProcessorCount rather than GetSystemInfo().dwNumberOfProcessors: the latter
    // is silently capped to the calling thread's processor group (64 logical processors),
    // which under-reports on higher core-count machines.
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0 ? static_cast<u32>(count) : 1u;
}

// -------------------------------------------------------------------------------------------
// Mutex
// -------------------------------------------------------------------------------------------

static_assert(sizeof(SRWLOCK) <= sizeof(Mutex) && alignof(SRWLOCK) <= alignof(Mutex),
              "SRWLOCK does not fit Mutex's opaque storage");

namespace {
[[nodiscard]] SRWLOCK* AsSRWLock(void* storage) {
    return std::launder(reinterpret_cast<SRWLOCK*>(storage));
}
}  // namespace

Mutex::Mutex() {
    // SRWLOCK needs no destroy call -- it owns no OS resource, only this memory -- so
    // placement-new here (immediately handed to InitializeSRWLock) is the entire
    // lifecycle; ~Mutex below only needs to end the object's lifetime, not release
    // anything.
    InitializeSRWLock(::new (static_cast<void*>(m_storage)) SRWLOCK);
}

Mutex::~Mutex() {
    std::destroy_at(AsSRWLock(m_storage));
}

void Mutex::Lock() { AcquireSRWLockExclusive(AsSRWLock(m_storage)); }
void Mutex::Unlock() { ReleaseSRWLockExclusive(AsSRWLock(m_storage)); }
bool Mutex::TryLock() { return TryAcquireSRWLockExclusive(AsSRWLock(m_storage)) != 0; }

// -------------------------------------------------------------------------------------------
// ConditionVariable
// -------------------------------------------------------------------------------------------

static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(ConditionVariable) &&
                  alignof(CONDITION_VARIABLE) <= alignof(ConditionVariable),
              "CONDITION_VARIABLE does not fit ConditionVariable's opaque storage");

namespace {
[[nodiscard]] CONDITION_VARIABLE* AsConditionVariable(void* storage) {
    return std::launder(reinterpret_cast<CONDITION_VARIABLE*>(storage));
}
}  // namespace

ConditionVariable::ConditionVariable() {
    InitializeConditionVariable(::new (static_cast<void*>(m_storage)) CONDITION_VARIABLE);
}

ConditionVariable::~ConditionVariable() {
    std::destroy_at(AsConditionVariable(m_storage));
}

void ConditionVariable::Wait(Mutex& mutex) {
    // 0 selects SRWLOCK's exclusive mode, matching Mutex::Lock/AcquireSRWLockExclusive --
    // there is no shared/read-locking API on Mutex, so this is the only mode ever in use.
    // Return value is deliberately ignored: with an INFINITE timeout, failure here is not
    // a condition this void-returning interface has anywhere to report.
    SleepConditionVariableSRW(AsConditionVariable(m_storage), AsSRWLock(mutex.m_storage),
                               INFINITE, 0);
}

void ConditionVariable::NotifyOne() { WakeConditionVariable(AsConditionVariable(m_storage)); }
void ConditionVariable::NotifyAll() { WakeAllConditionVariable(AsConditionVariable(m_storage)); }

// -------------------------------------------------------------------------------------------
// Thread
// -------------------------------------------------------------------------------------------

static_assert(sizeof(HANDLE) <= sizeof(Thread) && alignof(HANDLE) <= alignof(Thread),
              "HANDLE does not fit Thread's opaque storage");

namespace {

// Thread's storage holds a plain HANDLE (a scalar pointer value), not a struct needing an
// Initialize call, so it is read and written by byte-copy rather than placement-new --
// this is the textbook type-punning idiom for a trivially-copyable value and needs no
// object-lifetime reasoning at all, unlike Mutex/ConditionVariable above.
[[nodiscard]] HANDLE LoadHandle(const std::byte* storage) {
    HANDLE handle;
    std::memcpy(&handle, storage, sizeof(handle));
    return handle;
}

void StoreHandle(std::byte* storage, HANDLE handle) {
    std::memcpy(storage, &handle, sizeof(handle));
}

}  // namespace

Thread::Thread() noexcept { StoreHandle(m_storage, nullptr); }

Thread::~Thread() {
    MONARC_CHECK(!IsRunning(),
                 "Thread destroyed while still running -- call Join() before it goes out "
                 "of scope");
    // Not a silent detach: if the check above did not stop the program, joining here is
    // the fallback that keeps the OS thread from outliving this object. When Start() was
    // properly followed by Join() already, the stored handle is null and this returns
    // immediately.
    Join();
}

Thread::Thread(Thread&& other) noexcept {
    StoreHandle(m_storage, LoadHandle(other.m_storage));
    StoreHandle(other.m_storage, nullptr);
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        MONARC_CHECK(!IsRunning(),
                     "Thread move-assigned over while still running -- call Join() first");
        Join();   // Same fallback as the destructor, and for the same reason.
        StoreHandle(m_storage, LoadHandle(other.m_storage));
        StoreHandle(other.m_storage, nullptr);
    }
    return *this;
}

Status Thread::StartTask(StringView name, Detail::IThreadTask* task) {
    MONARC_CHECK(!IsRunning(), "Thread::Start called while this Thread is already running");
    if (IsRunning()) {
        delete task;
        return Err(ErrorCode::AlreadyExists, "thread is already running");
    }

    unsigned  threadId = 0;
    const auto created = _beginthreadex(nullptr, 0, &ThreadMain, task, 0, &threadId);
    if (created == 0) {
        delete task;
        // _beginthreadex reports failure through errno (EAGAIN/EINVAL/EACCES), not
        // GetLastError. Every documented cause is resource exhaustion from Monarc's point
        // of view, since every argument passed above is a fixed, valid constant -- there
        // is no bad-argument case reachable from this call site.
        return Err(ErrorCode::OutOfMemory, "could not create thread");
    }

    const HANDLE handle = reinterpret_cast<HANDLE>(created);
    StoreHandle(m_storage, handle);

    // Best-effort: a thread name is a profiler/debugger convenience, not a correctness
    // requirement, so failure here -- including the thread having already run to
    // completion before this line, which is possible since it started running immediately
    // above -- is not surfaced to the caller.
    wchar_t wideName[kMaxThreadNameWide];
    ThreadNameToWide(name, wideName);
    SetThreadDescription(handle, wideName);

    return {};
}

void Thread::Join() {
    const HANDLE handle = LoadHandle(m_storage);
    if (handle == nullptr) {
        return;
    }
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
    StoreHandle(m_storage, nullptr);
}

bool Thread::IsRunning() const { return LoadHandle(m_storage) != nullptr; }

}  // namespace Monarc::Platform
