#include <Monarc/Render/RenderGraph.h>

#include <span>

// Compilation: everything the graph decides between the last declaration and the first
// recorded command.
//
// **Nothing in this file touches a device, and that is the design the phase turns on rather
// than a property it happens to have.** Every stage below reads `m_passes`, `m_resources` and
// `m_accesses` -- three arrays of plain aggregates filled by declaration -- and writes back
// into fields of the same three. There is no `RHI::IDevice`, no `RHI::ICommandList` and no
// `RHI::TextureHandle` on any path here: the only RHI type this file names at all is
// `RHI::TextureDescription`, and it reads two of its fields to decide whether two transients
// could share memory. So a machine with no GPU, no Vulkan driver and no display can check the
// whole of a frame's ordering, culling, lifetimes and aliasing, which is where CI runs. See
// `RenderGraph`'s class comment; Task 3's derivation joins this file's discipline in
// DeriveBarriers.cpp, and Task 4's `Execute` is the first line of the module that needs a
// device at all.
//
// **What is still absent, stated rather than stubbed**: no barriers are derived, so
// `GraphInspection::barriers` is empty and Task 3 of the phase plan is what fills it. A green
// suite here is not evidence that any barrier is derived.
//
// ---------------------------------------------------------------------------------------
// The dependency edges, once, because four places below walk them.
//
// **An edge runs from a pass that writes a resource to a *different* pass that reads it, and
// nothing else is an edge.** The two omissions are deliberate and each has a reason that is
// not "it was not needed yet".
//
// **No write-after-write edge.** Two passes writing one resource with nothing reading it
// between them are not ordered by anything real, and an edge in declaration order between
// them would invent cycles that are not there: pass A writes T and reads S, pass B writes T
// and writes S. The only true dependency is B before A, for S -- but a declaration-order WAW
// edge on T says A before B, and the graph would refuse a frame that has a perfectly good
// order. Their relative order is settled by the sort's declaration-order tie-break instead,
// which is what a caller who wrote them in that order asked for.
//
// That tie-break is an answer only while **nothing reads** the twice-written resource, which is
// the case above: no read means no access whose contents depend on which write came last. Put a
// read on it and the two unordered writes stop being harmless and the graph refuses --
// `RefuseUnorderedOverwrites` below is that refusal, and derives its exact shape from this rule
// and the next one.
//
// **No write-after-read edge either, and this one cannot be written down at all.** An
// anti-dependency is "this read must happen before that overwrite", which presupposes knowing
// which of the two comes first -- and that is the thing being computed. Adding the edge
// naively is self-contradictory: any resource with a writer P and a reader Q gets a
// write-then-read edge P->Q *and* a read-then-write edge Q->P, so every single
// producer/consumer pair in the graph would be reported as a cycle.
//
// **What follows from that is the opposite of a tie-break, and it is worth stating plainly
// because a comment here claimed the opposite until a review measured it.** A read is *not*
// ordered against an overwrite by the tie-break: the write-then-read edge puts every writer of
// a resource before every different pass that reads it, so an inter-pass write-after-read is
// not merely untested here, it is unrepresentable -- P is before Q in every topological order
// this sort can produce. `RefuseUnorderedOverwrites` is where that stops being a silent wrong
// answer: the declaration that needs the missing edge is refused rather than ordered, and
// `DiagnosticKind::UnorderedOverwrite` says that resource versioning is what would make it
// expressible. `RefuseUnwrittenTransients` leans on the same theorem for the opposite
// purpose, and states it again at its own head.
//
// **And no self-edge.** A pass that declares both a read and a write of one resource is the
// read-modify-write attachment `PassBuilder::Write` states is legal -- a `LoadOp::Load` target
// or a blend. Under an edge rule that did not exclude the reader being the writer it would be
// a one-pass cycle, and the legal declaration would be refused.
//
// The edge *set* is what `ForEachSuccessor` and `ForEachPredecessor` walk, in the two
// directions, and its multiplicity matters: one visit per (resource, write, read) triple, so a
// pair of passes joined by two resources is joined by two edges. The topological sort counts
// and decrements the same triples, and would leave a pass unorderable if the two disagreed.
// ---------------------------------------------------------------------------------------

namespace Monarc::Render {

namespace {

/// What `m_passIndegree` holds for a pass the topological sort has already placed.
///
/// Not a count, and never reachable by decrementing one: the sort only ever decrements a pass
/// it has not placed -- argued at the decrement -- so a placed pass cannot come back down to
/// zero and be placed twice.
constexpr u32 kOrdered = static_cast<u32>(-1);

/// `m_resourceBin`'s "in no bin" value. Distinct from `kNoAliasGroup` in what it means rather
/// than in its bits: a bin is first-fit's own bookkeeping, and a group is what survives it.
constexpr u32 kNoBin = static_cast<u32>(-1);

/// `m_passMark`'s flags, used only while reporting a cycle.
///
/// `kSettled` means a pass has had its answer decided -- either its cycle was reported, or it
/// was found to be downstream of one and to belong to no cycle itself. The other two are the
/// two reachability sweeps, and are cleared before each sweep pair.
/// @{
constexpr u32 kReachedForward  = 1u << 0;
constexpr u32 kReachedBackward = 1u << 1;
constexpr u32 kSettled         = 1u << 2;
/// @}

/// `ComputeLifetimes`'s minimum leans on this, so it is stated rather than left implicit in
/// `kNoPass`'s definition: an unset `firstPass` must lose a `<` against every real execution
/// position, and it does because `kNoPass` is the largest `u32` there is and a position is
/// bounded by the pass count.
static_assert(kNoPass == static_cast<u32>(-1));

/// Buckets `accesses` by `keyOf`, filling `starts` with `keyCount + 1` bucket boundaries and
/// `items` with one access index per access.
///
/// **Declaration order survives inside each bucket**, which is what makes every stage's answer
/// the same for the same declarations -- the property `WriteInspectionText` promises about its
/// whole report.
///
/// A counting sort with no cursor array: count, turn the counts into bucket *ends*, then place
/// from the back, decrementing each end as it goes. What is left in `starts` afterwards is the
/// bucket *starts*, and bucket `k` is therefore `[starts[k], starts[k + 1])` -- the end
/// sentinel at `keyCount` is the one entry that is never decremented.
template <typename KeyOf>
void BucketAccesses(std::span<const AccessInspection> accesses, u32 keyCount, Array<u32>& starts,
                    Array<u32>& items, KeyOf keyOf) {
    for (u32 key = 0; key < keyCount; ++key) {
        starts[key] = 0;
    }
    for (const AccessInspection& access : accesses) {
        ++starts[keyOf(access)];
    }
    u32 total = 0;
    for (u32 key = 0; key < keyCount; ++key) {
        total += starts[key];
        starts[key] = total;
    }
    starts[keyCount] = total;

    for (usize i = accesses.size(); i-- > 0;) {
        items[--starts[keyOf(accesses[i])]] = static_cast<u32>(i);
    }
}

/// Whether `resource` could share memory with another resource at all.
///
/// **Imported resources are never candidates**, because the graph does not own their memory:
/// `ResourceOrigin::Imported` says so, and handing one an alias group would be claiming a
/// decision about somebody else's allocation. Neither is a resource with no first write, which
/// after culling means no pass that runs writes it -- there is no live range to place, so there
/// is nothing to place it beside.
///
/// **`HasNoWrite()` and not `IsUnused()`, and for a transient the two cannot disagree**: a
/// surviving reader of a transient implies a surviving writer, because culling keeps a pass
/// whose reader survives. The two come apart only for an imported resource, which the first
/// condition has already excluded. `ResourceLifetime` argues the split; the question asked here
/// is "is there a live range to place", which is the one `HasNoWrite()` answers.
[[nodiscard]] bool IsAliasCandidate(const ResourceInspection& resource) {
    return resource.origin == ResourceOrigin::Transient && !resource.lifetime.HasNoWrite();
}

/// Whether two lifetimes are both live at some execution position.
///
/// Only ever asked about two candidates, so both ends of both are real positions: a candidate
/// has a first write, and a resource with a first write has a last use -- the write itself, at
/// worst.
[[nodiscard]] bool LifetimesOverlap(const ResourceLifetime& a, const ResourceLifetime& b) {
    return a.firstPass <= b.lastPass && b.firstPass <= a.lastPass;
}

/// Whether two descriptions permit their resources to share memory.
///
/// **Same format and same extent, and nothing cleverer, which is a deliberately conservative
/// rule rather than a complete one.** Two textures of different formats or extents cannot
/// share an allocation whatever their lifetimes say, so this is a necessary condition; whether
/// it is a *sufficient* one is a question for the allocator that would honour the grouping, and
/// there is no such allocator -- see `GroupAliases`. `RHI::TextureUsage` is deliberately not
/// compared: a real rule would be keyed on the size and alignment a device reports for an
/// image, which is measured from a device and is exactly the work the phase plan defers until
/// one exists. Nothing here claims usage is irrelevant to that rule; it claims only that
/// guessing at the rule now would be worse than stating the two fields this compares.
[[nodiscard]] bool DescriptionsAreCompatible(const RHI::TextureDescription& a,
                                             const RHI::TextureDescription& b) {
    return a.format == b.format && a.extent == b.extent;
}

}  // namespace

std::span<const u32> RenderGraph::AccessesOfResource(u32 resource) const {
    // `resource` is an index into `m_resources`, and every access holds one that resolved:
    // `DeclareAccess` refuses an id that names no resource in this build, and the resource list
    // only ever grows, so an index that was in range when the access was declared still is.
    const u32 from = m_resourceBucketStart[resource];
    const u32 to   = m_resourceBucketStart[resource + 1];
    return std::span<const u32>(m_accessesByResource.Data() + from, to - from);
}

std::span<const u32> RenderGraph::AccessesOfPass(u32 pass) const {
    const u32 from = m_passBucketStart[pass];
    const u32 to   = m_passBucketStart[pass + 1];
    return std::span<const u32>(m_accessesByPass.Data() + from, to - from);
}

template <typename Visit>
void RenderGraph::ForEachSuccessor(u32 pass, Visit visit) const {
    for (const u32 writeIndex : AccessesOfPass(pass)) {
        const AccessInspection& write = m_accesses[writeIndex];
        if (!IsWrite(write.access)) {
            continue;
        }
        for (const u32 readIndex : AccessesOfResource(write.resource.index)) {
            const AccessInspection& read = m_accesses[readIndex];
            if (IsWrite(read.access) || read.pass == pass) {
                continue;
            }
            visit(read.pass);
        }
    }
}

template <typename Visit>
void RenderGraph::ForEachPredecessor(u32 pass, Visit visit) const {
    // **The `write.pass == pass` exclusion below is not observable today, and is kept so that
    // the two directions describe one edge set.** Its only caller marks a pass before pushing
    // it, so a pass offered itself as its own predecessor is ignored -- mutating the exclusion
    // away leaves the whole suite green, which was measured rather than assumed. It stays
    // because the claim these two functions make about each other is that they walk the same
    // edges, and a reader-modifier pass being its own predecessor and not its own successor
    // would make that false for the next thing that walks them.
    for (const u32 readIndex : AccessesOfPass(pass)) {
        const AccessInspection& read = m_accesses[readIndex];
        if (IsWrite(read.access)) {
            continue;
        }
        for (const u32 writeIndex : AccessesOfResource(read.resource.index)) {
            const AccessInspection& write = m_accesses[writeIndex];
            if (!IsWrite(write.access) || write.pass == pass) {
                continue;
            }
            visit(write.pass);
        }
    }
}

void RenderGraph::BuildAccessBuckets() {
    const std::span<const AccessInspection> accesses(m_accesses.Data(), m_accesses.Size());
    BucketAccesses(accesses, static_cast<u32>(m_resources.Size()), m_resourceBucketStart,
                   m_accessesByResource,
                   [](const AccessInspection& access) { return access.resource.index; });
    BucketAccesses(accesses, static_cast<u32>(m_passes.Size()), m_passBucketStart,
                   m_accessesByPass,
                   [](const AccessInspection& access) { return access.pass; });
}

Status RenderGraph::OrderPasses() {
    // **Execution order is derived, not taken from declaration order, and that is the decision
    // the rest of compilation stands on.** Ids are handed out in declaration order, so a pass
    // can only name a resource created at or before itself -- but *which* pass reads it is
    // unconstrained, so a reader can be declared before its writer and a cycle is a shape a
    // legal set of declarations can have. If order were declaration order, every dependency
    // edge would point forwards by construction, a cycle would be unreachable, and the refusal
    // below would be dead code.
    //
    // **Declaration order is the tie-break, and it is not optional.** Two passes with no
    // dependency between them keep the order they were declared in, which is what makes a
    // frame's report identical between builds of the same declarations -- the property
    // `WriteInspectionText` is built on, and what `PassInspection::index` and
    // `::executionOrder` being separate fields is for.
    const u32 passCount = static_cast<u32>(m_passes.Size());

    for (u32 pass = 0; pass < passCount; ++pass) {
        m_passIndegree[pass] = 0;
    }
    for (u32 pass = 0; pass < passCount; ++pass) {
        ForEachSuccessor(pass, [this](u32 reader) { ++m_passIndegree[reader]; });
    }

    for (u32 placed = 0; placed < passCount; ++placed) {
        // The lowest-numbered pass with nothing left to wait for. **A linear scan rather than a
        // heap, and the scan is what makes the tie-break declaration order** -- there is no
        // comparator to get wrong and no container whose iteration order could decide it. It
        // costs one sweep of the pass list per placed pass, over a list bounded by
        // `Config::maxPasses`.
        u32 next = passCount;
        for (u32 pass = 0; pass < passCount; ++pass) {
            if (m_passIndegree[pass] == 0) {
                next = pass;
                break;
            }
        }
        if (next == passCount) {
            // Every unplaced pass is waiting for another unplaced one, so following predecessors
            // from any of them must come back round: there is at least one cycle.
            return std::unexpected(ReportDependencyCycles());
        }

        m_passOrder[placed] = next;
        m_passIndegree[next] = kOrdered;
        // **No `kOrdered` check on `reader`, because a placed pass can never be one.** A pass is
        // placed only when its counter reaches zero, which means every edge into it was already
        // relaxed -- including this one, which would mean `next` was already placed. It is
        // being placed now, so it was not.
        ForEachSuccessor(next, [this](u32 reader) { --m_passIndegree[reader]; });
    }

    return {};
}

Error RenderGraph::ReportDependencyCycles() {
    const u32 passCount = static_cast<u32>(m_passes.Size());
    for (u32 pass = 0; pass < passCount; ++pass) {
        m_passMark[pass] = 0;
    }

    // **One group per set of passes among which no order exists, which is a narrower promise
    // than "one group per cycle" and the narrowing is on purpose.** Two cycles sharing a pass
    // are one such set, and arrive as one group of three rather than two groups of two.
    // Reporting every elementary cycle separately is exponential in the pass count -- a graph
    // of n passes can have that many distinct cycles -- while the mutually-reachable set is two
    // sweeps per group and is the minimal set of declarations that has to change: no subset of
    // it can be ordered either. `GraphDiagnostic::group` records this where a reader of the
    // report will meet it.
    //
    // **A pass that cannot be ordered is not necessarily in a cycle**, and gets no row when it
    // is not. A pass reading what a cycle produces has no order and no way to acquire one, but
    // it is not what is wrong; naming it would put a row on a declaration that is correct.
    u32 group = 0;
    for (u32 seed = 0; seed < passCount; ++seed) {
        if (m_passIndegree[seed] == kOrdered || (m_passMark[seed] & kSettled) != 0) {
            continue;
        }

        for (u32 pass = 0; pass < passCount; ++pass) {
            m_passMark[pass] &= ~(kReachedForward | kReachedBackward);
        }
        MarkReachable(seed, true);
        MarkReachable(seed, false);

        // Both sweeps mark their own start, so `seed` is always in the intersection and counts
        // itself: a member count of one means nothing else is mutually reachable with it, which
        // is exactly "unordered but in no cycle".
        u32 members = 0;
        for (u32 pass = 0; pass < passCount; ++pass) {
            const u32 marks = m_passMark[pass] & (kReachedForward | kReachedBackward);
            members += marks == (kReachedForward | kReachedBackward) ? 1u : 0u;
        }
        if (members < 2) {
            m_passMark[seed] |= kSettled;
            continue;
        }

        for (u32 pass = 0; pass < passCount; ++pass) {
            const u32 marks = m_passMark[pass] & (kReachedForward | kReachedBackward);
            if (marks != (kReachedForward | kReachedBackward)) {
                continue;
            }
            m_passMark[pass] |= kSettled;
            // Rows in increasing declaration index, which is membership in a stable order.
            // `GraphDiagnostic::group` is explicit that the field promises membership and not
            // the order round the cycle, and this detector has no cycle order to offer: what it
            // computed is a set.
            //
            // **The `Error` is discarded because it carries nothing this function does not
            // already have** -- `Refuse` hands back the same {code, literal} pair for every row
            // of every cycle, and the pass is in the row. What the caller is handed is composed
            // below, from a literal about the graph rather than about one of its passes.
            static_cast<void>(Refuse(DiagnosticKind::DependencyCycle, ErrorCode::InvalidArgument,
                                     "RenderGraph::Compile: this pass is in a dependency cycle",
                                     pass, TextureId{}, group));
        }
        ++group;
    }

    return Error{ErrorCode::InvalidArgument,
                 "RenderGraph::Compile: the declared reads and writes have a dependency cycle"};
}

void RenderGraph::MarkReachable(u32 from, bool forward) {
    const u32 flag = forward ? kReachedForward : kReachedBackward;

    // Depth-first over the unplaced passes only. Bounded by the pass count because a pass is
    // marked before it is pushed and never pushed again, which is what makes `m_passStack`'s
    // `maxPasses` entries enough -- and that bound does not depend on the `kOrdered` test
    // below.
    //
    // **That test prunes the sweep; it does not decide the answer, and the difference was
    // measured rather than reasoned about.** Removing it changes no report: the forward sweep
    // provably cannot leave the unplaced set -- a placed pass has every edge into it already
    // relaxed, so its predecessors are all placed, and an unplaced seed can therefore never
    // reach one -- and the extra passes the backward sweep would then reach are placed ones,
    // which by the same argument are never in the forward set and so never in the intersection
    // `ReportDependencyCycles` takes. Mutating it away leaves the whole suite green, which is
    // recorded here rather than left for the next reader to rediscover as a hole in the tests.
    u32 depth            = 0;
    m_passStack[depth++] = from;
    m_passMark[from] |= flag;

    while (depth > 0) {
        const u32  pass = m_passStack[--depth];
        const auto step = [&](u32 other) {
            if (m_passIndegree[other] == kOrdered || (m_passMark[other] & flag) != 0) {
                return;
            }
            m_passMark[other] |= flag;
            m_passStack[depth++] = other;
        };
        if (forward) {
            ForEachSuccessor(pass, step);
        } else {
            ForEachPredecessor(pass, step);
        }
    }
}

Status RenderGraph::RefuseUnwrittenTransients() {
    // **A transient nothing writes and something reads is a declaration error the graph can
    // name precisely, and an imported resource read first is not the same thing at all.** An
    // import states its `ResourceInspection::incoming` state, so its contents came from outside
    // the graph and reading it first is meaningful. A transient has no contents until a pass
    // writes it, so a pass reading one nothing wrote reads undefined memory -- the phase plan
    // left this open for Task 2 to answer, and this is the answer.
    //
    // **It needs no execution order, which is why it runs whether or not the sort succeeded.**
    // A write-then-read edge puts every writer of a resource before every reader of it, so the
    // earliest pass to touch a written transient always writes it, in every topological order
    // there is; the question therefore collapses to whether a write was declared at all. The
    // read-modify-write case falls out for free: the pass writes the resource, so a pass that
    // reads and writes one it is alone with is not this refusal.
    bool refused = false;

    for (u32 resource = 0; resource < static_cast<u32>(m_resources.Size()); ++resource) {
        if (m_resources[resource].origin != ResourceOrigin::Transient) {
            continue;
        }

        bool written    = false;
        u32  firstRead  = kNoPass;
        for (const u32 index : AccessesOfResource(resource)) {
            const AccessInspection& access = m_accesses[index];
            if (IsWrite(access.access)) {
                written = true;
            } else if (firstRead == kNoPass) {
                firstRead = access.pass;
            }
        }
        if (written || firstRead == kNoPass) {
            continue;
        }

        // One row for the resource rather than one per reading pass: what is wrong is that
        // nothing writes it, which is one mistake however many passes read it. The pass named
        // is the one that declared the earliest of those reads.
        static_cast<void>(Refuse(DiagnosticKind::TransientNeverWritten,
                                 ErrorCode::InvalidArgument,
                                 "RenderGraph::Compile: this transient is read and no pass "
                                 "writes it",
                                 firstRead, m_resources[resource].id));
        refused = true;
    }

    if (!refused) {
        return {};
    }
    return Err(ErrorCode::InvalidArgument,
               "RenderGraph::Compile: a transient this build reads is written by no pass");
}

bool RenderGraph::PassReadsResource(u32 pass, u32 resource) const {
    for (const u32 index : AccessesOfResource(resource)) {
        const AccessInspection& access = m_accesses[index];
        if (access.pass == pass && !IsWrite(access.access)) {
            return true;
        }
    }
    return false;
}

Status RenderGraph::RefuseUnorderedOverwrites() {
    // **The one declaration this graph would answer wrongly rather than refuse, and the reason
    // this stage exists.** Take the ordinary frame "A renders into T, B samples T, C reuses T
    // as a scratch target". A writes T, B reads T, C writes T; the edges are A->B and C->B and
    // there is no A/C edge, because there is deliberately no write-after-write one. In-degrees
    // are A:0, B:2, C:0, so lowest-ready-first places A, then C, then B -- and B samples what C
    // already overwrote. The declaration's meaning is not in doubt; the order the sort emits
    // for it reads the wrong bytes, and nothing said so.
    //
    // **The fix is not an extra edge, it is versioning, and Monarc has none.** "This read
    // happens before that overwrite" needs the two accesses to name different things -- A
    // writes version 1, B reads version 1, C writes version 2 -- and then C's write is ordered
    // after B's read by a write-after-read edge between two distinct versions. Without
    // versions the read and the overwrite name one resource, the anti-dependency cannot be
    // written down (the head of this file argues why), and the frame is unrepresentable. So it
    // is refused. `DiagnosticKind::UnorderedOverwrite` carries that in the report; the shapes
    // refused here become legal declarations when versioning arrives.
    //
    // **The predicate, derived from the edge rule rather than guessed.** Every foreign write of
    // a resource precedes every read of it, in every order this sort can produce, so what a
    // read sees is the *last* foreign write -- and that is determined only if the foreign
    // writers have a last one that every order agrees on. A writer that also reads the resource
    // has every other writer of it before it, by the same edge, so it is that last write; and
    // there is at most one such pass, because two of them are a two-pass cycle on that resource
    // alone and are already refused as one. Writers that do *not* read the resource have no
    // edge between them, so nothing declared orders them and the declaration-order tie-break
    // picks which one a reader sees. **Two of those plus one read is exactly the refusable
    // shape**, and everything else on one resource is orderable as declared:
    //
    //   - one writer and any number of readers -- one foreign write, so no choice to make;
    //   - a read-modify-write pass alone with a resource -- no foreign write at all;
    //   - one write-only writer and one read-modify-write pass, the `LoadOp::Load` chain
    //     `PassBuilder::Write` calls legal: the writer is ordered before the modifier by the
    //     edge on the resource itself, so the modifier's read has a determined write behind it
    //     and any later reader has the modifier's write behind it;
    //   - two writers and *no* reader, which is the write-after-write case this file's head
    //     argues is not ordered by anything real and is settled by the tie-break on purpose.
    //     Nothing reads it, so nothing reads the wrong bytes.
    //
    // **It needs no execution order**, for `RefuseUnwrittenTransients`' reason: it counts
    // declarations, and the counts are the same whatever order the sort found or failed to
    // find.
    //
    // **This is an addition, not a checkbox.** No line of the phase plan's Task 2 asks for it;
    // it follows from the edge rule, and it exists because a review proved the consequence of
    // that rule and nothing in the suite had noticed. It is recorded as an addition so a later
    // reader does not take it for a requirement and preserve it for the wrong reason -- and the
    // plan's open questions record what it costs Task 3, which now has no inter-pass
    // write-after-read declaration left to exercise.
    bool refused = false;

    for (u32 resource = 0; resource < static_cast<u32>(m_resources.Size()); ++resource) {
        u32 firstRead       = kNoPass;
        u32 firstWriteOnly  = kNoPass;
        u32 secondWriteOnly = kNoPass;

        for (const u32 index : AccessesOfResource(resource)) {
            const AccessInspection& access = m_accesses[index];
            if (!IsWrite(access.access)) {
                if (firstRead == kNoPass) {
                    firstRead = access.pass;
                }
                continue;
            }
            // Distinct *passes* are what count, not accesses: one pass may declare two
            // different writes of one resource, and that is one writer.
            if (access.pass == firstWriteOnly || access.pass == secondWriteOnly ||
                PassReadsResource(access.pass, resource)) {
                continue;
            }
            if (firstWriteOnly == kNoPass) {
                firstWriteOnly = access.pass;
            } else if (secondWriteOnly == kNoPass) {
                secondWriteOnly = access.pass;
            }
        }
        if (firstRead == kNoPass || secondWriteOnly == kNoPass) {
            continue;
        }

        // One row for the resource, `TransientNeverWritten`'s shape and its reason: one
        // resource declared this way is one mistake however many passes read or overwrite it.
        // The pass named is the one that declared the earliest read, which is the access that
        // would have been handed the wrong bytes.
        static_cast<void>(Refuse(DiagnosticKind::UnorderedOverwrite, ErrorCode::InvalidArgument,
                                 "RenderGraph::Compile: this pass reads a resource two other "
                                 "passes overwrite in no declared order",
                                 firstRead, m_resources[resource].id));
        refused = true;
    }

    if (!refused) {
        return {};
    }
    return Err(ErrorCode::InvalidArgument,
               "RenderGraph::Compile: a resource this build reads is written by two passes "
               "nothing orders");
}

void RenderGraph::CullPasses() {
    // **A pass survives because something consumes what it wrote, and a recording callback is
    // not something it wrote.** ADR-0006's contract is that a pass declares its reads and
    // writes and the graph decides the rest, so culling reads the declarations and nothing
    // else. Letting `PassInspection::hasRecord` keep a pass alive would make culling almost
    // inert -- every pass that records anything would survive -- and would mean a feature could
    // opt out of the mechanism by capturing a lambda. The consequence is worth stating plainly:
    // a pass whose callback has a side effect the graph cannot see will be culled, and in Phase
    // A4 there is no such callback, because `PassCommandList` offers nothing to call.
    //
    // **A graph that imports nothing is therefore culled entirely**, which is not a degenerate
    // case but the correct answer: if nothing outside the graph consumes anything the graph
    // produced, the frame's whole output is unobserved.
    for (PassInspection& pass : m_passes) {
        pass.culled = true;
    }

    // **Backwards through execution order, which turns a transitive question into one sweep.**
    // Every pass that reads what this one writes is *later* in this order -- the edge that makes
    // it a reader is what put it there -- so its own answer is already final when this one is
    // decided. No work list and no second iteration to a fixed point.
    for (u32 position = static_cast<u32>(m_passes.Size()); position-- > 0;) {
        const u32 pass = m_passOrder[position];

        for (const u32 index : AccessesOfPass(pass)) {
            const AccessInspection& access = m_accesses[index];
            if (IsWrite(access.access) &&
                m_resources[access.resource.index].origin == ResourceOrigin::Imported) {
                // Never culled: something outside the graph consumes it.
                m_passes[pass].culled = false;
            }
        }
        ForEachSuccessor(pass, [this, pass](u32 reader) {
            if (!m_passes[reader].culled) {
                m_passes[pass].culled = false;
            }
        });
    }
}

void RenderGraph::NumberSurvivingPasses() {
    // `PassInspection::executionOrder` is a position among the passes that *run*, so the
    // survivors are numbered densely and a culled pass leaves no gap. The sorted order is what
    // this walks; what it produces is the order a frame is recorded in.
    u32 next = 0;
    for (u32 position = 0; position < static_cast<u32>(m_passes.Size()); ++position) {
        PassInspection& pass = m_passes[m_passOrder[position]];
        if (pass.culled) {
            pass.executionOrder = kNoPass;
            continue;
        }
        pass.executionOrder = next++;
    }
}

void RenderGraph::ComputeLifetimes() {
    // **Culled passes contribute nothing, which is the whole reason this runs after culling.**
    // A transient whose last reader does not run is live for a shorter span than its
    // declarations suggest, and computing the two the other way round would record a reader
    // that never runs.
    for (u32 resource = 0; resource < static_cast<u32>(m_resources.Size()); ++resource) {
        ResourceLifetime lifetime{};

        for (const u32 index : AccessesOfResource(resource)) {
            const AccessInspection& access = m_accesses[index];
            const PassInspection&   pass   = m_passes[access.pass];
            if (pass.culled) {
                continue;
            }
            // `firstPass` is the first *write*: a resource is not live before something puts
            // contents in it. An unset `firstPass` is `kNoPass`, which loses this comparison to
            // every real position -- see the `static_assert` at the head of this file. `lastPass`
            // is the other way round and needs its emptiness tested, since `kNoPass` wins every
            // `>`.
            if (IsWrite(access.access) && pass.executionOrder < lifetime.firstPass) {
                lifetime.firstPass = pass.executionOrder;
            }
            if (lifetime.lastPass == kNoPass || pass.executionOrder > lifetime.lastPass) {
                lifetime.lastPass = pass.executionOrder;
            }
        }

        m_resources[resource].lifetime = lifetime;
    }
}

void RenderGraph::GroupAliases() {
    // ---------------------------------------------------------------------------------
    // **The decision below is real and tested. The memory saving is not, and this graph does
    // not save a byte.**
    //
    // Nothing honours the grouping: `RenderGraph::Execute` refuses rather than recording, and
    // the memory model it will be written against is one `VkDeviceMemory` per resource -- a
    // placeholder `VulkanDevice.cpp` records as such, and Docs/Rendering/RHI.md with it. Two
    // resources sharing one allocation needs sub-allocation from an allocator that does not
    // exist. So an `aliasGroup` in the report says "these two were computed to be able to share
    // memory", never "these two shared memory", and a green aliasing test must not be read as
    // "aliasing works": what works is the grouping.
    //
    // This is written at the code that makes the decision rather than only in Docs/Status.md,
    // because whoever reads a populated alias group is reading this function, and the phase
    // plan asks for the honest half of the M0 principle to be the half that does not go
    // unwritten.
    // ---------------------------------------------------------------------------------
    const u32 resourceCount = static_cast<u32>(m_resources.Size());
    for (u32 resource = 0; resource < resourceCount; ++resource) {
        m_resources[resource].aliasGroup = kNoAliasGroup;
        m_resourceBin[resource]          = kNoBin;
    }

    // Greedy first fit: each candidate joins the lowest-numbered bin every one of whose members
    // it could share memory with, or opens the next bin. Good enough by the phase plan's own
    // measure, and the alternative -- an optimal packing -- would be tuning a decision nothing
    // honours.
    //
    // Both conditions, together: two lifetimes that do not overlap still cannot share an
    // allocation if the descriptions disagree, and two matching descriptions cannot if the
    // lifetimes do. Membership of a bin is pairwise, so a candidate is checked against every
    // member rather than against the bin's span -- the union of two disjoint lifetimes is not
    // an interval, and treating it as one would let a third resource in that overlaps neither
    // end but sits in the gap.
    u32 binCount = 0;
    for (u32 resource = 0; resource < resourceCount; ++resource) {
        if (!IsAliasCandidate(m_resources[resource])) {
            continue;
        }

        u32 chosen = binCount;
        for (u32 bin = 0; bin < binCount; ++bin) {
            bool fits = true;
            for (u32 other = 0; other < resource; ++other) {
                if (m_resourceBin[other] != bin) {
                    continue;
                }
                if (!DescriptionsAreCompatible(m_resources[resource].description,
                                               m_resources[other].description) ||
                    LifetimesOverlap(m_resources[resource].lifetime,
                                     m_resources[other].lifetime)) {
                    fits = false;
                    break;
                }
            }
            if (fits) {
                chosen = bin;
                break;
            }
        }

        m_resourceBin[resource] = chosen;
        if (chosen == binCount) {
            ++binCount;
        }
    }

    // **A bin with one member is not a group**, because a resource that shares memory with
    // nothing is exactly what `kNoAliasGroup` says. Numbering the survivors in bin order is
    // what keeps the report stable: bins are opened in declaration order, so the group ids are
    // a function of the declarations and of nothing else.
    u32 group = 0;
    for (u32 bin = 0; bin < binCount; ++bin) {
        u32 members = 0;
        for (u32 resource = 0; resource < resourceCount; ++resource) {
            members += m_resourceBin[resource] == bin ? 1u : 0u;
        }
        if (members < 2) {
            continue;
        }
        for (u32 resource = 0; resource < resourceCount; ++resource) {
            if (m_resourceBin[resource] == bin) {
                m_resources[resource].aliasGroup = group;
            }
        }
        ++group;
    }
}

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
    // later reader does not take it for a requirement and preserve it for the wrong reason.
    // Task 2 found no build worth compiling despite a refusal and kept it.
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

    BuildAccessBuckets();

    // **All three refusals run, and none short-circuits another, because they are about
    // different mistakes.** A cycle is a mutual dependency, an unwritten transient is an absent
    // one, and an unordered overwrite is a dependency the declarations cannot express at all; a
    // build can have all three, and reporting only whichever check happens to run first would
    // hide most of what is wrong from the one report the caller gets to read. The cycle's rows
    // come first because that is the order the stages run in, and the code the caller is handed
    // is the earliest failing stage's for the same reason.
    const Status ordered    = OrderPasses();
    const Status allWritten = RefuseUnwrittenTransients();
    const Status allOrdered = RefuseUnorderedOverwrites();
    if (!ordered || !allWritten || !allOrdered) {
        // Nothing downstream runs, so no execution order, culling decision, lifetime or alias
        // group is reported: every pass keeps `executionOrder == kNoPass` and `culled == false`,
        // and every resource an empty lifetime and no group. A graph that had reported half a
        // frame would be a graph whose report disagreed with its refusal.
        m_phase = GraphPhase::CompileFailed;
        if (!ordered) {
            return ordered;
        }
        return allWritten.has_value() ? allOrdered : allWritten;
    }

    // **The remaining four stages are ordered by what they read, and two of the orderings are
    // load-bearing.** Culling comes before lifetimes, because a transient whose last reader is
    // culled has to be live for a shorter span and not a longer one. Numbering comes before
    // lifetimes too, because a lifetime is a pair of execution positions and the positions are
    // what numbering assigns. Grouping comes last because it compares lifetimes.
    CullPasses();
    NumberSurvivingPasses();
    ComputeLifetimes();
    GroupAliases();

    m_phase = GraphPhase::Compiled;
    return {};
}

}  // namespace Monarc::Render
