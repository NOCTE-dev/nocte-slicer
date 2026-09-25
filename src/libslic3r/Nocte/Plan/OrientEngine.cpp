// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The orientation engine behind OrientEngine.hpp: generate the rotations the part could come to
// rest in, score each one with Scores.cpp, filter by the intent's hard constraints, rank what
// survives, and carry the reason for every candidate that did not survive.
//
// Four rules run through the whole file, and each of them is a specific defect this engine exists
// not to repeat:
//
//  * A measurement flag is a FILTER, never a number. combine() reads the raw fields and consults no
//    flag at all, and says so — the "caller must also drop" paragraph of its contract in Scores.hpp,
//    at :316-322. A term that could not be measured is left at 0, and 0 is the best value on every
//    term combine() inverts, so dropping the unmeasured candidates is this file's job and nothing
//    else's.
//  * The cancellation predicate is CONSULTED. Orca's AutoOrienter accepts a `stopcond_` at
//    Orient.cpp:84 and never reads it again, so a long plan there cannot be stopped at all. Here it
//    is read before the run, between every batch of candidates, and once more before ranking.
//  * Nothing is rotated. The engine proposes; applying a candidate is the caller's act, taken after
//    a person has looked at the numbers.
//  * Every constant below is named and its comment gives the physical or algorithmic reason it has
//    the value it has. The fork exists because Orient.cpp carries 28 that do not.
//
// Frames: `its` and `inv` are in object coordinates, Z up, millimetres. A candidate's `rotation`
// maps object coordinates into the build frame, and evaluate() drops the part to the bed itself, so
// no candidate built here carries a translation.

#include "libslic3r/Nocte/Plan/OrientEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

// Degrees per radian and its inverse, spelled out once, as in Scores.cpp.
constexpr double RAD_TO_DEG = 180. / PI;
constexpr double DEG_TO_RAD = PI / 180.;

// Two unit vectors are treated as (anti)parallel when the magnitude of their dot product is within
// this of one. At that point the cross product that a from-to quaternion is built on has collapsed
// into round-off and an explicit axis has to be chosen instead. Same value and same reason as
// Scores.cpp's PARALLEL_DOT_EPS.
constexpr double PARALLEL_DOT_EPS = 1e-12;

// A direction shorter than this is read as "not given" rather than normalised: normalising round-off
// amplifies it into an arbitrary axis, and an arbitrary showcase normal or load direction would
// silently answer the question the user was asked precisely because we cannot answer it. Same value
// and reason as Scores.cpp's DIR_MIN_NORM.
constexpr double DIR_MIN_NORM = 1e-9;

// Floor under the duplicate-candidate angle, in radians. Two candidates whose bed directions agree
// this closely are the same orientation reached twice by different arithmetic — a cube's +X hull
// facet and the quarter turn about Y are the same orientation, and differ only in the round-off of
// cos(pi/2) — and scoring the second one costs a whole slicing pass for an answer already in hand.
// It is a floor under the caller's merge angle and not the test itself, so that a user who sets
// `angular_merge_deg` to zero, meaning "merge nothing", still does not pay for exact duplicates.
//
// The value is set by double precision and not by geometry. The test is a dot product against
// cos(theta), and 1 - cos(theta) is theta^2/2, so at 1e-8 rad that difference is 5e-17 — below the
// 2.2e-16 resolution of a double near one, where cos(theta) rounds to exactly 1 and the floor
// silently stops working. At 1e-6 rad it is 5e-13, four orders clear of the resolution, while
// 1e-6 rad is 6e-5 degrees and so still far below any angle a part distinguishes.
constexpr double DUPLICATE_MIN_RAD = 1e-6;

// Bounds on the caller's merge angle. Below zero the test means nothing. At a half turn every
// direction on the sphere falls inside one cluster and the engine is left with the current
// orientation alone to offer, which is the honest consequence of asking for that merge angle and is
// why the ceiling is the half turn rather than something smaller.
constexpr double MERGE_DEG_MIN = 0.;
constexpr double MERGE_DEG_MAX = 180.;

// Most clusters of hull normals one part may contribute, before ordering and truncation. The merge
// sweep compares each hull facet against the clusters accepted so far, so its cost is the product of
// the two, and this is what bounds it: a scanned part can present a hull of tens of thousands of
// facets and an unbounded cluster list makes that sweep quadratic. Facets are visited largest first,
// so what a full budget turns away is the smallest faces of an already very finely divided hull —
// faces that `max_candidates` would drop a few lines later in any case.
constexpr size_t HULL_NORMAL_BUDGET = 4096;

// Candidates handed to one TBB worker. One candidate is a whole slicing pass, tens of milliseconds
// on a small part and seconds on a tall one, which is four or five orders of magnitude more than the
// cost of scheduling a task. There is therefore nothing to gain by batching candidates into a worker
// and a great deal to lose: a grain of two would leave half the cores idle on a three-candidate set.
constexpr size_t CANDIDATE_GRAIN_SIZE = 1;

// Candidates scored between two consultations of the cancellation predicate. `cancel` is a caller's
// std::function of unknown thread safety — a GUI predicate reading a widget is the ordinary case —
// so it is called from the calling thread only, between parallel batches, and never from a worker.
// The batch size is therefore the worst-case delay between a user pressing cancel and the loop
// noticing, and it is chosen to fill the cores of a desktop machine once over: large enough that the
// scheduler is not re-entered per candidate, small enough that the delay is one batch of work rather
// than the remainder of the run.
constexpr size_t CANCEL_BATCH_CANDIDATES = 16;

// The tally of rejection reasons has one slot per enumerator, REJECT_REASON_COUNT, which lives in
// OrientEngine.hpp beside the enumeration because OrientResult carries the tally. Every write to it
// below is bounds-checked against that figure all the same: a value cast in from outside the enum
// must be a miscount, never an out-of-bounds write.

// Quanta of the ranking keys. std::sort needs a strict weak ordering, and "equal within a
// tolerance" is not one — it is not transitive — so each key is rounded to a whole number of its
// quantum and the integers are compared exactly. Each quantum sits well above the round-off the key
// carries and well below any difference a user would act on:
//  * SCORE: a weighted sum of five terms in [0, 1] whose weights sum to 1, so it lies in [0, 1]
//    itself. Round-off in it is ~1e-16; anything past the ninth decimal is not a preference.
//  * HEIGHT: a candidate laid onto a hull face built from float vertices rests up to ~1e-7 rad off
//    flat, which changes the height of a 20 mm part by ~2e-6 mm, and a rotation built with a square
//    root carries ~4e-15 mm on top. A micrometre clears both by a wide margin and is already past
//    what the model can defend — PlanToJson reports millimetres to three decimals for that reason.
//  * TURN: the same float normals put ~1e-7 rad into a rotation's angle. 1e-4 rad is 0.006 degrees,
//    three orders clear of that and three orders below any turn a person would call different.
// Support is quantised to the caller's support indifference floor instead, in plan_orientation(),
// because that floor is exactly the statement "a difference smaller than this is not a decision".
constexpr double SCORE_RANK_QUANTUM     = 1e-9;
constexpr double HEIGHT_RANK_QUANTUM_MM = 1e-3;
constexpr double TURN_RANK_QUANTUM_RAD  = 1e-4;
// Used for the support key when the caller's floor is zero, negative or not a number: the same
// reporting resolution as the height, in mm^3.
constexpr double SUPPORT_RANK_QUANTUM_FALLBACK_MM3 = 1e-3;
// Bound on a quantised key before std::llround, whose result is unspecified past the range of a
// long long (9.2e18). No key comes near it: a score of 1 is 1e9 quanta and a metre-tall part 1e6.
constexpr double RANK_KEY_LIMIT = 1e18;

// The axis-aligned turns generated, and therefore the slots reserved for them out of
// `max_candidates`. It is both the count the header promises (OrientEngine.hpp:115-117) and the
// length of the array axis_aligned_rotations() returns, and the two must not drift apart: a reserve
// smaller than the array would overrun the budget, a larger one would starve the hull of slots held
// for turns that are never generated. That is why the reserve is this name and not a literal six.
constexpr size_t AXIS_ALIGNED_COUNT = 6;

// The cancellation predicate, consulted safely. An empty std::function throws std::bad_function_call
// when it is called, and "no predicate" is the ordinary case for a CLI run, so the emptiness test is
// not decoration.
bool is_cancelled(const CancelFn &cancel)
{
    return cancel && cancel();
}

// A value about to be used as a sort key, as a whole number of `quantum`. std::sort requires a
// strict weak ordering and every comparison against a NaN is false, so one non-finite key would
// make the comparator intransitive and the sort undefined. A key we cannot read is mapped to the
// worst value instead of being trusted: an unreadable support volume or height must never sort to
// the front of the result. The same holds for a key past RANK_KEY_LIMIT, which std::llround could
// not represent; the negated test is what sends a NaN there as well.
long long rank_key(double v, double quantum)
{
    const double q = v / quantum;
    if (! (std::abs(q) < RANK_KEY_LIMIT))
        return (std::numeric_limits<long long>::max)();
    return std::llround(q);
}

// The rotation that lays a face whose outward unit normal is `n_unit` onto the build plate, that is,
// the rotation taking `n_unit` to -Z.
//
// Eigen's from-two-vectors construction rests on the cross product of its arguments, which vanishes
// at both ends of the range, so both ends are handled here rather than left to it: a normal already
// pointing at -Z needs no rotation at all, and a normal pointing at +Z needs a half turn about some
// horizontal axis, for which X is as good as any other because every axis orthogonal to Z gives the
// same bed direction and the score terms depend on nothing else (see build_candidates()).
//
// The half turn is written as the sign matrix diag(1, -1, -1) rather than built from
// AngleAxisd(PI, X). cos(PI) and sin(PI) in double are -1 and 1.2e-16, and that 1.2e-16 leaks into
// every coordinate the rotation maps: a 20 mm part comes out 20.000000000000004 mm tall, which is
// one phantom layer the moment anything rounds the height up. The sign matrix is exact.
Transform3d rotation_normal_to_bed(const Vec3d &n_unit)
{
    Transform3d  t        = Transform3d::Identity();
    const Vec3d  down     = Vec3d(0., 0., -1.);
    const double dot_down = n_unit.dot(down);
    if (dot_down >= 1. - PARALLEL_DOT_EPS)
        return t;
    if (dot_down <= -1. + PARALLEL_DOT_EPS) {
        Matrix3d half_turn_x = Matrix3d::Identity();
        half_turn_x(1, 1) = -1.;
        half_turn_x(2, 2) = -1.;
        t.linear() = half_turn_x;
        return t;
    }
    t.rotate(Eigen::Quaterniond::FromTwoVectors(n_unit, down));
    return t;
}

// The size of a candidate's move, in radians: the geodesic distance on SO(3) from the orientation
// the part arrived in to the one the candidate proposes.
//
// The current orientation is the identity by construction, so the candidate's rotation IS the
// displacement, and its angle-axis angle is the whole of it — the single smallest turn, about
// whatever axis, that takes the part from where it sits to where the candidate would put it. That is
// exactly the quantity the tie-break wants when it says "a part is never flipped for no gain".
// Nothing else available here is that number: Euler angles depend on a decomposition convention and
// would rank the same two candidates differently under XYZ and under ZYX, and counting axis flips
// cannot compare a quarter turn with a ten-degree nudge at all. Eigen returns the angle in [0, pi],
// so the two equivalent descriptions of a half turn, +180 and -180 degrees, give the one value,
// which is what makes the tie-break reproducible rather than merely defined.
double rotation_angle_rad(const Transform3d &rotation)
{
    const Matrix3d linear = rotation.linear();
    return Eigen::AngleAxisd(linear).angle();
}

// One facet of the convex hull, with its normal already turned outward.
struct HullFacet
{
    Vec3d  normal = Vec3d::Zero();
    double area   = 0.;   // mm^2
    size_t index  = 0;    // facet index in the hull, kept only as a deterministic sort tie-break
};

// A cluster of hull facets that all face the same way to within the merge angle.
struct MergedNormal
{
    // The representative direction: the normal of the LARGEST facet of the cluster, because that is
    // the face the part would actually come to rest on. A cluster's mean direction would be a face
    // the part does not have.
    Vec3d  normal = Vec3d::Zero();
    // The summed area of every facet merged into the cluster, which is what orders the candidates.
    double area   = 0.;   // mm^2
    // The hull facet that opened the cluster, kept so that equal areas resolve the same way on every
    // run of the same part.
    size_t seed   = 0;
};

// The outward unit normals of the convex hull, near-parallel facets merged, largest merged face
// first.
//
// Merging is not an optimisation, it is the difference between a plan and a waste of the budget: a
// tessellated cylinder presents hundreds of nearly parallel side facets and scoring each of them
// separately would spend the whole candidate budget re-measuring one orientation
// (OrientEngine.hpp:96-98).
//
// The outward direction is taken from the hull's own interior rather than from the facet winding.
// PartInvariants.cpp:188-190 states plainly that qhull's winding is consistent but not guaranteed,
// and it takes the magnitude of the hull volume for that reason. An inverted winding would hand
// every candidate the antipode of the face it meant to lay down, and for a part without antipodal
// symmetry — a tetrahedron is the smallest example — the largest face would then never reach the
// bed in any candidate. The mean of the hull's vertices is a convex combination with strictly
// positive weights, so it lies in the interior of a full-dimensional hull, and PartInvariants only
// builds a hull when the bounding box has extent on all three axes (PartInvariants.cpp:181-183).
// The facet centroid minus that point therefore points outward, whatever the winding.
std::vector<MergedNormal> merged_hull_normals(const indexed_triangle_set &hull, double merge_deg)
{
    std::vector<MergedNormal> out;
    if (hull.indices.empty() || hull.vertices.size() < 3)
        return out;

    Vec3d interior = Vec3d::Zero();
    for (const Vec3f &v : hull.vertices)
        interior += v.cast<double>();
    interior /= double(hull.vertices.size());
    if (! interior.allFinite())
        return out;

    const size_t           n_vertices = hull.vertices.size();
    std::vector<HullFacet> facets;
    facets.reserve(hull.indices.size());
    for (size_t i = 0; i < hull.indices.size(); ++ i) {
        const stl_triangle_vertex_indices &tri = hull.indices[i];
        bool in_range = true;
        for (int k = 0; k < 3; ++ k) {
            const int v = tri(k);
            if (v < 0 || static_cast<size_t>(v) >= n_vertices)
                in_range = false;
        }
        if (! in_range)
            // A hull facet pointing outside its own vertex array is dropped rather than thrown at
            // the rest of the hull, the same rule the score terms apply to the part's own facets.
            continue;

        // its_unnormalized_normal() yields the normal scaled by twice the facet area, so one call
        // gives both the direction and the area, and a degenerate facet is recognised by a zero
        // length rather than by producing a NaN direction. Same construction as
        // PartInvariants.cpp:120-126.
        const Vec3d  un  = its_unnormalized_normal(hull, i).cast<double>();
        const double len = un.norm();
        if (! std::isfinite(len) || ! (len > 0.))
            continue;

        const Vec3d centroid = (hull.vertices[tri(0)].cast<double>() +
                                hull.vertices[tri(1)].cast<double>() +
                                hull.vertices[tri(2)].cast<double>()) / 3.;
        Vec3d n = un / len;
        if (n.dot(centroid - interior) < 0.)
            n = -n;
        if (! n.allFinite())
            continue;

        HullFacet f;
        f.normal = n;
        f.area   = 0.5 * len;
        f.index  = i;
        facets.push_back(f);
    }
    if (facets.empty())
        return out;

    // Largest facet first, so that the facet which opens a cluster is already the one the part would
    // rest on and the representative direction never has to be replaced afterwards. The facet index
    // is the tie-break, so that two facets of exactly equal area — the two triangles of any
    // rectangular face, on every box-shaped part there is — always resolve the same way and the
    // candidate list is reproducible from one run to the next.
    std::sort(facets.begin(), facets.end(), [](const HullFacet &l, const HullFacet &r) {
        if (l.area != r.area)
            return l.area > r.area;
        return l.index < r.index;
    });

    // Bound first, as everywhere in this subsystem: max(FLOOR, x) yields the floor for a NaN x while
    // the reversed spelling hands the NaN through. A NaN merge angle therefore comes out as zero,
    // which merges nothing — the safe direction, because it costs scoring time and cannot collapse
    // the candidate set.
    const double merge_deg_clamped = (std::min)(MERGE_DEG_MAX, (std::max)(MERGE_DEG_MIN, merge_deg));
    const double merge_cos         = std::cos(merge_deg_clamped * DEG_TO_RAD);

    for (const HullFacet &f : facets) {
        bool merged = false;
        for (MergedNormal &m : out) {
            if (f.normal.dot(m.normal) >= merge_cos) {
                m.area += f.area;
                merged  = true;
                break;
            }
        }
        if (merged)
            continue;
        if (out.size() >= HULL_NORMAL_BUDGET)
            // The budget stops new clusters being OPENED and never stops merging, so the facets past
            // it still contribute their area to whichever cluster they belong to. Dropping them from
            // the sweep entirely would understate the area of the faces that remain.
            continue;
        MergedNormal m;
        m.normal = f.normal;
        m.area   = f.area;
        m.seed   = f.index;
        out.push_back(m);
    }

    // Largest merged face first. The summed area is the area of the real face the part would rest
    // on, which is what makes it the right ordering for a budget that truncates from the end: a part
    // comes to rest on a large face, not on a sliver. The seed index is the tie-break for the same
    // reason it was above.
    std::sort(out.begin(), out.end(), [](const MergedNormal &l, const MergedNormal &r) {
        if (l.area != r.area)
            return l.area > r.area;
        return l.seed < r.seed;
    });
    return out;
}

// The six axis-aligned turns a person would try by hand: a quarter turn each way about X and about
// Y, and a half turn about each of them (OrientEngine.hpp:115-117).
//
// Five of the six lay a different side of a box down — -Y, +Y, +X, -X and +Z — and the two half
// turns lay the same side down as each other, differing only by a quarter turn about the build Z.
// The sixth side, -Z, is the current orientation and is already candidate zero. The duplicate is
// generated rather than pruned here because the header promises six; build_candidates() collapses it
// against whichever candidate already lays +Z on the bed, which is what the duplicate test is for.
//
// Each turn is written out as its exact signed permutation matrix rather than built from an
// AngleAxisd. cos(PI/2) in double is 6.1e-17, not 0, and a rotation built from it maps a 20 mm cube
// to a height of 20.000000000000004 mm — a phantom 101st layer wherever the height is rounded up, and
// a tie against the identity that is then decided by round-off rather than by the tie-break chain.
// These matrices are exact, so a turn that lays a face flat lays it exactly flat.
std::array<Transform3d, AXIS_ALIGNED_COUNT> axis_aligned_rotations()
{
    // Row-major 3x3 linear parts, every entry -1, 0 or 1. Rx(a) is [1 0 0; 0 c -s; 0 s c] and
    // Ry(a) is [c 0 s; 0 1 0; -s 0 c], evaluated at a = +90, -90 and 180 degrees.
    static const double TURNS[][9] = {
        { 1.,  0.,  0.,   0.,  0., -1.,   0.,  1.,  0. },   // Rx(+90)
        { 1.,  0.,  0.,   0.,  0.,  1.,   0., -1.,  0. },   // Rx(-90)
        { 1.,  0.,  0.,   0., -1.,  0.,   0.,  0., -1. },   // Rx(180)
        { 0.,  0.,  1.,   0.,  1.,  0.,  -1.,  0.,  0. },   // Ry(+90)
        { 0.,  0., -1.,   0.,  1.,  0.,   1.,  0.,  0. },   // Ry(-90)
        {-1.,  0.,  0.,   0.,  1.,  0.,   0.,  0., -1. },   // Ry(180)
    };
    // A brace list shorter than the array it initialises COMPILES and initialises the rest by
    // default — for a std::array of Eigen transforms that is an uninitialised matrix scored under
    // the AxisAligned label. So the table is sized by its own initialiser and its row count is
    // checked here. A row written with fewer than nine entries would zero-fill in the same way,
    // which is why every row is spelled out in full.
    static_assert(sizeof(TURNS) / sizeof(TURNS[0]) == AXIS_ALIGNED_COUNT,
                  "the axis-aligned turn table and AXIS_ALIGNED_COUNT disagree");

    std::array<Transform3d, AXIS_ALIGNED_COUNT> out;
    for (size_t k = 0; k < AXIS_ALIGNED_COUNT; ++ k) {
        Matrix3d m;
        for (int r = 0; r < 3; ++ r)
            for (int c = 0; c < 3; ++ c)
                m(r, c) = TURNS[k][3 * r + c];
        out[k] = Transform3d::Identity();
        out[k].linear() = m;
    }
    return out;
}

// One proposed orientation, before it has been scored.
struct CandidateSeed
{
    Transform3d     rotation = Transform3d::Identity();
    // The OBJECT direction this candidate lays on the bed, that is, R^T * (-Z). It is what the
    // duplicate test compares, and for a hull candidate it is exactly the hull normal that produced
    // the rotation, because R n = -Z means n = R^T (-Z).
    Vec3d           down_obj = Vec3d(0., 0., -1.);
    CandidateSource source   = CandidateSource::Current;
};

// The candidate set, ordered and truncated.
//
// Two candidates are the same candidate when they lay the same object direction on the bed, and the
// engine's whole answer really does depend on that direction alone. Writing row_z = R^T * Z, every
// field of OrientScores reads the rotation through row_z and through nothing else: the cusp sweep
// dots each facet normal with it (Scores.cpp:366), the support carry and the footprint slice are
// taken on planes normal to it, `height_mm` is the mesh's extent along it, the strength term uses
// only d.z() = row_z . load (Scores.cpp:805-807), and the tipping margin is a distance in the bed
// plane, which a rotation about the build Z carries around with the footprint hull. The showcase
// constraint below reads only the Z component of the rotated normal, so it is row_z as well. A
// second rotation about the build Z therefore produces a candidate that is identical in every number
// this engine reports, and scoring it is a slicing pass spent on an answer already in hand.
//
// This is the same test, on the same quantity, that merged_hull_normals() applies to the hull: there
// it merges parallel facets of one part, here it collapses a hull face against the axis-aligned turn
// that lays the same face down. On a cube the six quarter and half turns all land on hull faces and
// the whole axis-aligned set collapses, leaving six candidates for six sides.
std::vector<CandidateSeed> build_candidates(const PartInvariants &inv, const OrientParams &params)
{
    std::vector<CandidateSeed> out;

    const double merge_deg_clamped = (std::min)(MERGE_DEG_MAX,
                                                (std::max)(MERGE_DEG_MIN, params.angular_merge_deg));
    const double dedup_cos = std::cos((std::max)(DUPLICATE_MIN_RAD, merge_deg_clamped * DEG_TO_RAD));
    // The current orientation merges only with an EXACT duplicate of itself, never within the merge
    // angle. Every other merge keeps a rotation that lays the merged face down, so dropping the
    // near-duplicate costs a few degrees at most; the current candidate keeps the identity, which
    // lays nothing down. A hull face 3 degrees off the bed absorbed into it would therefore never be
    // laid flat by any candidate — the part would be offered only as it sits, resting on an edge.
    // Take a 100 x 60 x 3 plate imported 3 degrees tilted: its identity is unstable, its large face
    // is inside the merge angle of the identity, and with the merge angle applied here the whole set
    // came out degenerate for a part whose right answer is obvious.
    // The floor is the angle below which rotation_normal_to_bed() already returns the identity
    // (1 - cos a < PARALLEL_DOT_EPS, i.e. a < sqrt(2 * PARALLEL_DOT_EPS) = 1.41e-6 rad): a face that
    // close would otherwise enter as a HullFace whose rotation is exactly the identity.
    const double exact_cos = std::cos((std::max)(DUPLICATE_MIN_RAD, std::sqrt(2. * PARALLEL_DOT_EPS)));

    // When two candidates merge, the surviving label is the SMALLEST CandidateSource value: Current
    // beats HullFace, and HullFace beats AxisAligned.
    //
    // This is not cosmetic and it is not an accident of the order things are added in. Most parts
    // arrive resting on a hull facet, so the hull normal that reproduces the identity is the common
    // case, not the corner one. Labelling that candidate HullFace would report the orientation the
    // part is ALREADY IN as a face to rotate it onto — the engine would present every orientation as
    // something to change, and the never-flip-for-no-gain rule, which is true of the transform,
    // would be invisible in the result a person actually reads.
    //
    // It is enforced here rather than left to the generation order. The order below happens to add
    // Current, then HullFace, then AxisAligned, so the candidate already in the list always holds
    // the smaller value and the minimum would come out right by itself. That is exactly why the rule
    // has to be written down: it would keep coming out right until somebody reordered the three
    // blocks, and then it would quietly stop, with nothing failing to say so.
    auto add = [&out, dedup_cos, exact_cos](const Transform3d &rotation, const Vec3d &down_obj,
                                            CandidateSource source) {
        for (CandidateSeed &c : out) {
            // Either side being the current orientation selects the exact test, so the rule holds
            // whichever of the two was generated first.
            const bool   with_current = c.source == CandidateSource::Current ||
                                        source == CandidateSource::Current;
            const double limit_cos    = with_current ? exact_cos : dedup_cos;
            if (c.down_obj.dot(down_obj) >= limit_cos) {
                if (static_cast<uint8_t>(source) < static_cast<uint8_t>(c.source))
                    c.source = source;
                return;
            }
        }
        CandidateSeed seed;
        seed.rotation = rotation;
        seed.down_obj = down_obj;
        seed.source   = source;
        out.push_back(seed);
    };

    // The budget is floored at one. That is the single place a caller's parameter is overridden
    // here, and the header requires it: a degenerate result has to hold the current orientation as
    // its fallback, and a budget of zero would leave the engine with nothing at all to hand back.
    const size_t budget = (std::max)(size_t(1), params.max_candidates);

    // The axis-aligned turns are RESERVED out of the budget rather than left to compete for it
    // (OrientEngine.hpp:109-112). The hull is truncated; the reserved set is not. Without the reserve,
    // a scanned or smooth part whose merged hull alone fills every slot would never score the turns
    // a person would try by hand, which is the one outcome that makes a plan look broken.
    //
    // The subtraction saturates. `budget` can be smaller than the slot for the current orientation
    // plus the reserve, and an unsigned wrap there would turn "no room for hull faces at all" into
    // "room for every hull face there is" — the exact inversion of the intent, and silent.
    const size_t reserve     = params.include_axis_aligned ? AXIS_ALIGNED_COUNT : size_t(0);
    const size_t hull_budget = budget > 1 + reserve ? budget - 1 - reserve : size_t(0);

    // The identity, always first and always present. It is how the part already sits, it costs the
    // user nothing to accept, the tie-break chain measures every other candidate's turn away from
    // it, and the degenerate branch hands it back as the fallback.
    add(Transform3d::Identity(), Vec3d(0., 0., -1.), CandidateSource::Current);

    // Hull faces, largest merged face first, truncated at the hull's share of the budget.
    //
    // Truncating from the end is safe in this order and only in this order: what falls off is always
    // a face SMALLER than every face kept, and a part does not come to rest on its fifty-eighth
    // largest face while fifty-seven larger ones are available to it. The resting face is chosen by
    // gravity, and gravity picks area.
    //
    // Only a face that actually took a slot is charged for one. A hull normal the duplicate test
    // collapses into a candidate already in the list — the common case on a box, where every hull
    // face also appears as an axis-aligned turn — has cost nothing, so counting it against the
    // budget would spend slots on candidates that are not there.
    size_t hull_added = 0;
    for (const MergedNormal &m : merged_hull_normals(inv.hull, params.angular_merge_deg)) {
        if (hull_added >= hull_budget)
            break;
        const size_t before = out.size();
        add(rotation_normal_to_bed(m.normal), m.normal, CandidateSource::HullFace);
        if (out.size() != before)
            ++ hull_added;
    }

    if (params.include_axis_aligned) {
        const std::array<Transform3d, AXIS_ALIGNED_COUNT> turns = axis_aligned_rotations();
        for (const Transform3d &r : turns) {
            // The reserve guarantees this loop room at any budget that can hold the reserved set at
            // all. Below that — a caller asking for fewer than eight candidates, which is smaller
            // than the set of sides a box has — the upper bound wins over the reserve, because
            // `max_candidates` is documented as a bound on candidates SCORED and a bound that a
            // reserve could push through is not a bound. Such a caller gets as many of the turns as
            // fit, in generation order, and never more work than it asked for.
            if (out.size() >= budget)
                break;
            const Vec3d down_obj = r.linear().transpose() * Vec3d(0., 0., -1.);
            add(r, down_obj, CandidateSource::AxisAligned);
        }
    }

    // No truncation pass follows, and none is needed: the current orientation takes one slot, the
    // hull loop takes at most `hull_budget` and the turn loop stops at `budget`, so the list is
    // within the budget by construction rather than by being cut back to it afterwards.
    return out;
}

// Whether the showcase-clean constraint is active: the intent's constraint AND the user's own
// toggle.
//
// constraints_for() may already fold `showcase_support_free` into `require_showcase_clean`, in which
// case conjoining them again is a no-op; if it does not, this is where the toggle takes effect.
// Either way the toggle cannot be lost. It lives in one function because two callers read it — the
// pre-flight tier check and classify() — and a filter that disagreed with the pre-flight about what
// is active would either answer a question nobody asked or refuse to answer one somebody did.
bool showcase_clean_required(const PlanIntent &intent, const PlanConstraints &constraints)
{
    return constraints.require_showcase_clean && intent.showcase_support_free;
}

// The one intent-and-tier combination that no candidate can ever satisfy, recognised before a single
// slice is taken rather than after every candidate has been scored and thrown away.
//
// Tier 0 computes no contact area at all and reports `contact_measured == false` however the
// visibility flags came out, and says so (Scores.cpp:704-710). The showcase-clean constraint reads
// exactly that field, so on tier 0 it cannot pass for any orientation of any part: step 2 of the
// filter would reject every candidate as NotMeasured, and the user would be handed a blank plan
// naming a measurement failure, with nothing anywhere to say that a speed setting caused it. The
// real cause is the tier and the fix is to raise it, so that is what the result has to say — and it
// can be said without slicing anything, because the answer does not depend on the geometry.
//
// Only FacetSweep is caught. FullDetect is not reachable from this entry point and evaluate() falls
// back to the tier-1 path for it (Scores.cpp:701-703), which does compute contact.
bool tier_cannot_answer(const PlanIntent      &intent,
                        const PlanConstraints &constraints,
                        const ScoreParams     &scores)
{
    return showcase_clean_required(intent, constraints) &&
           scores.support_tier == SupportTier::FacetSweep;
}

// The two filter steps of the contract, in the contract's order (OrientEngine.hpp:181-191):
// measurement flags first, hard constraints second.
//
// The order is not a matter of taste. An unmeasured term is left at zero by Scores.cpp, and a zero
// fails the stability floor, reads as "no load-bearing section" and reads as "no support on the
// visible face". Testing the constraints first would therefore report a candidate we could not
// measure as Unstable, or as NoLoadSection — the name of whichever constraint its zero happened to
// land on — and the user would go and relax a constraint that was never the problem. NotMeasured is
// the honest answer and it has to be reached first.
//
// `section_required` says whether anything in this run reads the load-bearing section — see where
// plan_orientation() computes it. It is decided once per run rather than here because it depends on
// the weights, which this function has no other reason to see.
RejectReason classify(const OrientScores    &s,
                      const Transform3d     &rotation,
                      const PlanIntent      &intent,
                      const PlanConstraints &constraints,
                      bool                   section_required)
{
    // --- step 2: the measurement flags ----------------------------------------------------------

    // `measured` is the geometry the candidate rests on: the invariants were usable AND the first
    // layer produced a footprint hull (Scores.hpp:244-249). Without it the stability and both
    // footprint figures are zero rather than measured, and the constraints below would name whichever
    // of the two those zeros happened to fail.
    if (! s.measured)
        return RejectReason::NotMeasured;

    // The support volume is required whatever the weights say, because it is not only the support
    // term: it is added to the extruded volume inside the time estimate (Scores.cpp:793). A failed
    // support sweep therefore understates this candidate's time as well as its support, and both
    // errors point the same way, towards winning.
    if (! s.support_measured)
        return RejectReason::NotMeasured;

    // Zero cusp is a perfectly smooth part, the best value on that term. An inverted mesh — every
    // facet wound inward, an ordinary broken STL — reports exactly that on every candidate
    // (Scores.hpp:154-160), and the term would quietly stop discriminating with nobody told.
    if (! s.cusp_measured)
        return RejectReason::NotMeasured;

    // Contact is demanded only when a constraint actually reads it. It feeds no weighted term at
    // all — the header calls it "a hard constraint for signage, not a weighted term" — so requiring
    // it of an ornament would reject every candidate of a scanned mesh over a question nobody asked.
    // When the showcase constraint IS active the flag is mandatory: an unmeasured contact is a zero,
    // and a zero there would walk a fully supported showcase face straight through the gate below.
    //
    // The one case where this could never be satisfied by any candidate — the constraint against
    // tier 0, which computes no contact at all — has already been answered before scoring, by
    // tier_cannot_answer(). What reaches here is a tier that CAN measure contact and did not on this
    // particular candidate, which is a genuine per-candidate measurement failure.
    const bool showcase_clean = showcase_clean_required(intent, constraints);
    if (showcase_clean && ! s.contact_measured)
        return RejectReason::NotMeasured;

    // The load-bearing section, by the same rule as contact: demanded only when something reads it.
    // With a load direction given, a sweep that came back empty leaves the section and the failure
    // force at zero (Scores.hpp on `section_measured`). The hard constraint would then name that zero
    // NoLoadSection — telling the user the part has no load path, which a closed solid always has —
    // and the strength term would rank on a force nobody measured. A closed solid always has a
    // positive section, so the zero is the sweep failing, and NotMeasured is the name for that.
    //
    // With NO load direction the flag is false as well, and deliberately not caught here: that case
    // is the user's missing input and falls through to NoLoadSection below, as documented there.
    if (section_required && ! s.section_measured)
        return RejectReason::NotMeasured;

    // --- step 3: the intent's hard constraints --------------------------------------------------

    // Negated comparisons throughout, so that a value we could not read fails rather than passes:
    // every comparison against a NaN is false, so `! (x >= floor)` rejects it while `x < floor`
    // would admit it.
    if (! (s.stability >= constraints.min_stability))
        return RejectReason::Unstable;

    // The RAW first-layer area, never the hull area: a ring's convex hull would claim the whole disc
    // and overstate its grip on the plate (Scores.hpp:237-240). A floor of zero means no requirement
    // and is tested first so that a part with no measurable adhesion is not rejected by a constraint
    // the intent did not ask for.
    if (constraints.min_footprint_mm2 > 0. &&
        ! (s.footprint_area_mm2 >= constraints.min_footprint_mm2))
        return RejectReason::FootprintTooSmall;

    // A missing load direction lands here too, and deliberately. min_section_area_along() returns 0
    // for a zero-length direction, so every candidate fails and the run ends degenerate with
    // NoLoadSection as the blocking reason — which is the truth: the intent asked for a load-bearing
    // orientation and no load was given. PlanIntent::missing_input() names the missing input, this
    // names its consequence, and neither of them invents a direction.
    if (constraints.require_load_section && ! (s.min_section_area_mm2 > 0.))
        return RejectReason::NoLoadSection;

    if (constraints.require_showcase_up) {
        const double normal_norm = intent.showcase_normal_obj.norm();
        if (! std::isfinite(normal_norm) || ! (normal_norm > DIR_MIN_NORM))
            // The intent requires a showcase normal and has not been given one. Rejecting every
            // candidate is the honest outcome; defaulting the normal to +Z would answer the question
            // the user was asked precisely because we cannot answer it.
            return RejectReason::ShowcaseNotUp;
        // The showcase face is given by its normal in OBJECT coordinates, so the test is to rotate
        // that normal into the build frame and ask how far it has come to rest from straight up.
        const Vec3d n_build = rotation.linear() * (intent.showcase_normal_obj / normal_norm);
        // Bound first in both clamps: a NaN Z component comes out as -1 and therefore as a half turn
        // from vertical, which is rejected, and a NaN tolerance comes out as zero, which admits only
        // an exactly upward face. Both are the safe direction.
        const double up        = (std::min)(1., (std::max)(-1., n_build.z()));
        const double angle_deg = std::acos(up) * RAD_TO_DEG;
        const double tol_deg   = (std::max)(0., intent.showcase_tol_deg);
        if (! (angle_deg <= tol_deg))
            return RejectReason::ShowcaseNotUp;
    }

    // Reached only with `contact_measured` true, checked at step 2, so this zero is a measurement and
    // not a silence. Note the granularity the field carries (Scores.hpp:175-178): it is the whole
    // part's visible contact, which is a sound necessary condition for the per-face question — zero
    // here implies zero on the showcase face — and nothing more until the panel carries a face
    // selection. It can therefore reject a candidate whose support lands on some other visible face,
    // which is conservative in the direction a signage job wants.
    if (showcase_clean && s.support_contact_mm2 > 0.)
        return RejectReason::ShowcaseSupported;

    return RejectReason::None;
}

// The reason that removed the most candidates, not the first one encountered.
//
// "First" is an accident of the candidate order. Candidate zero is always the identity, so the first
// rejection would report whatever the orientation the part HAPPENED TO ARRIVE IN failed — a property
// of how the user exported the file, not of the part. The most common reason is the constraint that
// emptied the search space, and therefore the one a user has to relax before this part can be
// planned at all. Ties resolve to the lowest enumerator so that the answer is reproducible.
//
// It reads the tally rather than recounting, so the summary and the per-reason counts the result
// carries (OrientResult::rejection_counts) cannot disagree.
RejectReason most_common_reason(const std::array<size_t, REJECT_REASON_COUNT> &tally)
{
    RejectReason reason = RejectReason::None;
    size_t       count  = 0;
    // Slot 0 is None and is skipped: an accepted candidate is not a reason for the set being empty,
    // and in the degenerate case there are none of them to count anyway.
    for (size_t slot = 1; slot < REJECT_REASON_COUNT; ++ slot) {
        // Strictly greater, so the first slot to reach a given count keeps it and a tie resolves to
        // the lowest enumerator.
        if (tally[slot] > count) {
            count  = tally[slot];
            reason = static_cast<RejectReason>(slot);
        }
    }
    return reason;
}

// How many of `candidates` carry each rejection reason. An accepted candidate counts in no slot, so
// slot 0 stays at zero, and a value from outside the enumeration is dropped rather than written past
// the end of the array.
std::array<size_t, REJECT_REASON_COUNT> tally_reasons(const std::vector<OrientCandidate> &candidates)
{
    std::array<size_t, REJECT_REASON_COUNT> tally{};   // value-initialised: every slot starts at 0
    for (const OrientCandidate &c : candidates) {
        const size_t slot = static_cast<size_t>(c.rejected);
        if (slot > 0 && slot < REJECT_REASON_COUNT)
            ++ tally[slot];
    }
    return tally;
}

// The ranking keys of one surviving candidate, each a whole number of its quantum. See rank_key().
struct RankKey
{
    long long score   = 0;
    long long support = 0;
    long long height  = 0;
    long long turn    = 0;   // quanta of rotation away from the current orientation
    size_t    index   = 0;   // index into the candidate vector, the final tie-break
};

} // namespace

const char *reject_reason_name(RejectReason reason)
{
    switch (reason) {
    case RejectReason::None:              return "None";
    case RejectReason::NotMeasured:       return "NotMeasured";
    case RejectReason::Unstable:          return "Unstable";
    case RejectReason::FootprintTooSmall: return "FootprintTooSmall";
    case RejectReason::NoLoadSection:     return "NoLoadSection";
    case RejectReason::ShowcaseNotUp:     return "ShowcaseNotUp";
    case RejectReason::ShowcaseSupported: return "ShowcaseSupported";
    case RejectReason::TierCannotAnswer:  return "TierCannotAnswer";
    }
    return "Unknown";
}

const char *candidate_source_name(CandidateSource source)
{
    switch (source) {
    case CandidateSource::Current:     return "Current";
    case CandidateSource::HullFace:    return "HullFace";
    case CandidateSource::AxisAligned: return "AxisAligned";
    }
    return "Unknown";
}

OrientResult plan_orientation(const indexed_triangle_set &its,
                              const PartInvariants       &inv,
                              const PlanIntent           &intent,
                              const OrientParams         &params,
                              const CancelFn             &cancel)
{
    OrientResult result;

    // An unusable mesh comes back as `ok == false` and nothing else, exactly as the header promises.
    // The invariants are what every score term reads, so a part they could not measure has no
    // candidate worth proposing — not even the current orientation, since there is not one true
    // number we could say about it.
    if (! inv.valid || its.vertices.empty() || its.indices.empty())
        return result;

    if (is_cancelled(cancel)) {
        result.cancelled = true;
        return result;
    }

    const PlanConstraints constraints = constraints_for(intent);

    // The one question this configuration cannot answer, answered before any work is done. Scoring
    // every candidate first and rejecting them all as NotMeasured would spend the whole budget on a
    // conclusion that was available from two flags, and would then report a measurement failure for
    // what is really a settings failure — a blank plan whose actual cause is a speed setting, which
    // is the class of misleading answer this engine exists not to give.
    //
    // The result is degenerate rather than `ok == false`: the engine DID reach a conclusion, and it
    // is a conclusion the user can act on. The fallback candidate is the current orientation with
    // untouched scores, because nothing was measured and the flags inside `scores` say so; it keeps
    // TierCannotAnswer as its reason and appears in both arrays, exactly as the other degenerate
    // path does. `considered` stays 0, which is the truth: no candidate was scored.
    if (tier_cannot_answer(intent, constraints, params.scores)) {
        OrientCandidate fallback;
        fallback.source   = CandidateSource::Current;
        fallback.rejected = RejectReason::TierCannotAnswer;
        result.best.push_back(fallback);
        result.rejected.push_back(fallback);
        result.degenerate      = true;
        result.blocking_reason = RejectReason::TierCannotAnswer;
        result.rejection_counts = tally_reasons(result.rejected);
        result.ok              = true;
        return result;
    }

    const std::vector<CandidateSeed> seeds = build_candidates(inv, params);
    if (seeds.empty())
        // build_candidates() always emits the current orientation, so this is unreachable on a mesh
        // that got past the gate above. It is kept because the loops below index `seeds` and an
        // empty set would make `candidates.front()` in the degenerate branch an out-of-bounds read.
        return result;

    // One sweep for the whole run, never one per candidate. The minimum section normal to the load
    // direction is measured in the OBJECT frame and no rotation can change it (Scores.hpp:252-258);
    // repeating it per candidate would slice the part up to `section_max_planes` times over again
    // for a number already in hand.
    Vec3d  load_dir_obj    = Vec3d::Zero();
    double min_section_mm2 = 0.;
    bool   load_given      = false;
    {
        const double load_norm = intent.load_dir_obj.norm();
        if (std::isfinite(load_norm) && load_norm > DIR_MIN_NORM) {
            load_dir_obj    = intent.load_dir_obj;
            min_section_mm2 = min_section_area_along(its, load_dir_obj, params.scores);
            load_given      = true;
        }
    }

    // The weights are the intent's, and combine() normalises them itself. They are resolved here
    // rather than at step 4 because the filter needs one fact from them too.
    const ScoreWeights weights = weights_for(intent.kind);

    // Whether the load-bearing section is READ by this run, and therefore has to have been measured:
    // a load direction was given, and either the intent's hard constraint tests the section or its
    // weights rank on the failure force built from it. A load direction is used whenever it is given,
    // not only by FunctionalStrength — Unspecified and FunctionalVisual weigh strength too (HLSD §7) —
    // but under Ornament or Draft the strength weight is zero, nothing reads the section, and
    // rejecting every candidate over a sweep nobody consulted would be the over-reach the contact
    // rule in classify() already refuses.
    const bool section_required = load_given &&
                                  (constraints.require_load_section || weights.strength > 0.);

    const size_t              n = seeds.size();
    std::vector<OrientScores> scored(n);

    try {
        // Cancellation, done so that it works. The predicate is read on the calling thread between
        // batches rather than inside a worker, because `cancel` is a caller's std::function of
        // unknown thread safety and calling it from several workers at once would be a data race
        // this engine introduced. The batch bounds the delay: a cancel is noticed within one batch
        // of candidates, not at the end of the run. Orient.cpp:84 takes the predicate and never
        // reads it at all, which is the behaviour this loop exists to replace.
        for (size_t base = 0; base < n; base += CANCEL_BATCH_CANDIDATES) {
            if (is_cancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            const size_t end = (std::min)(n, base + CANCEL_BATCH_CANDIDATES);
            tbb::parallel_for(tbb::blocked_range<size_t>(base, end, CANDIDATE_GRAIN_SIZE),
                [&its, &inv, &params, &load_dir_obj, min_section_mm2, &seeds, &scored]
                (const tbb::blocked_range<size_t> &range) {
                    // Each iteration writes its own entry of `scored` and reads nothing that any
                    // other iteration writes, so the loop needs no synchronisation. evaluate()
                    // touches only its arguments, all of which are const here, and its own locals.
                    for (size_t i = range.begin(); i < range.end(); ++ i)
                        scored[i] = evaluate(its, inv, seeds[i].rotation, params.scores,
                                             load_dir_obj, min_section_mm2);
                });
        }
    } catch (...) {
        // evaluate() and min_section_area_along() both guard their own geometry, so what can still
        // arrive here is exhaustion rather than user geometry — an allocation that failed, or a TBB
        // worker rethrowing one. Either way the engine did not run, which is precisely what
        // `ok == false` means, and a half-scored candidate set must never be ranked.
        return OrientResult();
    }

    if (is_cancelled(cancel)) {
        result.cancelled = true;
        return result;
    }

    std::vector<OrientCandidate> candidates(n);
    for (size_t i = 0; i < n; ++ i) {
        candidates[i].rotation = seeds[i].rotation;
        candidates[i].source   = seeds[i].source;
        candidates[i].scores   = scored[i];
    }
    result.considered = n;

    // `constraints` was resolved before scoring, for the pre-flight tier check, and is reused here
    // rather than recomputed: two calls to constraints_for() for one run would be two chances for
    // the filter to apply a different rule from the one the pre-flight cleared.
    for (size_t i = 0; i < n; ++ i)
        candidates[i].rejected = classify(candidates[i].scores, candidates[i].rotation,
                                          intent, constraints, section_required);

    std::vector<size_t>       survivors;
    std::vector<OrientScores> survivor_scores;
    survivors.reserve(n);
    survivor_scores.reserve(n);
    for (size_t i = 0; i < n; ++ i) {
        if (candidates[i].accepted()) {
            survivors.push_back(i);
            survivor_scores.push_back(candidates[i].scores);
        }
    }

    // --- step 4: normalise and weight the survivors, and only the survivors ---------------------
    // combine() normalises each term across the set it is handed, so a rejected candidate left in
    // would stretch the min-max range of every term around a value that cannot be chosen. That is
    // the same reason the header gives for filtering rather than penalising (PlanIntent.hpp:99-101).
    if (! survivors.empty()) {
        // The weights were resolved above. The indifference floors come from the caller, because
        // they are a statement about what differences matter to a user rather than a numerical
        // tolerance: a caller that cares about a 200 mm^3 support difference has to be able to say
        // so, and the defaults are only a default.
        std::vector<std::array<double, 5>> normalized;
        const std::vector<double> combined = combine(survivor_scores, weights, params.epsilons,
                                                     &normalized);

        // combine() returns one entry per input and fills `normalized` to the same length. The two
        // are indexed together here, so the length is folded into one minimum rather than assumed: a
        // mismatch would be an out-of-bounds write instead of a wrong answer.
        const size_t m = (std::min)((std::min)(combined.size(), normalized.size()), survivors.size());
        for (size_t k = 0; k < m; ++ k) {
            OrientCandidate &c = candidates[survivors[k]];
            c.score = combined[k];
            c.terms = normalized[k];
        }
    }

    // --- step 5: rank, with the deterministic tie-break chain ------------------------------------
    // Less support, then lower height, then the smaller turn away from the current orientation, then
    // the lower candidate index. The last link makes the order total rather than merely weak, so the
    // result does not depend on which candidate std::sort happened to look at first.
    //
    // Every key is compared as a whole number of its quantum, never as a raw double. Compared raw,
    // the chain is decided by round-off before it reaches the rule it states: a cube laid on a side
    // by a rotation built from a square root is 20.000000000000004 mm tall, so even once the score
    // stops charging it a phantom layer it still loses on HEIGHT to the identity by four
    // femtometres, and "never flip for no gain" would hold only because the round-off happened to
    // fall that way — and on a part imported tilted, where every face candidate carries float noise,
    // it would not hold at all. Quantised, the candidates tie on height and the turn decides, which
    // is the reason the rule gives.
    //
    // The support key uses the caller's own indifference floor as its quantum: combine() has just
    // declared a support spread below that floor irrelevant to the score, and re-deciding the same
    // difference one link down the chain would undo that. Rounding to a quantum makes a boundary at
    // half of it — 240 mm^3 and 260 mm^3 against a 500 mm^3 floor land in different buckets — which
    // is the price of a comparator that is transitive; a tolerance compare would not be.
    const bool   support_floor_usable = (params.epsilons.support_mm3 > 0.) &&
                                        std::isfinite(params.epsilons.support_mm3);
    const double support_quantum      = support_floor_usable ? params.epsilons.support_mm3
                                                             : SUPPORT_RANK_QUANTUM_FALLBACK_MM3;
    std::vector<RankKey> keys;
    keys.reserve(survivors.size());
    for (size_t k = 0; k < survivors.size(); ++ k) {
        const OrientCandidate &c = candidates[survivors[k]];
        RankKey key;
        key.score   = rank_key(c.score, SCORE_RANK_QUANTUM);
        key.support = rank_key(c.scores.support_volume_mm3, support_quantum);
        key.height  = rank_key(c.scores.height_mm, HEIGHT_RANK_QUANTUM_MM);
        key.turn    = rank_key(rotation_angle_rad(c.rotation), TURN_RANK_QUANTUM_RAD);
        key.index   = survivors[k];
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](const RankKey &l, const RankKey &r) {
        if (l.score   != r.score)   return l.score   < r.score;
        if (l.support != r.support) return l.support < r.support;
        if (l.height  != r.height)  return l.height  < r.height;
        if (l.turn    != r.turn)    return l.turn    < r.turn;
        return l.index < r.index;
    });

    for (const RankKey &k : keys) {
        if (result.best.size() >= params.max_results)
            break;
        result.best.push_back(candidates[k.index]);
    }

    for (size_t i = 0; i < n; ++ i) {
        if (! candidates[i].accepted())
            result.rejected.push_back(candidates[i]);
    }
    result.rejection_counts = tally_reasons(result.rejected);

    if (survivors.empty()) {
        result.degenerate      = true;
        result.blocking_reason = most_common_reason(result.rejection_counts);
        // The fallback is the current orientation, candidate zero by construction: it is where the
        // part already sits, so it is the one proposal that cannot make anything worse.
        //
        // Its `rejected` field keeps the reason it failed rather than being cleared to None. Clearing
        // it would hand back a rejected candidate dressed as an accepted one, which is the class of
        // silent lie this engine exists to stop; `degenerate` is the flag that says how to read the
        // entry, and the same candidate appears in `rejected` alongside its reason.
        //
        // `max_results` does not gate it. The fallback is not one of the ranked results that figure
        // bounds — there are none — it is the engine saying where the part sits and, in
        // `blocking_reason`, why nothing beat it. The header's degenerate paragraph asks for it
        // outright (OrientEngine.hpp:134-145).
        result.best.push_back(candidates.front());
    }

    // The engine ran. A degenerate set is a finding, not a failure: `ok` says the numbers below are
    // real, `degenerate` says the filter left nothing, and `blocking_reason` says what did it.
    result.ok = true;
    return result;
}

} // namespace Nocte
} // namespace Slic3r
