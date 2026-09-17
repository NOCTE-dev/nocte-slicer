// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include <algorithm>
#include <map>

#include <catch2/catch_all.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/MeshDiagnostics.hpp"
#include "libslic3r/Nocte/MeshRepair.hpp"

using namespace Slic3r;
using namespace Slic3r::Nocte;

namespace {

indexed_triangle_set cube_with_top_face_removed(double size)
{
    indexed_triangle_set cube = its_make_cube(size, size, size);
    const float          top  = float(size);
    auto on_top = [&cube, top](const stl_triangle_vertex_indices &tri) {
        for (int i = 0; i < 3; ++ i)
            if (cube.vertices[tri(i)].z() < top)
                return false;
        return true;
    };
    cube.indices.erase(std::remove_if(cube.indices.begin(), cube.indices.end(), on_top), cube.indices.end());
    return cube;
}

indexed_triangle_set cube_with_degenerate_facet(double size)
{
    indexed_triangle_set cube = its_make_cube(size, size, size);
    cube.indices.push_back(stl_triangle_vertex_indices(0, 0, 1));
    return cube;
}

// A closed cube whose top two facets were given a private copy of the four top vertices. No vertex
// moved, but the cap no longer shares an edge with the sides: eight open edges, sitting exactly on
// top of each other in pairs. Merging the vertices would close it; so should stitching the borders.
indexed_triangle_set cube_with_unmerged_top_cap(double size)
{
    indexed_triangle_set cube = its_make_cube(size, size, size);
    const float          top  = float(size);

    std::map<int, int> copy_of;
    for (stl_triangle_vertex_indices &tri : cube.indices) {
        bool on_top = true;
        for (int i = 0; i < 3; ++ i)
            if (cube.vertices[tri(i)].z() < top)
                on_top = false;
        if (! on_top)
            continue;
        for (int i = 0; i < 3; ++ i) {
            std::map<int, int>::const_iterator it = copy_of.find(tri(i));
            if (it == copy_of.end()) {
                const stl_vertex position = cube.vertices[tri(i)];
                const int        fresh    = int(cube.vertices.size());
                cube.vertices.push_back(position);
                it = copy_of.emplace(tri(i), fresh).first;
            }
            tri(i) = it->second;
        }
    }
    return cube;
}

indexed_triangle_set inside_out_cube(double size)
{
    indexed_triangle_set cube = its_make_cube(size, size, size);
    its_flip_triangles(cube);
    return cube;
}

} // namespace

TEST_CASE("nocte repair: filling the hole in a cube with a missing face closes it", "[NocteRepair]")
{
    RepairSession     session(cube_with_top_face_removed(10.));
    const RepairPlan &plan = session.plan();

    // Steps come out ordered by tier, so the cheap lossless ones are offered first.
    for (size_t i = 1; i < plan.size(); ++ i)
        REQUIRE(plan.ops[i - 1].tier <= plan.ops[i].tier);

    const int index = plan.index_of(RepairOpKind::FillHolesCgal);
    REQUIRE(index >= 0);

    const RepairOp &op = plan.ops[size_t(index)];
    REQUIRE(op.tier == 1);
    REQUIRE(op.implemented);
    REQUIRE_FALSE(op.lossy);
    REQUIRE(op.targets_issue_ids.size() == 1);
    REQUIRE(op.targets_issue_ids.front() == "open-loop-1");
    REQUIRE_THAT(op.rationale, Catch::Matchers::ContainsSubstring("open-loop-1"));

    const RepairStepResult result = session.accept(size_t(index));
    REQUIRE(result.succeeded);
    REQUIRE(result.before.open_edges == 4);
    REQUIRE(result.after.open_edges == 0);
    // CGAL is free to triangulate the square hole however it likes, so assert the topology and the
    // volume rather than a facet count.
    REQUIRE(result.after.tris >= 12);
    REQUIRE_THAT(result.after.volume, Catch::Matchers::WithinRel(1000., 1e-4));
    REQUIRE_FALSE(result.message.empty());

    REQUIRE(session.state_of(size_t(index)) == RepairStepState::Applied);
    REQUIRE(session.diagnostics().manifold);
    REQUIRE_FALSE(session.diagnostics().has(IssueKind::OpenBoundaryLoop));
}

TEST_CASE("nocte repair: undo after an accepted CGAL step restores the mesh it started from", "[NocteRepair]")
{
    RepairSession session(cube_with_top_face_removed(10.));

    const int index = session.plan().index_of(RepairOpKind::FillHolesCgal);
    REQUIRE(index >= 0);
    REQUIRE(session.accept(size_t(index)).succeeded);
    REQUIRE(session.can_undo());
    REQUIRE(session.diagnostics().open_edges == 0);

    session.undo();

    REQUIRE(session.current().indices.size() == 10);
    REQUIRE(session.diagnostics().open_edges == 4);
    REQUIRE(session.diagnostics().has(IssueKind::OpenBoundaryLoop));
    REQUIRE(session.state_of(size_t(index)) == RepairStepState::Pending);
    REQUIRE_FALSE(session.can_undo());
}

TEST_CASE("nocte repair: the tier-2 rebuild is still offered but cannot run in this build", "[NocteRepair]")
{
    RepairSession session(cube_with_top_face_removed(10.));

    const int index = session.plan().index_of(RepairOpKind::NormalizeManifold);
    REQUIRE(index >= 0);
    REQUIRE(session.plan().ops[size_t(index)].tier == 2);
    REQUIRE_FALSE(session.plan().ops[size_t(index)].implemented);

    const RepairStepResult result = session.accept(size_t(index));
    REQUIRE_FALSE(result.succeeded);
    REQUIRE_THAT(result.message, Catch::Matchers::ContainsSubstring("not implemented"));
    REQUIRE(session.state_of(size_t(index)) == RepairStepState::Failed);
    // A step that fails changes nothing and leaves nothing to undo.
    REQUIRE_FALSE(session.can_undo());
    REQUIRE(session.current().indices.size() == 10);
}

TEST_CASE("nocte repair: an inside-out cube is turned the right way round", "[NocteRepair]")
{
    const indexed_triangle_set mesh = inside_out_cube(10.);
    REQUIRE(its_volume(mesh) < 0.f);

    RepairSession session(mesh);
    REQUIRE(session.diagnostics().has(IssueKind::InvertedShell));

    const int index = session.plan().index_of(RepairOpKind::FlipShell);
    REQUIRE(index >= 0);
    REQUIRE(session.plan().ops[size_t(index)].tier == 1);
    REQUIRE(session.plan().ops[size_t(index)].implemented);
    REQUIRE_FALSE(session.plan().ops[size_t(index)].lossy);

    const RepairStepResult result = session.accept(size_t(index));
    REQUIRE(result.succeeded);
    // Flipping swaps two indices per facet: the facet count cannot change.
    REQUIRE(result.after.tris == 12);
    REQUIRE(result.touched_faces.size() == 12);
    REQUIRE_THAT(result.after.volume, Catch::Matchers::WithinRel(1000., 1e-4));
    REQUIRE_FALSE(session.diagnostics().has(IssueKind::InvertedShell));
    REQUIRE(session.diagnostics().issues.empty());
}

TEST_CASE("nocte repair: stitching borders closes a cube whose top cap was never welded on", "[NocteRepair]")
{
    const indexed_triangle_set mesh = cube_with_unmerged_top_cap(10.);
    // Four edges around the opening and four around the cap, each pair sitting on the same segment.
    REQUIRE(its_num_open_edges(mesh) == 8);

    RepairSession session(mesh);

    // Merging the vertices would close it too. The point of this test is the other route, so that
    // step is turned down.
    const int merge_index = session.plan().index_of(RepairOpKind::MergeVertices);
    if (merge_index >= 0)
        session.reject(size_t(merge_index));

    const int index = session.plan().index_of(RepairOpKind::StitchBorders);
    REQUIRE(index >= 0);
    REQUIRE(session.plan().ops[size_t(index)].implemented);

    const RepairStepResult result = session.accept(size_t(index));
    REQUIRE(result.succeeded);
    REQUIRE(result.before.open_edges == 8);
    // CGAL only sews borders that already line up; how many it finds is its business, but it has to
    // find some.
    REQUIRE(result.after.open_edges < result.before.open_edges);
    REQUIRE_THAT(result.after.volume, Catch::Matchers::WithinRel(1000., 1e-4));
}

TEST_CASE("nocte repair: splitting non-manifold vertices is offered and runs", "[NocteRepair]")
{
    // Two cubes that share one corner vertex: the facets around it fall into two fans.
    indexed_triangle_set mesh   = its_make_cube(10., 10., 10.);
    indexed_triangle_set second = its_make_cube(10., 10., 10.);
    its_translate(second, Vec3f(10.f, 10.f, 10.f));
    const int first_cube_vertices = int(mesh.vertices.size());
    its_merge(mesh, second);

    int keep = -1;
    int drop = -1;
    for (int v = 0; v < int(mesh.vertices.size()); ++ v) {
        const stl_vertex &p = mesh.vertices[v];
        if (p.x() != 10.f || p.y() != 10.f || p.z() != 10.f)
            continue;
        if (v < first_cube_vertices)
            keep = v;
        else
            drop = v;
    }
    REQUIRE(keep >= 0);
    REQUIRE(drop >= 0);
    for (stl_triangle_vertex_indices &tri : mesh.indices)
        for (int i = 0; i < 3; ++ i)
            if (tri(i) == drop)
                tri(i) = keep;

    RepairSession session(mesh);
    REQUIRE(session.diagnostics().has(IssueKind::NonManifoldVertex));

    const int index = session.plan().index_of(RepairOpKind::DuplicateNonManifoldVertices);
    REQUIRE(index >= 0);
    REQUIRE(session.plan().ops[size_t(index)].implemented);
    REQUIRE_THAT(session.plan().ops[size_t(index)].rationale,
                 Catch::Matchers::ContainsSubstring("nonmanifold-vertices-1"));

    const RepairStepResult result = session.accept(size_t(index));
    REQUIRE(result.succeeded);
    REQUIRE_FALSE(session.diagnostics().has(IssueKind::NonManifoldVertex));
    // The pinch is gone and neither cube lost anything: 24 facets and 12 faces of 100 mm² still
    // there. Surface area rather than volume, because splitting the vertex may go through CGAL's
    // polygon-soup path, which is free to choose either orientation for a component.
    REQUIRE(result.after.open_edges == 0);
    REQUIRE(result.after.tris == 24);
    REQUIRE_THAT(result.after.surface_area, Catch::Matchers::WithinRel(1200., 1e-4));
}

TEST_CASE("nocte repair: metrics count self-intersections on a mesh small enough to test", "[NocteRepair]")
{
    const MeshMetrics clean = compute_metrics(its_make_cube(10., 10., 10.));
    REQUIRE(clean.self_intersections == 0);

    indexed_triangle_set crossing = its_make_cube(10., 10., 10.);
    indexed_triangle_set second   = its_make_cube(10., 10., 10.);
    its_translate(second, Vec3f(5.f, 5.f, 5.f));
    its_merge(crossing, second);

    // 0 means none and 1 means at least one; the exact number of crossing pairs is not counted.
    REQUIRE(compute_metrics(crossing).self_intersections == 1);
}

TEST_CASE("nocte repair: removing a degenerate face succeeds and updates metrics", "[NocteRepair]")
{
    RepairSession session(cube_with_degenerate_facet(10.));

    const int index = session.plan().index_of(RepairOpKind::RemoveDegenerateFaces);
    REQUIRE(index >= 0);
    REQUIRE(session.plan().ops[size_t(index)].implemented);

    // preview() reports the same change without touching the session.
    const RepairStepResult preview = session.preview(size_t(index));
    REQUIRE(preview.succeeded);
    REQUIRE(preview.before.tris == 13);
    REQUIRE(preview.after.tris == 12);
    REQUIRE(session.current().indices.size() == 13);

    const RepairStepResult result = session.accept(size_t(index));
    REQUIRE(result.succeeded);
    REQUIRE(result.before.tris == 13);
    REQUIRE(result.after.tris == 12);
    REQUIRE(result.touched_faces.size() == 1);
    REQUIRE(result.touched_faces.front() == 12);
    // The degenerate facet was what left edges unpaired; without it the cube is a closed solid.
    REQUIRE(result.after.open_edges == 0);
    REQUIRE_THAT(result.after.volume, Catch::Matchers::WithinRel(1000., 1e-4));
    REQUIRE_THAT(result.after.surface_area, Catch::Matchers::WithinRel(600., 1e-4));

    REQUIRE(session.state_of(size_t(index)) == RepairStepState::Applied);
    REQUIRE(session.current().indices.size() == 12);
    REQUIRE(session.diagnostics().manifold);
    REQUIRE(session.diagnostics().issues.empty());

    const NocteReport report = session.report();
    REQUIRE(report.schema == std::string(NOCTE_REPORT_SCHEMA));
    REQUIRE(report.diagnostics.has(IssueKind::DegenerateFacet));
    REQUIRE(report.diagnostics_after.has_value());
    REQUIRE_FALSE(report.to_json().empty());
    REQUIRE_THAT(report.to_markdown(), Catch::Matchers::ContainsSubstring("Remove degenerate facets"));
}

TEST_CASE("nocte repair: undo restores previous mesh", "[NocteRepair]")
{
    RepairSession session(cube_with_degenerate_facet(10.));

    const int index = session.plan().index_of(RepairOpKind::RemoveDegenerateFaces);
    REQUIRE(index >= 0);
    REQUIRE_FALSE(session.can_undo());

    REQUIRE(session.accept(size_t(index)).succeeded);
    REQUIRE(session.current().indices.size() == 12);
    REQUIRE(session.can_undo());

    session.undo();

    REQUIRE(session.current().indices.size() == 13);
    // Eigen's operator== is coefficient-wise, so compare the indices one by one.
    const stl_triangle_vertex_indices &restored = session.current().indices.back();
    REQUIRE(restored(0) == 0);
    REQUIRE(restored(1) == 0);
    REQUIRE(restored(2) == 1);
    REQUIRE_FALSE(session.can_undo());
    REQUIRE(session.state_of(size_t(index)) == RepairStepState::Pending);
    REQUIRE(session.diagnostics().has(IssueKind::DegenerateFacet));

    // Undoing with nothing on the stack is a no-op, not an error.
    session.undo();
    REQUIRE(session.current().indices.size() == 13);
}
