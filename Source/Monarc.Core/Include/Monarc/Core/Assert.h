#pragma once

#include <Monarc/Core/Types.h>

namespace Monarc {

/// Returns true if the caller should break into the debugger.
/// Replaceable so that assertion behaviour itself can be tested.
using AssertHandler = bool (*)(const char* expression,
                               const char* file,
                               int         line,
                               const char* message);

/// Installs a handler and returns the previous one. Not thread-safe; intended for
/// process start-up and for tests.
AssertHandler SetAssertHandler(AssertHandler handler);

namespace Detail {
bool OnAssertFailed(const char* expression, const char* file, int line, const char* message);
}  // namespace Detail

}  // namespace Monarc

#if defined(_MSC_VER)
#    define MONARC_DEBUG_BREAK() __debugbreak()
#else
#    define MONARC_DEBUG_BREAK() __builtin_trap()
#endif

/// Checked in every configuration except Shipping. Use for conditions whose violation
/// means the program is already wrong.
#define MONARC_CHECK(expression, message)                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            if (::Monarc::Detail::OnAssertFailed(#expression, __FILE__, __LINE__,         \
                                                 (message))) {                            \
                MONARC_DEBUG_BREAK();                                                     \
            }                                                                             \
        }                                                                                 \
    } while (false)

/// Checked only when MONARC_ENABLE_ASSERTS is on. Use for expensive invariants.
///
/// When disabled, both operands are still referenced inside `sizeof`, which is
/// unevaluated. That keeps `expression` type-checked, and — just as importantly — keeps
/// anything either operand mentions "used". Dropping `message` would mean a diagnostic
/// built from a local variable turned that local into an unreferenced one, failing /WX
/// in Release while Debug stayed green: the worst shape a build error can have.
///
/// Note: like the standard `assert`, a top-level comma in `expression` splits the macro
/// call. Parenthesise such expressions — MONARC_ASSERT((is_same<A, B>::value), "...").
#if MONARC_ENABLE_ASSERTS
#    define MONARC_ASSERT(expression, message) MONARC_CHECK(expression, message)
#else
#    define MONARC_ASSERT(expression, message)                                            \
        do {                                                                              \
            (void)sizeof(!(expression));                                                  \
            (void)sizeof(message);                                                        \
        } while (false)
#endif
