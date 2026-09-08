#include <TestSupport/GraphLogCapture.h>

#include <algorithm>

namespace Monarc::Render::TestSupport {

namespace {

struct CaptureState {
    char  lines[LogCapture::kMaxLines][LogCapture::kMaxLineLength] = {};
    usize lengths[LogCapture::kMaxLines]                           = {};
    usize count                                                    = 0;
    usize dropped                                                  = 0;
    bool  truncated                                                = false;
};

// A function-local static rather than a namespace-scope one, so that nothing depends on
// static-initialisation order between this translation unit and Monarc.Core's Log.cpp -- the
// sink `Install` replaces is itself set up by a static initialiser there.
CaptureState& State() {
    static CaptureState state;
    return state;
}

void CaptureSink(const LogRecord& record) {
    CaptureState& state = State();
    if (state.count == LogCapture::kMaxLines) {
        ++state.dropped;
        return;
    }
    const usize length = std::min(record.message.size(), LogCapture::kMaxLineLength);
    if (length < record.message.size()) {
        state.truncated = true;
    }
    std::copy_n(record.message.data(), length, state.lines[state.count]);
    state.lengths[state.count] = length;
    ++state.count;
}

}  // namespace

LogSink LogCapture::Install() { return SetLogSink(&CaptureSink); }

void LogCapture::Restore(LogSink previous) { SetLogSink(previous); }

void LogCapture::Clear() {
    CaptureState& state = State();
    state.count         = 0;
    state.dropped       = 0;
    state.truncated     = false;
}

usize LogCapture::Count() { return State().count; }

usize LogCapture::Dropped() { return State().dropped; }

bool LogCapture::Truncated() { return State().truncated; }

std::string_view LogCapture::Line(usize index) {
    const CaptureState& state = State();
    if (index >= state.count) {
        return {};
    }
    return std::string_view(state.lines[index], state.lengths[index]);
}

bool LogCapture::Contains(std::string_view needle) {
    const CaptureState& state = State();
    for (usize i = 0; i < state.count; ++i) {
        if (std::string_view(state.lines[i], state.lengths[i]).find(needle) !=
            std::string_view::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace Monarc::Render::TestSupport
