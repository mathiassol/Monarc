#pragma once

#include <Monarc/Core/Types.h>

#include <format>
#include <string_view>
#include <utility>

namespace Monarc {

enum class LogLevel : u8 {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

const char* ToString(LogLevel level);

/// Longest message a single log call can emit. Formatting happens into a stack buffer
/// of this size, so the runtime log path performs no allocation; longer messages are
/// truncated.
inline constexpr usize kMaxLogMessageLength = 1024;

/// A category groups related messages and carries its own threshold, so verbosity is
/// controlled per subsystem rather than globally.
struct LogCategory {
    const char* name;
    LogLevel    minLevel;
};

struct LogRecord {
    std::string_view category;
    LogLevel         level;
    std::string_view message;
    const char*      file;
    int              line;
};

using LogSink = void (*)(const LogRecord& record);

/// Installs a sink and returns the previous one.
LogSink SetLogSink(LogSink sink);

namespace Detail {
void Emit(const LogCategory& category, LogLevel level, const char* file, int line,
          std::string_view message);

template <typename... Args>
void Format(const LogCategory& category, LogLevel level, const char* file, int line,
            std::format_string<Args...> fmt, Args&&... args) {
    char        buffer[kMaxLogMessageLength];
    const auto  result = std::format_to_n(buffer, kMaxLogMessageLength, fmt,
                                          std::forward<Args>(args)...);
    const usize written = static_cast<usize>(result.out - buffer);
    Emit(category, level, file, line, std::string_view(buffer, written));
}
}  // namespace Detail

}  // namespace Monarc

/// Declares a log category. Place at namespace scope.
#define MONARC_LOG_CATEGORY(CategoryName, MinimumLevel)                                   \
    inline ::Monarc::LogCategory CategoryName{#CategoryName, ::Monarc::LogLevel::MinimumLevel}

/// Logs a formatted message. Arguments are not evaluated when the level is filtered out,
/// so expensive arguments cost nothing in a build that discards them.
#define MONARC_LOG(Category, Level, ...)                                                  \
    do {                                                                                  \
        if (::Monarc::LogLevel::Level >= (Category).minLevel) {                           \
            ::Monarc::Detail::Format((Category), ::Monarc::LogLevel::Level, __FILE__,     \
                                     __LINE__, __VA_ARGS__);                              \
        }                                                                                 \
    } while (false)
