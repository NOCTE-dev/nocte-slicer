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

// A cube whose facets all face inwards. Nothing about the shape changed, so it is still closed and
// still consistently oriented — only the wrong way round, which is what InvertedShell is about.
indexed_triangle_set inside_out_cube(double size)
{
    indexed_triangle_set cube = its_make_cube(size, size, size);
    its_flip_triangles(cube);
    return cube;
}

// Two cubes joined at exactly one corner vertex, which they genuinely share: the facets around that
// vertex fall into two fans that touch at a point without sharing an edge.
indexed_triangle_set two_cubes_sharing_a_corner(double size)
{
    indexed_triangle_set mesh   = its_make_cube(size, size, size);
    indexed_triangle_set second = its_make_cube(size, size, size);
    its_translate(second, Vec3f(float(size), float(size), float(size)));

    const int first_cube_vertices = int(mesh.vertices.size());
    its_merge(mesh, second);

    const float corner = float(size);
    int         keep   = -1;
    int         drop   = -1;
    for (int v = 0; v < int(mesh.vertices.size()); ++ v) {
        const stl_vertex &p = mesh.vertices[v];
        if (p.x() != corner || p.y() != corner || p.z() != corner)
            continue;
        if (v < first_cube_vertices)
            keep = v;
        else
            drop = v;
    }
    if (keep < 0 || drop < 0)
        return mesh;
    for (stl_triangle_vertex_indices &tri : mesh.indices)
        for (int i = 0; i < 3; ++ i)
            if (tri(i) == drop)
                tri(i) = keep;
    return mesh;
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
    // Every check ran: a 12-facet cube is far below both the self-intersection and the
    // winding-number budget, so nothing is accounted for as skipped.
    REQUIRE(result.skipped_checks.empty());
}

TEST_CASE("nocte diagnostics: a check that is turned off is accounted for as skipped", "[NocteDiagnostics]")
{
    DiagnosticsParams params;
    params.check_self_intersections = false;
    params.check_inverted_shell     = false;

    const DiagnosticsResult result = diagnose(its_make_cube(10., 10., 10.), params);

    REQUIRE(result.issues.empty());
    REQUIRE(result.skipped_checks.size() == 2);
}

TEST_CASE("nocte diagnostics: a self-intersection check too large to run is reported as skipped", "[NocteDiagnostics]")
{
    DiagnosticsParams params;
    // Below the 12 facets of a cube, so the guard trips on a mesh small enough to test with.
    params.max_faces_self_intersection = 4;

    const DiagnosticsResult result = diagnose(its_make_cube(10., 10., 10.), params);

    REQUIRE_FALSE(result.has(IssueKind::SelfIntersection));
    REQUIRE(result.skipped_checks.size() == 1);
    REQUIRE_THAT(result.skipped_checks.front(), Catch::Matchers::ContainsSubstring("SelfIntersection"));
}

TEST_CASE("nocte diagnostics: two interpenetrating cubes report a self-intersection", "[NocteDiagnostics]")
{
    indexed_triangle_set mesh   = its_make_cube(10., 10., 10.);
    indexed_triangle_set second = its_make_cube(10., 10., 10.);
    // Half a cube's offset along every axis, so the two solids overlap in a 5 mm corner.
    its_translate(second, Vec3f(5.f, 5.f, 5.f));
    its_merge(mesh, second);

    const DiagnosticsResult result = diagnose(mesh);

    const MeshIssue *issue = result.find(IssueKind::SelfIntersection);
    REQUIRE(issue != nullptr);
    REQUIRE(issue->issue_id == "self-intersections-1");
    REQUIRE(issue->severity == Severity::Blocking);
    REQUIRE_FALSE(issue->explanation.empty());

    // Both shells are closed and face outwards. Their bounding boxes overlap without either
    // containing the other, which is exactly the case the nesting test must not mistake for a cavity.
    REQUIRE_FALSE(result.has(IssueKind::InvertedShell));
}

TEST_CASE("nocte diagnostics: an inside-out cube is reported as an inverted shell", "[NocteDiagnostics]")
{
    const indexed_triangle_set mesh = inside_out_cube(10.);
    REQUIRE(its_num_open_edges(mesh) == 0);

    const DiagnosticsResult result = diagnose(mesh);

    REQUIRE(result.manifold);
    // Every facet agrees with its neighbours; the shell as a whole is what is wrong.
    REQUIRE_FALSE(result.has(IssueKind::InvertedNormal));

    const MeshIssue *issue = result.find(IssueKind::InvertedShell);
    REQUIRE(issue != nullptr);
    REQUIRE(issue->count == 12);
    REQUIRE(issue->issue_id == "inverted-shell-1");
    REQUIRE(issue->severity == Severity::Blocking);
    REQUIRE(issue->metric < 0.);
    REQUIRE_FALSE(issue->explanation.empty());
}

TEST_CASE("nocte diagnostics: an inward-facing shell inside another one is a cavity, not an inverted shell",
          "[NocteDiagnostics]")
{
    indexed_triangle_set mesh   = its_make_cube(20., 20., 20.);
    indexed_triangle_set cavity = inside_out_cube(10.);
    its_translate(cavity, Vec3f(5.f, 5.f, 5.f));
    its_merge(mesh, cavity);

    const DiagnosticsResult result = diagnose(mesh);

    REQUIRE(result.shells == 2);
    REQUIRE(result.manifold);
    // 20³ minus 10³: the inward-facing inner shell subtracts its volume, which is what a cavity does.
    REQUIRE_THAT(result.volume, Catch::Matchers::WithinRel(7000., 1e-4));
    REQUIRE_FALSE(result.has(IssueKind::InvertedShell));

    // The same inner shell on its own, with nothing around it, is inside out.
    const DiagnosticsResult alone = diagnose(inside_out_cube(10.));
    REQUIRE(alone.has(IssueKind::InvertedShell));
}

TEST_CASE("nocte diagnostics: two cubes joined at a single corner report a non-manifold vertex", "[NocteDiagnostics]")
{
    const indexed_triangle_set mesh = two_cubes_sharing_a_corner(10.);

    const DiagnosticsResult result = diagnose(mesh);

    const MeshIssue *issue = result.find(IssueKind::NonManifoldVertex);
    REQUIRE(issue != nullptr);
    REQUIRE(issue->count == 1);
    REQUIRE_FALSE(issue->face_ids.empty());
    REQUIRE(issue->severity == Severity::Blocking);
    REQUIRE_FALSE(issue->explanation.empty());

    // The pinch is at a vertex, not at an edge: no edge is shared by more than two facets.
    REQUIRE_FALSE(result.has(IssueKind::NonManifoldEdge));
    REQUIRE(result.open_edges == 0);
}

TEST_CASE("nocte diagnostics: a closed cube has no non-manifold vertex", "[NocteDiagnostics]")
{
    const DiagnosticsResult result = diagnose(its_make_cube(10., 10., 10.));
    REQUIRE_FALSE(result.has(IssueKind::NonManifoldVertex));
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
