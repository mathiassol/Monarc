#include <Monarc/Render/RenderGraph.h>

// Compilation: everything the graph decides between the last declaration and the first
// recorded command.
//
// **This file is where the phase's value lives, and in Task 1 it is nearly empty.** The plan's
// Task 2 fills in the dependency graph, cycle detection, culling, lifetimes and alias
// grouping; Task 3 adds the derivation, in DeriveBarriers.cpp. What is here now is the
// validation gate and the execution order, and the rest is stated as absent rather than
// stubbed as present:
//
//   - no dependency graph, so no cycle can be detected;
//   - no culling, so `PassInspection::culled` is false for every pass;
//   - no lifetimes, so every `ResourceLifetime` is empty;
//   - no alias grouping, so every `aliasGroup` is `kNoAliasGroup`;
//   - no derivation, so `GraphInspection::barriers` is empty.
//
// Tests/TestPassDeclaration.cpp asserts the last four of those directly, naming the task that
// changes each -- so a green suite here cannot be mistaken for evidence that any of them
// works. The first is the one no test can assert: there is nothing to look at when no
// dependency graph is built, and a case asserting that a cycle goes undetected would have to
// be deleted by Task 2 rather than extended. It is stated here instead.
//
// **Nothing in this file, or in the two that will join it, touches a device.** That is the
// design the phase turns on: compilation is a pure function over declarations, so a machine
// with no GPU can check the whole of a frame's synchronisation. See `RenderGraph`'s class
// comment.

namespace Monarc::Render {

Status RenderGraph::Compile() {
    if (m_phase != GraphPhase::Declaring) {
        // Not "this build is already compiled": the same branch fires in `CompileFailed`, where
        // nothing was compiled at all. `AddPass`'s sibling refusal carries the argument.
        return std::unexpected(Refuse(DiagnosticKind::AlreadyCompiled, ErrorCode::InvalidArgument,
                                      "RenderGraph::Compile: this build is no longer accepting "
                                      "declarations",
                                      kNoPass, TextureId{}));
    }

    // **A build with a refused declaration does not compile, whether or not its caller looked
    // at the `Status` that refusal returned.** Every declaration is `[[nodiscard]]`, so
    // ignoring one takes a deliberate cast -- but a graph that compiled anyway would hand back
    // a frame missing exactly the access somebody got wrong, which is worse than the refusal
    // they skipped. The diagnostics list is the record; the first entry's code is what the
    // caller gets, because that is the refusal that came first.
    //
    // **This is an addition, not a checkbox.** No line of the phase plan's Task 1 asks for it;
    // it follows from the diagnostics list existing at all, which is what makes "was anything
    // refused?" a question compilation can ask. It is recorded as an addition here so that a
    // later reader does not take it for a requirement and preserve it for the wrong reason --
    // if Task 2 finds a build worth compiling despite a refusal, this is a decision to revisit
    // and not a spec to honour. It forecloses nothing either way: the phase becomes
    // `CompileFailed`, and inspection stays readable, which is the case `GraphPhase` is for.
    //
    // Deliberately no new diagnostic here: the ones already recorded say what happened, and
    // adding a summary entry would put a row in the list that names no pass and no resource.
    //
    // **`diagnosticsDropped` is read as well as the list, and it is not redundant.** A
    // `Config` with `maxDiagnostics` at zero records nothing and counts everything, so a
    // graph configured that way would have refused a declaration, held an empty diagnostics
    // list, and compiled -- which is the exact hole this refusal exists to close, reopened by
    // a configuration value. The code is then `Unknown`, because the refusal that would have
    // named one was the thing that got dropped; Tests/TestPassDeclaration.cpp pins both
    // halves.
    if (!m_diagnostics.IsEmpty() || m_diagnosticsDropped != 0) {
        m_phase = GraphPhase::CompileFailed;
        const ErrorCode code =
            m_diagnostics.IsEmpty() ? ErrorCode::Unknown : m_diagnostics[0].code;
        return Err(code, "RenderGraph::Compile: a declaration in this build was refused");
    }

    // Execution order is declaration order, and nothing is culled. Written as a loop over the
    // pass list rather than left to `AddPass`'s defaults so that Task 2 replaces one thing
    // instead of reconciling two: `AddPass` leaves `executionOrder` at `kNoPass`, and this is
    // the only place an order is ever assigned.
    //
    // **The `culled` write is a placeholder and is provably a no-op today**, which is worth
    // saying so that it does not read as load-bearing: `AddPass` already writes `false` and
    // nothing anywhere writes `true`, so removing this line changes no value. It is kept
    // because it is the line Task 2's culling replaces, and a loop that settled order without
    // mentioning culling would make the two look like separate decisions.
    for (usize i = 0; i < m_passes.Size(); ++i) {
        m_passes[i].executionOrder = static_cast<u32>(i);
        m_passes[i].culled         = false;
    }

    m_phase = GraphPhase::Compiled;
    return {};
}

}  // namespace Monarc::Render
