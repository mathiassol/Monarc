#include <Monarc/Render/GraphInspection.h>

#include <Monarc/RHI/Types.h>

#include <cstddef>
#include <format>
#include <utility>

namespace Monarc::Render {

// All five switches below are deliberately `default`-less, the shape
// Monarc.RHI/Private/Barrier.cpp uses and for its reason: adding an enumerator becomes a
// compile error here rather than a silent fall-through to the trailing return. MSVC's C4062 is
// off by default and /W4 does not enable it, so CMake/MonarcTargetOptions.cmake passes /w44062
// by name; Clang's -Wswitch is on at /W4; /WX makes both fatal.
//
// The trailing returns still have to exist: each enum has a fixed underlying type and can hold
// a value no enumerator names, which is what Tests/TestGraphInspection.cpp's completeness
// checks hand them.

const char* ToString(GraphPhase phase) {
    switch (phase) {
        case GraphPhase::Declaring:     return "Declaring";
        case GraphPhase::Compiled:      return "Compiled";
        case GraphPhase::CompileFailed: return "CompileFailed";
    }
    return "<invalid GraphPhase>";
}

const char* ToString(ResourceOrigin origin) {
    switch (origin) {
        case ResourceOrigin::Transient: return "Transient";
        case ResourceOrigin::Imported:  return "Imported";
    }
    return "<invalid ResourceOrigin>";
}

const char* ToString(GraphQueue queue) {
    switch (queue) {
        case GraphQueue::Graphics: return "Graphics";
    }
    return "<invalid GraphQueue>";
}

const char* ToString(BarrierCauseKind kind) {
    switch (kind) {
        case BarrierCauseKind::ImportIncoming:    return "ImportIncoming";
        case BarrierCauseKind::TransientCreation: return "TransientCreation";
        case BarrierCauseKind::PassAccess:        return "PassAccess";
        case BarrierCauseKind::ImportOutgoing:    return "ImportOutgoing";
    }
    return "<invalid BarrierCauseKind>";
}

const char* ToString(DiagnosticKind kind) {
    switch (kind) {
        case DiagnosticKind::PassPoolExhausted:       return "PassPoolExhausted";
        case DiagnosticKind::ResourcePoolExhausted:   return "ResourcePoolExhausted";
        case DiagnosticKind::AccessPoolExhausted:     return "AccessPoolExhausted";
        case DiagnosticKind::UnknownResource:         return "UnknownResource";
        case DiagnosticKind::UnknownPass:             return "UnknownPass";
        case DiagnosticKind::AccessDirectionMismatch: return "AccessDirectionMismatch";
        case DiagnosticKind::AccessNamesNoTexture:    return "AccessNamesNoTexture";
        case DiagnosticKind::DuplicateAccess:         return "DuplicateAccess";
        case DiagnosticKind::AccessLayoutConflict:    return "AccessLayoutConflict";
        case DiagnosticKind::DuplicateImport:         return "DuplicateImport";
        case DiagnosticKind::InvalidImport:           return "InvalidImport";
        case DiagnosticKind::RecordAlreadySet:        return "RecordAlreadySet";
        case DiagnosticKind::AlreadyCompiled:         return "AlreadyCompiled";
        case DiagnosticKind::DependencyCycle:         return "DependencyCycle";
        case DiagnosticKind::TransientNeverWritten:   return "TransientNeverWritten";
        case DiagnosticKind::UnorderedOverwrite:      return "UnorderedOverwrite";
        case DiagnosticKind::BarrierPoolExhausted:    return "BarrierPoolExhausted";
    }
    return "<invalid DiagnosticKind>";
}

namespace {

// ---------------------------------------------------------------------------------------
// Rendering the inspection as text.
//
// `std::format_to_n` and never `std::format`, which is ADR-0003's condition on `<format>` in
// runtime code: format into a fixed buffer, never allocate a `std::string`.
// `Monarc::Detail::Format` in Monarc/Core/Log.h and `Describe` in Monarc.RHI/Private/Barrier.cpp
// are the precedents.
//
// **What this file actually allocates, measured rather than asserted.** Every call below was
// run under a counting global `operator new` on MSVC 19.51 and clang-cl 22.1, with the exact
// format strings and argument types used here. Both `std::format_to_n` and
// `std::formatted_size` allocate nothing for every field rendered -- **except** that an integer
// given an explicit presentation type allocates exactly one 16-byte block, freed inside the
// same call. `{:x}`, `{:d}`, `{:o}` and `{:b}` all do it; a plain `{}` never does, whatever its
// type. So it is the presentation type that buys the allocation, not the entry point: the count
// is identical on the two, and identical on the two compilers, which share the MSVC STL.
//
// **It is the debug standard library's checked-iterator container proxy, not `<format>`
// producing storage**, and the flag is `_ITERATOR_DEBUG_LEVEL` rather than the runtime library:
// measured at 1 allocation under `/MDd` and `/MTd`, 0 under `/MD` and `/MT`, 0 under `/MDd`
// with `_ITERATOR_DEBUG_LEVEL=0`, and back to 1 under `/MD` with `_ITERATOR_DEBUG_LEVEL=1`.
// Of the six presets, the two that compile `-MDd` -- msvc-debug and clang-debug -- are the ones
// where the `usage=0x{:x}` on a resource line, and the four `0x{:x}` fields on a barrier line,
// cost one transient proxy each. msvc-release, clang-release and
// clang-asan are `-MD` and clang-ubsan is `-MT`, so on those four the report allocates nothing
// at all.
//
// Recorded because this file said "allocates nothing" before anything measured it.
// ---------------------------------------------------------------------------------------

/// Appends formatted text to a caller's buffer, counting what fitted and what there was.
///
/// The count continues past the end of the buffer, which is the whole point: a caller can pass
/// a zero-length span to ask how much room the report needs, and a caller whose buffer was too
/// small is told rather than left with a report clipped at the end -- where the barriers are.
class TextWriter {
public:
    explicit TextWriter(std::span<char> out) : m_out(out) {}

    template <typename... Args>
    void Line(std::format_string<Args...> format, Args&&... args) {
        const usize room = m_out.size() - m_written;
        if (room == 0) {
            // `std::formatted_size` counts through a counting iterator and produces no output
            // storage of its own. Used rather than a zero-count `format_to_n` so that no
            // pointer is formed from an empty span's null `data()`.
            //
            // **The one `<format>` entry point in this tree that is not `std::format_to_n`,
            // and it satisfies ADR-0003's condition rather than sidestepping it.** That
            // amendment permits `<format>` in runtime code "on the condition that it formats
            // into a fixed buffer via `std::format_to_n` rather than allocating a
            // `std::string`" -- the mechanism and its purpose in one sentence, and the purpose
            // is what this meets: the call returns a `usize` and formats into nothing at all,
            // which is a strict subset of what the condition permits. Read as a whitelist of
            // one function name it would forbid `std::format_to` into a fixed array too, which
            // nobody intends. Noted here rather than by a fresh ADR amendment because the
            // decision has not changed and its wording is not wrong about the tree -- an
            // amendment would have nothing to restate. See the file header for what both entry
            // points were measured to allocate, which is the same for each.
            m_needed += std::formatted_size(format, std::forward<Args>(args)...);
            return;
        }
        const std::format_to_n_result<char*> result =
            std::format_to_n(m_out.data() + m_written, static_cast<std::ptrdiff_t>(room), format,
                             std::forward<Args>(args)...);
        m_needed += static_cast<usize>(result.size);
        m_written = m_needed < m_out.size() ? m_needed : m_out.size();
    }

    [[nodiscard]] InspectionText Result() const { return InspectionText{m_written, m_needed}; }

private:
    std::span<char> m_out;
    usize           m_written = 0;
    usize           m_needed  = 0;
};

/// A short field rendered into a fixed buffer and returned by value.
///
/// `AdapterUuidString`'s shape in Monarc.RHI/Include/Monarc/RHI/Adapter.h, chosen for its
/// reason: there is no buffer size to get wrong at a call site and nothing to document about
/// one. Every field below is at most two `u32`s in decimal with a separator, which is 21
/// characters plus a terminator; 24 rounds it.
///
/// NUL-terminated, unlike `WriteInspectionText`'s own output, because `Get()` is handed
/// straight to `std::format` as a `const char*`. Every caller uses the value within the
/// full-expression that made it.
struct FieldText {
    char text[24] = {};

    [[nodiscard]] const char* Get() const { return text; }
};

template <typename... Args>
[[nodiscard]] FieldText Field(std::format_string<Args...> format, Args&&... args) {
    FieldText out{};
    const std::format_to_n_result<char*> result =
        std::format_to_n(out.text, sizeof(out.text) - 1, format, std::forward<Args>(args)...);
    *result.out = '\0';
    return out;
}

/// One `TextureId`, as `index:generation`, or `none` for an invalid one.
[[nodiscard]] FieldText Describe(TextureId id) {
    return id.IsValid() ? Field("{}:{}", id.index, id.generation) : Field("none");
}

/// One `RHI::TextureHandle`, in the same shape. A separate overload rather than a cast into
/// `TextureId`: the two are not interconvertible, which is the point of ResourceId.h, and a
/// renderer that reached for one to print the other would be the first crack in that.
[[nodiscard]] FieldText Describe(RHI::TextureHandle handle) {
    return handle.IsValid() ? Field("{}:{}", handle.index, handle.generation) : Field("none");
}

/// A `u32` that may be a "nothing here" sentinel, as a number or as `none`.
[[nodiscard]] FieldText DescribeOptional(u32 value, u32 sentinel) {
    return value == sentinel ? Field("none") : Field("{}", value);
}

/// One pass's place in the rendered pass list: its execution order, with its position in the
/// array breaking ties.
///
/// **Distinct for every pass**, which is what lets the ordering below be a selection over the
/// caller's own span rather than a sort of a copy of it. `WriteInspectionText` owns no storage
/// but the caller's buffer and takes no allocator, so there is nowhere to put a permutation.
struct PassOrderKey {
    u32   order    = 0;
    usize position = 0;

    [[nodiscard]] constexpr bool operator<(const PassOrderKey& other) const {
        return order != other.order ? order < other.order : position < other.position;
    }
};

void WritePassLine(TextWriter& writer, const PassInspection& pass) {
    writer.Line("pass {} order={} queue={} culled={} record={} name=\"{}\"\n", pass.index,
                DescribeOptional(pass.executionOrder, kNoPass).Get(), ToString(pass.queue),
                pass.culled ? "yes" : "no", pass.hasRecord ? "yes" : "no", pass.name);
}

/// One line per pass, **in execution order rather than in the order the array holds them**.
///
/// Docs/Rendering/Render-Graph.md asks for a "pass list in execution order", and
/// `GraphInspection::passes` deliberately is not one: it is declaration order with
/// `executionOrder` as a field, so that a pass's index into the array is stable and a culled
/// pass still has a row. **The array keeps that shape; the text does not, because the text is
/// the diffable artifact and a diff is worth far more read in the order the frame runs.** Once
/// Task 2 reorders passes, the reordering appears in the report itself rather than having to be
/// reconstructed from `order=` fields scattered down the list. Today every compiled graph has
/// `executionOrder == index`, so this changes no existing output -- which is the point of doing
/// it now rather than noticing it later.
///
/// **Culled passes land at the end**, because `kNoPass` is the largest `u32` and sorts there
/// for free; each is still marked in place by `order=none culled=yes`, so nothing about the
/// line depends on where it fell. A graph inspected mid-declaration has `kNoPass` for every
/// pass, which ties, and the tie-break keeps the array's order -- so an uncompiled graph
/// renders exactly as it did.
///
/// A selection, O(n^2) over a pass list bounded by `RenderGraph::Config::maxPasses` and
/// rendered at most once per frame by a debug view or an artifact dump. Nothing about that is
/// on a hot path, and the alternative needs the storage this function does not have.
void WritePassLines(TextWriter& writer, std::span<const PassInspection> passes) {
    PassOrderKey emitted{};
    bool         anyEmitted = false;

    for (usize n = 0; n < passes.size(); ++n) {
        usize        next = passes.size();
        PassOrderKey nextKey{};
        for (usize i = 0; i < passes.size(); ++i) {
            const PassOrderKey key{passes[i].executionOrder, i};
            if (anyEmitted && !(emitted < key)) {
                continue;
            }
            if (next == passes.size() || key < nextKey) {
                next    = i;
                nextKey = key;
            }
        }
        if (next == passes.size()) {
            // Unreachable, and here for the reason the trailing returns above are: the keys are
            // distinct and `emitted` only ever advances to the smallest key above itself, so
            // after n picks exactly `size() - n` keys remain above it. The compiler cannot know
            // that; no case can reach this.
            return;
        }
        emitted    = nextKey;
        anyEmitted = true;
        WritePassLine(writer, passes[next]);
    }
}

void WriteResourceLines(TextWriter& writer, usize index, const ResourceInspection& resource) {
    // One line per resource, and a second only for an imported one. The extent, format and
    // usage are on the first line because every resource has them; the import states are on
    // their own because only an imported resource has any, and a line of `none`s for every
    // transient would make a diff of a graph full of transients mostly noise.
    writer.Line("resource {} id={} origin={} format={} extent={}x{} usage=0x{:x} "
                "lifetime={}..{} alias={} name=\"{}\"\n",
                index, Describe(resource.id).Get(), ToString(resource.origin),
                RHI::ToString(resource.description.format), resource.description.extent.width,
                resource.description.extent.height,
                static_cast<u32>(resource.description.usage),
                DescribeOptional(resource.lifetime.firstPass, kNoPass).Get(),
                DescribeOptional(resource.lifetime.lastPass, kNoPass).Get(),
                DescribeOptional(resource.aliasGroup, kNoAliasGroup).Get(), resource.name);

    if (resource.origin != ResourceOrigin::Imported) {
        return;
    }
    // The layout leads in each state for the reason `Describe(const TextureBarrier&)` in
    // Monarc.RHI/Include/Monarc/RHI/Barrier.h gives: it is the half that is never a mask, so
    // it always reads as a real spelling. The stage and access carry their hex beside their
    // name because either can be a mask of several bits, which `ToString` reports with a
    // not-a-single-value name -- the number is what keeps the line decodable when it does.
    writer.Line("resource {} import texture={} incoming={}/{}(0x{:x})/{}(0x{:x}) "
                "outgoing={}/{}(0x{:x})/{}(0x{:x})\n",
                index, Describe(resource.importedTexture).Get(),
                RHI::ToString(resource.incoming.layout), RHI::ToString(resource.incoming.stage),
                static_cast<u32>(resource.incoming.stage),
                RHI::ToString(resource.incoming.access),
                static_cast<u32>(resource.incoming.access),
                RHI::ToString(resource.outgoing.layout), RHI::ToString(resource.outgoing.stage),
                static_cast<u32>(resource.outgoing.stage),
                RHI::ToString(resource.outgoing.access),
                static_cast<u32>(resource.outgoing.access));
}

/// One cause side, as `kind:order:access`.
///
/// **`access` renders as `none` for a side that is not a `PassAccess`, and that is the whole
/// reason this is a function rather than three arguments inline.** `ResourceAccess` has no
/// not-an-access value, so `BarrierCauseSide::access` is filler on the other three kinds -- and a
/// line that printed the filler would say `ImportIncoming:order=none:ColorAttachmentRead`, which
/// reads as an import whose incoming state was a colour attachment read. `order=none` beside it
/// is not enough of a signal: a reader has to know that the two fields go silent together.
/// A `const char*` rather than a `FieldText`, because both halves already are one and
/// `FieldText`'s 24-byte buffer would silently clip `DepthStencilAttachmentWrite`.
[[nodiscard]] const char* DescribeCauseAccess(const BarrierCauseSide& side) {
    return side.kind == BarrierCauseKind::PassAccess ? ToString(side.access) : "none";
}

void WriteBarrierLine(TextWriter& writer, usize index, const DerivedBarrier& barrier) {
    // `before-order=` and the `order=` inside each cause are execution positions, where the
    // `decl-pass=` on an access or diagnostic line is a declaration index. Every pass number
    // in this format says which of the two it is -- see `WriteInspectionText`'s comment, and
    // note that the two coincide until Task 2 reorders something, so the naming is what stops
    // a reader from learning the wrong one first. Only the numbers are marked: `PassAccess`
    // and `ColorAttachmentWrite` in a cause triple cannot be mistaken for an index.
    writer.Line("barrier {} resource={} before-order={} layout={}->{} "
                "sync={}(0x{:x})->{}(0x{:x}) access={}(0x{:x})->{}(0x{:x}) "
                "cause={}:order={}:{}->{}:order={}:{}\n",
                index, Describe(barrier.resource).Get(),
                DescribeOptional(barrier.emittedBeforePass, kNoPass).Get(),
                RHI::ToString(barrier.layoutBefore), RHI::ToString(barrier.layoutAfter),
                RHI::ToString(barrier.syncBefore), static_cast<u32>(barrier.syncBefore),
                RHI::ToString(barrier.syncAfter), static_cast<u32>(barrier.syncAfter),
                RHI::ToString(barrier.accessBefore), static_cast<u32>(barrier.accessBefore),
                RHI::ToString(barrier.accessAfter), static_cast<u32>(barrier.accessAfter),
                ToString(barrier.cause.before.kind),
                DescribeOptional(barrier.cause.before.pass, kNoPass).Get(),
                DescribeCauseAccess(barrier.cause.before),
                ToString(barrier.cause.after.kind),
                DescribeOptional(barrier.cause.after.pass, kNoPass).Get(),
                DescribeCauseAccess(barrier.cause.after));
}

}  // namespace

InspectionText WriteInspectionText(const GraphInspection& inspection, std::span<char> out) {
    TextWriter writer(out);

    writer.Line("graph build={} phase={}\n", inspection.buildGeneration,
                ToString(inspection.phase));

    // A counts line, because a diff of two reports should say *that* something appeared before
    // it says what -- and because a truncated report still carries the counts, which is how a
    // reader knows what is missing from the end.
    writer.Line("counts passes={} resources={} accesses={} barriers={} diagnostics={} "
                "dropped={}\n",
                inspection.passes.size(), inspection.resources.size(),
                inspection.accesses.size(), inspection.barriers.size(),
                inspection.diagnostics.size(), inspection.diagnosticsDropped);

    WritePassLines(writer, inspection.passes);

    for (usize i = 0; i < inspection.resources.size(); ++i) {
        WriteResourceLines(writer, i, inspection.resources[i]);
    }

    for (usize i = 0; i < inspection.accesses.size(); ++i) {
        const AccessInspection& access = inspection.accesses[i];
        writer.Line("access {} decl-pass={} resource={} access={}\n", i, access.pass,
                    Describe(access.resource).Get(), ToString(access.access));
    }

    for (usize i = 0; i < inspection.barriers.size(); ++i) {
        WriteBarrierLine(writer, i, inspection.barriers[i]);
    }

    for (usize i = 0; i < inspection.diagnostics.size(); ++i) {
        const GraphDiagnostic& diagnostic = inspection.diagnostics[i];
        writer.Line("diagnostic {} kind={} code={} decl-pass={} resource={} group={} "
                    "message=\"{}\"\n",
                    i, ToString(diagnostic.kind), Monarc::ToString(diagnostic.code),
                    DescribeOptional(diagnostic.pass, kNoPass).Get(),
                    Describe(diagnostic.resource).Get(),
                    DescribeOptional(diagnostic.group, kNoDiagnosticGroup).Get(),
                    diagnostic.message);
    }

    return writer.Result();
}

}  // namespace Monarc::Render
