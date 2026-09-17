// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Compiled into libslic3r_cgal, not into libslic3r. Everything CGAL stays behind NocteCgal.hpp,
// which mentions no CGAL type, so the rest of Nocte/ can call these from libslic3r.
//
// The include style below is the one MeshBoolean.cpp uses (`libslic3r/<header>.hpp`). It resolves
// for this target because the top-level CMakeLists adds `src/` to every target's include path
// (`include_directories(SYSTEM ${LIBDIR})`, CMakeLists.txt:694, with LIBDIR set at :688), not
// because of anything libslic3r_cgal declares for itself.

#include "libslic3r/Nocte/NocteCgal.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TryCatchSignal.hpp"
// CGAL's kernels define a PI of their own; libslic3r's has to get out of the way first, exactly as
// in MeshBoolean.cpp.
#undef PI

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/boost/graph/graph_traits_Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/border.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>

namespace Slic3r {
namespace Nocte {
namespace cgal {

// Same aliases MeshBoolean.cpp:109-115 defines, kept file-local so nothing leaks into the header.
namespace CGALProc = CGAL::Polygon_mesh_processing;
using EpicKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using _EpicMesh  = CGAL::Surface_mesh<EpicKernel::Point_3>;

namespace {

using VertexIndex        = _EpicMesh::Vertex_index;
using FaceDescriptor     = boost::graph_traits<_EpicMesh>::face_descriptor;
using HalfedgeDescriptor = boost::graph_traits<_EpicMesh>::halfedge_descriptor;

// Straight translation, vertex for vertex and facet for facet, so face and vertex indices survive.
// Returns false as soon as Surface_mesh refuses a facet, which it does for anything that would
// create a non-manifold edge; the caller then falls back to the polygon-soup path.
bool its_to_cgal_direct(const indexed_triangle_set &its, _EpicMesh &out)
{
    out.clear();
    if (its.indices.empty())
        return false;

    out.reserve(its.vertices.size(), (its.indices.size() * 3) / 2, its.indices.size());
    for (const stl_vertex &v : its.vertices)
        out.add_vertex(_EpicMesh::Point(double(v.x()), double(v.y()), double(v.z())));

    const int vertex_count = int(its.vertices.size());
    for (const stl_triangle_vertex_indices &f : its.indices) {
        // A facet that names the same vertex twice, or one out of range, has no place in a
        // half-edge structure at all.
        if (f(0) == f(1) || f(1) == f(2) || f(0) == f(2))
            return false;
        if (f(0) < 0 || f(1) < 0 || f(2) < 0 || f(0) >= vertex_count || f(1) >= vertex_count || f(2) >= vertex_count)
            return false;
        const FaceDescriptor fd = out.add_face(VertexIndex(std::size_t(f(0))),
                                               VertexIndex(std::size_t(f(1))),
                                               VertexIndex(std::size_t(f(2))));
        if (fd == _EpicMesh::null_face())
            return false;
    }
    return true;
}

// The route MeshBoolean::cgal::repair() takes for broken input (MeshBoolean.cpp:489-507): clean the
// soup, orient it — which duplicates whatever vertices it needs to — and only then build the mesh.
// Facet and vertex indices do not survive this.
bool its_to_cgal_soup(const indexed_triangle_set &its, _EpicMesh &out)
{
    std::vector<_EpicMesh::Point>         points;
    std::vector<std::vector<std::size_t>> polygons;
    points.reserve(its.vertices.size());
    polygons.reserve(its.indices.size());

    for (const stl_vertex &v : its.vertices)
        points.emplace_back(double(v.x()), double(v.y()), double(v.z()));
    for (const stl_triangle_vertex_indices &f : its.indices)
        polygons.push_back({ std::size_t(f(0)), std::size_t(f(1)), std::size_t(f(2)) });

    CGALProc::repair_polygon_soup(points, polygons);
    CGALProc::orient_polygon_soup(points, polygons);

    out.clear();
    CGALProc::polygon_soup_to_polygon_mesh(points, polygons, out);
    return out.number_of_faces() > 0;
}

// Direct first, soup as the fallback. `conversion` says which one was taken.
bool its_to_cgal(const indexed_triangle_set &its, _EpicMesh &out, Conversion &conversion)
{
    conversion = Conversion::Failed;
    if (its_to_cgal_direct(its, out)) {
        conversion = Conversion::Direct;
        return true;
    }
    if (its_to_cgal_soup(its, out)) {
        conversion = Conversion::PolygonSoup;
        return true;
    }
    return false;
}

// Skips removed elements and remaps vertex indices, the way TextureToColor/CgalUtils.hpp:31-50
// does, because the PMP operations leave garbage behind and the iteration order of Surface_mesh is
// not the insertion order once that happens. Only triangles are emitted, as in
// MeshBoolean.cpp:205-224.
indexed_triangle_set cgal_to_its(const _EpicMesh &mesh)
{
    indexed_triangle_set its;
    its.vertices.reserve(mesh.number_of_vertices());
    its.indices.reserve(mesh.number_of_faces());

    std::vector<int> vertex_map(mesh.num_vertices(), -1);
    for (const VertexIndex v : mesh.vertices()) {
        if (mesh.is_removed(v))
            continue;
        const std::size_t slot = std::size_t(v);
        if (slot >= vertex_map.size())
            continue;
        const _EpicMesh::Point &p = mesh.point(v);
        vertex_map[slot] = int(its.vertices.size());
        its.vertices.emplace_back(float(p.x()), float(p.y()), float(p.z()));
    }

    for (const FaceDescriptor f : mesh.faces()) {
        if (mesh.is_removed(f))
            continue;
        Vec3i32 facet(-1, -1, -1);
        int     i = 0;
        for (const VertexIndex v : mesh.vertices_around_face(mesh.halfedge(f))) {
            const std::size_t slot = std::size_t(v);
            if (i > 2 || slot >= vertex_map.size() || vertex_map[slot] < 0) {
                i = -1;
                break;
            }
            facet(i ++) = vertex_map[slot];
        }
        if (i == 3)
            its.indices.emplace_back(facet);
    }
    // Per-facet properties cannot follow a CGAL rebuild; dropping them keeps `properties` from
    // going out of step with `indices`, which the rest of libslic3r assumes.
    its.properties.clear();
    return its;
}

struct CycleInfo
{
    std::size_t edges     = 0;
    double      perimeter = 0.;
};

// Walks one boundary cycle, as TextureToColor/Repair.hpp:67-76 does, and measures it on the way.
CycleInfo cycle_info(const _EpicMesh &mesh, HalfedgeDescriptor h0)
{
    CycleInfo info;
    HalfedgeDescriptor h = h0;
    do {
        const _EpicMesh::Point &a = mesh.point(mesh.source(h));
        const _EpicMesh::Point &b = mesh.point(mesh.target(h));
        const double dx = b.x() - a.x();
        const double dy = b.y() - a.y();
        const double dz = b.z() - a.z();
        info.perimeter += std::sqrt(dx * dx + dy * dy + dz * dz);
        ++ info.edges;
        h = mesh.next(h);
    } while (h != h0);
    return info;
}

int border_halfedge_count(const _EpicMesh &mesh)
{
    std::vector<HalfedgeDescriptor> cycles;
    CGALProc::extract_boundary_cycles(mesh, std::back_inserter(cycles));
    std::size_t total = 0;
    for (const HalfedgeDescriptor h : cycles)
        total += cycle_info(mesh, h).edges;
    return int(total);
}

std::string fixed(double value, int decimals = 2)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss.precision(decimals);
    oss << value;
    return oss.str();
}

// The note appended to a message when the mesh had to be rebuilt from a polygon soup.
std::string soup_note(Conversion conversion)
{
    return conversion == Conversion::PolygonSoup
               ? " The mesh could not be loaded facet for facet — CGAL rejected at least one facet as"
                 " non-manifold — so it was rebuilt from a polygon soup first, which also merged"
                 " coincident points and dropped degenerate facets."
               : std::string();
}

} // namespace

std::string to_string(Conversion conversion)
{
    switch (conversion) {
    case Conversion::Failed:      return "Failed";
    case Conversion::Direct:      return "Direct";
    case Conversion::PolygonSoup: return "PolygonSoup";
    }
    return "Unknown";
}

bool available() { return true; }

SelfIntersectionResult self_intersections(const indexed_triangle_set &its, size_t /* max_pairs */)
{
    SelfIntersectionResult result;
    if (its.indices.empty()) {
        result.ok         = true;
        result.intersects = false;
        result.pairs      = 0;
        result.conversion = Conversion::Direct;
        result.message    = "Empty mesh: nothing can intersect.";
        return result;
    }

    try {
        _EpicMesh mesh;
        if (! its_to_cgal(its, mesh, result.conversion)) {
            result.message = "CGAL could not build a surface mesh from this input, so it cannot be "
                             "tested for self-intersections.";
            return result;
        }

        bool intersects = false;
        bool hw_fail    = false;
        // Same shape as MeshBoolean.cpp:349-351: CGAL can trip a hardware fault on degenerate
        // input, and a failed check must not take the whole slicer down.
        try_catch_signal({SIGSEGV, SIGFPE}, [&intersects, &mesh] {
            intersects = CGALProc::does_self_intersect(mesh);
        }, [&hw_fail] { hw_fail = true; });

        if (hw_fail) {
            result.message = "The self-intersection test crashed inside CGAL on this mesh.";
            return result;
        }

        result.ok         = true;
        result.intersects = intersects;
        // PMP::self_intersections() has no call site in this tree, so only the predicate is used and
        // the number of intersecting pairs stays unknown.
        result.pairs      = -1;
        result.message    = intersects ? "The surface passes through itself." + soup_note(result.conversion)
                                       : "No facet pair intersects." + soup_note(result.conversion);
    } catch (const std::exception &e) {
        result.ok      = false;
        result.message = std::string("CGAL self-intersection test failed: ") + e.what();
    } catch (...) {
        result.ok      = false;
        result.message = "CGAL self-intersection test failed with an unknown error.";
    }
    return result;
}

StitchResult stitch_borders(indexed_triangle_set &its)
{
    StitchResult result;
    if (its.indices.empty()) {
        result.ok      = true;
        result.message = "Empty mesh: no borders to stitch.";
        result.conversion = Conversion::Direct;
        return result;
    }

    try {
        _EpicMesh mesh;
        if (! its_to_cgal(its, mesh, result.conversion)) {
            result.message = "CGAL could not build a surface mesh from this input, so its borders "
                             "cannot be stitched.";
            return result;
        }

        result.border_edges_before = border_halfedge_count(mesh);

        bool hw_fail = false;
        // Called exactly as TextureToColor/Repair.hpp:89 and :197 call it: no named parameters, and
        // the return value ignored in favour of counting border half-edges ourselves, which works
        // the same on every CGAL version this fork might be built against.
        try_catch_signal({SIGSEGV, SIGFPE}, [&mesh] {
            CGALProc::stitch_borders(mesh);
        }, [&hw_fail] { hw_fail = true; });

        if (hw_fail) {
            result.message = "Stitching crashed inside CGAL on this mesh.";
            return result;
        }

        result.border_edges_after = border_halfedge_count(mesh);
        result.edges_stitched     = (std::max)(0, (result.border_edges_before - result.border_edges_after) / 2);

        its       = cgal_to_its(mesh);
        result.ok = true;
        result.message = result.edges_stitched > 0
                             ? "Sewed " + std::to_string(result.edges_stitched) + " pair(s) of border edges together; " +
                                   std::to_string(result.border_edges_before) + " border edge(s) became " +
                                   std::to_string(result.border_edges_after) + "." + soup_note(result.conversion)
                             : "No pair of borders lined up well enough to be sewn together." +
                                   soup_note(result.conversion);
    } catch (const std::exception &e) {
        result.ok      = false;
        result.message = std::string("CGAL stitch_borders failed: ") + e.what();
    } catch (...) {
        result.ok      = false;
        result.message = "CGAL stitch_borders failed with an unknown error.";
    }
    return result;
}

DuplicateVerticesResult duplicate_non_manifold_vertices(indexed_triangle_set &its)
{
    DuplicateVerticesResult result;
    if (its.indices.empty()) {
        result.ok      = true;
        result.message = "Empty mesh: nothing to split.";
        result.conversion = Conversion::Direct;
        return result;
    }

    try {
        _EpicMesh mesh;
        if (! its_to_cgal(its, mesh, result.conversion)) {
            result.message = "CGAL could not build a surface mesh from this input, so its vertices "
                             "cannot be split.";
            return result;
        }

        result.vertices_before = int(mesh.number_of_vertices());

        bool hw_fail = false;
        // Same call as TextureToColor/Repair.hpp:90 and MeshBoolean.cpp:512.
        try_catch_signal({SIGSEGV, SIGFPE}, [&mesh] {
            CGALProc::duplicate_non_manifold_vertices(mesh);
        }, [&hw_fail] { hw_fail = true; });

        if (hw_fail) {
            result.message = "Splitting non-manifold vertices crashed inside CGAL on this mesh.";
            return result;
        }

        result.vertices_after      = int(mesh.number_of_vertices());
        result.vertices_duplicated = (std::max)(0, result.vertices_after - result.vertices_before);

        its       = cgal_to_its(mesh);
        result.ok = true;
        result.message = result.vertices_duplicated > 0
                             ? "Split " + std::to_string(result.vertices_duplicated) +
                                   " vertex/vertices so every facet fan has a vertex of its own." +
                                   soup_note(result.conversion)
                             : "Every vertex already had a single facet fan around it." + soup_note(result.conversion);
    } catch (const std::exception &e) {
        result.ok      = false;
        result.message = std::string("CGAL duplicate_non_manifold_vertices failed: ") + e.what();
    } catch (...) {
        result.ok      = false;
        result.message = "CGAL duplicate_non_manifold_vertices failed with an unknown error.";
    }
    return result;
}

FillHolesResult fill_holes(indexed_triangle_set &its, double max_hole_perimeter, size_t max_hole_edges)
{
    FillHolesResult result;
    if (its.indices.empty()) {
        result.ok      = true;
        result.message = "Empty mesh: no holes to fill.";
        result.conversion = Conversion::Direct;
        return result;
    }

    try {
        _EpicMesh mesh;
        if (! its_to_cgal(its, mesh, result.conversion)) {
            result.message = "CGAL could not build a surface mesh from this input, so its holes "
                             "cannot be filled.";
            return result;
        }

        std::vector<HalfedgeDescriptor> cycles;
        CGALProc::extract_boundary_cycles(mesh, std::back_inserter(cycles));
        result.holes_found = int(cycles.size());

        if (cycles.empty()) {
            result.ok      = true;
            result.message = "The surface is already closed." + soup_note(result.conversion);
            return result;
        }

        bool hw_fail = false;
        for (const HalfedgeDescriptor h : cycles) {
            const CycleInfo info = cycle_info(mesh, h);
            if (info.edges > max_hole_edges || (max_hole_perimeter > 0. && info.perimeter > max_hole_perimeter)) {
                ++ result.holes_skipped;
                result.largest_skipped_perimeter = (std::max)(result.largest_skipped_perimeter, info.perimeter);
                continue;
            }

            std::vector<FaceDescriptor> patch;
            // Named-parameter form, copied from MeshBoolean.cpp:346. The three-argument form used at
            // TextureToColor/Repair.hpp:97 is the deprecated spelling of the same thing.
            try_catch_signal({SIGSEGV, SIGFPE}, [&mesh, h, &patch] {
                CGALProc::triangulate_hole(mesh, h,
                                           CGAL::parameters::default_values().face_output_iterator(std::back_inserter(patch)));
            }, [&hw_fail] { hw_fail = true; });

            if (hw_fail)
                break;
            if (patch.empty()) {
                // CGAL could not triangulate this cycle; it stays open and the tier-2 step will have
                // to deal with it.
                ++ result.holes_skipped;
                result.largest_skipped_perimeter = (std::max)(result.largest_skipped_perimeter, info.perimeter);
            } else {
                ++ result.holes_filled;
                result.faces_added += int(patch.size());
            }
        }

        if (hw_fail) {
            result.message = "Hole filling crashed inside CGAL on this mesh.";
            return result;
        }

        its       = cgal_to_its(mesh);
        result.ok = true;

        std::string message = "Found " + std::to_string(result.holes_found) + " hole(s), filled " +
                              std::to_string(result.holes_filled) + " with " + std::to_string(result.faces_added) +
                              " new facet(s).";
        if (result.holes_skipped > 0)
            message += " " + std::to_string(result.holes_skipped) + " hole(s) were left alone — the largest is " +
                       fixed(result.largest_skipped_perimeter) + " mm around, past the limit of " +
                       std::to_string(max_hole_edges) + " edges" +
                       (max_hole_perimeter > 0. ? " / " + fixed(max_hole_perimeter) + " mm" : std::string()) +
                       ", where triangulation stops being worth the wait.";
        result.message = message + soup_note(result.conversion);
    } catch (const std::exception &e) {
        result.ok      = false;
        result.message = std::string("CGAL hole filling failed: ") + e.what();
    } catch (...) {
        result.ok      = false;
        result.message = "CGAL hole filling failed with an unknown error.";
    }
    return result;
}

} // namespace cgal
} // namespace Nocte
} // namespace Slic3r
