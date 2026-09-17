// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/MeshDiagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace Slic3r {
namespace Nocte {

std::string to_string(IssueKind kind)
{
    switch (kind) {
    case IssueKind::DegenerateFacet:   return "DegenerateFacet";
    case IssueKind::DuplicateVertex:   return "DuplicateVertex";
    case IssueKind::NonManifoldEdge:   return "NonManifoldEdge";
    case IssueKind::NonManifoldVertex: return "NonManifoldVertex";
    case IssueKind::OpenBoundaryLoop:  return "OpenBoundaryLoop";
    case IssueKind::InvertedNormal:    return "InvertedNormal";
    case IssueKind::InvertedShell:     return "InvertedShell";
    case IssueKind::SelfIntersection:  return "SelfIntersection";
    case IssueKind::DisconnectedShell: return "DisconnectedShell";
    case IssueKind::ZeroVolumeShell:   return "ZeroVolumeShell";
    case IssueKind::TinyFeature:       return "TinyFeature";
    case IssueKind::ThinWall:          return "ThinWall";
    }
    return "Unknown";
}

std::string to_string(Severity severity)
{
    switch (severity) {
    case Severity::Info:     return "Info";
    case Severity::Warning:  return "Warning";
    case Severity::Blocking: return "Blocking";
    }
    return "Unknown";
}

bool DiagnosticsResult::has(IssueKind kind) const
{
    return this->find(kind) != nullptr;
}

size_t DiagnosticsResult::count_of(IssueKind kind) const
{
    size_t total = 0;
    for (const MeshIssue &issue : this->issues)
        if (issue.kind == kind)
            total += issue.count;
    return total;
}

const MeshIssue *DiagnosticsResult::find(IssueKind kind) const
{
    for (const MeshIssue &issue : this->issues)
        if (issue.kind == kind)
            return &issue;
    return nullptr;
}

std::vector<const MeshIssue *> DiagnosticsResult::all_of(IssueKind kind) const
{
    std::vector<const MeshIssue *> out;
    for (const MeshIssue &issue : this->issues)
        if (issue.kind == kind)
            out.push_back(&issue);
    return out;
}

const MeshIssue *DiagnosticsResult::find_by_id(const std::string &issue_id) const
{
    for (const MeshIssue &issue : this->issues)
        if (issue.issue_id == issue_id)
            return &issue;
    return nullptr;
}

std::vector<MeshEdge> its_mesh_edges(const indexed_triangle_set &its)
{
    struct HalfEdge
    {
        int  a;         // lower vertex index
        int  b;         // higher vertex index
        int  face;
        int  edge;
        bool forward;   // the face walks the edge from a to b
    };

    std::vector<HalfEdge> half_edges;
    half_edges.reserve(its.indices.size() * 3);
    for (int f = 0; f < int(its.indices.size()); ++ f) {
        const stl_triangle_vertex_indices &tri = its.indices[f];
        // A facet that references the same vertex twice has no well-defined edges. It is reported
        // as a degenerate facet, not as a topology problem.
        if (tri(0) == tri(1) || tri(1) == tri(2) || tri(0) == tri(2))
            continue;
        for (int e = 0; e < 3; ++ e) {
            const int  v0      = tri(e);
            const int  v1      = tri((e + 1) % 3);
            const bool forward = v0 < v1;
            half_edges.push_back(HalfEdge{ forward ? v0 : v1, forward ? v1 : v0, f, e, forward });
        }
    }

    std::sort(half_edges.begin(), half_edges.end(), [](const HalfEdge &l, const HalfEdge &r) {
        if (l.a != r.a) return l.a < r.a;
        if (l.b != r.b) return l.b < r.b;
        if (l.face != r.face) return l.face < r.face;
        return l.edge < r.edge;
    });

    std::vector<MeshEdge> out;
    for (size_t i = 0; i < half_edges.size(); ) {
        size_t j = i + 1;
        while (j < half_edges.size() && half_edges[j].a == half_edges[i].a && half_edges[j].b == half_edges[i].b)
            ++ j;
        MeshEdge edge;
        edge.v0 = half_edges[i].a;
        edge.v1 = half_edges[i].b;
        edge.incident.reserve(j - i);
        for (size_t k = i; k < j; ++ k)
            edge.incident.push_back(FaceEdgeRef{ half_edges[k].face, half_edges[k].edge, half_edges[k].forward });
        out.push_back(std::move(edge));
        i = j;
    }
    return out;
}

namespace {

std::string format_number(double value, int decimals = 3)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    return oss.str();
}

double triangle_area(const indexed_triangle_set &its, int face_id)
{
    const stl_triangle_vertex_indices &tri = its.indices[face_id];
    const Vec3d a = its.vertices[tri(0)].cast<double>();
    const Vec3d b = its.vertices[tri(1)].cast<double>();
    const Vec3d c = its.vertices[tri(2)].cast<double>();
    return 0.5 * (b - a).cross(c - a).norm();
}

void merge_vertex(BoundingBoxf3 &bbox, const indexed_triangle_set &its, int vertex_id)
{
    if (vertex_id < 0 || vertex_id >= int(its.vertices.size()))
        return;
    const Vec3d p = its.vertices[vertex_id].cast<double>();
    bbox.merge(p);
}

void merge_face(BoundingBoxf3 &bbox, const indexed_triangle_set &its, int face_id)
{
    const stl_triangle_vertex_indices &tri = its.indices[face_id];
    for (int i = 0; i < 3; ++ i)
        merge_vertex(bbox, its, tri(i));
}

// Appends at most params.max_reported_primitives entries; `count` is set by the caller.
template<typename T>
void store_capped(std::vector<T> &dst, const std::vector<T> &src, size_t cap)
{
    dst.assign(src.begin(), src.begin() + std::ptrdiff_t(std::min(cap, src.size())));
}

} // namespace

bool check_self_intersections(const indexed_triangle_set & /* its */, std::vector<MeshIssue> & /* out */, std::string *error_message)
{
    if (error_message)
        *error_message = "SelfIntersection: not implemented in M0. The check needs CGAL PMP and must live in "
                         "NocteCgal.cpp inside the libslic3r_cgal target.";
    return false;
}

bool check_inverted_shell(const indexed_triangle_set & /* its */, std::vector<MeshIssue> & /* out */, std::string *error_message)
{
    if (error_message)
        *error_message = "InvertedShell: not implemented in M0. Telling an inside-out shell from a correct one "
                         "needs a generalised winding number (libigl fast_winding_number).";
    return false;
}

DiagnosticsResult diagnose(const indexed_triangle_set &its, const DiagnosticsParams &params)
{
    DiagnosticsResult result;

    const size_t cap = params.max_reported_primitives;

    // ---------------------------------------------------------------- basic counts and statistics
    const double volume = double(its_volume(its));
    const size_t shells = its.indices.empty() ? 0 : its_number_of_patches(its);
    const size_t open_edges = its_num_open_edges(its);

    const BoundingBoxf3 mesh_bbox = Slic3r::bounding_box(its);

    result.volume     = volume;
    result.shells     = int(shells);
    result.open_edges = int(open_edges);

    result.stats.number_of_facets = uint32_t(its.indices.size());
    result.stats.volume           = float(volume);
    result.stats.number_of_parts  = int(shells);
    result.stats.open_edges       = int(open_edges);
    if (mesh_bbox.defined) {
        result.stats.min  = mesh_bbox.min.cast<float>();
        result.stats.max  = mesh_bbox.max.cast<float>();
        result.stats.size = result.stats.max - result.stats.min;
    }

    const std::vector<MeshEdge> edges = its_mesh_edges(its);

    // ------------------------------------------------------------------------- degenerate facets
    if (params.check_degenerate_facets) {
        std::vector<int> faces;
        double           total_area      = 0.;
        size_t           repeated_vertex = 0;
        for (int f = 0; f < int(its.indices.size()); ++ f) {
            const stl_triangle_vertex_indices &tri = its.indices[f];
            const bool repeated = tri(0) == tri(1) || tri(1) == tri(2) || tri(0) == tri(2);
            const double area = repeated ? 0. : triangle_area(its, f);
            if (repeated || area < params.degenerate_area_eps) {
                faces.push_back(f);
                total_area += area;
                if (repeated)
                    ++ repeated_vertex;
            }
        }
        if (! faces.empty()) {
            MeshIssue issue;
            issue.kind        = IssueKind::DegenerateFacet;
            issue.severity    = Severity::Warning;
            issue.count       = faces.size();
            issue.metric      = total_area;
            issue.issue_id    = "degenerate-facets-1";
            store_capped(issue.face_ids, faces, cap);
            for (int f : issue.face_ids)
                merge_face(issue.bbox, its, f);
            issue.explanation = std::to_string(faces.size()) + " facet(s) enclose no area — " +
                                std::to_string(repeated_vertex) + " of them reference the same vertex twice, the rest are "
                                "flat slivers below " + format_number(params.degenerate_area_eps, 10) + " mm². They carry no "
                                "surface, confuse slicing and can be removed without changing the shape.";
            result.issues.push_back(std::move(issue));
        }
    }

    // ------------------------------------------------------------------------ duplicate vertices
    if (params.check_duplicate_vertices) {
        // its_merge_vertices() is the exact rule the rest of libslic3r applies (bit-identical
        // positions), so ask it on a copy rather than reimplementing a tolerance of our own.
        indexed_triangle_set probe    = its;
        const int            removed  = its_merge_vertices(probe, false);
        if (removed > 0) {
            MeshIssue issue;
            issue.kind        = IssueKind::DuplicateVertex;
            issue.severity    = Severity::Warning;
            issue.count       = size_t(removed);
            issue.metric      = double(removed);
            issue.issue_id    = "duplicate-vertices-1";
            issue.bbox        = mesh_bbox;
            issue.explanation = std::to_string(removed) + " vertex/vertices sit exactly on top of another one. Facets that "
                                "should share an edge therefore do not, which shows up as open edges. Merging them is "
                                "lossless.";
            result.issues.push_back(std::move(issue));
        }
    }

    // ------------------------------------------------------------------- open boundary loops
    size_t boundary_edge_count = 0;
    if (params.check_open_boundaries) {
        std::vector<size_t> boundary;
        for (size_t i = 0; i < edges.size(); ++ i)
            if (edges[i].boundary())
                boundary.push_back(i);
        boundary_edge_count = boundary.size();

        // Vertex -> positions inside `boundary`. Walking undirected keeps the chaining working even
        // when the surrounding facets disagree about their orientation.
        std::unordered_map<int, std::vector<size_t>> incidence;
        incidence.reserve(boundary.size() * 2);
        for (size_t pos = 0; pos < boundary.size(); ++ pos) {
            const MeshEdge &e = edges[boundary[pos]];
            incidence[e.v0].push_back(pos);
            incidence[e.v1].push_back(pos);
        }

        auto next_unused = [&incidence](const std::vector<char> &used, int vertex) -> size_t {
            auto it = incidence.find(vertex);
            if (it != incidence.end())
                for (size_t cand : it->second)
                    if (! used[cand])
                        return cand;
            return std::numeric_limits<size_t>::max();
        };

        std::vector<char> used(boundary.size(), 0);
        int               loop_index = 0;
        for (size_t start = 0; start < boundary.size(); ++ start) {
            if (used[start])
                continue;
            std::deque<std::pair<int, int>> chain;
            used[start] = 1;
            const MeshEdge &e0 = edges[boundary[start]];
            chain.emplace_back(e0.v0, e0.v1);

            // Grow towards e0.v1 until the chain closes or runs out of boundary edges.
            int cur = e0.v1;
            while (cur != chain.front().first) {
                const size_t nxt = next_unused(used, cur);
                if (nxt == std::numeric_limits<size_t>::max())
                    break;
                used[nxt] = 1;
                const MeshEdge &e = edges[boundary[nxt]];
                const int other = (e.v0 == cur) ? e.v1 : e.v0;
                chain.emplace_back(cur, other);
                cur = other;
            }
            // ... then towards e0.v0, for a chain that did not close.
            cur = e0.v0;
            while (cur != chain.back().second) {
                const size_t prv = next_unused(used, cur);
                if (prv == std::numeric_limits<size_t>::max())
                    break;
                used[prv] = 1;
                const MeshEdge &e = edges[boundary[prv]];
                const int other = (e.v0 == cur) ? e.v1 : e.v0;
                chain.emplace_front(other, cur);
                cur = other;
            }

            const bool closed = chain.size() > 2 && chain.front().first == chain.back().second;

            MeshIssue issue;
            issue.kind     = IssueKind::OpenBoundaryLoop;
            issue.severity = Severity::Blocking;
            issue.count    = chain.size();
            issue.issue_id = "open-loop-" + std::to_string(++ loop_index);

            double length = 0.;
            for (const std::pair<int, int> &e : chain) {
                const Vec3d a = its.vertices[e.first].cast<double>();
                const Vec3d b = its.vertices[e.second].cast<double>();
                length += (b - a).norm();
                merge_vertex(issue.bbox, its, e.first);
                merge_vertex(issue.bbox, its, e.second);
            }
            issue.metric = length;
            {
                const std::vector<std::pair<int, int>> chain_vec(chain.begin(), chain.end());
                store_capped(issue.edges, chain_vec, cap);
            }
            issue.explanation = std::string(closed ? "A closed hole" : "An open crack") + " bounded by " +
                                std::to_string(chain.size()) + " edge(s), " + format_number(length) +
                                " mm around. The surface stops there, so the mesh does not enclose a solid and the "
                                "slicer cannot tell inside from outside.";
            result.issues.push_back(std::move(issue));
        }
    }

    // --------------------------------------------------------------------- non-manifold edges
    size_t non_manifold_edges = 0;
    if (params.check_non_manifold_edges) {
        std::vector<std::pair<int, int>> bad_edges;
        std::vector<int>                 bad_faces;
        size_t                           max_fan = 0;
        for (const MeshEdge &e : edges) {
            if (! e.non_manifold())
                continue;
            ++ non_manifold_edges;
            max_fan = std::max(max_fan, e.incident.size());
            bad_edges.emplace_back(e.v0, e.v1);
            for (const FaceEdgeRef &ref : e.incident)
                bad_faces.push_back(ref.face);
        }
        if (non_manifold_edges > 0) {
            std::sort(bad_faces.begin(), bad_faces.end());
            bad_faces.erase(std::unique(bad_faces.begin(), bad_faces.end()), bad_faces.end());

            MeshIssue issue;
            issue.kind     = IssueKind::NonManifoldEdge;
            issue.severity = Severity::Blocking;
            issue.count    = non_manifold_edges;
            issue.metric   = double(max_fan);
            issue.issue_id = "nonmanifold-edges-1";
            store_capped(issue.edges, bad_edges, cap);
            store_capped(issue.face_ids, bad_faces, cap);
            for (int f : issue.face_ids)
                merge_face(issue.bbox, its, f);
            issue.explanation = std::to_string(non_manifold_edges) + " edge(s) are shared by more than two facets (up to " +
                                std::to_string(max_fan) + "). The surface branches there, so it has no consistent inside, and "
                                "most repair operations have to split the edge before anything else can be fixed.";
            result.issues.push_back(std::move(issue));
        }
    }

    // ------------------------------------------------------------------------ inverted normals
    if (params.check_inverted_normals) {
        std::vector<int> bad_faces;
        size_t           inconsistent = 0;
        for (const MeshEdge &e : edges) {
            if (! e.inconsistently_oriented())
                continue;
            ++ inconsistent;
            bad_faces.push_back(e.incident[0].face);
            bad_faces.push_back(e.incident[1].face);
        }
        if (inconsistent > 0) {
            std::sort(bad_faces.begin(), bad_faces.end());
            bad_faces.erase(std::unique(bad_faces.begin(), bad_faces.end()), bad_faces.end());

            MeshIssue issue;
            issue.kind     = IssueKind::InvertedNormal;
            issue.severity = Severity::Blocking;
            issue.count    = inconsistent;
            issue.metric   = double(bad_faces.size());
            issue.issue_id = "inverted-normals-1";
            store_capped(issue.face_ids, bad_faces, cap);
            for (int f : issue.face_ids)
                merge_face(issue.bbox, its, f);
            issue.explanation = std::to_string(inconsistent) + " shared edge(s) are walked the same way by both of their "
                                "facets, which means the two normals point in opposite directions. " +
                                std::to_string(bad_faces.size()) + " facet(s) are involved; re-orienting the surface "
                                "consistently is lossless.";
            result.issues.push_back(std::move(issue));
        }
    }

    // --------------------------------------------------------------------- disconnected shells
    if (params.check_disconnected_shells && shells > 1) {
        MeshIssue issue;
        issue.kind        = IssueKind::DisconnectedShell;
        issue.severity    = Severity::Info;
        issue.count       = shells;
        issue.metric      = double(shells);
        issue.issue_id    = "disconnected-shells-1";
        issue.bbox        = mesh_bbox;
        issue.explanation = "The mesh falls into " + std::to_string(shells) + " separate surfaces that touch nowhere. That is "
                            "fine for a deliberately multi-part model, but stray specks left over from a bad export print as "
                            "debris and are usually worth dropping.";
        result.issues.push_back(std::move(issue));
    }

    // ---------------------------------------------------------------------- zero volume shell
    if (params.check_zero_volume && ! its.indices.empty() && open_edges == 0 &&
        std::abs(volume) < params.zero_volume_eps) {
        MeshIssue issue;
        issue.kind        = IssueKind::ZeroVolumeShell;
        issue.severity    = Severity::Blocking;
        issue.count       = 1;
        issue.metric      = volume;
        issue.issue_id    = "zero-volume-shell-1";
        issue.bbox        = mesh_bbox;
        issue.explanation = "The mesh is closed but encloses no volume (" + format_number(volume, 9) + " mm³). It is either a "
                            "flat sheet folded onto itself or a surface duplicated back to back; there is nothing to slice.";
        result.issues.push_back(std::move(issue));
    }

    // ------------------------------------------------------------------------- skipped checks
    if (params.check_self_intersections) {
        std::string message;
        if (! check_self_intersections(its, result.issues, &message))
            result.skipped_checks.push_back(message);
    } else {
        result.skipped_checks.push_back("SelfIntersection: not run (CGAL check is out of scope for M0).");
    }
    if (params.check_inverted_shell) {
        std::string message;
        if (! check_inverted_shell(its, result.issues, &message))
            result.skipped_checks.push_back(message);
    } else {
        result.skipped_checks.push_back("InvertedShell: not run (winding-number check is out of scope for M0).");
    }

    result.manifold = open_edges == 0 && non_manifold_edges == 0;

    // ---------------------------------------------------------------------------------- summary
    {
        std::ostringstream oss;
        // Two edge counts, on purpose: `open_edges` is what libslic3r reports for this mesh, so the
        // number lines up with the rest of the application, while the boundary count comes from the
        // edge table, which ignores degenerate facets. They differ exactly when degenerate facets are
        // what leaves edges unpaired.
        oss << its.indices.size() << " facet(s), " << its.vertices.size() << " vertex/vertices, " << shells
            << " shell(s), volume " << format_number(volume) << " mm³, " << open_edges
            << " open edge(s), of which " << boundary_edge_count << " bound a real surface border; "
            << (result.manifold ? "manifold" : "not manifold") << ". ";
        if (result.issues.empty()) {
            oss << "No issues found.";
        } else {
            oss << result.issues.size() << " issue(s): ";
            for (size_t i = 0; i < result.issues.size(); ++ i) {
                if (i)
                    oss << ", ";
                oss << result.issues[i].issue_id << " (" << to_string(result.issues[i].kind) << " × "
                    << result.issues[i].count << ")";
            }
            oss << ".";
        }
        result.summary = oss.str();
    }

    return result;
}

} // namespace Nocte
} // namespace Slic3r
