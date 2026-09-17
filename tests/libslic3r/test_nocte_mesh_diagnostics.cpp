// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include <algorithm>

#include <catch2/catch_all.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/MeshDiagnostics.hpp"

using namespace Slic3r;
using namespace Slic3r::Nocte;

namespace {

// its_make_cube() puts the box in the first octant, so the +Z face is the pair of triangles whose
// three vertices all sit at z == size. Removing them leaves a four-edge boundary loop.
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

} // namespace

TEST_CASE("nocte diagnostics: closed cube is clean", "[NocteDiagnostics]")
{
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    const DiagnosticsResult    result = diagnose(cube);

    REQUIRE(result.issues.empty());
    REQUIRE(result.manifold);
    REQUIRE(result.open_edges == 0);
    REQUIRE(result.shells == 1);
    REQUIRE(result.stats.number_of_facets == 12);
    REQUIRE_THAT(result.volume, Catch::Matchers::WithinRel(1000., 1e-4));
    REQUIRE_FALSE(result.summary.empty());
    // The two CGAL/libigl checks are always accounted for, never silently dropped.
    REQUIRE(result.skipped_checks.size() == 2);
}

TEST_CASE("nocte diagnostics: cube with one face removed reports a single open boundary loop", "[NocteDiagnostics]")
{
    const indexed_triangle_set mesh = cube_with_top_face_removed(10.);
    REQUIRE(mesh.indices.size() == 10);
    REQUIRE(its_num_open_edges(mesh) == 4);

    const DiagnosticsResult result = diagnose(mesh);

    REQUIRE_FALSE(result.manifold);
    REQUIRE(result.open_edges == 4);

    const std::vector<const MeshIssue *> loops = result.all_of(IssueKind::OpenBoundaryLoop);
    REQUIRE(loops.size() == 1);
    REQUIRE(loops.front()->count == 4);
    REQUIRE(loops.front()->edges.size() == 4);
    REQUIRE(loops.front()->issue_id == "open-loop-1");
    REQUIRE(loops.front()->severity == Severity::Blocking);
    REQUIRE_FALSE(loops.front()->explanation.empty());
    // The loop runs around the 10 mm square that used to be the top face.
    REQUIRE_THAT(loops.front()->metric, Catch::Matchers::WithinRel(40., 1e-4));

    // A missing face is a hole, not a duplicated or degenerate one.
    REQUIRE_FALSE(result.has(IssueKind::DegenerateFacet));
    REQUIRE_FALSE(result.has(IssueKind::NonManifoldEdge));
}

TEST_CASE("nocte diagnostics: two separate cubes report disconnected shells", "[NocteDiagnostics]")
{
    indexed_triangle_set mesh   = its_make_cube(10., 10., 10.);
    indexed_triangle_set second = its_make_cube(10., 10., 10.);
    its_translate(second, Vec3f(50.f, 0.f, 0.f));
    its_merge(mesh, second);

    const DiagnosticsResult result = diagnose(mesh);

    REQUIRE(result.shells == 2);
    REQUIRE(result.manifold);

    const MeshIssue *issue = result.find(IssueKind::DisconnectedShell);
    REQUIRE(issue != nullptr);
    REQUIRE(issue->count == 2);
    REQUIRE(issue->issue_id == "disconnected-shells-1");
    REQUIRE_FALSE(issue->explanation.empty());
}

TEST_CASE("nocte diagnostics: degenerate triangle is reported", "[NocteDiagnostics]")
{
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    // A facet that uses the same vertex twice encloses no area whatever its coordinates are.
    mesh.indices.push_back(stl_triangle_vertex_indices(0, 0, 1));

    const DiagnosticsResult result = diagnose(mesh);

    const MeshIssue *issue = result.find(IssueKind::DegenerateFacet);
    REQUIRE(issue != nullptr);
    REQUIRE(issue->count == 1);
    REQUIRE(issue->face_ids.size() == 1);
    REQUIRE(issue->face_ids.front() == 12);
    REQUIRE_FALSE(issue->explanation.empty());
    REQUIRE(issue->issue_id == "degenerate-facets-1");
}

TEST_CASE("nocte diagnostics: a sliver of zero area is reported as degenerate", "[NocteDiagnostics]")
{
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    // Three distinct but collinear vertices: a facet with no repeated index and no area.
    mesh.vertices.push_back(stl_vertex(0.f, 0.f, 20.f));
    mesh.vertices.push_back(stl_vertex(1.f, 0.f, 20.f));
    mesh.vertices.push_back(stl_vertex(2.f, 0.f, 20.f));
    const int base = int(mesh.vertices.size()) - 3;
    mesh.indices.push_back(stl_triangle_vertex_indices(base, base + 1, base + 2));

    const DiagnosticsResult result = diagnose(mesh);

    const MeshIssue *issue = result.find(IssueKind::DegenerateFacet);
    REQUIRE(issue != nullptr);
    REQUIRE(issue->count == 1);
    REQUIRE(issue->face_ids.front() == 12);
}
