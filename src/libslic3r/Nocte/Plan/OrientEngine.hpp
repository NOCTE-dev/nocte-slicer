// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The orientation engine: generate candidate rotations, score each with Scores.hpp, filter by the
// intent's hard constraints, rank, and return the best few WITH THE REASON EACH ONE WON.
//
// The reason is the product. Orca's Orient.cpp returns a rotation and nothing else, and it could
// not explain itself if it tried: 28 undocumented tuned constants, an objective function that
// divides by a field nothing ever assigns (Orient.cpp:479,483), and no tests. We rank on quantities
// a user can measure on a real print, and we hand back the numbers.
//
// The engine PROPOSES. It never rotates anything: applying a candidate is the caller's act, taken
// after a person has looked at the numbers.

#ifndef slic3r_Nocte_Plan_OrientEngine_hpp_
#define slic3r_Nocte_Plan_OrientEngine_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/Nocte/Plan/PartInvariants.hpp"
#include "libslic3r/Nocte/Plan/PlanIntent.hpp"
#include "libslic3r/Nocte/Plan/Scores.hpp"

namespace Slic3r {
namespace Nocte {

// Why a candidate was removed. Carried rather than discarded: when the filter empties the set, the
// user is owed the name of the constraint that did it, not a shrug.
enum class RejectReason : uint8_t {
    None = 0,
    NotMeasured,        // a score term could not be measured; see the per-term flags
    Unstable,           // stability below the intent's floor
    FootprintTooSmall,  // not enough bed contact to stick
    NoLoadSection,      // FunctionalStrength with no section across the load direction
    ShowcaseNotUp,      // FunctionalVisual with the read face not facing up
    ShowcaseSupported,  // FunctionalVisual with support landing on the read face
    TierCannotAnswer,   // the support tier cannot compute what this intent needs; see below
};

const char *reject_reason_name(RejectReason reason);

// Where a candidate rotation came from. Reported so a user can tell "this is how it already sits"
// from "this is a face it could rest on".
enum class CandidateSource : uint8_t {
    Current = 0,   // the identity: the orientation the part arrived in
    HullFace,      // a convex hull facet normal laid down onto the bed
    AxisAligned,   // a 90-degree rotation about a principal axis
};

// When two candidates merge into one, the SMALLEST source value wins — so a hull facet that happens
// to reproduce the orientation the part already sits in stays labelled `Current`. This is not
// cosmetic: it is how a user is told "this is how it already is" rather than "rotate it onto this
// face", and it is what makes the never-flip-for-no-gain rule visible in the result rather than only
// true of the transform. A part that arrives resting on a hull facet — which is most parts — would
// otherwise report every orientation as something to change.

const char *candidate_source_name(CandidateSource source);

struct OrientCandidate
{
    Transform3d     rotation = Transform3d::Identity();
    CandidateSource source   = CandidateSource::Current;

    OrientScores    scores;
    // Per-term dimensionless values in the field order (support, cusp, time, strength, stability),
    // each in [0,1] with 0 the best in the surviving set. This is what a panel renders as bars and
    // what makes "why did this win" answerable.
    std::array<double, 5> terms{{0., 0., 0., 0., 0.}};
    // Weighted sum of `terms`. Lower is better. Meaningful only as a rank, never as a quantity.
    double          score    = 0.;

    // None for a candidate that survived the filter.
    RejectReason    rejected = RejectReason::None;

    bool accepted() const { return rejected == RejectReason::None; }
};

struct OrientParams
{
    ScoreParams scores;

    // Two hull normals closer than this are the same candidate. A tessellated cylinder has hundreds
    // of nearly parallel side facets and scoring each one separately would spend the whole budget
    // re-measuring one orientation.
    double angular_merge_deg = 5.;

    // Indifference floors, per term, in that term's own unit. Reachable from here because they are
    // a statement about what differences matter to a user, not a numerical tolerance, and a caller
    // that cares about a 200 mm^3 support difference must be able to say so.
    ScoreEpsilons epsilons;

    // Upper bound on candidates scored, after merging. The set is ordered so that the cheapest and
    // most likely orientations come first, so truncation drops the least plausible.
    //
    // The axis-aligned turns are reserved OUT of this budget, not left to compete for it: on a
    // scanned or smooth part the merged hull alone can fill every slot, and the turns a person would
    // try by hand would then never be scored at all — which is the one outcome that makes a plan
    // look broken. Truncate the hull list, never the reserved set.
    size_t max_candidates    = 64;

    // Include the six axis-aligned quarter turns even when no hull face suggests them. They are
    // what a person would try by hand, and a plan that never considers them looks broken.
    bool   include_axis_aligned = true;

    // How many accepted candidates to return, best first. The rest are dropped from the result but
    // still counted in `considered`.
    size_t max_results       = 3;
};

struct OrientResult
{
    // Accepted candidates, best first, at most `max_results`.
    std::vector<OrientCandidate> best;

    // Every candidate that was rejected, with its reason. Kept so the panel and the CLI can say why
    // an obvious-looking orientation is not on the list.
    std::vector<OrientCandidate> rejected;

    size_t considered = 0;   // candidates scored, before filtering

    // True when the filter left nothing and `best` therefore holds only the current orientation as
    // a fallback. `blocking_reason` then names the constraint that emptied the set.
    //
    // That fallback candidate KEEPS its real `rejected` reason and appears in both arrays. Clearing
    // it would dress a rejected candidate as an accepted one, and it would be a lie in a specific
    // way that matters: when the set is degenerate `combine()` ran over nothing, so the fallback's
    // `terms` and `score` are untouched zeros — the best possible row. A consumer that sees
    // `rejected != None` knows not to render them; a consumer told the candidate was accepted would
    // render a perfect score for a part that satisfied no constraint at all.
    bool          degenerate      = false;
    RejectReason  blocking_reason = RejectReason::None;

    // False when the engine could not run at all (an unusable mesh, or cancellation). A caller must
    // check this before reading anything else.
    bool ok = false;

    // Set when the run was cancelled rather than completed.
    bool cancelled = false;
};

// Cancellation predicate: return true to abort. Consulted between candidates AND inside the scoring
// loop, unlike Orient.cpp's `stopcond_`, which is accepted as a parameter and never read
// (Orient.cpp:84) — a long plan there cannot be stopped at all.
//
// An EMPTY std::function throws std::bad_function_call when invoked, which would drive straight
// through this header's promise never to throw on user geometry. The implementation must therefore
// test the function before calling it, and a caller with nothing to cancel on may pass `{}`.
//
// It is called ONLY from the thread that called plan_orientation, never from a worker. The ordinary
// implementation reads a GUI widget or a job's shared flag, and calling that concurrently from every
// TBB worker would be a data race this engine introduced in a caller that did nothing wrong. The
// cost is that cancellation lands at a batch boundary rather than instantly, which is the right
// trade: a predicate that is not thread-safe is the common case, and requiring it to be would push
// the problem onto every caller to solve worse.
using CancelFn = std::function<bool()>;

// Plans `its`, which must be the mesh `inv` was computed from (see Scores.hpp's precondition).
//
// Ranking, in order, and all of it is this function's job rather than combine()'s:
//   1. score every candidate;
//   2. drop candidates whose measurement flags are false — an unmeasured term is zero, which is the
//      BEST value on every term, so an unmeasured candidate would otherwise win;
//   3. drop candidates failing a hard constraint of the intent;
//   4. normalise and weight the survivors;
//   5. break ties deterministically: less support, then lower height, then the smaller rotation
//      away from the current orientation, then the lower candidate index. A part is never flipped
//      for no gain.
//
// Never throws on user geometry: an unusable mesh yields `ok == false`.
OrientResult plan_orientation(const indexed_triangle_set &its,
                              const PartInvariants       &inv,
                              const PlanIntent           &intent,
                              const OrientParams         &params,
                              const CancelFn             &cancel);

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_Plan_OrientEngine_hpp_
