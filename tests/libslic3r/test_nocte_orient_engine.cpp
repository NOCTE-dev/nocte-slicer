// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Contract tests for the orientation engine and for the intent table it ranks with. Every expected
// value below is hand computed from the geometry of the fixture mesh and from the behaviours
// documented in src/libslic3r/Nocte/Plan/OrientEngine.hpp and PlanIntent.hpp; nothing here is a
// regression baseline captured from an implementation.
//
// The engine's whole claim is "it picks the sensible orientation and it can say why", so the
// strongest assertions in this file are the four that name a specific orientation — the signage
// plate that must lie flat, the plate that must be REFUSED rather than stood on its edge, the cap on
// a stem that must be turned over, and the cube that must be left exactly where it is.
//
// Frame convention, from OrientEngine.hpp and Scores.hpp: `OrientCandidate::rotation` maps OBJECT
// coordinates to BUILD coordinates, so a candidate lays object direction d down onto the bed when
// `rotation.linear() * d` points at build -Z, and holds it up when it points at build +Z.

#include <catch2/catch_all.hpp>

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/Plan/PartInvariants.hpp"
#include "libslic3r/Nocte/Plan/PlanIntent.hpp"
#include "libslic3r/Nocte/Plan/Scores.hpp"
#include "libslic3r/Nocte/Plan/OrientEngine.hpp"

using namespace Slic3r;
using namespace Slic3r::Nocte;

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

PartInvariants precompute_mesh(const indexed_triangle_set &its)
{
    const PrecomputeParams params;
    return precompute(its, params);
}

// The same fixture as tests/libslic3r/test_nocte_plan_scores.cpp, kept identical so the numbers
// carried over from there — stability 0.176 upright, 3.79 turned over, 576 mm^3 of support upright —
// mean the same thing here. A 20 x 20 x 4 cap on a square stem of side `stem_side_mm` and height
// `stem_height_mm`, the cap centred over the stem.
indexed_triangle_set cap_on_a_stem(double stem_height_mm, double stem_side_mm)
{
    indexed_triangle_set mesh   = its_make_cube(stem_side_mm, stem_side_mm, stem_height_mm);
    indexed_triangle_set cap    = its_make_cube(20., 20., 4.);
    const float          inset  = static_cast<float>(0.5 * (stem_side_mm - 20.));
    its_translate(cap, Vec3f(inset, inset, static_cast<float>(stem_height_mm)));
    its_merge(mesh, cap);
    return mesh;
}

// Every facet wound inward: an ordinary broken STL. The surface area, the bounding box and the
// centroid are all unchanged, so the part looks measurable, but every self-occlusion ray starts
// inside the surface and the whole part reports invisible — so `cusp_measured` and
// `contact_measured` are both false while every number looks perfect. Proven in
// test_nocte_plan_scores.cpp; used here to pin WHICH rejection reason the engine reports.
indexed_triangle_set inverted_cube(double side_mm)
{
    indexed_triangle_set mesh = its_make_cube(side_mm, side_mm, side_mm);
    for (stl_triangle_vertex_indices &f : mesh.indices)
        std::swap(f[0], f[1]);
    return mesh;
}

// A signage plate: 100 long, 60 wide, 3 thick, spanning [0,100] x [0,60] x [0,3] because
// its_make_cube() puts the box in the first octant. The three dimensions are deliberately far apart,
// because that is what makes "which face is up" a decision with a right answer:
//   +Z up  -> footprint 100 x 60, d_min = 30 mm, z_com = 1.5 mm, stability = 30 / 1.5 = 20
//   +X up  -> footprint  60 x  3, d_min = 1.5 mm, z_com = 50 mm, stability = 1.5 / 50 = 0.03
// The first sails past the 0.35 tipping floor by a factor of 57; the second misses it by a factor
// of 12.
indexed_triangle_set signage_plate()
{
    return its_make_cube(100., 60., 3.);
}

// `rotation` maps object coordinates to build coordinates, so this is where a candidate sends an
// object direction.
Vec3d map_dir(const Transform3d &rotation, const Vec3d &dir_obj)
{
    return Vec3d(rotation.linear() * dir_obj);
}

// The identity, stated on the three basis vectors rather than on the matrix, so a failure names the
// axis that moved.
bool is_identity_rotation(const Transform3d &rotation)
{
    const Vec3d ex = map_dir(rotation, Vec3d::UnitX());
    const Vec3d ey = map_dir(rotation, Vec3d::UnitY());
    const Vec3d ez = map_dir(rotation, Vec3d::UnitZ());
    return (ex - Vec3d::UnitX()).norm() < 1e-9 && (ey - Vec3d::UnitY()).norm() < 1e-9 &&
           (ez - Vec3d::UnitZ()).norm() < 1e-9;
}

// Any candidate that leaves the object's Z axis along the build Z axis — i.e. the part standing the
// way it was modelled, or stood on its head. Found by the ROTATION rather than by
// `CandidateSource::Current`, because a hull facet can produce the very same orientation and which
// of the two survives a merge is not a documented contract.
const OrientCandidate *find_z_aligned(const std::vector<OrientCandidate> &candidates)
{
    for (const OrientCandidate &c : candidates)
        if (std::abs(map_dir(c.rotation, Vec3d::UnitZ()).z()) > 0.999)
            return &c;
    return nullptr;
}

// The accepted list first, then the rejected one. Every candidate is SCORED before the filter runs,
// so its numbers are readable either way, and which side of a constraint a given orientation lands
// on is exactly what a test about ranking should not have to assume.
const OrientCandidate *find_z_aligned_anywhere(const OrientResult &result)
{
    const OrientCandidate *found = find_z_aligned(result.best);
    if (found != nullptr)
        return found;
    return find_z_aligned(result.rejected);
}

// Upright specifically: object +Z still pointing at build +Z.
const OrientCandidate *find_upright(const std::vector<OrientCandidate> &candidates)
{
    for (const OrientCandidate &c : candidates)
        if (map_dir(c.rotation, Vec3d::UnitZ()).z() > 0.999)
            return &c;
    return nullptr;
}

bool never_cancel()
{
    return false;
}

PlanIntent make_intent(PartIntent kind)
{
    PlanIntent intent;
    intent.kind = kind;
    return intent;
}

const PartIntent all_intents[5] = {
    PartIntent::Unspecified,
    PartIntent::Ornament,
    PartIntent::FunctionalStrength,
    PartIntent::FunctionalVisual,
    PartIntent::Draft,
};

// The largest of the five weights, so "this intent's preference is mostly X" can be said as a
// comparison rather than as a number the header never fixes.
double max_weight(const ScoreWeights &w)
{
    double m = w.support;
    if (w.cusp > m)      m = w.cusp;
    if (w.time > m)      m = w.time;
    if (w.strength > m)  m = w.strength;
    if (w.stability > m) m = w.stability;
    return m;
}

double min_weight(const ScoreWeights &w)
{
    double m = w.support;
    if (w.cusp < m)      m = w.cusp;
    if (w.time < m)      m = w.time;
    if (w.strength < m)  m = w.strength;
    if (w.stability < m) m = w.stability;
    return m;
}

std::string to_upper(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

// --- the four orientation decisions ------------------------------------------------------------

TEST_CASE("nocte plan: a signage plate is laid flat with its read face up", "[NoctePlan]")
{
    // 100 x 60 x 3, FunctionalVisual, showcase normal +Z — the face that is already up.
    //
    // The accepted winner must keep it there. Working the candidates through by hand:
    //   object +Z up    (the current orientation, and every quarter turn about build Z with it):
    //                   showcase up, stability 30 / 1.5 = 20, no downward surface above the bed, so
    //                   no support and no cusp — every vertical wall has |n.z| = 0 and both
    //                   horizontal faces are dropped by flat_exclude_deg. ACCEPTED.
    //   object -Z up    (the plate flipped over): the read face now points at the bed.
    //                   ShowcaseNotUp.
    //   object +-X up   footprint 60 x 3, d_min 1.5 mm, z_com 50 mm -> 0.03, and ShowcaseNotUp
    //                   anyway.
    //   object +-Y up   footprint 100 x 3, d_min 1.5 mm, z_com 30 mm -> 0.05, ShowcaseNotUp anyway.
    // So the surviving set is exactly the orientations with object +Z up, and the winner's rotation
    // must map (0,0,1) to within showcase_tol_deg of build +Z.
    const indexed_triangle_set plate = signage_plate();
    const PartInvariants       inv   = precompute_mesh(plate);
    REQUIRE(inv.valid);
    // 100 * 60 * 3 = 18000 mm^3 — the fixture really is the plate the arithmetic above assumes.
    REQUIRE_THAT(inv.volume, WithinRel(18000., 1e-4));

    PlanIntent intent = make_intent(PartIntent::FunctionalVisual);
    intent.showcase_normal_obj = Vec3d::UnitZ();
    REQUIRE(intent.missing_input() == nullptr);

    const OrientParams  params;
    const OrientResult  result = plan_orientation(plate, inv, intent, params, never_cancel);

    REQUIRE(result.ok);
    REQUIRE_FALSE(result.cancelled);
    REQUIRE_FALSE(result.degenerate);
    REQUIRE_FALSE(result.best.empty());
    REQUIRE(result.considered > static_cast<size_t>(0));

    const OrientCandidate &winner = result.best[0];
    REQUIRE(winner.accepted());
    REQUIRE(winner.rejected == RejectReason::None);

    // THE ASSERTION THAT MATTERS: the rotation, not the score. showcase_tol_deg is 5 degrees by
    // default, and cos(5 deg) = 0.996195, so the showcase normal's image must have a Z component at
    // or above that. A plate stood on any edge gives ~0 here and a plate flipped over gives ~-1.
    const Vec3d showcase_up = map_dir(winner.rotation, intent.showcase_normal_obj);
    REQUIRE_THAT(showcase_up.norm(), WithinAbs(1., 1e-9));
    REQUIRE(showcase_up.z() >= std::cos(5. * PI / 180.));

    // And it is the flat-lying orientation as measured, not merely as rotated: 3 mm tall, the whole
    // 100 x 60 = 6000 mm^2 in contact, nothing overhanging, and the read face excluded from the cusp
    // average as flat so the surviving walls are all vertical and report exactly 0.
    REQUIRE_THAT(winner.scores.height_mm, WithinRel(3., 1e-6));
    REQUIRE_THAT(winner.scores.footprint_area_mm2, WithinRel(6000., 1e-3));
    REQUIRE_THAT(winner.scores.support_volume_mm3, WithinAbs(0., 1e-6));
    REQUIRE_THAT(winner.scores.cusp_mean_mm, WithinAbs(0., 1e-9));
    // The zeros above are FINDINGS, not failures to measure: zero is the BEST value on both terms,
    // so only the flags separate "perfectly smooth" from "never measured".
    REQUIRE(winner.scores.cusp_measured);
    REQUIRE(winner.scores.support_measured);
    REQUIRE(winner.scores.measured);
    // 30 / 1.5 = 20.
    REQUIRE_THAT(winner.scores.stability, WithinRel(20., 0.02));

    // The flipped-over orientation was considered and refused for the RIGHT reason — it is perfectly
    // printable, it just shows the wrong face. Without this the test cannot tell a constraint from a
    // candidate that was never generated.
    bool saw_showcase_not_up = false;
    for (const OrientCandidate &c : result.rejected)
        if (c.rejected == RejectReason::ShowcaseNotUp)
            saw_showcase_not_up = true;
    REQUIRE(saw_showcase_not_up);
}

TEST_CASE("nocte plan: a plate is not stood on its edge to show a face", "[NoctePlan]")
{
    // The same 100 x 60 x 3 plate, but the read face is now the narrow 60 x 3 edge, normal +X.
    //
    // "Showcase up" and "stability >= 0.35" cannot both hold, and this is the case that proves hard
    // constraints are FILTERS rather than penalties. Any candidate that puts object +X up stands the
    // plate on a 60 x 3 = 180 mm^2 edge:
    //   d_min = min(60/2, 3/2) = 1.5 mm, z_com = 100/2 = 50 mm, stability = 1.5 / 50 = 0.03,
    // a twelfth of the 0.35 floor. Every other candidate leaves +X pointing sideways or down and
    // fails require_showcase_up. The intersection of the two constraints is EMPTY, so the filter
    // empties the set and the engine must fall back to the current orientation and name the
    // constraint that did it.
    //
    // A weighted penalty instead of a filter would have produced a confident answer here — the
    // plate stood on its edge, scoring badly but winning — which is a part that falls over.
    const indexed_triangle_set plate = signage_plate();
    const PartInvariants       inv   = precompute_mesh(plate);
    REQUIRE(inv.valid);

    PlanIntent intent = make_intent(PartIntent::FunctionalVisual);
    intent.showcase_normal_obj = Vec3d::UnitX();
    REQUIRE(intent.missing_input() == nullptr);

    const OrientParams params;
    const OrientResult result = plan_orientation(plate, inv, intent, params, never_cancel);

    // The engine RAN. It did not fail, and it was not cancelled — it answered "no orientation
    // satisfies this", which is a different statement and the one the user is owed.
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.considered > static_cast<size_t>(0));

    REQUIRE(result.degenerate);
    REQUIRE(result.blocking_reason != RejectReason::None);
    REQUIRE((result.blocking_reason == RejectReason::Unstable ||
             result.blocking_reason == RejectReason::ShowcaseNotUp));

    // The documented fallback: `best` holds only the current orientation.
    REQUIRE(result.best.size() == static_cast<size_t>(1));
    REQUIRE(result.best[0].source == CandidateSource::Current);
    REQUIRE(is_identity_rotation(result.best[0].rotation));

    // Nothing survived, so every candidate is accounted for in `rejected`, and the stood-on-edge one
    // is among them with a reason rather than silently missing.
    REQUIRE_FALSE(result.rejected.empty());
    bool saw_showcase_up_candidate = false;
    for (const OrientCandidate &c : result.rejected) {
        REQUIRE(c.rejected != RejectReason::None);
        if (map_dir(c.rotation, Vec3d::UnitX()).z() > 0.999)
            saw_showcase_up_candidate = true;
    }
    REQUIRE(saw_showcase_up_candidate);
}

TEST_CASE("nocte plan: a top-heavy part is turned over", "[NoctePlan]")
{
    // cap_on_a_stem(10, 4): a 20 x 20 x 4 cap on a 4 x 4 x 10 stem, Unspecified intent.
    //
    // Upright, from test_nocte_plan_scores.cpp's own arithmetic:
    //   volume  = 4*4*10 + 20*20*4 = 1760 mm^3, z_com = 20000 / 1760 = 11.3636 mm
    //   contact = the 4 x 4 stem base = 16 mm^2, so d_min = 2 mm
    //   stability = 2 / 11.3636 = 0.176, BELOW the 0.35 floor -> rejected as Unstable
    //   support   = 0.15 * (400 - 16) mm^2 * 10 mm = 0.15 * 3840 = 576 mm^3
    // Turned over (the cap on the bed):
    //   contact = the cap's 20 x 20 face = 400 mm^2, d_min = 10 mm, z_com = 14 - 11.3636 = 2.6364 mm
    //   stability = 10 / 2.6364 = 3.79, and the overhang is gone entirely.
    //
    // Lying on a side is not a way out either: Rx(90) puts the cap's 20 x 4 face on the bed with
    // z_com = 10 mm and d_min = 1.36 mm (the centre of mass sits 1.36 mm from the near edge of that
    // 4 mm deep footprint), giving 0.136 — also below the floor. So the only stable orientation is
    // cap down, and that is what the engine has to return.
    const indexed_triangle_set part = cap_on_a_stem(10., 4.);
    const PartInvariants       inv  = precompute_mesh(part);
    REQUIRE(inv.valid);
    REQUIRE_THAT(inv.volume, WithinRel(1760., 1e-4));

    const PlanIntent   intent = make_intent(PartIntent::Unspecified);
    const OrientParams params;
    const OrientResult result = plan_orientation(part, inv, intent, params, never_cancel);

    REQUIRE(result.ok);
    REQUIRE_FALSE(result.degenerate);
    REQUIRE_FALSE(result.best.empty());

    const OrientCandidate &winner = result.best[0];
    REQUIRE(winner.accepted());
    REQUIRE(winner.scores.measured);

    // It cleared the tipping floor, and by a wide margin rather than by a hair: 3.79 / 0.35 = 10.8.
    REQUIRE(winner.scores.stability > 0.35);
    REQUIRE(winner.scores.stability > 2.);
    // And the 576 mm^3 of support is gone. The sibling file measures the turned-over part at under
    // 50 mm^3; 100 leaves room for which layer the contact is taken at and still excludes 576 by
    // nearly a factor of six.
    REQUIRE(winner.scores.support_measured);
    REQUIRE(winner.scores.support_volume_mm3 < 100.);
    // Said as the orientation itself: the cap is on the bed, so object +Z now points at build -Z.
    REQUIRE(map_dir(winner.rotation, Vec3d::UnitZ()).z() < -0.9);
    // 400 mm^2 of cap on the bed, not the 16 mm^2 stem base.
    REQUIRE_THAT(winner.scores.footprint_area_mm2, WithinRel(400., 1e-2));
    // The height is the one thing the flip does not change: 10 + 4 either way.
    REQUIRE_THAT(winner.scores.height_mm, WithinRel(14., 1e-6));

    // The upright orientation is in `rejected`, named, with the reason the numbers give: it tips.
    // This is the half that makes the win meaningful — it was considered and beaten on a constraint,
    // not quietly left out of the candidate set.
    const OrientCandidate *upright = find_upright(result.rejected);
    REQUIRE(upright != nullptr);
    REQUIRE(upright->rejected == RejectReason::Unstable);
    // 2 / 11.3636 = 0.176.
    REQUIRE_THAT(upright->scores.stability, WithinRel(0.176, 0.05));
    REQUIRE(upright->scores.stability < 0.35);
    // 0.15 * 384 * 10 = 576 mm^3, the band the sibling file pins it in.
    REQUIRE(upright->scores.support_volume_mm3 > 250.);
    REQUIRE(winner.scores.support_volume_mm3 < 0.5 * upright->scores.support_volume_mm3);
}

TEST_CASE("nocte plan: a cube is left exactly where it is", "[NoctePlan]")
{
    // A 20 mm cube is the same part whichever face it stands on: support 0, cusp 0, height 20,
    // stability 10 / 10 = 1, and the same layer count, on every one of the seven candidates
    // (the current orientation plus the six hull faces). Every term ties, so the whole decision
    // falls to the tie-break chain, and its last documented rule is the one under test:
    // "A part is never flipped for no gain."
    //
    // Nothing else in this file can catch an engine that returns an arbitrary member of a tied set.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const PartInvariants       inv  = precompute_mesh(cube);
    REQUIRE(inv.valid);

    const PlanIntent   intent = make_intent(PartIntent::Unspecified);
    const OrientParams params;
    const OrientResult result = plan_orientation(cube, inv, intent, params, never_cancel);

    REQUIRE(result.ok);
    REQUIRE_FALSE(result.degenerate);
    REQUIRE_FALSE(result.best.empty());

    const OrientCandidate &winner = result.best[0];
    REQUIRE(winner.accepted());
    REQUIRE(winner.source == CandidateSource::Current);
    REQUIRE(is_identity_rotation(winner.rotation));

    // The tie really is a tie, measured: this is the cube's own arithmetic from
    // test_nocte_plan_scores.cpp, so the identity did not win by being the only thing scored.
    REQUIRE(winner.scores.measured);
    REQUIRE_THAT(winner.scores.stability, WithinAbs(1., 1e-6));
    REQUIRE_THAT(winner.scores.footprint_area_mm2, WithinRel(400., 1e-3));
    REQUIRE_THAT(winner.scores.height_mm, WithinRel(20., 1e-6));
    REQUIRE_THAT(winner.scores.support_volume_mm3, WithinAbs(0., 1e-6));
    REQUIRE_THAT(winner.scores.cusp_mean_mm, WithinAbs(0., 1e-9));
    // Six faces to rest on plus the identity: more than one candidate was in the running, and every
    // one of them passed the filter.
    REQUIRE(result.considered > static_cast<size_t>(1));
    REQUIRE(result.rejected.empty());
}

// --- cancellation ------------------------------------------------------------------------------

TEST_CASE("nocte plan: the cancellation predicate is actually read", "[NoctePlan]")
{
    // Orca's Orient.cpp accepts a `stopcond_` and never reads it (Orient.cpp:84), so a long plan
    // there cannot be stopped at all. `ok == false` is the struct's own initialiser, so on its own
    // it is satisfied by a function that does nothing; each section below pairs it with something
    // that can only be true if the engine ran and consulted the predicate.
    const indexed_triangle_set part = cap_on_a_stem(10., 4.);
    const PartInvariants       inv  = precompute_mesh(part);
    REQUIRE(inv.valid);

    const PlanIntent   intent = make_intent(PartIntent::Unspecified);
    const OrientParams params;

    SECTION("cancelled before the first candidate") {
        int  calls = 0;
        auto cancel_now = [&calls]() {
            ++calls;
            return true;
        };
        const OrientResult result = plan_orientation(part, inv, intent, params, cancel_now);

        REQUIRE_FALSE(result.ok);
        REQUIRE(result.cancelled);
        // THE COMPANION. Without this line the two assertions above are satisfied by a body of
        // `return OrientResult{};` — they are both the initialiser. The predicate having been called
        // at all is the thing that cannot be faked.
        REQUIRE(calls > 0);
    }

    SECTION("cancelled part way through") {
        int  calls = 0;
        auto cancel_later = [&calls]() {
            ++calls;
            return calls > 3;
        };
        const OrientResult result = plan_orientation(part, inv, intent, params, cancel_later);

        REQUIRE_FALSE(result.ok);
        REQUIRE(result.cancelled);
        // It kept asking. Three "keep going" answers were honoured and the fourth was obeyed, so the
        // predicate is consulted repeatedly rather than once at the top.
        REQUIRE(calls > 3);
    }

    SECTION("the control: the same call completes when nothing cancels it") {
        // The section that gives the two above their meaning. Identical mesh, identical intent,
        // identical params; only the predicate changes, and now the engine must succeed.
        int  calls = 0;
        auto never = [&calls]() {
            ++calls;
            return false;
        };
        const OrientResult result = plan_orientation(part, inv, intent, params, never);

        REQUIRE(result.ok);
        REQUIRE_FALSE(result.cancelled);
        REQUIRE_FALSE(result.best.empty());
        REQUIRE(result.considered > static_cast<size_t>(0));
        REQUIRE(calls > 0);
    }
}

// --- candidate generation, merging and truncation ----------------------------------------------

TEST_CASE("nocte plan: nearly parallel hull facets are merged into one candidate", "[NoctePlan]")
{
    // A 36-sided cylinder. its_make_cylinder(r, h, 2*PI/36) builds 36 sectors, so the hull carries
    // 36 side planes whose normals are 10 degrees apart, plus the two caps: 38 distinct directions
    // out of roughly 140 hull TRIANGLES (72 on the side, the rest in the two cap fans). Scoring one
    // candidate per hull facet would re-measure the same orientation dozens of times, which is the
    // cost `angular_merge_deg` exists to avoid.
    //
    // max_candidates is raised to 256 in both sections so that the count below is the work of the
    // MERGE and never of the truncation — the default 64 would cap the first section by itself and
    // the comparison would prove nothing.
    const indexed_triangle_set drum = its_make_cylinder(10., 20., 2. * PI / 36.);
    const PartInvariants       inv  = precompute_mesh(drum);
    REQUIRE(inv.valid);
    // 36 sectors x 2 triangles on the side, plus two cap fans: well over a hundred facets in, and
    // the merged candidate counts below are what comes out.
    REQUIRE(drum.indices.size() > static_cast<size_t>(100));

    const PlanIntent intent = make_intent(PartIntent::Unspecified);

    OrientParams fine;
    fine.angular_merge_deg = 5.;
    fine.max_candidates    = 256;
    const OrientResult fine_result = plan_orientation(drum, inv, intent, fine, never_cancel);
    REQUIRE(fine_result.ok);

    // At 5 degrees NOTHING on the side merges: the neighbours are 10 degrees apart, twice the
    // threshold. So the ceiling is 36 side normals + 2 caps + the 6 axis-aligned quarter turns = 44,
    // and 48 leaves headroom for whether the axis-aligned set is deduplicated against the hull.
    REQUIRE(fine_result.considered <= static_cast<size_t>(48));
    // And it did not collapse to a handful either — the side normals ARE distinct candidates here.
    REQUIRE(fine_result.considered >= static_cast<size_t>(12));

    OrientParams coarse;
    coarse.angular_merge_deg = 15.;
    coarse.max_candidates    = 256;
    const OrientResult coarse_result = plan_orientation(drum, inv, intent, coarse, never_cancel);
    REQUIRE(coarse_result.ok);

    // At 15 degrees the 10-degree neighbours DO merge. Retained normals are pairwise more than 15
    // degrees apart, so at most 360 / 15 = 24 of them survive around the side, plus 2 caps and at
    // most the 6 axis-aligned: 32.
    REQUIRE(coarse_result.considered <= static_cast<size_t>(32));
    // THE ASSERTION THAT PROVES THE PARAMETER IS READ AT ALL. Same mesh, same intent, same budget;
    // only the merge angle moved, and the candidate count has to move with it.
    REQUIRE(coarse_result.considered < fine_result.considered);
}

TEST_CASE("nocte plan: max_candidates and max_results are both honoured", "[NoctePlan]")
{
    const PlanIntent intent = make_intent(PartIntent::Unspecified);

    SECTION("the candidate budget truncates the set before scoring") {
        // The same 36-sided cylinder, whose unmerged set is 38 directions — far more than the budget
        // of 5 below, so the truncation is what governs and not the mesh.
        const indexed_triangle_set drum = its_make_cylinder(10., 20., 2. * PI / 36.);
        const PartInvariants       inv  = precompute_mesh(drum);
        REQUIRE(inv.valid);

        OrientParams params;
        params.angular_merge_deg = 5.;
        params.max_candidates    = 5;
        const OrientResult result = plan_orientation(drum, inv, intent, params, never_cancel);

        REQUIRE(result.ok);
        REQUIRE(result.considered <= static_cast<size_t>(5));
        // It still did the work: a budget of 5 is a truncation, not an abort.
        REQUIRE(result.considered > static_cast<size_t>(0));
        REQUIRE_FALSE(result.best.empty());
    }

    SECTION("the result list is capped independently of the candidate budget") {
        // A 20 mm cube: all seven candidates pass the filter (stability 1 on every face), so the
        // accepted set is larger than any cap asked for below and `best.size()` is decided by
        // max_results alone.
        const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        const PartInvariants       inv  = precompute_mesh(cube);
        REQUIRE(inv.valid);

        OrientParams one;
        one.max_results = 1;
        const OrientResult one_result = plan_orientation(cube, inv, intent, one, never_cancel);
        REQUIRE(one_result.ok);
        REQUIRE(one_result.best.size() == static_cast<size_t>(1));

        OrientParams two;
        two.max_results = 2;
        const OrientResult two_result = plan_orientation(cube, inv, intent, two, never_cancel);
        REQUIRE(two_result.ok);
        REQUIRE(two_result.best.size() == static_cast<size_t>(2));

        // The default is 3, and the cube has more than three accepted candidates, so the default
        // binds as well.
        const OrientParams def;
        REQUIRE(def.max_results == static_cast<size_t>(3));
        const OrientResult def_result = plan_orientation(cube, inv, intent, def, never_cancel);
        REQUIRE(def_result.ok);
        REQUIRE(def_result.best.size() == static_cast<size_t>(3));
        // The candidates that did not make the list are still counted, not forgotten.
        REQUIRE(def_result.considered > def_result.best.size());
    }
}

// --- the filter order, and unusable geometry ----------------------------------------------------

TEST_CASE("nocte plan: an unmeasurable part is rejected as unmeasured, not as unstable", "[NoctePlan]")
{
    // The documented ranking drops un-measured candidates at step 2 and constraint failures at step
    // 3, IN THAT ORDER, and this is the only test that can tell the two apart. An inverted cube has
    // a correct 2400 mm^2 of surface area, a correct (10,10,10) centroid and a bounding box that
    // says 20 mm on every side, so it passes every gate that looks at those — but every outward
    // normal points into the solid, every self-occlusion ray starts inside the surface, and both
    // `cusp_measured` and `contact_measured` come back false (proven in test_nocte_plan_scores.cpp).
    //
    // Run the filter the other way round and the part still gets rejected, so the ANSWER is the
    // same, but the REASON handed to the user is a constraint it never actually failed. A misleading
    // reason is worse than a wrong answer here, because the reason is the product.
    const indexed_triangle_set bad = inverted_cube(20.);
    const PartInvariants       inv = precompute_mesh(bad);

    // It really does look measurable.
    REQUIRE(inv.valid);
    REQUIRE_THAT(inv.surface_area, WithinRel(2400., 1e-4));
    REQUIRE(inv.visibility_available);

    const PlanIntent   intent = make_intent(PartIntent::Unspecified);
    const OrientParams params;
    const OrientResult result = plan_orientation(bad, inv, intent, params, never_cancel);

    // The engine ran and produced a verdict; it did not fall over.
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.considered > static_cast<size_t>(0));

    REQUIRE(result.degenerate);
    REQUIRE(result.blocking_reason == RejectReason::NotMeasured);

    // Every single candidate went out at step 2, and none of them at step 3. Unstable here would be
    // the giveaway: the inverted cube's stability is whatever the first-layer slice makes of a
    // reversed winding, and that number must never be what decides its fate.
    REQUIRE_FALSE(result.rejected.empty());
    for (const OrientCandidate &c : result.rejected)
        REQUIRE(c.rejected == RejectReason::NotMeasured);

    // And the fallback is still the current orientation, as for any degenerate set.
    REQUIRE(result.best.size() == static_cast<size_t>(1));
    REQUIRE(result.best[0].source == CandidateSource::Current);
}

TEST_CASE("nocte plan: an unusable mesh yields a failed plan, not an exception", "[NoctePlan]")
{
    const PlanIntent   intent = make_intent(PartIntent::Unspecified);
    const OrientParams params;

    SECTION("an empty mesh") {
        const indexed_triangle_set empty;
        const PartInvariants       inv = precompute_mesh(empty);
        REQUIRE_FALSE(inv.valid);

        OrientResult result;
        REQUIRE_NOTHROW(result = plan_orientation(empty, inv, intent, params, never_cancel));
        REQUIRE_FALSE(result.ok);
        // Not a cancellation: the caller has to be able to tell "I stopped it" from "it could not
        // run", and both leave `ok` false.
        REQUIRE_FALSE(result.cancelled);
        REQUIRE(result.best.empty());
    }

    SECTION("a single zero-area triangle") {
        // Three collinear vertices: the cross product is the zero vector, so the facet has area 0
        // and a zero normal, and there is no solid to rest on any face of.
        indexed_triangle_set sliver;
        sliver.vertices.push_back(stl_vertex(0.f, 0.f, 0.f));
        sliver.vertices.push_back(stl_vertex(10.f, 0.f, 0.f));
        sliver.vertices.push_back(stl_vertex(20.f, 0.f, 0.f));
        sliver.indices.push_back(stl_triangle_vertex_indices(0, 1, 2));

        const PartInvariants inv = precompute_mesh(sliver);
        REQUIRE_FALSE(inv.valid);

        OrientResult result;
        REQUIRE_NOTHROW(result = plan_orientation(sliver, inv, intent, params, never_cancel));
        REQUIRE_FALSE(result.ok);
        REQUIRE_FALSE(result.cancelled);
    }

    SECTION("the control: a usable mesh through the identical call") {
        // `ok == false` is the struct's initialiser, so the two sections above are satisfied by a
        // function that returns a default-constructed result for everything. This is the section
        // that is not.
        const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        const PartInvariants       inv  = precompute_mesh(cube);
        REQUIRE(inv.valid);

        const OrientResult result = plan_orientation(cube, inv, intent, params, never_cancel);
        REQUIRE(result.ok);
        REQUIRE_FALSE(result.best.empty());
    }
}

// --- FunctionalStrength --------------------------------------------------------------------------

TEST_CASE("nocte plan: a loaded part is laid so the load runs along the layers", "[NoctePlan]")
{
    // A 20 x 20 x 40 box, loaded along OBJECT +Z.
    //
    // The minimum section normal to +Z is the 20 x 20 square, 400 mm^2, wherever the plane is cut,
    // and it is a property of the part and the load direction — not of the candidate — so it is the
    // same 400 for every orientation. What the orientation changes is which material property
    // carries it, because the layers are horizontal in BUILD coordinates:
    //   upright, object +Z -> build +Z:  ACROSS the layers, sigma = k * sigma_xy = 0.5 * 50 = 25 MPa
    //                                    F = 400 * 25 = 10000 N
    //   on its side, object +Z -> build horizontal: ALONG the layers, sigma = sigma_xy = 50 MPa
    //                                    F = 400 * 50 = 20000 N
    // Exactly a factor of anisotropy_k apart, and the sign is unambiguous: laying the part down so
    // the load never has to cross a layer bond DOUBLES the force it survives.
    //
    // The side-lying candidate also dominates on every other term — 20 mm tall instead of 40 so
    // fewer layers and less time, stability 10/10 = 1 instead of 10/20 = 0.5, and support and cusp
    // both 0 either way — so it wins under ANY weight table, which is what keeps this test from
    // depending on numbers the header never fixes. Upright is not rejected: 0.5 clears the 0.35
    // floor, so both are in the accepted set and the comparison is a genuine ranking, not a filter.
    const indexed_triangle_set box = its_make_cube(20., 20., 40.);
    const PartInvariants       inv = precompute_mesh(box);
    REQUIRE(inv.valid);
    // 20 * 20 * 40 = 16000 mm^3.
    REQUIRE_THAT(inv.volume, WithinRel(16000., 1e-4));

    SECTION("with no load direction the intent says what is missing") {
        const PlanIntent intent = make_intent(PartIntent::FunctionalStrength);
        REQUIRE_THAT(intent.load_dir_obj.norm(), WithinAbs(0., 1e-12));

        const char *missing = intent.missing_input();
        REQUIRE(missing != nullptr);
        // It names the load direction rather than shrugging: a default here would answer a question
        // the user was asked precisely because we cannot answer it. The string is fixed by
        // PlanIntent.hpp:60-62 because the CLI refuses to plan on it and names the option to supply.
        REQUIRE(std::string(missing) == "load direction");
    }

    SECTION("with a load direction the plan puts it across the grain") {
        PlanIntent intent = make_intent(PartIntent::FunctionalStrength);
        intent.load_dir_obj = Vec3d::UnitZ();
        REQUIRE(intent.missing_input() == nullptr);

        OrientParams params;
        // Six candidates survive the merge and the upright one is the worst of them, so the default
        // cap of 3 would push it off the end of the list, and this test needs to read its numbers.
        params.max_results = 8;

        const OrientResult result = plan_orientation(box, inv, intent, params, never_cancel);
        REQUIRE(result.ok);
        REQUIRE_FALSE(result.degenerate);
        REQUIRE(result.best.size() >= static_cast<size_t>(2));

        const OrientCandidate &winner = result.best[0];
        REQUIRE(winner.accepted());
        REQUIRE(winner.scores.measured);

        // The section the engine swept for itself, not one handed in: 20 x 20.
        REQUIRE_THAT(winner.scores.min_section_area_mm2, WithinRel(400., 5e-3));
        // 400 mm^2 * 50 MPa = 20000 N. The load now lies in the layer plane.
        REQUIRE_THAT(winner.scores.failure_force_n, WithinRel(20000., 5e-3));
        // Said as the orientation: object +Z ends up horizontal in the build frame.
        REQUIRE_THAT(map_dir(winner.rotation, Vec3d::UnitZ()).z(), WithinAbs(0., 1e-6));

        // The upright orientation, for the contrast that gives the number its meaning. Found by its
        // rotation rather than by its source label, because a hull facet produces the same
        // orientation as the identity and which of the two survives a merge is not a contract; and
        // looked for in the rejected list too, because whether 10/20 = 0.5 clears THIS intent's
        // tipping floor is a number the header does not fix.
        const OrientCandidate *standing = find_z_aligned_anywhere(result);
        REQUIRE(standing != nullptr);
        // 400 mm^2 * 0.5 * 50 MPa = 10000 N: the same section, carried by the interlayer bond.
        REQUIRE_THAT(standing->scores.failure_force_n, WithinRel(10000., 5e-3));
        REQUIRE(winner.scores.failure_force_n > standing->scores.failure_force_n);
        // And the ratio is exactly the anisotropy: 20000 / 10000 = 2 = 1 / 0.5.
        REQUIRE_THAT(winner.scores.failure_force_n,
                     WithinRel(standing->scores.failure_force_n / params.scores.anisotropy_k, 1e-2));
        // The winner is also the shorter print, which is why it wins under any weights: 20 mm laid
        // down against 40 mm standing up.
        REQUIRE_THAT(winner.scores.height_mm, WithinRel(20., 1e-6));
        REQUIRE_THAT(standing->scores.height_mm, WithinRel(40., 1e-6));
    }
}

// --- PlanIntent ----------------------------------------------------------------------------------

TEST_CASE("nocte plan: the intent weight table is exactly what the header publishes", "[NoctePlan]")
{
    // The table lives in the CONTRACT (PlanIntent.hpp:73-78), not only in the .cpp, so this test can
    // pin all 25 numbers without reading the implementation it is testing:
    //
    //   Intent              support  cusp  time  strength  stability
    //   Unspecified          0.25    0.25  0.20    0.15      0.15
    //   Ornament             0.30    0.45  0.15    0.00      0.10
    //   FunctionalStrength   0.20    0.05  0.10    0.50      0.15
    //   FunctionalVisual     0.25    0.40  0.10    0.10      0.15
    //   Draft                0.30    0.00  0.60    0.00      0.10
    //
    // READ A FAILURE HERE AS A QUESTION, NOT AS A BUG REPORT. Every one of these numbers is a
    // PREFERENCE and none of them is fitted to anything: they are arguable by construction, and the
    // physics in Scores.hpp does not move when a row here does. So changing a row changes what an
    // intent MEANS, which is a product decision — and this test is the place it has to be taken
    // deliberately rather than slipped in. Updating the expected value is a legitimate way to fix a
    // red line, provided the header, docs/HLSD/nocte-planning-engine.md section 7 and this file move
    // in the same commit.
    SECTION("every row sums to one and holds no negative preference") {
        // A different property from the values themselves, and the one the header states as an
        // INVARIANT ("Always normalised, so they sum to 1") rather than as a choice — so it should
        // survive a future re-weighting that legitimately changes every number below.
        for (const PartIntent kind : all_intents) {
            const ScoreWeights w = weights_for(kind);
            REQUIRE_THAT(w.sum(), WithinAbs(1., 1e-12));
            // A preference, not a penalty: no negative weights, and no single term is the whole.
            REQUIRE(min_weight(w) >= 0.);
            REQUIRE(max_weight(w) <= 1.);
        }
    }

    SECTION("Unspecified") {
        // 0.25 + 0.25 + 0.20 + 0.15 + 0.15 = 1.00, and these are exactly the ScoreWeights defaults
        // (Scores.hpp:265-270) — which is what "balanced; no question asked of the user" has to
        // mean: the intent that asks nothing must not quietly prefer anything.
        const ScoreWeights w = weights_for(PartIntent::Unspecified);
        REQUIRE_THAT(w.support,   WithinAbs(0.25, 1e-12));
        REQUIRE_THAT(w.cusp,      WithinAbs(0.25, 1e-12));
        REQUIRE_THAT(w.time,      WithinAbs(0.20, 1e-12));
        REQUIRE_THAT(w.strength,  WithinAbs(0.15, 1e-12));
        REQUIRE_THAT(w.stability, WithinAbs(0.15, 1e-12));
    }

    SECTION("Ornament") {
        // 0.30 + 0.45 + 0.15 + 0.00 + 0.10 = 1.00. "Looked at, never loaded": cusp carries 0.45,
        // nearly half the preference, and the strength zero is deliberate — a zero weight drops the
        // term entirely (Scores.hpp:266-268), which is how "never loaded" is said in numbers.
        const ScoreWeights w = weights_for(PartIntent::Ornament);
        REQUIRE_THAT(w.support,   WithinAbs(0.30, 1e-12));
        REQUIRE_THAT(w.cusp,      WithinAbs(0.45, 1e-12));
        REQUIRE_THAT(w.time,      WithinAbs(0.15, 1e-12));
        REQUIRE_THAT(w.strength,  WithinAbs(0.00, 1e-12));
        REQUIRE_THAT(w.stability, WithinAbs(0.10, 1e-12));
    }

    SECTION("FunctionalStrength") {
        // 0.20 + 0.05 + 0.10 + 0.50 + 0.15 = 1.00. Half the whole preference on the one term the
        // user was asked a question to obtain, and the cusp down to 0.05: a part that carries a load
        // is allowed to look like a part that carries a load.
        const ScoreWeights w = weights_for(PartIntent::FunctionalStrength);
        REQUIRE_THAT(w.support,   WithinAbs(0.20, 1e-12));
        REQUIRE_THAT(w.cusp,      WithinAbs(0.05, 1e-12));
        REQUIRE_THAT(w.time,      WithinAbs(0.10, 1e-12));
        REQUIRE_THAT(w.strength,  WithinAbs(0.50, 1e-12));
        REQUIRE_THAT(w.stability, WithinAbs(0.15, 1e-12));
    }

    SECTION("FunctionalVisual") {
        // 0.25 + 0.40 + 0.10 + 0.10 + 0.15 = 1.00. "Signage: one face is read, and it must be
        // flawless." The showcase rules are hard FILTERS, so what is left for the weights to say is
        // that the staircase governs the ranking among the candidates that already show the face.
        const ScoreWeights w = weights_for(PartIntent::FunctionalVisual);
        REQUIRE_THAT(w.support,   WithinAbs(0.25, 1e-12));
        REQUIRE_THAT(w.cusp,      WithinAbs(0.40, 1e-12));
        REQUIRE_THAT(w.time,      WithinAbs(0.10, 1e-12));
        REQUIRE_THAT(w.strength,  WithinAbs(0.10, 1e-12));
        REQUIRE_THAT(w.stability, WithinAbs(0.15, 1e-12));
    }

    SECTION("Draft") {
        // 0.30 + 0.00 + 0.60 + 0.00 + 0.10 = 1.00. "A shape check, printed to be thrown away": time
        // takes 0.60 outright, and BOTH the cusp and the strength are zero, because nobody looks at
        // a draft's surface and nothing is ever hung off it.
        const ScoreWeights w = weights_for(PartIntent::Draft);
        REQUIRE_THAT(w.support,   WithinAbs(0.30, 1e-12));
        REQUIRE_THAT(w.cusp,      WithinAbs(0.00, 1e-12));
        REQUIRE_THAT(w.time,      WithinAbs(0.60, 1e-12));
        REQUIRE_THAT(w.strength,  WithinAbs(0.00, 1e-12));
        REQUIRE_THAT(w.stability, WithinAbs(0.10, 1e-12));
    }
}

TEST_CASE("nocte plan: the hard constraints follow from the intent", "[NoctePlan]")
{
    SECTION("the tipping floor applies to every intent") {
        for (const PartIntent kind : all_intents) {
            const PlanConstraints c = constraints_for(make_intent(kind));
            REQUIRE(c.min_stability > 0.);
            REQUIRE(c.min_footprint_mm2 >= 0.);
        }
        // The default, and the number the whole engine screens on: cap_on_a_stem's 0.176 has to fail
        // it and its turned-over 3.79 has to pass.
        const PlanConstraints c = constraints_for(make_intent(PartIntent::Unspecified));
        REQUIRE_THAT(c.min_stability, WithinAbs(0.35, 1e-12));
        REQUIRE_FALSE(c.require_load_section);
        REQUIRE_FALSE(c.require_showcase_up);
        REQUIRE_FALSE(c.require_showcase_clean);
    }

    SECTION("only FunctionalStrength requires a load section") {
        PlanIntent loaded = make_intent(PartIntent::FunctionalStrength);
        loaded.load_dir_obj = Vec3d::UnitZ();
        REQUIRE(constraints_for(loaded).require_load_section);
        REQUIRE_FALSE(constraints_for(make_intent(PartIntent::Ornament)).require_load_section);
        REQUIRE_FALSE(constraints_for(make_intent(PartIntent::Draft)).require_load_section);
    }

    SECTION("only FunctionalVisual requires the showcase face") {
        PlanIntent visual = make_intent(PartIntent::FunctionalVisual);
        visual.showcase_normal_obj = Vec3d::UnitZ();
        const PlanConstraints c = constraints_for(visual);
        REQUIRE(c.require_showcase_up);
        REQUIRE(c.require_showcase_clean);

        REQUIRE_FALSE(constraints_for(make_intent(PartIntent::Ornament)).require_showcase_up);
        REQUIRE_FALSE(constraints_for(make_intent(PartIntent::Unspecified)).require_showcase_clean);

        // `showcase_support_free` is the user's own switch — "whether support touching the showcase
        // face disqualifies a candidate outright" — so turning it off has to turn the clean
        // constraint off with it, while leaving "the face still has to point up" alone.
        visual.showcase_support_free = false;
        const PlanConstraints relaxed = constraints_for(visual);
        REQUIRE_FALSE(relaxed.require_showcase_clean);
        REQUIRE(relaxed.require_showcase_up);
    }
}

TEST_CASE("nocte plan: an intent reports the input it is missing", "[NoctePlan]")
{
    SECTION("the three intents that ask for nothing") {
        // "Zero means 'not given', which is not an error for every intent."
        REQUIRE(make_intent(PartIntent::Unspecified).missing_input() == nullptr);
        REQUIRE(make_intent(PartIntent::Ornament).missing_input() == nullptr);
        REQUIRE(make_intent(PartIntent::Draft).missing_input() == nullptr);
    }

    SECTION("FunctionalStrength wants a load direction") {
        // PlanIntent.hpp:60-62 fixes the two strings exactly, so they are pinned exactly: the CLI
        // prints them and names the option to supply, which makes a reworded string a user-visible
        // change rather than an internal one.
        PlanIntent intent = make_intent(PartIntent::FunctionalStrength);
        const char *missing = intent.missing_input();
        REQUIRE(missing != nullptr);
        REQUIRE(std::string(missing) == "load direction");

        intent.load_dir_obj = Vec3d::UnitZ();
        REQUIRE(intent.missing_input() == nullptr);
        // A showcase normal is not what it was missing, so supplying one instead must not silence it.
        PlanIntent wrong_input = make_intent(PartIntent::FunctionalStrength);
        wrong_input.showcase_normal_obj = Vec3d::UnitZ();
        REQUIRE(wrong_input.missing_input() != nullptr);
    }

    SECTION("FunctionalVisual wants a showcase normal") {
        PlanIntent intent = make_intent(PartIntent::FunctionalVisual);
        const char *missing = intent.missing_input();
        REQUIRE(missing != nullptr);
        REQUIRE(std::string(missing) == "showcase face");

        intent.showcase_normal_obj = Vec3d::UnitZ();
        REQUIRE(intent.missing_input() == nullptr);
        // And a load direction is the wrong answer to this question.
        PlanIntent wrong_input = make_intent(PartIntent::FunctionalVisual);
        wrong_input.load_dir_obj = Vec3d::UnitZ();
        REQUIRE(wrong_input.missing_input() != nullptr);
    }
}

TEST_CASE("nocte plan: an intent name round-trips and an unknown one is refused", "[NoctePlan]")
{
    // The spelling is persisted in nocte_report.json and parsed back from the CLI, so the pair has
    // to be an exact inverse — and PlanIntent.hpp:31-33 fixes the five spellings themselves, which
    // makes them pinnable rather than merely required to be distinct.
    SECTION("the five spellings are the ones the header names") {
        // Pinned as LITERALS, not only through the round-trip below. A round-trip is satisfied by
        // any pair of functions that agree with each other, including a pair that agrees on "fs" —
        // and the spelling is written into every nocte_report.json we have ever produced, so what it
        // is matters separately from whether it survives a round trip.
        REQUIRE(std::string(intent_name(PartIntent::Unspecified)) == "unspecified");
        REQUIRE(std::string(intent_name(PartIntent::Ornament)) == "ornament");
        REQUIRE(std::string(intent_name(PartIntent::FunctionalStrength)) == "functional-strength");
        REQUIRE(std::string(intent_name(PartIntent::FunctionalVisual)) == "functional-visual");
        REQUIRE(std::string(intent_name(PartIntent::Draft)) == "draft");

        // And the parser accepts those same literals, which is the other half of the persistence
        // contract: a report written today has to be readable tomorrow.
        PartIntent out = PartIntent::Unspecified;
        REQUIRE(parse_intent("functional-strength", out));
        REQUIRE(out == PartIntent::FunctionalStrength);
        REQUIRE(parse_intent("functional-visual", out));
        REQUIRE(out == PartIntent::FunctionalVisual);
        REQUIRE(parse_intent("draft", out));
        REQUIRE(out == PartIntent::Draft);
        REQUIRE(parse_intent("ornament", out));
        REQUIRE(out == PartIntent::Ornament);
        REQUIRE(parse_intent("unspecified", out));
        REQUIRE(out == PartIntent::Unspecified);
    }

    SECTION("every name round-trips") {
        for (const PartIntent kind : all_intents) {
            const char *name = intent_name(kind);
            REQUIRE(name != nullptr);
            REQUIRE(std::strlen(name) > static_cast<size_t>(0));

            // Seeded with an intent that is deliberately NOT `kind`, so `out == kind` afterwards is
            // a statement about parse_intent and not about the initialiser.
            PartIntent out = (kind == PartIntent::Draft) ? PartIntent::Unspecified : PartIntent::Draft;
            REQUIRE(parse_intent(std::string(name), out));
            REQUIRE(out == kind);
        }
    }

    // There is deliberately no "the five names are distinct" section. The literals are pinned above,
    // so distinctness follows from them and a loop asserting it could no longer fail.

    SECTION("parsing ignores case") {
        // Contract, not convenience: PlanIntent.hpp:31-33 says the five spellings are matched
        // CASE-INSENSITIVELY because they are typed on a command line. "Draft" and "draft" are the
        // same answer.
        for (const PartIntent kind : all_intents) {
            const std::string upper = to_upper(std::string(intent_name(kind)));
            PartIntent out = (kind == PartIntent::Draft) ? PartIntent::Unspecified : PartIntent::Draft;
            REQUIRE(parse_intent(upper, out));
            REQUIRE(out == kind);
        }
    }

    SECTION("an unknown name is refused and the output is untouched") {
        // PlanIntent.hpp:35-37: false, and `out` LEFT UNTOUCHED, rather than a fallback to
        // Unspecified. Seeded with FunctionalVisual precisely because it is not the enum's zero
        // value: a parse_intent that wrote Unspecified on failure — the "helpful" default the header
        // forbids, which would re-weight every candidate without saying so — fails these lines, and
        // that is the whole point of them.
        PartIntent out = PartIntent::FunctionalVisual;
        REQUIRE_FALSE(parse_intent("sculpture", out));
        REQUIRE(out == PartIntent::FunctionalVisual);

        REQUIRE_FALSE(parse_intent("", out));
        REQUIRE(out == PartIntent::FunctionalVisual);

        REQUIRE_FALSE(parse_intent("functional", out));
        REQUIRE(out == PartIntent::FunctionalVisual);
    }
}

// --- the names carried in the result --------------------------------------------------------------

TEST_CASE("nocte plan: every reason and every source has a name", "[NoctePlan]")
{
    // "The reason is the product": a panel or a CLI that renders a rejection has to have something
    // to print for every value the enum can take, including the ones no fixture in this file
    // produces.
    const RejectReason reasons[7] = {
        RejectReason::None,
        RejectReason::NotMeasured,
        RejectReason::Unstable,
        RejectReason::FootprintTooSmall,
        RejectReason::NoLoadSection,
        RejectReason::ShowcaseNotUp,
        RejectReason::ShowcaseSupported,
    };
    for (size_t i = 0; i < static_cast<size_t>(7); ++i) {
        const char *name = reject_reason_name(reasons[i]);
        REQUIRE(name != nullptr);
        REQUIRE(std::strlen(name) > static_cast<size_t>(0));
    }
    // Distinct, or the panel cannot tell an unstable candidate from an unmeasured one.
    for (size_t i = 0; i < static_cast<size_t>(7); ++i)
        for (size_t j = i + 1; j < static_cast<size_t>(7); ++j)
            REQUIRE(std::string(reject_reason_name(reasons[i])) !=
                    std::string(reject_reason_name(reasons[j])));

    const CandidateSource sources[3] = {
        CandidateSource::Current,
        CandidateSource::HullFace,
        CandidateSource::AxisAligned,
    };
    for (size_t i = 0; i < static_cast<size_t>(3); ++i) {
        const char *name = candidate_source_name(sources[i]);
        REQUIRE(name != nullptr);
        REQUIRE(std::strlen(name) > static_cast<size_t>(0));
    }
    // "Reported so a user can tell 'this is how it already sits' from 'this is a face it could rest
    // on'" — which needs the two to read differently.
    for (size_t i = 0; i < static_cast<size_t>(3); ++i)
        for (size_t j = i + 1; j < static_cast<size_t>(3); ++j)
            REQUIRE(std::string(candidate_source_name(sources[i])) !=
                    std::string(candidate_source_name(sources[j])));
}

// --- the shape of a result -------------------------------------------------------------------------

TEST_CASE("nocte plan: an accepted candidate carries the numbers that explain it", "[NoctePlan]")
{
    // `terms` is what a panel renders as bars and `score` is the rank. The contract on `terms` is
    // inherited from combine(): each entry is in [0, 1] with 0 the best in the surviving set, and
    // `score` is their weighted sum, so with weights summing to 1 it lies in [0, 1] too. The best
    // candidate is at position 0 by definition of "best first", so its score cannot exceed any
    // other's.
    const indexed_triangle_set part = cap_on_a_stem(10., 4.);
    const PartInvariants       inv  = precompute_mesh(part);
    REQUIRE(inv.valid);

    const PlanIntent   intent = make_intent(PartIntent::Unspecified);
    const OrientParams params;
    const OrientResult result = plan_orientation(part, inv, intent, params, never_cancel);

    REQUIRE(result.ok);
    REQUIRE_FALSE(result.best.empty());

    for (const OrientCandidate &c : result.best) {
        REQUIRE(c.accepted());
        for (double t : c.terms) {
            REQUIRE(t >= -1e-12);
            REQUIRE(t <= 1. + 1e-12);
        }
        REQUIRE(std::isfinite(c.score));
        REQUIRE(c.score >= -1e-12);
        REQUIRE(c.score <= 1. + 1e-12);
    }

    // Best first, and said as a strict ordering over the list rather than as a property of one
    // entry: a sort that ran backwards fails here and nowhere else in this file.
    for (size_t i = 1; i < result.best.size(); ++i)
        REQUIRE(result.best[i].score >= result.best[i - 1].score - 1e-12);

    // Accepted and rejected are disjoint populations, and together they account for the work.
    for (const OrientCandidate &c : result.rejected)
        REQUIRE_FALSE(c.accepted());
    REQUIRE(result.best.size() + result.rejected.size() <= result.considered);
}
