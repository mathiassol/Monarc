// Monarc.Jobs' fatal guards, invoked one per child process.
//
// The mechanism, the declining assert handler, and why the mode is handled before doctest
// starts are all written down once in Monarc.Core/Tests/TestDeathGuards.cpp. This file is that
// file's shape applied to this module's guards, and only what differs is argued here.
//
// **Two of this module's three guards are covered.** `JobSystem::Wait`'s worker guard and
// `OnResourceExhausted` are below. `JobSystem::PopQueueLocked`'s guard is not, and cannot be:
// it is a private member called from `WorkerLoop` only after `AnyQueuedLocked()` has already
// returned true under the same lock, so reaching it means corrupting the pool's internal state
// from outside -- there is no public call, legal or illegal, that arrives there. Recorded rather
// than left as an apparent omission.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/Core/Memory/SystemAllocator.h>
#include <Monarc/Core/Types.h>
#include <Monarc/Jobs/JobHandle.h>
#include <Monarc/Jobs/JobSystem.h>

#include <cstdio>
#include <string_view>

using Monarc::IAllocator;
using Monarc::JobHandle;
using Monarc::JobSystem;
using Monarc::Result;
using Monarc::StringView;
using Monarc::SystemAllocator;
using Monarc::usize;

namespace {

constexpr std::string_view kGuardOption    = "--monarc-death-guard=";
constexpr std::string_view kEnteredMarker  = "MONARC_DEATH_GUARD_ENTERED";
constexpr std::string_view kSurvivedMarker = "MONARC_DEATH_GUARD_SURVIVED";

bool DeclineToBreak(const char* expression, const char* file, int line, const char* message) {
    std::fprintf(stderr, "[declining assert handler] %s:%d: (%s) %s\n", file, line, expression,
                 message != nullptr ? message : "");
    std::fflush(stderr);
    return false;
}

/// An allocator that succeeds below `kFailFromBytes` and fails at or above it.
///
/// **A threshold and not "fail everything", because which allocation fails decides whether the
/// mutation proof means anything.** `JobSystem`'s constructor makes five or six allocations and
/// hands every failure to the same `OnResourceExhausted`; failing the first of them -- the job
/// pool -- reaches the guard, but with the guard removed the very next line is
/// `std::construct_at(&m_slots[0], ...)` through a null pointer, so the child would crash
/// either way and the death test would pass whether the guard was there or not.
///
/// The profile buffer is the one allocation whose failure the rest of the constructor survives:
/// `m_profileRecords` stays null, nothing reads it unless a job completes, and the destructor
/// already guards it with `if (m_profileRecords != nullptr)`. So a child that submits no job
/// returns cleanly with the guard removed and dies with it present, which is the difference the
/// harness measures.
///
/// A size threshold rather than a call count, so the test does not depend on the order the
/// constructor allocates in. With `maxJobs` and `workerCount` of one, the job pool is one
/// `JobSlot` (64 bytes of inline callable plus its fields), each ready queue is one `u32`, and
/// the worker array is one `Platform::Thread` -- all far below the threshold -- while the
/// profile buffer is 1024 `JobProfileRecord`s, which is tens of kilobytes. If that ever stopped
/// being true the death test would fail loudly rather than quietly: the harness matches the
/// profile buffer's own message.
constexpr usize kFailFromBytes = 4096;

class ThresholdAllocator final : public IAllocator {
public:
    void* Allocate(usize size, usize alignment) override {
        if (size >= kFailFromBytes) {
            std::fprintf(stderr, "[threshold allocator] refusing %zu bytes\n", size);
            std::fflush(stderr);
            return nullptr;
        }
        return m_system.Allocate(size, alignment);
    }

    void Deallocate(void* pointer, usize size, usize alignment) override {
        m_system.Deallocate(pointer, size, alignment);
    }

    [[nodiscard]] usize BytesAllocated() const override { return m_system.BytesAllocated(); }
    [[nodiscard]] const char* Name() const override { return "ThresholdAllocator"; }

private:
    SystemAllocator m_system;
};

// ---------------------------------------------------------------------------------------
// The guards.
// ---------------------------------------------------------------------------------------

/// `OnResourceExhausted`, through the profile buffer's allocation failing.
void JobSystemResourceExhausted() {
    ThresholdAllocator allocator;
    JobSystem::Config  config{};
    config.workerCount     = 1;
    config.maxJobs         = 1;
    config.profileCapacity = 1024;   // Tens of kilobytes: the only allocation over the threshold.
    JobSystem jobs(allocator, config);
    // No job is submitted, so nothing reads `m_profileRecords` -- see ThresholdAllocator above
    // for why that is what makes this guard's removal observable rather than a second crash.
}

/// `JobSystem::Wait`'s worker guard: a job that waits, from inside the pool.
///
/// **The handle is a default one, deliberately.** The guard is the first thing `Wait` does,
/// before any lock and before the handle is looked at, so which handle is passed changes
/// nothing about whether it fires -- and a handle that could not possibly complete is the
/// honest way to say that the wait was never going to return.
///
/// With the guard removed this **hangs** rather than returning: execution falls into
/// `while (!IsCompleteLocked(handle)) m_cv.Wait(m_mutex);` with the pool's only worker inside
/// it. That is the outcome `JobSystem::Wait`'s own comment records as measured, and it is why
/// `Tools/run_death_test.py` treats a child that times out as a failure rather than as a death.
void JobSystemWaitFromWorker() {
    SystemAllocator   allocator;
    JobSystem::Config config{};
    config.workerCount = 1;
    config.maxJobs     = 2;
    JobSystem jobs(allocator, config);

    const Result<JobHandle> submitted =
        jobs.Submit(StringView("waits-from-a-worker"), [&jobs] { jobs.Wait(JobHandle{}); });
    if (!submitted) {
        std::fprintf(stderr, "could not submit the waiting job\n");
        std::fflush(stderr);
        return;
    }
    jobs.Wait(*submitted);
}

struct Guard {
    std::string_view name;
    void (*invoke)();
};

constexpr Guard kGuards[] = {
    {"jobsystem-resource-exhausted", &JobSystemResourceExhausted},
    {"jobsystem-wait-from-worker", &JobSystemWaitFromWorker},
};

int RunGuard(std::string_view name) {
    for (const Guard& guard : kGuards) {
        if (guard.name != name) {
            continue;
        }
        Monarc::SetAssertHandler(&DeclineToBreak);

        std::printf("%.*s %.*s\n", static_cast<int>(kEnteredMarker.size()),
                    kEnteredMarker.data(), static_cast<int>(name.size()), name.data());
        std::fflush(stdout);

        guard.invoke();

        // Flushed explicitly on both sides: MONARC_DEBUG_BREAK() ends the process without
        // flushing stdio, so a buffered marker would be lost on the run that must print it.
        std::printf("%.*s %.*s\n", static_cast<int>(kSurvivedMarker.size()),
                    kSurvivedMarker.data(), static_cast<int>(name.size()), name.data());
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(stderr, "no such death guard: %.*s\nknown guards:\n",
                 static_cast<int>(name.size()), name.data());
    for (const Guard& guard : kGuards) {
        std::fprintf(stderr, "  %.*s\n", static_cast<int>(guard.name.size()),
                     guard.name.data());
    }
    std::fflush(stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.starts_with(kGuardOption)) {
            return RunGuard(argument.substr(kGuardOption.size()));
        }
    }

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
