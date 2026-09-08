// Monarc.Core's fatal guards, invoked one per child process.
//
// **The file that makes "this guard ends the process" a test rather than a comment.** Three of
// Monarc's guards live in this module -- `Array<T>::OnAllocationFailed`,
// `HashMap<K,V>::OnAllocationFailed` and `String::OnAllocationFailed` -- and none of them had a
// case in any suite until A3 Task 5. They cannot have an in-process one: the assertion is that
// the process *stops*, and a doctest case that stopped would take the other 400 with it. So
// this binary grows a mode: `--monarc-death-guard=<name>` invokes exactly one guard and nothing
// else, and `Tools/run_death_test.py` runs it as a child and judges what came out.
//
// **The declining assert handler is the whole reason these are testable, and it is not a
// contrivance.** `MONARC_CHECK` reports and then breaks *if the handler says to*; the default
// handler says to, so a child running with it would die inside `MONARC_CHECK` whatever the
// guard below it did -- and removing the guard entirely would not change the outcome. Every one
// of these guards is written unconditionally for exactly that reason, and every one of their
// comments says so: "under a handler that declines to break -- Shipping, or any test harness --
// execution would fall through". `DeclineToBreak` *is* that handler. It is what the hand-run
// scratch programs installed, and it is the configuration the guards exist to survive.
//
// **The mode is handled in `main` before doctest starts, deliberately.** doctest installs an
// SEH filter, and a breakpoint exception raised inside `context.run()` is caught by it and
// reported as a failed assertion -- exit code 1, with the process alive long enough to write a
// summary. Outside it the break is unhandled and the process dies at 0x80000003, which is what
// the harness is written against. Running the guard before doctest exists also means no
// registry, no reporter and no partial results to interpret.
//
// **What is deliberately not here.** `Guid::Generate`'s entropy guard is in this module and is
// not covered: it fires when `BCryptGenRandom` fails, and nothing a test can do makes the
// platform's preferred RNG fail. Recorded rather than left as an apparent omission.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Containers/Array.h>
#include <Monarc/Core/Containers/HashMap.h>
#include <Monarc/Core/Containers/String.h>
#include <Monarc/Core/Memory/Allocator.h>
#include <Monarc/Core/Types.h>

#include <cstdio>
#include <string_view>

using Monarc::Array;
using Monarc::HashMap;
using Monarc::IAllocator;
using Monarc::String;
using Monarc::u32;
using Monarc::usize;

namespace {

/// The option `Tools/run_death_test.py` passes, and the two lines it looks for.
///
/// Spelled here and in that script. A mismatch makes every death test fail loudly rather than
/// pass silently -- the harness *requires* the entered marker, so a typo on either side means
/// no test can be satisfied. That is the only direction a constant duplicated across two
/// languages is allowed to break in.
constexpr std::string_view kGuardOption   = "--monarc-death-guard=";
constexpr std::string_view kEnteredMarker = "MONARC_DEATH_GUARD_ENTERED";
constexpr std::string_view kSurvivedMarker = "MONARC_DEATH_GUARD_SURVIVED";

/// An assert handler that reports and returns false: "do not break".
///
/// Shipping's handler and a test harness's handler are the same shape, and it is the shape
/// every guard in Monarc is written against.
bool DeclineToBreak(const char* expression, const char* file, int line, const char* message) {
    std::fprintf(stderr, "[declining assert handler] %s:%d: (%s) %s\n", file, line, expression,
                 message != nullptr ? message : "");
    std::fflush(stderr);
    return false;
}

/// An allocator whose every allocation fails.
///
/// `IAllocator::Allocate` documents nullptr as its failure return, so this is a legitimate
/// allocator and not a broken one -- which is the point: the guards below are what a real
/// out-of-memory condition reaches, and nothing else in the tree can reach them.
class FailingAllocator final : public IAllocator {
public:
    void* Allocate(usize, usize) override { return nullptr; }
    void  Deallocate(void*, usize, usize) override {}
    [[nodiscard]] usize       BytesAllocated() const override { return 0; }
    [[nodiscard]] const char* Name() const override { return "FailingAllocator"; }
};

// ---------------------------------------------------------------------------------------
// The guards. Each one does the smallest thing that reaches its guard, and nothing after --
// so that with the guard removed the child returns cleanly and prints the survived marker,
// rather than crashing on the state the guard was protecting.
// ---------------------------------------------------------------------------------------

/// `Array<T>::OnAllocationFailed`, through `AllocateBuffer`'s null return.
void ArrayAllocationFailed() {
    FailingAllocator allocator;
    Array<u32>       array(allocator);
    array.Reserve(1);
    // With the guard removed this leaves `m_data` null and `m_capacity` 1, which the
    // destructor handles -- `Clear` over zero elements and `Deallocate(nullptr, ...)`, both
    // no-ops. Nothing here dereferences it, so "the guard returned" is observable as a clean
    // exit rather than as a second crash that would look like the guard firing.
}

/// `HashMap<K, V>::OnAllocationFailed`, through `AllocateEntries`' null return.
///
/// **Weaker than the `Array` entry above, and the difference was measured rather than
/// assumed.** There is no HashMap operation that reaches this guard and then leaves the
/// returned pointer alone: `AllocateEntries` and `AllocateOccupied` are both called only from
/// `Grow`, and `Insert` goes straight on to `InsertNew`, which reads `m_occupied[index]` and
/// constructs into `m_entries + index`. So with the guard turned into a plain `return nullptr`
/// the child still dies -- at the *other* guard on the same path (measured: `HashMap.h:357`,
/// exit `0x80000003`), and with both removed at an access violation (`0xC0000005`).
///
/// What this entry therefore asserts is that an allocation failure stops the process **and
/// says why**. It goes red when the diagnostic disappears -- measured: with both
/// `MONARC_CHECK`s and both guard calls removed, the child crashes at `0xC0000005` having
/// printed nothing, and the expected-message condition fails. That is a real regression and not
/// a tautology; it is simply not the same claim the `Array` entry makes.
void HashMapAllocationFailed() {
    FailingAllocator  allocator;
    HashMap<u32, u32> map(allocator);
    map.Insert(1, 1);
}

/// `String::OnAllocationFailed`, through `AllocateBuffer`'s null return.
///
/// `HashMapAllocationFailed`'s caveat, for the same reason and with the same measurement:
/// `String::Reserve` writes the terminator through the buffer it was just handed
/// (`newData[m_size] = '\0'`), so a guard edited into `return nullptr` produces an access
/// violation (measured: `0xC0000005`) rather than a return. Every other caller of
/// `AllocateBuffer` writes through it too, so no path here can survive its removal.
///
/// Which is itself worth knowing about this guard: what it buys `String` is a message and a
/// stack, not safety -- the failure was going to be loud either way. The `Array` guard is the
/// one whose absence would be *silent*, and that asymmetry is now recorded rather than assumed
/// uniform.
void StringAllocationFailed() {
    FailingAllocator allocator;
    String           text(allocator);
    text.Reserve(1);
}

struct Guard {
    std::string_view name;
    void (*invoke)();
};

/// Every guard this binary can reach. The names are what CTest entries pass.
constexpr Guard kGuards[] = {
    {"array-allocation-failed", &ArrayAllocationFailed},
    {"hashmap-allocation-failed", &HashMapAllocationFailed},
    {"string-allocation-failed", &StringAllocationFailed},
};

/// Runs the named guard, or reports that there is no such guard.
///
/// Returns the process exit code. Reached only when `--monarc-death-guard` was passed, and the
/// only way out of a firing guard is that the process ends inside `invoke`.
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

        // Reached only if the guard returned. `MONARC_DEBUG_BREAK()` kills the process without
        // flushing anything, so the marker above is flushed before the call and this one after
        // -- a buffered marker would be lost on the very run that is supposed to print it.
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
