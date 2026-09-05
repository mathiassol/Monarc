#include <doctest/doctest.h>

#include <Monarc/Core/Log.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

struct CapturedLine {
    std::string      category;
    Monarc::LogLevel level;
    std::string      message;
};

std::vector<CapturedLine> g_lines;

void CaptureSink(const Monarc::LogRecord& record) {
    g_lines.push_back({std::string(record.category), record.level, std::string(record.message)});
}

struct ScopedSink {
    Monarc::LogSink previous;
    ScopedSink() : previous(Monarc::SetLogSink(&CaptureSink)) { g_lines.clear(); }
    ~ScopedSink() { Monarc::SetLogSink(previous); }
};

MONARC_LOG_CATEGORY(LogTest, Info);

}  // namespace

TEST_CASE("MONARC_LOG delivers category, level and formatted message") {
    ScopedSink sink;
    MONARC_LOG(LogTest, Warning, "disk {} is {}% full", "C:", 91);

    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0].category == "LogTest");
    CHECK(g_lines[0].level == Monarc::LogLevel::Warning);
    CHECK(g_lines[0].message == "disk C: is 91% full");
}

TEST_CASE("messages below the category's minimum level are dropped") {
    ScopedSink sink;
    MONARC_LOG(LogTest, Trace, "should not appear");
    MONARC_LOG(LogTest, Debug, "should not appear either");
    MONARC_LOG(LogTest, Info, "should appear");

    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0].message == "should appear");
}

TEST_CASE("a category's minimum level can be changed at runtime") {
    ScopedSink sink;
    const Monarc::LogLevel original = LogTest.minLevel;

    LogTest.minLevel = Monarc::LogLevel::Error;
    MONARC_LOG(LogTest, Warning, "filtered out");
    CHECK(g_lines.empty());

    LogTest.minLevel = original;
    MONARC_LOG(LogTest, Warning, "now visible");
    CHECK(g_lines.size() == 1);
}

TEST_CASE("arguments are not evaluated when the message is filtered out") {
    ScopedSink sink;
    int calls = 0;
    auto expensive = [&calls] { ++calls; return 42; };

    MONARC_LOG(LogTest, Trace, "value {}", expensive());
    CHECK(calls == 0);

    MONARC_LOG(LogTest, Info, "value {}", expensive());
    CHECK(calls == 1);
}

TEST_CASE("over-long messages are truncated rather than overflowing") {
    ScopedSink sink;
    const std::string huge(4000, 'x');
    MONARC_LOG(LogTest, Info, "{}", huge);

    REQUIRE(g_lines.size() == 1);
    CHECK(g_lines[0].message.size() < huge.size());
    CHECK(g_lines[0].message.size() <= Monarc::kMaxLogMessageLength);
}

TEST_CASE("ToString names every level") {
    using Monarc::LogLevel;
    CHECK(std::string_view(Monarc::ToString(LogLevel::Trace)) == "Trace");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Debug)) == "Debug");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Info)) == "Info");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Warning)) == "Warning");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Error)) == "Error");
    CHECK(std::string_view(Monarc::ToString(LogLevel::Fatal)) == "Fatal");
}
