// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Explainable mesh diagnostics. Classifies what is wrong with an indexed_triangle_set and says
// why in plain language, so the repair planner can name what it targets and the report can show
// a before/after. The module is wx-free and unit-testable. It never includes a CGAL header: the
// self-intersection test goes through Nocte/NocteCgal.hpp, whose implementation lives in the
// libslic3r_cgal target. The winding number is computed in plain Eigen, so the module depends on
// nothing libslic3r does not already carry.

#ifndef slic3r_Nocte_MeshDiagnostics_hpp_
#define slic3r_Nocte_MeshDiagnostics_hpp_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace Nocte {

enum class IssueKind {
    DegenerateFacet,
    DuplicateVertex,
    NonManifoldEdge,
    NonManifoldVertex,
    OpenBoundaryLoop,
    InvertedNormal,
    InvertedShell,
    SelfIntersection,
    DisconnectedShell,
    ZeroVolumeShell,
    TinyFeature,
    ThinWall
};

enum class Severity { Info, Warning, Blocking };

// Stable, machine-readable names. Used by the JSON report and by issue ids.
std::string to_string(IssueKind kind);
std::string to_string(Severity severity);

struct MeshIssue
{
    IssueKind                       kind     = IssueKind::DegenerateFacet;
    Severity                        severity = Severity::Info;
    // How many primitives of this kind were found (faces, vertices, edges, shells, ...).
    size_t                          count    = 0;
    // Kind-dependent scalar: total area for degenerate facets, loop length for boundary loops,
    // signed volume for a zero-volume shell. Zero when the kind carries no useful scalar.
    double                          metric   = 0.;
    std::vector<int>                face_ids;
    // Vertex index pairs. For an open boundary loop these are in walk order.
    std::vector<std::pair<int, int>> edges;
    BoundingBoxf3                   bbox;
    // One sentence a user can act on.
    std::string                     explanation;
    // Stable within one diagnose() run, e.g. "open-loop-1". Repair steps reference these.
    std::string                     issue_id;
};

struct DiagnosticsParams
{
    // A facet whose area is below this counts as degenerate (mm²). Facets that reference the same
    // vertex index twice are always degenerate, whatever their area would be.
    double degenerate_area_eps = 1e-8;
    // |volume| below which a closed shell is considered empty (mm³).
    double zero_volume_eps     = 1e-6;
    // Cap on face_ids / edges stored per issue, so a pathological mesh cannot blow up the report.
    // The `count` field always reports the true number.
    size_t max_reported_primitives = 4096;

    // Above this many facets the self-intersection test is skipped with an entry in
    // `skipped_checks` instead of run: it builds an AABB tree over every facet pair candidate and
    // a large scan would stall the dialog it feeds.
    size_t max_faces_self_intersection = 200000;
    // Above this many facets the winding-number containment test for nested shells is skipped the
    // same way. Shells that no other shell's bounding box encloses are still judged, because those
    // need no winding number at all.
    size_t max_faces_inverted_shell    = 200000;

    bool check_degenerate_facets  = true;
    bool check_duplicate_vertices = true;
    bool check_open_boundaries    = true;
    bool check_non_manifold_edges = true;
    bool check_non_manifold_vertices = true;
    bool check_inverted_normals   = true;
    bool check_disconnected_shells = true;
    bool check_zero_volume        = true;
    // Runs CGAL PMP through Nocte/NocteCgal.hpp, subject to max_faces_self_intersection.
    bool check_self_intersections = true;
    // Runs the per-shell signed volume plus a generalized winding number, subject to
    // max_faces_inverted_shell.
    bool check_inverted_shell     = true;
};

struct DiagnosticsResult
{
    TriangleMeshStats       stats;
    std::vector<MeshIssue>  issues;
    // Open edges as counted by libslic3r's its_num_open_edges(), so the number matches what the
    // rest of the application reports.
    int                     open_edges = 0;
    int                     shells     = 0;
    double                  volume     = 0.;
    // No open edges and no edge shared by more than two faces.
    bool                    manifold   = false;
    std::string             summary;
    // Human-readable reasons for checks that did not run in this build/milestone.
    std::vector<std::string> skipped_checks;

    bool                    has(IssueKind kind) const;
    size_t                  count_of(IssueKind kind) const;
    // First issue of that kind, or nullptr.
    const MeshIssue        *find(IssueKind kind) const;
    std::vector<const MeshIssue *> all_of(IssueKind kind) const;
    const MeshIssue        *find_by_id(const std::string &issue_id) const;
    bool                    clean() const { return issues.empty(); }
};

// One side of a mesh edge: the face that uses it and which of its three edges it is.
// `forward` is true when the face traverses the edge from the lower to the higher vertex index.
struct FaceEdgeRef
{
    int  face    = -1;
    int  edge    = -1;
    bool forward = false;
};

// An undirected mesh edge with every face incident to it.
struct MeshEdge
{
    int                      v0 = -1;   // v0 < v1
    int                      v1 = -1;
    std::vector<FaceEdgeRef> incident;

    bool boundary() const { return incident.size() == 1; }
    bool non_manifold() const { return incident.size() > 2; }
    // Exactly two faces that walk the edge the same way, i.e. their normals disagree.
    bool inconsistently_oriented() const
    {
        return incident.size() == 2 && incident[0].forward == incident[1].forward;
    }
};

// Builds the undirected edge table of `its`, sorted by (v0, v1). Faces that reference the same
// vertex index twice carry no meaningful edges and are skipped; they are reported as degenerate
// facets instead. Plain C++17, no libigl, so the module builds wherever libslic3r does.
std::vector<MeshEdge> its_mesh_edges(const indexed_triangle_set &its);

// One edge-connected surface of `its`, with everything needed to judge whether it faces the right
// way. A closed shell at an even nesting depth has to enclose a positive volume; one at an odd
// depth is a cavity and has to enclose a negative one.
struct ShellOrientation
{
    std::vector<int> faces;
    // Signed volume as the shell is oriented right now (mm³).
    double           volume      = 0.;
    BoundingBoxf3    bbox;
    // Every edge of the shell is shared by exactly two of its facets and no facet is degenerate.
    // Only a closed shell has a meaningful orientation.
    bool             closed      = false;
    // How many other shells enclose this one. Meaningless unless `depth_known`.
    int              depth       = 0;
    bool             depth_known = false;
    // Closed, non-empty, depth known, and pointing the wrong way for that depth.
    bool             inverted    = false;
};

// Splits `its` into edge-connected shells and works out which of them are inside out.
//
// Nesting is decided with a generalized winding number, but only for pairs whose bounding boxes are
// strictly nested. That pre-filter is what keeps two interpenetrating solids — whose boxes overlap
// without either containing the other — from being mistaken for a solid inside a cavity. A shell
// no other box encloses is at depth 0 by construction, so it is judged even when the winding pass
// is skipped for being too expensive.
//
// Cost is O(nested pairs × facets); `max_faces_for_winding` caps it. `skipped_reason`, when given,
// is filled if the winding pass was skipped and some shell's depth stayed unknown.
std::vector<ShellOrientation> its_shell_orientations(const indexed_triangle_set &its,
                                                     size_t                      max_faces_for_winding = 200000,
                                                     std::string                *skipped_reason        = nullptr);

DiagnosticsResult diagnose(const indexed_triangle_set &its, const DiagnosticsParams &params = DiagnosticsParams());

// Appends a SelfIntersection issue when the surface passes through itself. The test itself is CGAL
// PMP, reached through Nocte/NocteCgal.hpp so the GPL-licensed code stays inside the libslic3r_cgal
// target (ADR-002). Returns false and fills `error_message` when the check could not run — because
// the mesh is above `max_faces`, or because CGAL refused it — and leaves `out` untouched.
bool check_self_intersections(const indexed_triangle_set &its, std::vector<MeshIssue> &out,
                              std::string *error_message = nullptr, size_t max_faces = 200000);

// Appends one InvertedShell issue per shell that faces the wrong way, using its_shell_orientations().
// Same contract as above.
bool check_inverted_shell(const indexed_triangle_set &its, std::vector<MeshIssue> &out,
                          std::string *error_message = nullptr, size_t max_faces_for_winding = 200000);

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_MeshDiagnostics_hpp_
