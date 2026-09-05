#include <Monarc/Core/Assert.h>

#include <cstdio>

namespace Monarc {
namespace {

bool DefaultAssertHandler(const char* expression, const char* file, int line,
                          const char* message) {
    std::fprintf(stderr, "[assert] %s:%d: (%s) %s\n", file, line, expression,
                 message ? message : "");
    std::fflush(stderr);
    return true;   // break into the debugger
}

AssertHandler g_handler = &DefaultAssertHandler;

}  // namespace

AssertHandler SetAssertHandler(AssertHandler handler) {
    AssertHandler previous = g_handler;
    g_handler = handler ? handler : &DefaultAssertHandler;
    return previous;
}

namespace Detail {

bool OnAssertFailed(const char* expression, const char* file, int line, const char* message) {
    return g_handler(expression, file, line, message);
}

}  // namespace Detail
}  // namespace Monarc
