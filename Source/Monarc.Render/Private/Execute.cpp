#include <Monarc/Render/RenderGraph.h>

// Execution: the only file in this module that will ever touch an `RHI::ICommandList`.
//
// **It records nothing yet, and it says so by refusing.** Task 4 of the phase plan is what
// writes this: the derived barriers go in, the transient resources are created and destroyed
// around the frame, and each pass's callback contributes its own commands through a
// `PassCommandList`. Until then `Execute` returns `ErrorCode::Unsupported` rather than
// returning success having done nothing -- a call that reported green having recorded no
// commands is precisely the kind of vacuous pass this codebase has spent a phase deleting.
//
// **The signature is the deliverable this file has today.** `RenderGraph`'s constructor takes
// an allocator and a `Config`; the device and the command list arrive here, at execute time.
// That is what keeps `Compile` -- the dependency graph, the culling, the lifetimes, the alias
// groups and the derivation -- callable on a machine with no GPU, which is where CI runs. A
// device threaded through construction instead would have put the most important code in the
// rendering layer behind a `Skipped` test, which is the failure Phase A3 spent its whole
// design avoiding and A4 is meant to invert.
//
// The parameters are unnamed because there is nothing to do with them yet, and `/W4 /WX`
// treats a named-but-unused parameter as an error on both compilers.

namespace Monarc::Render {

Status RenderGraph::Execute(RHI::IDevice&, RHI::ICommandList&) {
    return Err(ErrorCode::Unsupported,
               "RenderGraph::Execute: not implemented until Phase A4 Task 4");
}

}  // namespace Monarc::Render
