// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include <algorithm>

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

} // namespace

TEST_CASE("nocte repair: plan for cube with missing face proposes FillHolesCgal and marks it not implemented in M0",
          "[NocteRepair]")
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
    REQUIRE_FALSE(op.implemented);
    REQUIRE(op.targets_issue_ids.size() == 1);
    REQUIRE(op.targets_issue_ids.front() == "open-loop-1");
    REQUIRE_THAT(op.rationale, Catch::Matchers::ContainsSubstring("open-loop-1"));

    const RepairStepResult result = session.accept(size_t(index));
    REQUIRE_FALSE(result.succeeded);
    REQUIRE_THAT(result.message, Catch::Matchers::ContainsSubstring("not implemented in M0"));
    REQUIRE(session.state_of(size_t(index)) == RepairStepState::Failed);
    // A step that fails changes nothing and leaves nothing to undo.
    REQUIRE_FALSE(session.can_undo());
    REQUIRE(session.current().indices.size() == 10);
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
