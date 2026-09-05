#include <Monarc/Core/Log.h>

#include <cstdio>

namespace Monarc {
namespace {

void ConsoleSink(const LogRecord& record) {
    std::fprintf(record.level >= LogLevel::Error ? stderr : stdout,
                 "[%-7s] %.*s: %.*s\n",
                 ToString(record.level),
                 static_cast<int>(record.category.size()), record.category.data(),
                 static_cast<int>(record.message.size()), record.message.data());
}

LogSink g_sink = &ConsoleSink;

}  // namespace

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:   return "Trace";
        case LogLevel::Debug:   return "Debug";
        case LogLevel::Info:    return "Info";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Error:   return "Error";
        case LogLevel::Fatal:   return "Fatal";
    }
    return "Unknown";
}

LogSink SetLogSink(LogSink sink) {
    LogSink previous = g_sink;
    g_sink = sink != nullptr ? sink : &ConsoleSink;
    return previous;
}

namespace Detail {

void Emit(const LogCategory& category, LogLevel level, const char* file, int line,
          std::string_view message) {
    g_sink(LogRecord{category.name, level, message, file, line});
}

}  // namespace Detail
}  // namespace Monarc
