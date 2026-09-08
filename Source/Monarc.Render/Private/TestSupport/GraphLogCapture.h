#pragma once

#include <Monarc/Core/Log.h>
#include <Monarc/Core/Types.h>

#include <string_view>

namespace Monarc::Render::TestSupport {

/// Captures Monarc's log output for the whole of a test run, instead of printing it.
///
/// **Two reasons, and the first is the one that makes this worth a file.** `RenderGraph::Refuse`
/// emits a `MONARC_LOG(..., Error, ...)` beside every diagnostic it records, and the suite
/// refuses on purpose about thirty times -- so without this the run prints thirty `[Error]`
/// lines and passes, which trains a reader to ignore the word `Error` in a CI log. Neither
/// `Monarc.Core.Tests` nor `Monarc.RHI.Tests` prints one, and this suite should not be the
/// exception.
///
/// The second is that the log line is a *claim* the code makes -- Refuse's own comment says
/// the log carries the composed detail that `Error::message` cannot, because that message is a
/// non-owning view and therefore a string literal. A claim about what is logged is only worth
/// making if something reads it back, and nothing in-process can read a line that went to
/// stderr. Captured, it can be asserted; see TestPassDeclaration.cpp.
///
/// `Monarc/Core/Tests/TestLog.cpp`'s `ScopedSink` is the same mechanism at case scope. This one
/// is installed by the suite's `main` for the whole run, because the alternative -- an RAII
/// object in each of the fifteen cases that refuse -- would be fifteen chances to forget one
/// and go back to printing.
///
/// Not thread-safe, and it does not need to be: `SetLogSink` is documented as unsynchronised
/// and doctest runs cases on the calling thread.
class LogCapture {
public:
    /// Longest line kept. Longer lines are stored truncated, and `Truncated()` says whether
    /// any was -- a silently clipped line would make `Contains` answer "no" for a line that
    /// did contain what was asked about.
    static constexpr usize kMaxLineLength = 256;

    /// How many lines are kept. Beyond this the oldest is dropped and `Dropped()` counts it,
    /// so a case cannot read a partial history as a complete one.
    static constexpr usize kMaxLines = 64;

    /// Installs the capturing sink and returns the one it replaced.
    static LogSink Install();

    /// Puts `previous` back.
    static void Restore(LogSink previous);

    /// Forgets every captured line, and resets `Dropped()` and `Truncated()`. A case that
    /// asserts on the log calls this first, so that it reads its own lines and not the
    /// previous case's.
    static void Clear();

    [[nodiscard]] static usize Count();
    [[nodiscard]] static usize Dropped();
    [[nodiscard]] static bool  Truncated();

    /// Line `index`, or an empty view if there is no such line.
    [[nodiscard]] static std::string_view Line(usize index);

    /// Whether any captured line contains `needle`.
    [[nodiscard]] static bool Contains(std::string_view needle);
};

}  // namespace Monarc::Render::TestSupport
