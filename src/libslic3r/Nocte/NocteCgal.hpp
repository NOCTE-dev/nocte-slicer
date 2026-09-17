// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The one place where the NØCTE mesh pipeline talks to CGAL. NocteCgal.cpp is compiled into the
// libslic3r_cgal target, not into libslic3r, so the GPL-licensed CGAL code stays isolated exactly
// the way MeshBoolean.cpp does (ADR-002). That is why nothing below mentions a CGAL type: the
// header has to be includable from libslic3r, where no CGAL include directory exists.
//
// Every entry point is total. CGAL throws on input it cannot make sense of, so each function
// catches everything and reports `ok == false` with a message the repair step can show, rather
// than letting an exception escape into the slicing pipeline.

#ifndef slic3r_Nocte_NocteCgal_hpp_
#define slic3r_Nocte_NocteCgal_hpp_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace Nocte {
namespace cgal {

// How the mesh reached CGAL. A Surface_mesh cannot hold a non-manifold edge, so add_face() refuses
// facets that would create one; when that happens the mesh is rebuilt from a polygon soup instead,
// which is lossy in the sense that repair_polygon_soup() merges coincident points and drops
// degenerate polygons, and orient_polygon_soup() may flip facets. Callers report it, because it
// explains why a step changed more than the user asked for.
enum class Conversion { Failed, Direct, PolygonSoup };

std::string to_string(Conversion conversion);

// True when this build can actually call CGAL. Always true today; kept so a future build that
// drops the dependency has one thing to flip.
bool available();

struct SelfIntersectionResult
{
    bool        ok      = false;
    std::string message;
    Conversion  conversion = Conversion::Failed;

    // Whether any pair of facets intersects. Meaningful only when `ok`.
    bool        intersects = false;
    // Number of intersecting facet pairs, or -1 when only the yes/no answer was computed.
    // CGAL's PMP::self_intersections() has no call site anywhere else in this tree, so we use the
    // predicate PMP::does_self_intersect() that does, and leave the count unknown.
    long long   pairs   = -1;
    // Empty while `pairs` is -1.
    std::vector<std::pair<int, int>> face_pairs;
};

struct StitchResult
{
    bool        ok      = false;
    std::string message;
    Conversion  conversion = Conversion::Failed;

    // Border half-edges before and after, summed over every boundary cycle.
    int         border_edges_before = 0;
    int         border_edges_after  = 0;
    // Pairs of border half-edges that were sewn together.
    int         edges_stitched      = 0;
};

struct DuplicateVerticesResult
{
    bool        ok      = false;
    std::string message;
    Conversion  conversion = Conversion::Failed;

    int         vertices_before     = 0;
    int         vertices_after      = 0;
    // Vertices CGAL added to pull apart facet fans that only met at a point.
    int         vertices_duplicated = 0;
};

struct FillHolesResult
{
    bool        ok      = false;
    std::string message;
    Conversion  conversion = Conversion::Failed;

    int         holes_found   = 0;
    int         holes_filled  = 0;
    // Holes left alone because they exceeded one of the two limits.
    int         holes_skipped = 0;
    int         faces_added   = 0;
    // Perimeter of the largest hole that was skipped, in mm. Zero when nothing was skipped.
    double      largest_skipped_perimeter = 0.;
};

// Does any pair of facets of `its` intersect? `max_pairs` caps how many intersecting pairs would be
// listed in `face_pairs`; it is accepted and documented now but unused while the count is unknown,
// so the signature does not have to change when PMP::self_intersections() is adopted.
SelfIntersectionResult self_intersections(const indexed_triangle_set &its, size_t max_pairs = 0);

// Sews together border edges that already coincide geometrically. Adds no geometry: a border only
// disappears when a matching one was sitting on top of it. `its` is replaced on success.
StitchResult stitch_borders(indexed_triangle_set &its);

// Splits vertices whose incident facets form more than one fan, so every vertex has a single
// umbrella of facets around it. Moves no vertex; it only duplicates positions.
DuplicateVerticesResult duplicate_non_manifold_vertices(indexed_triangle_set &its);

// Triangulates every boundary cycle. A hole is left alone when it is longer than
// `max_hole_perimeter` mm (zero means no limit) or has more than `max_hole_edges` edges — the cost
// of triangulate_hole() grows non-linearly with the cycle length, and 500 is the same bound the
// texture-to-color repair uses (TextureToColor/Repair.hpp:30).
FillHolesResult fill_holes(indexed_triangle_set &its, double max_hole_perimeter = 0., size_t max_hole_edges = 500);

} // namespace cgal
} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_NocteCgal_hpp_
