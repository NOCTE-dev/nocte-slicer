// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/GeometryAnalysis.hpp"

using namespace Slic3r;
using namespace Slic3r::Nocte;

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// its_make_cube() puts the box in the first octant, so a cube of side s spans [0, s] on every axis
// and its bottom face lies exactly on the bed plane.
PartFeatures analyze_mesh(const indexed_triangle_set &its)
{
    const AnalysisParams    params;
    const DynamicPrintConfig ctx;
    return analyze(its, params, ctx);
}

// A 20 mm square cap carried on a 4 mm square stem. The underside of the cap is a horizontal
// downward-facing surface 10 mm above the bed: a 90 deg overhang that is not bed contact.
indexed_triangle_set cube_on_a_stem()
{
    indexed_triangle_set mesh = its_make_cube(4., 4., 10.);
    indexed_triangle_set cap  = its_make_cube(20., 20., 4.);
    its_translate(cap, Vec3f(-8.f, -8.f, 10.f));
    its_merge(mesh, cap);
    return mesh;
}

} // namespace

TEST_CASE("nocte analysis: a cube measures its own volume, area and footprint", "[NocteAnalysis]")
{
    const PartFeatures f = analyze_mesh(its_make_cube(20., 20., 20.));

    REQUIRE(analysis_available());

    REQUIRE_THAT(f.volume, WithinRel(8000., 1e-4));
    REQUIRE_THAT(f.surface_area, WithinRel(2400., 1e-4));
    REQUIRE_THAT(f.hull_volume, WithinRel(8000., 1e-3));
    REQUIRE_THAT(f.solidity, WithinRel(1., 1e-3));
    REQUIRE(f.shell_count == 1);

    REQUIRE_THAT(f.bbox_size.x(), WithinRel(20., 1e-6));
    REQUIRE_THAT(f.bbox_size.y(), WithinRel(20., 1e-6));
    REQUIRE_THAT(f.bbox_size.z(), WithinRel(20., 1e-6));

    REQUIRE_THAT(f.center_of_mass.x(), WithinAbs(10., 1e-3));
    REQUIRE_THAT(f.center_of_mass.y(), WithinAbs(10., 1e-3));
    REQUIRE_THAT(f.center_of_mass.z(), WithinAbs(10., 1e-3));

    // The whole bottom face is first-layer contact; nothing else faces down.
    REQUIRE_THAT(f.footprint_area, WithinRel(400., 1e-4));
    // sqrt(400) / 10 mm centre-of-mass height.
    REQUIRE_THAT(f.stability_ratio, WithinRel(2., 1e-3));
    REQUIRE_THAT(f.tall_thin_ratio, WithinRel(1., 1e-6));
}

TEST_CASE("nocte analysis: a cube has no overhangs", "[NocteAnalysis]")
{
    const PartFeatures f = analyze_mesh(its_make_cube(20., 20., 20.));

    // The bottom face faces down but rests on the bed, so it is first-layer contact rather than an
    // overhang. Every other facet is vertical or upward-facing.
    REQUIRE_THAT(f.max_overhang_angle, WithinAbs(0., 1e-6));
    REQUIRE_THAT(f.overhang_area_fraction, WithinAbs(0., 1e-9));
    REQUIRE_THAT(f.steep_overhang_area, WithinAbs(0., 1e-9));
}

TEST_CASE("nocte analysis: a cube measures its wall thickness and cross section", "[NocteAnalysis]")
{
    const AnalysisParams params;
    const PartFeatures   f = analyze_mesh(its_make_cube(20., 20., 20.));

    // Every inward ray crosses the full 20 mm of solid before it reaches the opposite wall.
    REQUIRE_THAT(f.p05_wall_thickness, WithinRel(20., 0.02));
    REQUIRE_THAT(f.min_wall_thickness, WithinRel(20., 0.02));
    REQUIRE(f.thickness_hist.size() == size_t(params.thickness_bins));

    // Every layer of a cube is the same 20 x 20 square.
    REQUIRE_THAT(f.min_cross_section_area, WithinRel(400., 1e-3));
}

TEST_CASE("nocte analysis: a thin plate reports its thickness", "[NocteAnalysis]")
{
    const PartFeatures f = analyze_mesh(its_make_cube(20., 20., 0.6));

    REQUIRE_THAT(f.min_wall_thickness, WithinAbs(0.6, 0.02));
    REQUIRE_THAT(f.p05_wall_thickness, WithinAbs(0.6, 0.02));
    REQUIRE_THAT(f.volume, WithinRel(240., 1e-4));
    REQUIRE_THAT(f.footprint_area, WithinRel(400., 1e-4));
}

TEST_CASE("nocte analysis: a cube on a stem reports a horizontal overhang", "[NocteAnalysis]")
{
    const PartFeatures f = analyze_mesh(cube_on_a_stem());

    // The underside of the cap is horizontal: 90 deg from the vertical.
    REQUIRE_THAT(f.max_overhang_angle, WithinAbs(90., 1e-3));
    REQUIRE(f.overhang_area_fraction > 0.);
    // The whole 20 x 20 underside is steeper than 70 deg.
    REQUIRE_THAT(f.steep_overhang_area, WithinRel(400., 1e-3));

    // Only the stem touches the bed.
    REQUIRE_THAT(f.footprint_area, WithinRel(16., 1e-4));
    REQUIRE_THAT(f.volume, WithinRel(1760., 1e-4));
    REQUIRE(f.shell_count == 2);

    // The part is far from convex: the hull fills in everything around the stem.
    REQUIRE(f.solidity < 0.8);
    REQUIRE(f.solidity > 0.2);

    // The narrowest layer is the 4 x 4 stem.
    REQUIRE(f.min_cross_section_area > 0.);
    REQUIRE(f.min_cross_section_area < 20.);
}

TEST_CASE("nocte analysis: a tall pillar is tall against its footprint", "[NocteAnalysis]")
{
    const PartFeatures f = analyze_mesh(its_make_cube(5., 5., 60.));

    REQUIRE_THAT(f.tall_thin_ratio, WithinRel(12., 1e-6));
    // sqrt(25) / 30 mm centre-of-mass height.
    REQUIRE_THAT(f.stability_ratio, WithinRel(5. / 30., 1e-3));
    REQUIRE_THAT(f.footprint_area, WithinRel(25., 1e-4));
}

TEST_CASE("nocte analysis: an empty mesh yields zeroed features", "[NocteAnalysis]")
{
    const indexed_triangle_set empty;
    const PartFeatures         f = analyze_mesh(empty);

    REQUIRE_THAT(f.volume, WithinAbs(0., 1e-12));
    REQUIRE_THAT(f.surface_area, WithinAbs(0., 1e-12));
    REQUIRE_THAT(f.hull_volume, WithinAbs(0., 1e-12));
    REQUIRE_THAT(f.solidity, WithinAbs(0., 1e-12));
    REQUIRE_THAT(f.footprint_area, WithinAbs(0., 1e-12));
    REQUIRE_THAT(f.p05_wall_thickness, WithinAbs(0., 1e-12));
    REQUIRE_THAT(f.min_cross_section_area, WithinAbs(0., 1e-12));
    REQUIRE(f.shell_count == 0);
    REQUIRE(f.thickness_hist.empty());
}

TEST_CASE("nocte analysis: a mesh with no enclosed volume does not crash", "[NocteAnalysis]")
{
    // A single facet: no volume, no hull, no thickness to measure.
    indexed_triangle_set sheet;
    sheet.vertices.push_back(stl_vertex(0.f, 0.f, 0.f));
    sheet.vertices.push_back(stl_vertex(10.f, 0.f, 0.f));
    sheet.vertices.push_back(stl_vertex(0.f, 10.f, 0.f));
    sheet.indices.push_back(stl_triangle_vertex_indices(0, 1, 2));

    const PartFeatures f = analyze_mesh(sheet);

    REQUIRE_THAT(f.volume, WithinAbs(0., 1e-6));
    REQUIRE_THAT(f.surface_area, WithinRel(50., 1e-4));
    // The tetrahedra collapse, so the centre of mass falls back to the bounding-box centre.
    REQUIRE_THAT(f.center_of_mass.z(), WithinAbs(0., 1e-6));
    // A flat input is never handed to qhull.
    REQUIRE_THAT(f.hull_volume, WithinAbs(0., 1e-12));
    REQUIRE_THAT(f.solidity, WithinAbs(0., 1e-12));
}

// The overhang angle is measured from the vertical, which is the complement of Orca's
// support_threshold_angle. A facet that is exactly at the threshold is not an overhang.
TEST_CASE("nocte analysis: the overhang threshold selects which facets count", "[NocteAnalysis]")
{
    const indexed_triangle_set mesh = cube_on_a_stem();

    AnalysisParams       params;
    const DynamicPrintConfig ctx;

    params.overhang_threshold = 45.;
    const PartFeatures at_45 = analyze(mesh, params, ctx);

    // Raised just past the steepest facet on the part, nothing is counted any more.
    params.overhang_threshold = at_45.max_overhang_angle + 1.;
    const PartFeatures above_all = analyze(mesh, params, ctx);

    REQUIRE(at_45.overhang_area_fraction > 0.);
    REQUIRE_THAT(above_all.overhang_area_fraction, WithinAbs(0., 1e-9));
    // max_overhang_angle is a property of the mesh, not of the threshold.
    REQUIRE_THAT(above_all.max_overhang_angle, WithinAbs(at_45.max_overhang_angle, 1e-9));
}
