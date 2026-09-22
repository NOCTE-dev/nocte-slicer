// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Contract tests for the orientation-plan measurements. Every expected value below is hand
// computed from the geometry of the fixture mesh and from the formula documented in
// src/libslic3r/Nocte/Plan/Scores.hpp; nothing here is a regression baseline captured from an
// implementation.

#include <catch2/catch_all.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/Plan/PartInvariants.hpp"
#include "libslic3r/Nocte/Plan/Scores.hpp"

using namespace Slic3r;
using namespace Slic3r::Nocte;

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// its_make_cube() puts the box in the first octant (TriangleMesh.cpp:886), so a cube of side s
// spans [0, s] on every axis and its bottom face lies exactly on the bed plane.
PartInvariants precompute_mesh(const indexed_triangle_set &its)
{
    const PrecomputeParams params;
    return precompute(its, params);
}

// A 20 x 20 x 4 cap carried on a square stem of side `stem_side_mm` and height `stem_height_mm`,
// the shape the GeometryAnalysis tests use, with both stem dimensions opened up. The cap is centred
// over the stem, so with side 4 the cap spans [-8, 12] and the stem [0, 4] — the original fixture.
//
// The two parameters vary the two things the support term must get right INDEPENDENTLY:
//   stem_height_mm  changes the HEIGHT the column is carried through, area fixed;
//   stem_side_mm    changes how much of the ceiling is SELF-SUPPORTED, height fixed.
//
// The cap underside is a horizontal downward-facing surface `stem_height_mm` above the bed: it is
// 20 x 20 = 400 mm^2 of ceiling, of which stem_side^2 directly over the stem is self-supported,
// leaving 400 - stem_side^2 of genuine overhang that has to be carried the whole way to the bed and
// that cannot be confused with bed contact.
//
// It is also the file's one NON-PRISMATIC fixture: it is far wider at the top than at its base, so
// its bed contact (stem_side^2) and its silhouette (400 mm^2) are different objects. Every other
// fixture here is prismatic from the bed up, where the two coincide and a footprint taken from the
// silhouette would pass unnoticed.
indexed_triangle_set cap_on_a_stem(double stem_height_mm, double stem_side_mm)
{
    indexed_triangle_set mesh   = its_make_cube(stem_side_mm, stem_side_mm, stem_height_mm);
    indexed_triangle_set cap    = its_make_cube(20., 20., 4.);
    const float          inset  = static_cast<float>(0.5 * (stem_side_mm - 20.));
    its_translate(cap, Vec3f(inset, inset, static_cast<float>(stem_height_mm)));
    its_merge(mesh, cap);
    return mesh;
}

// A sphere of radius `radius_mm` resting on the bed: its_make_sphere() centres on the origin, so
// the translate puts the lowest point at z = 0 and the centre at z = radius_mm.
//
// Unlike a flat ceiling, a sphere presents every overhang angle continuously, which is what lets an
// overhang threshold be seen to bite. PI/12 is a coarse 24 x 12 tessellation, deliberately: the
// tests built on it assert ratios and bands, never an exact curved-surface volume.
indexed_triangle_set ball_on_the_bed(double radius_mm)
{
    indexed_triangle_set mesh = its_make_sphere(radius_mm, PI / 12.);
    its_translate(mesh, Vec3f(0.f, 0.f, static_cast<float>(radius_mm)));
    return mesh;
}

// A square tube: an `outer_mm` box with an `inner_mm` box cut out of it, both `height_mm` tall, so
// the first-layer contact is a RING. Its raw area is outer^2 - inner^2 while the convex hull of
// that ring is the whole outer^2 square, and the two fields that report them are only
// distinguishable on a fixture like this one. Every other first-layer slice in this file is convex
// — a square, a 24-gon, a 36-gon — where the contact and its hull are the same polygon.
//
// The cavity is cut by merging an inner box with its winding reversed, so the signed volume is
// outer^2*h - inner^2*h and the slice at any z strictly inside the part is the outer square with
// the inner square as a hole.
indexed_triangle_set hollow_box(double outer_mm, double inner_mm, double height_mm)
{
    indexed_triangle_set mesh = its_make_cube(outer_mm, outer_mm, height_mm);
    indexed_triangle_set hole = its_make_cube(inner_mm, inner_mm, height_mm);
    const float          off  = static_cast<float>(0.5 * (outer_mm - inner_mm));
    its_translate(hole, Vec3f(off, off, 0.f));
    for (stl_triangle_vertex_indices &f : hole.indices)
        std::swap(f[0], f[1]);
    its_merge(mesh, hole);
    return mesh;
}

// Every facet wound inward: an ordinary broken STL, and the one case that defeats the cusp term
// without tripping any other gate. The surface area, the bounding box and the centroid are all
// unchanged, so the part looks measurable, but every outward normal now points INTO the solid, so
// every self-occlusion ray starts inside the surface and the whole part reports invisible.
indexed_triangle_set inverted_cube(double side_mm)
{
    indexed_triangle_set mesh = its_make_cube(side_mm, side_mm, side_mm);
    for (stl_triangle_vertex_indices &f : mesh.indices)
        std::swap(f[0], f[1]);
    return mesh;
}

// A rotation of `deg` degrees about the X axis, as a candidate orientation: `rotation` maps object
// coordinates to build coordinates, so Rx(90) sends object +Z to build -Y and object +Y to build
// +Z. Every other evaluate() call in this file passes the identity.
Transform3d rot_x(double deg)
{
    Transform3d t = Transform3d::Identity();
    t.rotate(Eigen::AngleAxisd(deg * PI / 180., Vec3d::UnitX()));
    return t;
}

// A synthetic candidate for combine(). Every field that carries a term is set, and `measured` is
// true so the candidate is never skipped as un-measured.
OrientScores make_candidate(double support_mm3, double cusp_mm, double time_s, double force_n, double tip_ratio)
{
    OrientScores s;
    s.support_volume_mm3 = support_mm3;
    s.support_tier       = SupportTier::SliceUnion;
    s.cusp_mean_mm       = cusp_mm;
    s.cusp_p95_mm        = cusp_mm;
    s.time_estimate_s    = time_s;
    s.failure_force_n    = force_n;
    s.stability          = tip_ratio;
    s.measured           = true;
    return s;
}

// Weights that put the whole preference on one term, so a single term can be isolated.
ScoreWeights support_only_weights()
{
    ScoreWeights w;
    w.support   = 1.;
    w.cusp      = 0.;
    w.time      = 0.;
    w.strength  = 0.;
    w.stability = 0.;
    return w;
}

} // namespace

// --- PartInvariants ----------------------------------------------------------------------------

TEST_CASE("nocte plan: a cube's invariants are its own volume, area and normals", "[NoctePlan]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const PartInvariants       inv  = precompute_mesh(cube);

    REQUIRE(inv.valid);

    // 6 quad faces, 2 triangles each.
    REQUIRE(cube.indices.size() == static_cast<size_t>(12));
    REQUIRE(inv.facet_count() == cube.indices.size());
    REQUIRE(inv.facet_normal.size() == cube.indices.size());

    // 6 faces x 20 x 20 = 2400 mm^2; 20^3 = 8000 mm^3.
    REQUIRE_THAT(inv.surface_area, WithinRel(2400., 1e-4));
    REQUIRE_THAT(inv.volume, WithinRel(8000., 1e-4));

    // Each facet is a right triangle with 20 mm legs: 0.5 * 20 * 20 = 200 mm^2, and 12 * 200 = 2400.
    double area_sum = 0.;
    for (size_t i = 0; i < inv.facet_count(); ++i) {
        const double a = static_cast<double>(inv.facet_area[i]);
        REQUIRE_THAT(a, WithinRel(200., 1e-4));
        area_sum += a;

        // Every face of an axis-aligned cube has a unit normal along one axis: its length is 1 and
        // exactly one component is +-1.
        const Vec3d n(static_cast<double>(inv.facet_normal[i].x()),
                      static_cast<double>(inv.facet_normal[i].y()),
                      static_cast<double>(inv.facet_normal[i].z()));
        REQUIRE_THAT(n.norm(), WithinAbs(1., 1e-5));
        const double axis_sum = std::abs(n.x()) + std::abs(n.y()) + std::abs(n.z());
        REQUIRE_THAT(axis_sum, WithinAbs(1., 1e-5));
    }
    REQUIRE_THAT(area_sum, WithinRel(2400., 1e-4));

    // Centre of a cube spanning [0, 20] on every axis.
    REQUIRE_THAT(inv.center_of_mass.x(), WithinAbs(10., 1e-3));
    REQUIRE_THAT(inv.center_of_mass.y(), WithinAbs(10., 1e-3));
    REQUIRE_THAT(inv.center_of_mass.z(), WithinAbs(10., 1e-3));

    REQUIRE_THAT(inv.bbox.min.z(), WithinAbs(0., 1e-6));
    REQUIRE_THAT(inv.bbox.max.z(), WithinAbs(20., 1e-6));
    REQUIRE_THAT(inv.bbox.size().x(), WithinRel(20., 1e-6));

    // The visibility pass runs: 12 facets is far under PrecomputeParams::visibility_facet_budget.
    REQUIRE(inv.visibility_available);
    REQUIRE(inv.facet_visible.size() == inv.facet_count());
}

TEST_CASE("nocte plan: an empty or degenerate mesh is invalid, not an exception", "[NoctePlan]")
{
    SECTION("an empty mesh") {
        const indexed_triangle_set empty;
        PartInvariants             inv;
        REQUIRE_NOTHROW(inv = precompute_mesh(empty));
        REQUIRE_FALSE(inv.valid);
        REQUIRE(inv.facet_count() == static_cast<size_t>(0));
        REQUIRE_THAT(inv.volume, WithinAbs(0., 1e-12));
        REQUIRE_THAT(inv.surface_area, WithinAbs(0., 1e-12));
        REQUIRE_THAT(inv.hull_volume, WithinAbs(0., 1e-12));
    }

    SECTION("a single zero-area triangle") {
        // Three collinear vertices: the cross product is the zero vector, so the facet has area 0
        // and a zero normal. Consumers must skip it by the area, never by the normal.
        indexed_triangle_set sliver;
        sliver.vertices.push_back(stl_vertex(0.f, 0.f, 0.f));
        sliver.vertices.push_back(stl_vertex(10.f, 0.f, 0.f));
        sliver.vertices.push_back(stl_vertex(20.f, 0.f, 0.f));
        sliver.indices.push_back(stl_triangle_vertex_indices(0, 1, 2));

        PartInvariants inv;
        REQUIRE_NOTHROW(inv = precompute_mesh(sliver));
        REQUIRE_FALSE(inv.valid);
        REQUIRE_THAT(inv.surface_area, WithinAbs(0., 1e-9));
        REQUIRE_THAT(inv.volume, WithinAbs(0., 1e-9));
        REQUIRE_THAT(inv.hull_volume, WithinAbs(0., 1e-9));
    }
}

TEST_CASE("nocte plan: the invariants do not change when the part is rotated", "[NoctePlan]")
{
    // This is the whole reason the struct exists: a candidate orientation may not re-measure any of
    // these. The centre of mass and the bounding box DO move with the rotation and are not compared.
    const indexed_triangle_set upright = its_make_cube(20., 20., 20.);

    indexed_triangle_set tilted = its_make_cube(20., 20., 20.);
    Transform3d          rot    = Transform3d::Identity();
    rot.rotate(Eigen::AngleAxisd(0.7, Vec3d(1., 2., 3.).normalized()));
    its_transform(tilted, rot);

    const PartInvariants a = precompute_mesh(upright);
    const PartInvariants b = precompute_mesh(tilted);

    REQUIRE(a.valid);
    REQUIRE(b.valid);
    REQUIRE(a.facet_count() == b.facet_count());

    // Vertices are stored as float (stl_vertex is Vec3f), so a rotate-and-round-trip costs about
    // 6e-8 of relative precision per coordinate and rather more in the signed-volume sum. 1e-5
    // relative is two orders of magnitude tighter than any real recomputation bug and still safely
    // above the float noise floor.
    REQUIRE_THAT(b.volume, WithinRel(a.volume, 1e-5));
    REQUIRE_THAT(b.surface_area, WithinRel(a.surface_area, 1e-5));
    REQUIRE_THAT(b.hull_volume, WithinRel(a.hull_volume, 1e-4));

    // And they are still the cube's own numbers, not merely equal to each other.
    REQUIRE_THAT(b.volume, WithinRel(8000., 1e-4));
    REQUIRE_THAT(b.surface_area, WithinRel(2400., 1e-4));
}

TEST_CASE("nocte plan: the convex hull bounds the part", "[NoctePlan]")
{
    SECTION("a convex solid is its own hull") {
        const PartInvariants inv = precompute_mesh(its_make_cube(20., 20., 20.));
        // The hull of 8 corners is the cube itself: 8000 mm^3.
        REQUIRE_THAT(inv.hull_volume, WithinRel(inv.volume, 1e-3));
        REQUIRE_THAT(inv.hull_volume, WithinRel(8000., 1e-3));
        REQUIRE_FALSE(inv.hull.indices.empty());
    }

    SECTION("a non-convex solid is strictly smaller than its hull") {
        const PartInvariants inv = precompute_mesh(cap_on_a_stem(10., 4.));
        // 4 * 4 * 10 stem + 20 * 20 * 4 cap = 160 + 1600 = 1760 mm^3.
        REQUIRE_THAT(inv.volume, WithinRel(1760., 1e-4));
        // The hull fills the frustum from the 4 x 4 stem base up to the 20 x 20 cap:
        // (10/3) * (16 + 400 + sqrt(16 * 400)) = (10/3) * 496 = 1653 mm^3, plus the 1600 mm^3 cap,
        // about 3253 mm^3. Only the strict inequality is pinned; the hull triangulation is qhull's.
        REQUIRE(inv.hull_volume > inv.volume);
        REQUIRE(inv.hull_volume > 1.2 * inv.volume);
    }
}

TEST_CASE("nocte plan: visibility can be switched off", "[NoctePlan]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);

    PrecomputeParams params;
    params.measure_visibility = false;
    const PartInvariants inv = precompute(cube, params);

    REQUIRE(inv.valid);
    // Documented contract: no flags at all, and the scores must read that as "every facet visible".
    REQUIRE_FALSE(inv.visibility_available);
    REQUIRE(inv.facet_visible.empty());
    // The rest of the measurement is unaffected.
    REQUIRE_THAT(inv.volume, WithinRel(8000., 1e-4));
    REQUIRE_THAT(inv.surface_area, WithinRel(2400., 1e-4));
}

// --- Scores: the cusp term ---------------------------------------------------------------------

TEST_CASE("nocte plan: a 45 degree face has a cusp of h / sqrt(2)", "[NoctePlan]")
{
    // its_make_pyramid(base, height) (TriangleMesh.cpp:1089) puts a square base of side `base`
    // centred on the origin at z = 0 and the apex at (0, 0, height). A side face spans the edge
    // y = -a and the apex, so its normal is (0, -height, a) / |.| with a = base / 2. Choosing
    // a == height makes |n.z| = a / sqrt(height^2 + a^2) = 1 / sqrt(2), i.e. asin(|n.z|) = 45 deg.
    // base = 20 -> a = 10, height = 10.
    const indexed_triangle_set wedge = its_make_pyramid(20.f, 10.f);
    const PartInvariants       inv   = precompute_mesh(wedge);
    REQUIRE(inv.valid);

    // This pins the angle convention shared with GeometryAnalysis.cpp:45-59:
    //   0 deg = vertical wall (normal horizontal), 90 deg = horizontal ceiling or floor,
    //   angle = asin(|n.z|).
    // 45 deg face: c = h * |n.z| = h * sin(45 deg) = h / sqrt(2).
    // The two base triangles sit at 90 deg and are dropped by flat_exclude_deg (default 5 deg, so
    // everything at or above 85 deg goes), leaving four identical 45 deg faces. Mean and p95 over
    // one repeated value are that value.
    const double inv_sqrt2 = 1. / std::sqrt(2.);

    // The extensive form, cusp_error_mm3 = sum(a_i * delta_i), needs the surviving AREA. One side
    // face has vertices (-10,-10,0), (10,-10,0), (0,0,10); its edge vectors are (20,0,0) and
    // (10,10,10), whose cross product is (0,-200,200) of length 200*sqrt(2), so the face area is
    // 100*sqrt(2) = 141.4214 mm^2 and the four of them total 400*sqrt(2) = 565.6854 mm^2.
    // (Cross-check: the pyramid's whole surface is that plus the 400 mm^2 base = 965.6854 mm^2.)
    const double surviving_area = 400. * std::sqrt(2.);

    SECTION("with the default 0.2 mm layer") {
        const ScoreParams  params;
        const OrientScores s = evaluate(wedge, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.measured);
        // 0.2 / sqrt(2) = 0.141421356237
        REQUIRE_THAT(s.cusp_mean_mm, WithinAbs(0.2 * inv_sqrt2, 1e-6));
        REQUIRE_THAT(s.cusp_p95_mm, WithinAbs(0.2 * inv_sqrt2, 1e-6));

        // The sqrt(2) cancels outright: 400*sqrt(2) mm^2 * 0.2/sqrt(2) mm = 400 * 0.2 = 80 mm^3.
        REQUIRE_THAT(s.cusp_error_mm3, WithinRel(80., 1e-4));
        // Same number said as the identity that defines it, so a failure shows which factor moved.
        REQUIRE_THAT(s.cusp_error_mm3, WithinRel(surviving_area * s.cusp_mean_mm, 1e-4));
    }

    SECTION("the cusp is linear in the layer height") {
        ScoreParams params;
        params.layer_height_mm = 0.3;
        const OrientScores s = evaluate(wedge, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        // 0.3 / sqrt(2) = 0.212132034356
        REQUIRE_THAT(s.cusp_mean_mm, WithinAbs(0.3 * inv_sqrt2, 1e-6));
        // 400*sqrt(2) * 0.3/sqrt(2) = 400 * 0.3 = 120 mm^3.
        REQUIRE_THAT(s.cusp_error_mm3, WithinRel(120., 1e-4));
    }
}

TEST_CASE("nocte plan: vertical walls have no cusp", "[NoctePlan]")
{
    // A cylinder standing on its axis: every side facet spans z = 0 to z = h at two fixed angular
    // positions, so it lies in a vertical plane and |n.z| = 0 -> c = h * 0 = 0 exactly. The two
    // caps are horizontal and are excluded as flat, so nothing else enters the average.
    const indexed_triangle_set drum = its_make_cylinder(10., 20., 2. * PI / 36.);
    const PartInvariants       inv  = precompute_mesh(drum);
    REQUIRE(inv.valid);

    const ScoreParams  params;
    const OrientScores s = evaluate(drum, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s.measured);
    REQUIRE_THAT(s.cusp_mean_mm, WithinAbs(0., 1e-9));
    REQUIRE_THAT(s.cusp_p95_mm, WithinAbs(0., 1e-9));
}

TEST_CASE("nocte plan: a flat top is excluded from the cusp average", "[NoctePlan]")
{
    // A cube has four vertical walls (|n.z| = 0 -> c = 0) and two horizontal faces (|n.z| = 1,
    // which the raw formula would report as c -> h = 0.2 mm). flat_exclude_deg = 5 removes both
    // horizontal faces, so the average is taken over the four walls alone and is exactly 0.
    // A flat top has no staircase at all; reporting h for it is the bug this test exists to catch.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const PartInvariants       inv  = precompute_mesh(cube);
    REQUIRE(inv.valid);

    const ScoreParams  params;
    const OrientScores s = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s.measured);
    REQUIRE_THAT(s.cusp_mean_mm, WithinAbs(0., 1e-9));
    REQUIRE_THAT(s.cusp_p95_mm, WithinAbs(0., 1e-9));
    // Said the other way round, so the failure message names the actual mistake: it must not be h.
    REQUIRE(s.cusp_mean_mm < 0.5 * params.layer_height_mm);
    REQUIRE(s.cusp_p95_mm < 0.5 * params.layer_height_mm);
    // The extensive form sums a_i * delta_i over the SURVIVING facets, and every one of them is a
    // vertical wall with delta = 0, so the sum is 0 — NOT the 1600 mm^2 of wall area times anything.
    // A cusp_error_mm3 that accumulated area rather than area-times-error would report 1600 * 0.2
    // = 320 mm^3 here.
    REQUIRE_THAT(s.cusp_error_mm3, WithinAbs(0., 1e-6));
    // All three zeros are FINDINGS: the four walls did survive the filter and were measured to be
    // perfectly smooth. That is a different statement from "no facet survived", and zero is the
    // BEST value on this term, so only the flag separates them.
    REQUIRE(s.cusp_measured);
}

TEST_CASE("nocte plan: an inverted mesh reports that its cusp was never measured", "[NoctePlan]")
{
    // The fifth way to get a perfect score for free. An inverted cube — every facet wound inward,
    // which is an ordinary broken STL, not a contrived input — keeps its 2400 mm^2 of surface area
    // and its (10,10,10) centroid, so it passes every gate that looks at those. But every outward
    // normal points into the solid, so every self-occlusion ray starts inside the surface and the
    // whole part comes back invisible. Nothing survives the visible-and-non-flat filter, all three
    // cusp numbers are 0, and 0 is the BEST cusp there is: without the flag this part scores
    // perfect surface quality on every candidate and the cusp term stops discriminating entirely.
    const indexed_triangle_set bad = inverted_cube(20.);
    const PartInvariants       inv = precompute_mesh(bad);

    // It really does look measurable: the area is unchanged and the visibility pass did run.
    REQUIRE(inv.valid);
    REQUIRE_THAT(inv.surface_area, WithinRel(2400., 1e-4));
    REQUIRE(inv.visibility_available);
    REQUIRE(inv.facet_visible.size() == inv.facet_count());

    const ScoreParams  params;
    const OrientScores s = evaluate(bad, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE_FALSE(s.cusp_measured);
    REQUIRE_THAT(s.cusp_mean_mm, WithinAbs(0., 1e-12));
    REQUIRE_THAT(s.cusp_p95_mm, WithinAbs(0., 1e-12));
    REQUIRE_THAT(s.cusp_error_mm3, WithinAbs(0., 1e-12));

    // `visibility_available` is true here — the pass ran and produced flags — but not one facet is
    // marked visible, so the data is present and useless. Contact has to report that as unmeasured
    // rather than as "no support lands on a visible face".
    REQUIRE_FALSE(s.contact_measured);

    // The contrast that gives the flag meaning: the same shape wound correctly measures its cusp,
    // and also reports 0 — an honest 0 from four smooth vertical walls.
    const indexed_triangle_set good     = its_make_cube(20., 20., 20.);
    const PartInvariants       good_inv = precompute_mesh(good);
    const OrientScores         good_s =
        evaluate(good, good_inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
    REQUIRE(good_s.cusp_measured);
    REQUIRE(good_s.contact_measured);
    REQUIRE_THAT(good_s.cusp_mean_mm, WithinAbs(0., 1e-9));
    // The two parts report the identical zeros and differ only in the flags. That is the whole
    // point: the numbers cannot tell you which is which.
    REQUIRE_THAT(s.cusp_mean_mm, WithinAbs(good_s.cusp_mean_mm, 1e-9));
}

TEST_CASE("nocte plan: the cusp p95 is never below the cusp mean", "[NoctePlan]")
{
    // A percentile of a set is never below its mean's lower bound in the only way that matters
    // here: p95 >= mean must hold for every shape, including one whose facets span every angle.
    const ScoreParams params;

    SECTION("a sphere, whose facets cover every angle") {
        const indexed_triangle_set ball = ball_on_the_bed(10.);
        const PartInvariants       inv  = precompute_mesh(ball);
        REQUIRE(inv.valid);
        const OrientScores s = evaluate(ball, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.measured);
        REQUIRE(s.cusp_p95_mm >= s.cusp_mean_mm - 1e-9);
        // A sphere genuinely has a spread of angles, so the percentile is strictly above the mean.
        REQUIRE(s.cusp_p95_mm > s.cusp_mean_mm);
        // And every cusp is bounded by the layer height: c = h * |n.z| <= h.
        REQUIRE(s.cusp_p95_mm <= params.layer_height_mm + 1e-9);
    }

    SECTION("a pyramid, whose facets are all the same angle") {
        const indexed_triangle_set wedge = its_make_pyramid(20.f, 10.f);
        const PartInvariants       inv   = precompute_mesh(wedge);
        REQUIRE(inv.valid);
        const OrientScores s = evaluate(wedge, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.cusp_p95_mm >= s.cusp_mean_mm - 1e-9);
    }

    SECTION("a cube, where both are zero") {
        const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        const PartInvariants       inv  = precompute_mesh(cube);
        const OrientScores s = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.cusp_p95_mm >= s.cusp_mean_mm - 1e-9);
    }
}

// --- Scores: stability -------------------------------------------------------------------------

TEST_CASE("nocte plan: a cube on the bed has a stability of exactly 1", "[NoctePlan]")
{
    // stability = d_min / z_com, where d_min is the distance from the projected centre of mass to
    // the nearest edge of the footprint hull. For a 20 mm cube resting on its own face the
    // projected centre of mass is the centre of a 20 x 20 square, so d_min = L/2 = 10 mm, and the
    // centre of mass is at z = L/2 = 10 mm. 10 / 10 = 1.
    //
    // The isotropic sqrt(footprint_area) / height formula this replaced gives sqrt(400) / 10 = 2
    // for the same cube, so this assertion separates the two conventions outright.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const PartInvariants       inv  = precompute_mesh(cube);
    REQUIRE(inv.valid);

    const ScoreParams  params;
    const OrientScores s = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s.measured);
    REQUIRE_THAT(s.stability, WithinAbs(1., 1e-6));
    // The whole 20 x 20 bottom face is the footprint.
    REQUIRE_THAT(s.footprint_area_mm2, WithinRel(400., 1e-3));
    REQUIRE_THAT(s.height_mm, WithinRel(20., 1e-6));
    REQUIRE(s.layer_count > 0);
}

TEST_CASE("nocte plan: a tall thin box is unstable", "[NoctePlan]")
{
    // 5 x 5 x 50: d_min = 2.5 mm, z_com = 25 mm, so stability = 2.5 / 25 = 0.1 — an order of
    // magnitude below the cube and far below the 0.35 screening threshold. The exact value is not
    // asserted because only the d_min / z_com ratio is a documented contract; the threshold is.
    const indexed_triangle_set pillar = its_make_cube(5., 5., 50.);
    const PartInvariants       inv    = precompute_mesh(pillar);
    REQUIRE(inv.valid);

    const ScoreParams  params;
    const OrientScores s = evaluate(pillar, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s.measured);
    REQUIRE(s.stability > 0.);
    REQUIRE(s.stability < 0.35);
    REQUIRE_THAT(s.footprint_area_mm2, WithinRel(25., 1e-3));
}

TEST_CASE("nocte plan: a wide slab is very stable", "[NoctePlan]")
{
    // 60 x 60 x 2: d_min = 30 mm, z_com = 1 mm, so stability = 30 / 1 = 30. Only "well above 1" is
    // pinned, which is already thirty times the margin of the cube.
    const indexed_triangle_set slab = its_make_cube(60., 60., 2.);
    const PartInvariants       inv  = precompute_mesh(slab);
    REQUIRE(inv.valid);

    const ScoreParams  params;
    const OrientScores s = evaluate(slab, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s.measured);
    REQUIRE(s.stability > 5.);
    REQUIRE_THAT(s.footprint_area_mm2, WithinRel(3600., 1e-3));
}

TEST_CASE("nocte plan: turning the part over changes its verdict", "[NoctePlan]")
{
    // Every other evaluate() in this file passes Transform3d::Identity(), and every fixture already
    // sits on the bed, so the internal drop-to-bed is the identity in all of them and the whole
    // orientation half of the entry point goes unexercised. These cases are the ones where the
    // ANSWER MOVES under the rotation, which is the only kind that can catch a transform that is
    // dropped, transposed or applied to the wrong vector.
    const ScoreParams params;

    SECTION("a cube on its side is still a cube") {
        // Rx(90) sends object (x,y,z) to build (x,-z,y). The cube [0,20]^3 becomes
        // x in [0,20], y in [-20,0], z in [0,20] — already on the bed, nothing to drop.
        //   footprint  = the image of the object's y=0 face, 20 x 20  = 400 mm^2
        //   z_com      = object (10,10,10) -> build (10,-10,10)       =  10 mm
        //   d_min      = (10,-10) is the centre of [0,20]x[-20,0]     =  10 mm
        //   S          = 10/10                                       =   1
        // The numbers match the upright cube, but every one of them had to be recomputed through a
        // non-identity transform to get there, so this is an invariance that means something.
        const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        const PartInvariants       inv  = precompute_mesh(cube);
        const OrientScores         s    = evaluate(cube, inv, rot_x(90.), params, Vec3d::Zero(), 0.);

        REQUIRE(s.measured);
        REQUIRE_THAT(s.footprint_area_mm2, WithinRel(400., 1e-3));
        REQUIRE_THAT(s.height_mm, WithinRel(20., 1e-6));
        REQUIRE_THAT(s.stability, WithinAbs(1., 1e-6));
        REQUIRE_THAT(s.support_volume_mm3, WithinAbs(0., 1e-6));
    }

    SECTION("a cube on its corner-edge is all staircase") {
        // Rx(45) tilts the four faces whose normals lie in the YZ plane to 45 deg from the vertical;
        // the two +-X faces stay vertical. Nothing reaches the 85 deg flat-exclusion, so ALL SIX
        // faces survive, 2400 mm^2 of them:
        //   4 faces x 400 mm^2 at |n.z| = sin45     -> delta = h/sqrt(2) = 0.1414214 mm
        //   2 faces x 400 mm^2 at |n.z| = 0         -> delta = 0
        //   cusp_mean  = (1600*0.1414214 + 800*0)/2400 = 0.0942809 mm
        //   cusp_error = 1600 * 0.1414214              = 226.274 mm^3
        //   cusp_p95   = 0.1414214 (two thirds of the area sits at the top value)
        // Upright the very same mesh reports 0, 0 and 0, because the four faces that are now
        // staircase were vertical and the two that are now vertical were excluded as flat. Nothing
        // but the rotation moved.
        const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        const PartInvariants       inv  = precompute_mesh(cube);
        const OrientScores         s    = evaluate(cube, inv, rot_x(45.), params, Vec3d::Zero(), 0.);

        REQUIRE(s.measured);
        REQUIRE(s.cusp_measured);
        const double delta45 = 0.2 / std::sqrt(2.);
        REQUIRE_THAT(s.cusp_mean_mm, WithinRel(2. * delta45 / 3., 1e-4));
        REQUIRE_THAT(s.cusp_p95_mm, WithinRel(delta45, 1e-4));
        REQUIRE_THAT(s.cusp_error_mm3, WithinRel(1600. * delta45, 1e-4));
        // And the upright answer, for contrast, from the same mesh and the same invariants.
        const OrientScores up = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE_THAT(up.cusp_error_mm3, WithinAbs(0., 1e-6));
        REQUIRE(s.cusp_error_mm3 > 100.);
    }

    SECTION("the cap on a stem turned over lands on the cap") {
        // Rx(180) sends (x,y,z) to (x,-y,-z). The stem [0,4]^2 x [0,10] goes to z in [-10,0] and the
        // cap [-8,12]^2 x [10,14] to z in [-14,-10]; the bbox is z in [-14,0], so the drop-to-bed
        // adds +14 and the CAP is now flat on the bed with the stem pointing up.
        //   footprint  = the cap's 20 x 20 face                       = 400 mm^2  (16 upright)
        //   z_com      = 14 - 11.363636                               = 2.636364 mm
        //   d_min      = COM projects to (2,-2), the centre of the cap footprint [-8,12]x[-12,8]
        //                so every edge is 10 mm away                  =  10 mm
        //   S          = 10 / 2.636364                                = 3.7931
        // Upright the same part scores 0.176. The 0.35 tipping gate therefore REJECTS it one way up
        // and ACCEPTS it the other — the first case in this file where the orientation changes the
        // verdict rather than merely the number, which is the entire point of the engine.
        const indexed_triangle_set part = cap_on_a_stem(10., 4.);
        const PartInvariants       inv  = precompute_mesh(part);

        const OrientScores upright = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        const OrientScores flipped = evaluate(part, inv, rot_x(180.), params, Vec3d::Zero(), 0.);

        REQUIRE(upright.measured);
        REQUIRE(flipped.measured);
        // The height is the one thing the flip does not change: 10 + 4 either way.
        REQUIRE_THAT(flipped.height_mm, WithinRel(14., 1e-6));

        REQUIRE_THAT(flipped.footprint_area_mm2, WithinRel(400., 1e-3));
        REQUIRE_THAT(flipped.stability, WithinRel(10. / (14. - 20000. / 1760.), 0.05));
        REQUIRE(flipped.stability > 0.35);   // accepted
        REQUIRE(upright.stability < 0.35);   // rejected
        REQUIRE(flipped.stability > 10. * upright.stability);

        // Turning it over also removes the overhang entirely: the cap underside is now the bed face,
        // and the stem's own downward 4 x 4 face rests directly on the cap, so `\ S_k` clears it on
        // the first layer below. Upright the same part needs 576 mm^3.
        REQUIRE(flipped.support_measured);
        REQUIRE(flipped.support_volume_mm3 < 50.);
        REQUIRE(flipped.support_volume_mm3 < 0.1 * upright.support_volume_mm3);
    }

    SECTION("rotating the part changes which direction is across the layers") {
        // THE ASSERTION THAT PINS `rotation.linear() * load_dir_obj`. The load direction is given in
        // OBJECT coordinates, but the layers are horizontal in BUILD coordinates, so the anisotropy
        // depends on where the rotation sends it. With Rx(90):
        //   object +Z -> build (0,-1,0), IN PLANE   -> sigma = sigma_xy = 50 -> 400*50 = 20000 N
        //   object +Y -> build (0, 0,1), ACROSS     -> sigma = k*sigma_xy = 25 -> 400*25 = 10000 N
        // which is the exact reverse of the upright case. A scorer that used load_dir_obj directly,
        // never rotating it, reports 10000 and 20000 here instead — it cannot tell the two apart,
        // and neither could any test in this file before this one.
        const indexed_triangle_set cube    = its_make_cube(20., 20., 20.);
        const PartInvariants       inv     = precompute_mesh(cube);
        const double               section = 400.;

        const OrientScores z_upright = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::UnitZ(), section);
        const OrientScores z_rotated = evaluate(cube, inv, rot_x(90.), params, Vec3d::UnitZ(), section);
        const OrientScores y_upright = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::UnitY(), section);
        const OrientScores y_rotated = evaluate(cube, inv, rot_x(90.), params, Vec3d::UnitY(), section);

        REQUIRE_THAT(z_upright.failure_force_n, WithinRel(10000., 1e-6));
        REQUIRE_THAT(z_rotated.failure_force_n, WithinRel(20000., 1e-6));
        REQUIRE_THAT(y_upright.failure_force_n, WithinRel(20000., 1e-6));
        REQUIRE_THAT(y_rotated.failure_force_n, WithinRel(10000., 1e-6));
        // Said as the cross-over it is: the two load directions swap roles under the rotation.
        REQUIRE(z_rotated.failure_force_n > z_upright.failure_force_n);
        REQUIRE(y_rotated.failure_force_n < y_upright.failure_force_n);
    }
}

TEST_CASE("nocte plan: the footprint is bed contact, not the part's silhouette", "[NoctePlan]")
{
    // EVERY other stability fixture in this file — the cube, the 5 x 5 x 50 pillar, the 60 x 60 x 2
    // slab — is prismatic from the bed up, and for a prism the bed contact and the silhouette are
    // the same polygon. A footprint taken from the shadow of the whole part therefore passes all
    // three of them. These two fixtures are the ones where the two objects differ.
    const ScoreParams params;

    SECTION("a cap on a stem stands on the stem, not on the cap") {
        // 20 x 20 x 4 cap on a 4 x 4 x 10 stem.
        //   volume    = 4*4*10 + 20*20*4 = 160 + 1600 = 1760 mm^3
        //   z_com     = (160 * 5 + 1600 * 12) / 1760 = (800 + 19200) / 1760 = 20000/1760 = 11.3636 mm
        //   contact   = the stem base [0,4]^2                      =   16 mm^2
        //   silhouette= the cap's shadow [-8,12]^2                 =  400 mm^2
        // The projected centre of mass is (2, 2), the centre of the stem base, so
        //   d_min     = 2 mm   and   S = 2 / 11.3636 = 0.176.
        // From the SILHOUETTE instead, d_min would be min(2 - (-8), 12 - 2) = 10 mm and
        //   S = 10 / 11.3636 = 0.880,
        // which sails past the S >= 0.35 tipping gate on a part that is genuinely top-heavy. That is
        // the whole defect: the shadow flatters a part that stands on a narrow base.
        const indexed_triangle_set part = cap_on_a_stem(10., 4.);
        const PartInvariants       inv  = precompute_mesh(part);
        REQUIRE(inv.valid);
        REQUIRE_THAT(inv.volume, WithinRel(1760., 1e-4));
        REQUIRE_THAT(inv.center_of_mass.z(), WithinRel(20000. / 1760., 1e-4));

        const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.measured);

        // 16 mm^2 of bed contact, not the 400 mm^2 shadow.
        REQUIRE_THAT(s.footprint_area_mm2, WithinRel(16., 1e-3));
        REQUIRE(s.footprint_area_mm2 < 100.);
        // The stem base is convex, so its support polygon is itself: the hull area is 16 too.
        REQUIRE_THAT(s.footprint_hull_area_mm2, WithinRel(16., 1e-3));

        // 2 / 11.3636 = 0.176, and it MUST fail the 0.35 gate. The silhouette answer, 0.880, fails
        // both of these lines.
        REQUIRE_THAT(s.stability, WithinRel(0.176, 0.05));
        REQUIRE(s.stability < 0.35);
    }

    SECTION("a sphere balances on a point") {
        // R = 10 resting on the bed. A uniform sphere has its centre of mass at its centre, so
        // z_com = 10 mm exactly.
        //   silhouette = pi * 10^2 = 314.2 mm^2, d_min = 10 -> S = 10 / 10 = 1.0, rock solid.
        //   real contact: the first-layer slice at height z has radius sqrt(2Rz - z^2), so anywhere
        //   in (0, 0.2] it is at most sqrt(2*10*0.2) = 2 mm, i.e. at most pi*4 = 12.6 mm^2, and less
        //   again on the 24-gon facets. d_min <= 2 gives S <= 0.2.
        // A ball does not stand up on a printer, and the number has to say so. The bounds are loose
        // on purpose: which z inside the first layer the contact is taken at moves the area between
        // roughly 1.8 and 21 mm^2, and none of that matters next to the 314 mm^2 it must not be.
        const indexed_triangle_set ball = ball_on_the_bed(10.);
        const PartInvariants       inv  = precompute_mesh(ball);
        REQUIRE(inv.valid);
        REQUIRE_THAT(inv.center_of_mass.z(), WithinAbs(10., 0.05));

        const OrientScores s = evaluate(ball, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.measured);

        REQUIRE(s.footprint_area_mm2 < 50.);
        REQUIRE(s.footprint_hull_area_mm2 < 50.);
        REQUIRE(s.stability < 0.35);
    }

    SECTION("a ring stands on its rim but tips about its hull") {
        // 40 x 40 outer, 30 x 30 inner, 10 mm tall: the first layer prints a 5 mm wide frame.
        //   raw contact        = 40^2 - 30^2 = 1600 - 900 = 700 mm^2   <- what adhesion reads
        //   support polygon    = the hull of that ring   = 40^2 = 1600 mm^2  <- what tipping reads
        // The two differ by 2.29x, and THIS IS THE ONLY FIXTURE IN THE FILE WHERE THEY DIFFER AT
        // ALL: every other first-layer slice here is convex, so an implementation that assigned the
        // hull area to both fields passes every other line in this file. A ring's hull would
        // overstate its adhesion by that same 2.29x, which is the reason the header keeps the two
        // fields apart.
        //
        // Tipping, by contrast, is genuinely about the hull: the centre of mass projects to
        // (20, 20), which is over the HOLE and not over any contact at all, yet the part obviously
        // does not tip. d_min is 20 mm to the hull edge and z_com is 5 mm, so S = 20/5 = 4.
        const indexed_triangle_set ring = hollow_box(40., 30., 10.);
        const PartInvariants       inv  = precompute_mesh(ring);
        REQUIRE(inv.valid);
        // (1600 - 900) * 10 = 7000 mm^3 confirms the cavity really was cut.
        REQUIRE_THAT(inv.volume, WithinRel(7000., 1e-4));

        const OrientScores s = evaluate(ring, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.measured);

        REQUIRE_THAT(s.footprint_area_mm2, WithinRel(700., 1e-3));
        REQUIRE_THAT(s.footprint_hull_area_mm2, WithinRel(1600., 1e-3));
        // Said as the inequality too, so the failure message names the redundancy directly.
        REQUIRE(s.footprint_hull_area_mm2 > 2. * s.footprint_area_mm2);

        REQUIRE_THAT(s.stability, WithinRel(4., 0.02));
    }

    SECTION("the support polygon is never smaller than the contact it encloses") {
        // A convex hull contains the region it is built from, so this holds for every part. On the
        // prismatic cube both are the same 400 mm^2 bottom face.
        const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
        const PartInvariants       inv  = precompute_mesh(cube);
        const OrientScores s = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE_THAT(s.footprint_area_mm2, WithinRel(400., 1e-3));
        REQUIRE_THAT(s.footprint_hull_area_mm2, WithinRel(400., 1e-3));
        REQUIRE(s.footprint_hull_area_mm2 >= s.footprint_area_mm2 - 1e-6);
    }
}

// --- Scores: strength --------------------------------------------------------------------------

TEST_CASE("nocte plan: the minimum section is measured normal to the load", "[NoctePlan]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const ScoreParams          params;

    // Every plane normal to an axis cuts the same 20 x 20 = 400 mm^2 square out of a cube.
    const double along_z = min_section_area_along(cube, Vec3d(0., 0., 1.), params);
    const double along_x = min_section_area_along(cube, Vec3d(1., 0., 0.), params);
    REQUIRE_THAT(along_z, WithinRel(400., 1e-3));
    REQUIRE_THAT(along_x, WithinRel(400., 1e-3));

    // The planes normal to -d are the same set of planes as those normal to +d, swept the other
    // way, so the minimum over them is the same number.
    const double along_minus_z = min_section_area_along(cube, Vec3d(0., 0., -1.), params);
    REQUIRE_THAT(along_minus_z, WithinRel(along_z, 1e-6));

    // Along the body diagonal (1,1,1)/sqrt(3). NOTE: this is a MINIMUM, not the mid-plane section.
    // The plane x + y + z = s cuts an equilateral triangle of area s^2 * sqrt(3) / 2 out of the
    // corner while s <= 20, so the section collapses towards the two corners: at s = 1.2 mm (a 2%
    // trim of the 60 mm span of s) the area is 1.2^2 * 0.8660 = 1.25 mm^2. The largest section on
    // that axis is the mid-plane hexagon at s = 30, area 0.8660 * (900 - 3 * 100) = 519.6 mm^2, and
    // even that is only reached in the middle. The minimum is therefore well BELOW 400 mm^2 for any
    // end-trim up to about a third of the span — it is the corner that governs, which is also the
    // physically correct bottleneck for a load along the diagonal.
    const double along_diagonal = min_section_area_along(cube, Vec3d(1., 1., 1.), params);
    REQUIRE(along_diagonal < 400.);
    // Not filler, and it is the ORDER that makes it worth keeping: a NaN or a +inf already fails
    // `< 400.` above, but a NEGATIVE area sails through it, and a negative area is exactly what a
    // polygon sum that forgot its absolute value produces on a clockwise contour. This line is the
    // only one that rejects that.
    REQUIRE(along_diagonal >= 0.);
}

TEST_CASE("nocte plan: a zero load direction yields no section", "[NoctePlan]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const ScoreParams          params;

    double area = -1.;
    REQUIRE_NOTHROW(area = min_section_area_along(cube, Vec3d::Zero(), params));
    REQUIRE_THAT(area, WithinAbs(0., 1e-9));

    // A degenerate mesh is the other documented zero.
    const indexed_triangle_set empty;
    double                     empty_area = -1.;
    REQUIRE_NOTHROW(empty_area = min_section_area_along(empty, Vec3d(0., 0., 1.), params));
    REQUIRE_THAT(empty_area, WithinAbs(0., 1e-9));

    // And with no load direction the strength fields of a candidate stay at zero rather than
    // reporting a number that was never measured.
    const PartInvariants inv = precompute_mesh(cube);
    const OrientScores   s   = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
    REQUIRE_THAT(s.min_section_area_mm2, WithinAbs(0., 1e-9));
    REQUIRE_THAT(s.failure_force_n, WithinAbs(0., 1e-9));
}

TEST_CASE("nocte plan: the failure force carries the layer anisotropy", "[NoctePlan]")
{
    // 1 MPa = 1 N/mm^2, so force = section_area_mm2 * sigma_MPa.
    //   along Z (across the layers): sigma_z = k * sigma_xy = 0.5 * 50 = 25 MPa
    //                                F = 400 * 25 = 10000 N
    //   in the XY plane (along the layers): sigma = sigma_xy = 50 MPa
    //                                F = 400 * 50 = 20000 N
    // The ratio of the two is exactly anisotropy_k. An isotropic model would return 20000 N for
    // both, which is the failure this test is here to catch.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const PartInvariants       inv  = precompute_mesh(cube);
    REQUIRE(inv.valid);

    ScoreParams params;
    params.sigma_xy_mpa = 50.;
    params.anisotropy_k = 0.5;

    const double section = 400.;

    const OrientScores along_z =
        evaluate(cube, inv, Transform3d::Identity(), params, Vec3d(0., 0., 1.), section);
    const OrientScores along_x =
        evaluate(cube, inv, Transform3d::Identity(), params, Vec3d(1., 0., 0.), section);
    const OrientScores along_y =
        evaluate(cube, inv, Transform3d::Identity(), params, Vec3d(0., 1., 0.), section);

    // The section is handed in by the caller, so it comes back unchanged.
    REQUIRE_THAT(along_z.min_section_area_mm2, WithinRel(section, 1e-9));
    REQUIRE_THAT(along_x.min_section_area_mm2, WithinRel(section, 1e-9));

    REQUIRE_THAT(along_z.failure_force_n, WithinRel(10000., 1e-6));
    REQUIRE_THAT(along_x.failure_force_n, WithinRel(20000., 1e-6));
    // The layer plane is isotropic: +X and +Y must agree.
    REQUIRE_THAT(along_y.failure_force_n, WithinRel(20000., 1e-6));
    REQUIRE_THAT(along_z.failure_force_n, WithinRel(params.anisotropy_k * along_x.failure_force_n, 1e-6));
}

TEST_CASE("nocte plan: the failure force is the same for a load and its negation", "[NoctePlan]")
{
    // The interlayer term depends on (d.z)^2, so d and -d are the same load case: a column does not
    // care whether it is pushed or pulled along the same axis.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const PartInvariants       inv  = precompute_mesh(cube);
    REQUIRE(inv.valid);

    const ScoreParams params;
    const double      section = 400.;

    const Vec3d dirs[3] = { Vec3d(0., 0., 1.), Vec3d(1., 0., 0.), Vec3d(1., 1., 1.).normalized() };
    for (const Vec3d &d : dirs) {
        const OrientScores plus =
            evaluate(cube, inv, Transform3d::Identity(), params, d, section);
        const OrientScores minus =
            evaluate(cube, inv, Transform3d::Identity(), params, Vec3d(-d), section);
        REQUIRE(plus.failure_force_n > 0.);
        REQUIRE_THAT(minus.failure_force_n, WithinRel(plus.failure_force_n, 1e-9));
    }
}

// --- Scores: support ---------------------------------------------------------------------------

TEST_CASE("nocte plan: a cube flat on the bed needs no support", "[NoctePlan]")
{
    // The only downward-facing surface of a cube is its bottom face, and that rests on the bed:
    // first-layer contact, not an overhang. Nothing above it is unsupported.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const PartInvariants       inv  = precompute_mesh(cube);
    REQUIRE(inv.valid);

    const ScoreParams  params;
    const OrientScores s = evaluate(cube, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s.measured);
    REQUIRE_THAT(s.support_volume_mm3, WithinAbs(0., 1e-6));
    REQUIRE_THAT(s.support_contact_mm2, WithinAbs(0., 1e-6));
    // Both zeros above are FINDINGS, not failures to measure, and the flags are what says so. A
    // sweep that threw would leave support_volume_mm3 at 0 as well — the best possible value on
    // that term — so without this line the two cases are indistinguishable here.
    REQUIRE(s.support_measured);
    REQUIRE(s.contact_measured);
}

TEST_CASE("nocte plan: an overhang needs a support column, sized by the density", "[NoctePlan]")
{
    // 20 x 20 = 400 mm^2 of horizontal ceiling 10 mm above the bed, of which the 4 x 4 stem carries
    // 16 mm^2, so 384 mm^2 has nothing under it and is carried down the full 10 mm to the bed.
    // Tier 1 is the top-down column carry documented at Scores.hpp:40-63, so
    //   solid column   = 384 mm^2 * 10 mm          = 3840 mm^3
    //   sparse at 0.15 = 0.15 * 3840               =  576 mm^3
    //
    // THIS ASSERTION EXISTS TO CATCH A SUPPORT TERM THAT SUMS INTERFACE AREAS INSTEAD OF COLUMNS.
    // That mistake evaluates area(B_k) * h bottom-up and so reports one layer's worth of material,
    //   0.15 * 384 mm^2 * 0.2 mm = 11.5 mm^3,
    // a factor of 50 short here, and short by a DIFFERENT factor for every candidate, because the
    // column height is exactly what the orientation changes. The band below is centred on 576 and
    // excludes 11.5 by more than an order of magnitude at the bottom and the un-sparsed 3840 at the
    // top, while leaving room for the slicing details that move the exact figure (which layer the
    // carry starts on, the end layers, the slab indexing, the cap's own 4 mm thickness). The exact
    // value is deliberately NOT pinned.
    const indexed_triangle_set part = cap_on_a_stem(10., 4.);
    const PartInvariants       inv  = precompute_mesh(part);
    REQUIRE(inv.valid);

    ScoreParams params;
    params.support_density = 0.15;
    const OrientScores sparse = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    params.support_density = 0.30;
    const OrientScores dense = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(sparse.measured);
    REQUIRE(dense.measured);
    REQUIRE(sparse.support_measured);
    REQUIRE(dense.support_measured);
    REQUIRE(sparse.support_volume_mm3 > 250.);
    REQUIRE(sparse.support_volume_mm3 < 1200.);
    // The density is a multiplier on the carried column volume, and the column did not change:
    // 0.30 / 0.15 = 2, so twice the density is exactly twice the material.
    REQUIRE_THAT(dense.support_volume_mm3, WithinRel(2. * sparse.support_volume_mm3, 1e-6));
}

TEST_CASE("nocte plan: the part under the overhang carries its own support", "[NoctePlan]")
{
    // The `R = R \ S_k` step of the column carry (Scores.hpp:49) is what makes tier 1 self-support
    // aware, and NOTHING ELSE IN THIS FILE DEFENDS IT. On the 4 x 4 stem the stem removes 16 mm^2
    // of a 400 mm^2 ceiling, so dropping the subtraction moves 576 mm^3 to 600 mm^3 — both inside
    // the band over there, and the density and height ratios are pure multipliers that survive it
    // untouched. The sphere cannot test it either: its annuli always fall outside S_k by
    // construction, which is exactly why that fixture works for the threshold.
    //
    // A stem almost as wide as the cap makes the subtraction the dominant term instead. With a
    // 18 x 18 stem under the same 20 x 20 cap, the overhang is a 1 mm rim:
    //   unsupported   = 400 - 18*18 = 400 - 324           =   76 mm^2
    //   with `\ S_k`  = 0.15 * 76 * 10                    =  114 mm^3
    //   without it    = 0.15 * 400 * 10                   =  600 mm^3
    // a factor of 5.26. The band below is centred on 114 and EXCLUDES 600 BY MORE THAN TWO TIMES:
    // an implementation that forgets the subtraction, or that subtracts only the first layer's
    // solid instead of every layer's, lands at 600 and fails here.
    const indexed_triangle_set part = cap_on_a_stem(10., 18.);
    const PartInvariants       inv  = precompute_mesh(part);
    REQUIRE(inv.valid);
    // 18*18*10 + 20*20*4 = 3240 + 1600 = 4840 mm^3, which confirms the fixture really is the wide
    // stem and not the narrow one.
    REQUIRE_THAT(inv.volume, WithinRel(4840., 1e-4));

    const ScoreParams  params;
    const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s.measured);
    REQUIRE(s.support_measured);
    // There is still a real overhang — the 1 mm rim — so this is not the trivially-zero case.
    REQUIRE(s.support_volume_mm3 > 40.);
    REQUIRE(s.support_volume_mm3 < 250.);
}

TEST_CASE("nocte plan: the support volume scales with the column height", "[NoctePlan]")
{
    // The same 384 mm^2 of unsupported ceiling in both fixtures; only the height it has to be
    // carried through changes, so the column volume changes with it and nothing else does:
    //   10 mm stem: 0.15 * 384 * 10 =  576 mm^3
    //   20 mm stem: 0.15 * 384 * 20 = 1152 mm^3
    //   ratio       1152 / 576      = 2
    //
    // A term that sums interface areas one layer thick returns 0.15 * 384 * 0.2 = 11.5 mm^3 for
    // BOTH fixtures — the very same number — so this is the assertion that names that bug outright
    // in its failure message, and it is stronger than any single-fixture band.
    //
    // 15% is allowed on the ratio rather than the 1e-6 used for the density scaling, because the
    // two carries run over 10 mm / 0.2 mm = 50 and 20 mm / 0.2 mm = 100 layers and an off-by-one
    // layer at either end moves the ratio between 99/51 = 1.94 and 101/49 = 2.06.
    const ScoreParams params;

    const indexed_triangle_set low  = cap_on_a_stem(10., 4.);
    const indexed_triangle_set high = cap_on_a_stem(20., 4.);

    const PartInvariants inv_low  = precompute_mesh(low);
    const PartInvariants inv_high = precompute_mesh(high);
    REQUIRE(inv_low.valid);
    REQUIRE(inv_high.valid);

    const OrientScores s_low  = evaluate(low, inv_low, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
    const OrientScores s_high = evaluate(high, inv_high, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(s_low.measured);
    REQUIRE(s_high.measured);
    REQUIRE(s_low.support_measured);
    REQUIRE(s_high.support_measured);
    // The fixtures differ only in the stem: 14 mm tall against 24 mm tall.
    REQUIRE_THAT(s_low.height_mm, WithinRel(14., 1e-6));
    REQUIRE_THAT(s_high.height_mm, WithinRel(24., 1e-6));

    REQUIRE(s_low.support_volume_mm3 > 250.);
    REQUIRE_THAT(s_high.support_volume_mm3, WithinRel(2. * s_low.support_volume_mm3, 0.15));
    // Said as a bare inequality as well, so a model that is blind to height fails on the plainer
    // line rather than only on the ratio.
    REQUIRE(s_high.support_volume_mm3 > 1.5 * s_low.support_volume_mm3);
}

TEST_CASE("nocte plan: the overhang threshold filters the tier 1 support region", "[NoctePlan]")
{
    // Why a sphere and not the cap on a stem: a horizontal ceiling sits at 90 deg from the vertical
    // and so passes ANY overhang threshold, so the cap-on-a-stem tests provably cannot tell a tier 1
    // that honours `overhang_threshold_deg` from one that supports every downward-facing facet
    // slice_mesh_slabs hands it. A sphere spans every angle continuously, so the threshold has
    // somewhere to bite. R = 10, resting on the bed, so the centre is at z = 10.
    //
    // At polar angle t from the BOTTOM pole the outward normal has |n.z| = cos t, so the NOCTE
    // overhang angle (asin|n.z|, Scores.hpp:13-16) is asin(cos t) = 90 - t. Support is needed where
    // 90 - t > threshold, i.e. on the cap t < tc = 90 - threshold. That cap reaches up to
    //   z_c  = R (1 - cos tc)      and projects to a disc of radius   r0 = R sin tc.
    // The sphere's own cross-section at height z has radius^2 = R^2 - (z - R)^2 = 2Rz - z^2, which
    // equals r0^2 exactly at z = z_c and SHRINKS below it, so the carried disc always lies outside
    // S_k and is never subtracted away: the column carry (Scores.hpp:40-63) keeps the full annulus
    // all the way down to the bed. Hence
    //   V = integral from 0 to z_c of pi * (r0^2 - (2Rz - z^2)) dz
    //     = pi * [ r0^2 * z_c - R * z_c^2 + z_c^3 / 3 ].
    //
    // R = 10, default threshold 45 deg -> tc = 45, cos45 = sin45 = 0.70711:
    //   z_c = 10 * (1 - 0.70711) = 2.92893,  z_c^2 = 8.57864,  z_c^3 = 25.12627,  r0^2 = 50
    //   V   = pi * (50*2.92893 - 10*8.57864 + 25.12627/3)
    //       = pi * (146.447 - 85.786 + 8.375) = pi * 69.036 = 216.88 mm^3
    //   sparse at support_density 0.15                      =  32.53 mm^3
    //
    // An UNFILTERED carry supports the whole lower hemisphere instead. Everything above z then
    // projects to the equatorial disc of radius R, so the region at z is pi*(R^2 - (2Rz - z^2))
    // = pi*(R - z)^2 and
    //   V = integral from 0 to R of pi (R - z)^2 dz = pi * R^3 / 3 = 1047.20 mm^3
    //   sparse at 0.15                                             =  157.08 mm^3
    // a factor of 4.83. THE UPPER BOUND OF THE BAND BELOW EXISTS TO CATCH AN UNFILTERED
    // slice_mesh_slabs OUTPUT: 157 is nowhere near 90.
    //
    // 32.5 is deliberately NOT pinned. At PI/12 the disc is a 24-gon (98.9% of the circle), the cap
    // boundary quantises to the 15 deg stack edges, and z_c = 2.93 mm is only 14.6 layers at 0.2 mm.
    const indexed_triangle_set ball = ball_on_the_bed(10.);
    const PartInvariants       inv  = precompute_mesh(ball);
    REQUIRE(inv.valid);
    REQUIRE_THAT(inv.bbox.min.z(), WithinAbs(0., 1e-5));

    ScoreParams params;
    REQUIRE_THAT(params.overhang_threshold_deg, WithinAbs(45., 1e-12));
    const OrientScores at_45 = evaluate(ball, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(at_45.measured);
    // A sphere on the bed genuinely needs support near the pole; a filter that threw everything
    // away would be just as wrong as one that kept everything.
    REQUIRE(at_45.support_volume_mm3 > 0.);
    REQUIRE(at_45.support_volume_mm3 > 10.);
    REQUIRE(at_45.support_volume_mm3 < 90.);

    // The half that does not lean on the absolute integral at all: raising the threshold can only
    // REMOVE facets from the support set, so the volume can only fall. The faceted cutoff lands on
    // a 15 deg stack edge, which is what the third column below accounts for:
    //   threshold 20 -> tc = 70, faceted cutoff t = 75: V = pi*277.90 = 873.06 mm^3, sparse 130.96
    //   threshold 45 -> tc = 45, cutoff exactly t = 45: V = pi* 69.04 = 216.88 mm^3, sparse  32.53
    //   threshold 70 -> tc = 20, faceted cutoff t = 15: V = pi*  1.13 =   3.56 mm^3, sparse   0.53
    // i.e. 4.03x up and 0.0164x down from the default. Asserted at 1.5x and 0.5x, two orders of
    // slack against the faceting error. A tier 1 that ignores the parameter returns the SAME number
    // for all three and fails both lines.
    params.overhang_threshold_deg = 20.;
    const OrientScores at_20 = evaluate(ball, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    params.overhang_threshold_deg = 70.;
    const OrientScores at_70 = evaluate(ball, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

    REQUIRE(at_20.support_volume_mm3 > 1.5 * at_45.support_volume_mm3);
    REQUIRE(at_70.support_volume_mm3 < 0.5 * at_45.support_volume_mm3);
    // `>= 0.` here would have been unfalsifiable. What is worth asserting is that the small number
    // at_70 reports is a measured small number and not a sweep that gave up.
    REQUIRE(at_70.support_measured);
    REQUIRE(at_20.support_measured);
}

TEST_CASE("nocte plan: support landing on a visible face is reported and flagged", "[NoctePlan]")
{
    // The cap underside is the overhang, and it is also a VISIBLE face: a ray leaving either of its
    // two facets along its own downward normal starts at (5.33, -1.33) or (-1.33, 5.33), both
    // outside the stem's [0,4]^2, so both escape the mesh and both are flagged visible. The support
    // column therefore lands on visible surface over the whole 400 - 16 = 384 mm^2 of it.
    //
    // Until now the only assertion on this field anywhere in the file was that a cube reports 0,
    // which exercises none of the visible-overhang path.
    const indexed_triangle_set part = cap_on_a_stem(10., 4.);

    SECTION("on the normal path the contact is real and flagged measured") {
        const PartInvariants inv = precompute_mesh(part);
        REQUIRE(inv.valid);
        REQUIRE(inv.visibility_available);

        const ScoreParams  params;
        const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

        REQUIRE(s.measured);
        REQUIRE(s.contact_measured);
        // Banded, not pinned: 384 mm^2 if the contact is taken from the carried region, 400 if from
        // the facets themselves, and the interface may be inset by a line width. Any of those is a
        // world away from the 0 this field reported before there was a fixture for it.
        REQUIRE(s.support_contact_mm2 > 100.);
        REQUIRE(s.support_contact_mm2 < 500.);
    }

    SECTION("with visibility switched off the zero is flagged, not reported") {
        // This is the case the flag exists for. Visibility is unavailable exactly when the user
        // cares most — a scanned mesh over the facet budget — and a bare 0 in support_contact_mm2
        // reads as "no support touches the showcase face", so the hard constraint would pass for
        // every candidate, silently, on the part where it matters.
        PrecomputeParams pre;
        pre.measure_visibility = false;
        const PartInvariants inv = precompute(part, pre);
        REQUIRE(inv.valid);
        REQUIRE_FALSE(inv.visibility_available);

        const ScoreParams  params;
        const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

        REQUIRE_FALSE(s.contact_measured);
        // `contact_measured` is false on a default-constructed OrientScores, so the line above also
        // passes for a scorer that never ran at all. These are what show the rest of the scoring
        // DID run and that only the contact half was withheld: the support sweep is unaffected by
        // visibility, and the cusp term reads an absent visibility array as "every facet visible"
        // (PartInvariants.hpp:30-31) and so still measures.
        REQUIRE(s.measured);
        REQUIRE(s.support_measured);
        REQUIRE(s.support_volume_mm3 > 250.);
        REQUIRE(s.cusp_measured);
        REQUIRE_THAT(s.footprint_area_mm2, WithinRel(16., 1e-3));
    }

    SECTION("tier 0 never computes contact at all") {
        const PartInvariants inv = precompute_mesh(part);
        ScoreParams          params;
        params.support_tier = SupportTier::FacetSweep;
        const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);

        // Documented in the header: tier 0 never computes contact at all, so it always reports
        // false here, however good the visibility data it was handed.
        REQUIRE(inv.visibility_available);
        REQUIRE_FALSE(s.contact_measured);
        // Same trap as above — false is also the initialiser. These show tier 0 really did score
        // the part and withheld only the contact: it produced a support volume (it overstates, but
        // it produces one), it measured the cusp, and it found the 16 mm^2 stem footprint.
        REQUIRE(s.measured);
        REQUIRE(s.support_measured);
        REQUIRE(s.support_volume_mm3 > 0.);
        REQUIRE(s.cusp_measured);
        REQUIRE_THAT(s.footprint_area_mm2, WithinRel(16., 1e-3));
    }
}

// `support_measured == false` IS NOT EXERCISED, and the attempts are recorded here rather than
// replaced by an assertion that passes without proving anything.
//
// Every test in this file asserts the flag TRUE, so an implementation that hard-codes it to true
// passes all of them. The only way `evaluate` can set it false is for the support sweep to throw,
// and the only lever we found is ClipperLib's coordinate ceiling: `AddPath` calls RangeTest on
// every point and throws above hiRange = 0x3FFFFFFFFFFFFFFF = 4.6116860e18 scaled units. The range
// test is live in Release for the int64 build (clipper.cpp:603-615, with no NDEBUG guard, unlike
// the int32 variant at :595). scale_() multiplies millimetres by 1e6, so the ceiling is 4.6116860e12
// mm and int64 itself caps at 9.2233720e12 mm. 6.9e12 mm sits between them.
//
// Two fixtures at that magnitude, two CI runs, and the sweep completed cleanly both times:
//
//   1. A plain box. Its only downward-facing surface is its base, which lands in `downward[0]`, and
//      the carry structurally never unions `downward[0]` — the part rests on the bed there. So the
//      region stayed empty, `diff(empty, solid)` short-circuited, and Clipper was never handed a
//      non-empty path. The coordinates were absurd and idle.
//   2. A cap on a stem, both plates huge, the cap underside a 0.75 s^2 overhang one millimetre up so
//      that it lands in `downward[k+1]` for k >= 1 and `union_` must range-test it. Still no throw.
//
// Why the second one did not throw is unresolved: it means the polygons are not reaching Clipper
// with those coordinates, and finding out where they are lost needs a trip into the slicer that a
// blind third build does not justify.
//
// Read the right thing into this. The flag is correct defensive design and the difficulty of
// tripping it is good news about how hard the sweep is to break — not evidence the flag is
// unnecessary. It stays. What is missing is only the regression test that would catch someone
// hard-coding it, and that gap is stated in docs/NOCTE-STATUS.md rather than papered over.

TEST_CASE("nocte plan: the support tier falls back from FullDetect", "[NoctePlan]")
{
    // SupportTier::FullDetect needs a PrintObject at posSlice, which this header cannot reach, so a
    // result must never claim it. The tier is carried in the result precisely so tier 0 is never
    // presented to the user in grams.
    const indexed_triangle_set part = cap_on_a_stem(10., 4.);
    const PartInvariants       inv  = precompute_mesh(part);
    REQUIRE(inv.valid);

    SECTION("with the default tier") {
        const ScoreParams  params;
        const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.support_tier != SupportTier::FullDetect);
    }

    SECTION("when FullDetect is asked for anyway") {
        ScoreParams params;
        params.support_tier = SupportTier::FullDetect;
        const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.support_tier != SupportTier::FullDetect);
    }

    SECTION("the cheap tier is still a number") {
        ScoreParams params;
        params.support_tier = SupportTier::FacetSweep;
        const OrientScores s = evaluate(part, inv, Transform3d::Identity(), params, Vec3d::Zero(), 0.);
        REQUIRE(s.support_tier != SupportTier::FullDetect);
        REQUIRE(s.support_volume_mm3 > 0.);
    }
}

// --- combine() ---------------------------------------------------------------------------------

TEST_CASE("nocte plan: an empty candidate set combines to nothing", "[NoctePlan]")
{
    const std::vector<OrientScores>    none;
    const ScoreWeights                 weights;
    const ScoreEpsilons                eps;
    std::vector<std::array<double, 5>> terms;

    std::vector<double> scores;
    REQUIRE_NOTHROW(scores = combine(none, weights, eps, nullptr));
    REQUIRE(scores.empty());

    REQUIRE_NOTHROW(scores = combine(none, weights, eps, &terms));
    REQUIRE(scores.empty());
    REQUIRE(terms.empty());
}

TEST_CASE("nocte plan: differences below the indifference floor do not decide", "[NoctePlan]")
{
    // ScoreEpsilons::support_mm3 is 500 mm^3 by default, and the whole preference is on support, so
    // the combined score IS the normalised support term.
    const ScoreWeights  weights = support_only_weights();
    const ScoreEpsilons eps;
    REQUIRE_THAT(eps.support_mm3, WithinAbs(500., 1e-12));

    // 1200 - 1000 = 200 mm^3, below the 500 mm^3 floor: these two are the same print.
    // Plain min-max normalisation would map them to 0 and 1 and separate them by the full 1.0,
    // which is the bug. Under the floor the separation is at most 200 / 500 = 0.4.
    std::vector<OrientScores>          close;
    std::vector<std::array<double, 5>> close_terms;
    close.push_back(make_candidate(1000., 0.1, 600., 5000., 1.));
    close.push_back(make_candidate(1200., 0.1, 600., 5000., 1.));
    const std::vector<double> close_scores = combine(close, weights, eps, &close_terms);
    REQUIRE(close_scores.size() == close.size());
    REQUIRE(close_terms.size() == close.size());
    const double close_gap = std::abs(close_scores[0] - close_scores[1]);
    REQUIRE(close_gap <= 0.5);
    REQUIRE(close_gap < 1.);

    // Field order is (support, cusp, time, strength, stability). Four of the five weights are 0, so
    // those columns are 0 in every row, and the support column stays inside [0, 1].
    for (const std::array<double, 5> &row : close_terms) {
        REQUIRE(row[0] >= -1e-12);
        REQUIRE(row[0] <= 1. + 1e-12);
        REQUIRE_THAT(row[1], WithinAbs(0., 1e-12));
        REQUIRE_THAT(row[2], WithinAbs(0., 1e-12));
        REQUIRE_THAT(row[3], WithinAbs(0., 1e-12));
        REQUIRE_THAT(row[4], WithinAbs(0., 1e-12));
    }

    // 3000 - 1000 = 2000 mm^3, four times the floor: this one IS a decision, and it must still span
    // the full range. Without this half the test would pass on a combine() that returns a constant.
    std::vector<OrientScores>          apart;
    std::vector<std::array<double, 5>> apart_terms;
    apart.push_back(make_candidate(1000., 0.1, 600., 5000., 1.));
    apart.push_back(make_candidate(3000., 0.1, 600., 5000., 1.));
    const std::vector<double> apart_scores = combine(apart, weights, eps, &apart_terms);
    REQUIRE(apart_scores.size() == apart.size());
    REQUIRE(apart_terms.size() == apart.size());
    const double apart_gap = std::abs(apart_scores[0] - apart_scores[1]);
    REQUIRE(apart_gap > 0.5);
    // Less support is better, so the 1000 mm^3 candidate scores lower.
    REQUIRE(apart_scores[0] < apart_scores[1]);
    // Here the range really does exceed the floor, so the best candidate on the term normalises to
    // 0, and with the whole weight on support and the other four columns at 0, its score is 0 too.
    REQUIRE_THAT(apart_terms[0][0], WithinAbs(0., 1e-9));
    REQUIRE_THAT(apart_scores[0], WithinAbs(0., 1e-9));
}

TEST_CASE("nocte plan: a single candidate scores without dividing by a zero range", "[NoctePlan]")
{
    // One candidate means every term has a range of zero. The min-max normalisation must not divide
    // by it.
    std::vector<OrientScores> one;
    one.push_back(make_candidate(1234., 0.12, 900., 7500., 1.4));

    const ScoreWeights                 weights;
    const ScoreEpsilons                eps;
    std::vector<std::array<double, 5>> terms;

    std::vector<double> scores;
    REQUIRE_NOTHROW(scores = combine(one, weights, eps, &terms));
    REQUIRE(scores.size() == static_cast<size_t>(1));
    REQUIRE(std::isfinite(scores[0]));
    REQUIRE(terms.size() == scores.size());
    // s_hat = (s - s_min) / max(s_max - s_min, eps). With one candidate s == s_min, so the
    // NUMERATOR is 0 and s_hat is 0 whatever the clamped denominator turns out to be — which is
    // the point of clamping it: this is 0 / eps, never 0 / 0. Every term is 0, and since the
    // weights sum to 1 the combined score is 0 as well.
    for (double t : terms[0])
        REQUIRE_THAT(t, WithinAbs(0., 1e-9));
    REQUIRE_THAT(scores[0], WithinAbs(0., 1e-9));
}

TEST_CASE("nocte plan: a term with weight zero does not influence the result", "[NoctePlan]")
{
    // Three candidates identical except for the time, whose spread (100 s to 3000 s) is far past
    // ScoreEpsilons::time_s = 60 s, so the floor cannot be what flattens them. With the time weight
    // at 0 the term is dropped entirely and all three scores must be the same number.
    std::vector<OrientScores> candidates;
    candidates.push_back(make_candidate(2000., 0.12, 100., 7500., 1.4));
    candidates.push_back(make_candidate(2000., 0.12, 600., 7500., 1.4));
    candidates.push_back(make_candidate(2000., 0.12, 3000., 7500., 1.4));

    ScoreWeights weights;
    weights.support   = 0.25;
    weights.cusp      = 0.25;
    weights.time      = 0.;
    weights.strength  = 0.25;
    weights.stability = 0.25;
    REQUIRE_THAT(weights.sum(), WithinAbs(1., 1e-12));

    const ScoreEpsilons eps;
    REQUIRE_THAT(eps.time_s, WithinAbs(60., 1e-12));

    std::vector<std::array<double, 5>> terms;
    const std::vector<double>          scores = combine(candidates, weights, eps, &terms);
    REQUIRE(scores.size() == candidates.size());
    REQUIRE(terms.size() == candidates.size());
    REQUIRE(std::isfinite(scores[0]));
    REQUIRE_THAT(scores[1], WithinAbs(scores[0], 1e-12));
    REQUIRE_THAT(scores[2], WithinAbs(scores[0], 1e-12));

    // Field order is (support, cusp, time, strength, stability), so the time term is column 2. A
    // term whose weight is zero contributes 0 to every row, however far apart the three times are.
    for (const std::array<double, 5> &row : terms)
        REQUIRE_THAT(row[2], WithinAbs(0., 1e-12));

    // THE CONTROL. Everything above is also true of a combine() that returns zeros for everything,
    // so on its own it does not show that the zero WEIGHT is what flattened the scores. Give the
    // same three candidates a non-zero time weight and the same term has to come alive.
    //
    // Five weights of 0.2 summing to 1. The other four terms are identical across the candidates,
    // so their ranges are 0 and, by s_hat = (s - s_min)/max(range, eps), every one of them is 0 for
    // every candidate. Only time moves, and its range 3000 - 100 = 2900 s is far past the 60 s
    // floor, so the term spans the full [0, 1]:
    //   100 s  -> s_hat = 0            -> score = 0.2 * 0 = 0
    //   3000 s -> s_hat = 1            -> score = 0.2 * 1 = 0.2
    // The middle candidate is deliberately not pinned: 600 s lands on 500/2900 = 0.1724 under a
    // clamped denominator and 500/2840 = 0.1761 under one that subtracts the floor, and the header
    // does not choose between them.
    ScoreWeights even;
    even.support   = 0.2;
    even.cusp      = 0.2;
    even.time      = 0.2;
    even.strength  = 0.2;
    even.stability = 0.2;
    REQUIRE_THAT(even.sum(), WithinAbs(1., 1e-12));

    std::vector<std::array<double, 5>> even_terms;
    const std::vector<double>          even_scores = combine(candidates, even, eps, &even_terms);
    REQUIRE(even_scores.size() == candidates.size());
    REQUIRE(even_terms.size() == candidates.size());
    REQUIRE(even_scores[0] < even_scores[2]);
    REQUIRE_THAT(even_scores[0], WithinAbs(0., 1e-9));
    REQUIRE_THAT(even_scores[2], WithinAbs(0.2, 1e-9));
    // And the time column is now the thing carrying that difference.
    REQUIRE_THAT(even_terms[0][2], WithinAbs(0., 1e-9));
    REQUIRE_THAT(even_terms[2][2], WithinAbs(1., 1e-9));
}

TEST_CASE("nocte plan: weights normalise to one", "[NoctePlan]")
{
    SECTION("the shipped defaults already sum to one") {
        const ScoreWeights w;
        // 0.25 + 0.25 + 0.20 + 0.15 + 0.15 = 1.00
        REQUIRE_THAT(w.sum(), WithinAbs(1., 1e-12));
        const ScoreWeights n = w.normalized();
        REQUIRE_THAT(n.sum(), WithinAbs(1., 1e-12));
        REQUIRE_THAT(n.support, WithinAbs(0.25, 1e-12));
        REQUIRE_THAT(n.time, WithinAbs(0.20, 1e-12));
    }

    SECTION("an unnormalised set is scaled, not clamped") {
        ScoreWeights w;
        w.support   = 2.;
        w.cusp      = 2.;
        w.time      = 2.;
        w.strength  = 2.;
        w.stability = 2.;
        REQUIRE_THAT(w.sum(), WithinAbs(10., 1e-12));
        const ScoreWeights n = w.normalized();
        // Each 2 / 10 = 0.2, and the ratios between the terms are unchanged.
        REQUIRE_THAT(n.sum(), WithinAbs(1., 1e-12));
        REQUIRE_THAT(n.support, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.cusp, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.time, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.strength, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.stability, WithinAbs(0.2, 1e-12));
    }

    SECTION("an all-zero set falls back to equal weights") {
        ScoreWeights w;
        w.support   = 0.;
        w.cusp      = 0.;
        w.time      = 0.;
        w.strength  = 0.;
        w.stability = 0.;
        REQUIRE_THAT(w.sum(), WithinAbs(0., 1e-12));
        const ScoreWeights n = w.normalized();
        // Five terms sharing 1: 1 / 5 = 0.2 each. Anything else is a division by zero.
        REQUIRE_THAT(n.sum(), WithinAbs(1., 1e-12));
        REQUIRE_THAT(n.support, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.cusp, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.time, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.strength, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(n.stability, WithinAbs(0.2, 1e-12));
    }
}

TEST_CASE("nocte plan: lower is better and a dominating candidate wins", "[NoctePlan]")
{
    // Candidate 0 is better on all five terms, and every gap clears its own indifference floor:
    //   support   10000 - 0    = 10000 mm^3 > 500 mm^3
    //   cusp      0.30 - 0.05  = 0.25 mm    > 0.02 mm
    //   time      5000 - 100   = 4900 s     > 60 s
    //   strength  20000 - 1000 = 19000 N    > 0.05 * 20000 = 1000 N
    //   stability 2.0 - 0.2    = 1.8        > 0.1
    // Scores.hpp:200-203 names TWO more-is-better terms that are inverted inside, failure_force_n
    // and stability, so candidate 0's higher force AND its higher tipping ratio both have to push
    // its score DOWN. An engine that forgot to invert stability would prefer the tippy candidate.
    std::vector<OrientScores> candidates;
    candidates.push_back(make_candidate(0., 0.05, 100., 20000., 2.0));
    candidates.push_back(make_candidate(10000., 0.30, 5000., 1000., 0.2));

    const ScoreWeights                 weights;
    const ScoreEpsilons                eps;
    std::vector<std::array<double, 5>> terms;

    const std::vector<double> scores = combine(candidates, weights, eps, &terms);
    REQUIRE(scores.size() == candidates.size());
    REQUIRE(terms.size() == candidates.size());
    REQUIRE(scores[0] < scores[1]);

    // Candidate 0 is the best in the set on all five terms and every gap clears its floor, so by
    // the [0, 1] contract each of its five normalised terms is 0, and since the weights sum to 1
    // its combined score is 0.25*0 + 0.25*0 + 0.20*0 + 0.15*0 + 0.15*0 = 0.
    for (double t : terms[0])
        REQUIRE_THAT(t, WithinAbs(0., 1e-9));
    REQUIRE_THAT(scores[0], WithinAbs(0., 1e-9));
    // Candidate 1 is the worst on all five, so it sits at the far end of every range: 1.
    REQUIRE(scores[1] > 0.5);

    // Every value handed back for the panel is a bar height in [0, 1].
    for (const std::array<double, 5> &row : terms)
        for (double t : row) {
            REQUIRE(t >= -1e-12);
            REQUIRE(t <= 1. + 1e-12);
        }
}
