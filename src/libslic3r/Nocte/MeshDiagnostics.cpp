// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/MeshDiagnostics.hpp"
#include "libslic3r/Nocte/NocteCgal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <Eigen/Core>

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

// Cap used by the two checks that do not receive DiagnosticsParams. Same value as the default.
constexpr size_t default_reported_primitives = 4096;

// A vertex is non-manifold when its incident facets fall into more than one fan: starting from one
// facet and hopping to facets that share an edge *through this vertex* cannot reach all of them.
// Two boxes joined at a single corner are the textbook case, and CGAL PMP's
// duplicate_non_manifold_vertices() is what pulls them apart.
//
// Facets that name the same vertex twice are ignored here, exactly as its_mesh_edges() ignores
// them; they are reported as degenerate facets instead.
size_t its_non_manifold_vertices(const indexed_triangle_set &its, std::vector<int> &out_vertices, std::vector<int> &out_faces)
{
    if (its.indices.empty() || its.vertices.empty())
        return 0;

    const VertexFaceIndex vertex_faces(its);

    size_t                           found = 0;
    std::vector<int>                 local_faces;
    std::vector<int>                 local_parent;
    // (the other endpoint of an edge at this vertex, the local facet that first used it)
    std::vector<std::pair<int, int>> spokes;

    for (int v = 0; v < int(its.vertices.size()); ++ v) {
        local_faces.clear();
        for (VertexFaceIndex::iterator it = vertex_faces.begin(size_t(v)); it != vertex_faces.end(size_t(v)); ++ it) {
            const stl_triangle_vertex_indices &tri = its.indices[*it];
            if (tri(0) == tri(1) || tri(1) == tri(2) || tri(0) == tri(2))
                continue;
            local_faces.push_back(int(*it));
        }
        if (local_faces.size() < 2)
            continue;

        local_parent.resize(local_faces.size());
        std::iota(local_parent.begin(), local_parent.end(), 0);
        auto root_of = [&local_parent](int x) {
            while (local_parent[size_t(x)] != x) {
                local_parent[size_t(x)] = local_parent[size_t(local_parent[size_t(x)])];
                x                       = local_parent[size_t(x)];
            }
            return x;
        };

        spokes.clear();
        for (size_t k = 0; k < local_faces.size(); ++ k) {
            const stl_triangle_vertex_indices &tri = its.indices[size_t(local_faces[k])];
            for (int c = 0; c < 3; ++ c) {
                if (tri(c) != v)
                    continue;
                const int neighbours[2] = { tri((c + 1) % 3), tri((c + 2) % 3) };
                for (int s = 0; s < 2; ++ s) {
                    bool joined = false;
                    for (const std::pair<int, int> &spoke : spokes)
                        if (spoke.first == neighbours[s]) {
                            const int a = root_of(int(k));
                            const int b = root_of(spoke.second);
                            if (a != b)
                                local_parent[size_t(b)] = a;
                            joined = true;
                            break;
                        }
                    if (! joined)
                        spokes.emplace_back(neighbours[s], int(k));
                }
                break;
            }
        }

        int fans = 0;
        for (size_t k = 0; k < local_faces.size(); ++ k)
            if (root_of(int(k)) == int(k))
                ++ fans;

        if (fans > 1) {
            ++ found;
            out_vertices.push_back(v);
            for (int f : local_faces)
                out_faces.push_back(f);
        }
    }
    return found;
}

// `outer` encloses `inner` with room to spare. Identical boxes do not count: a shell duplicated on
// top of itself is not nested inside its copy.
bool bbox_strictly_contains(const BoundingBoxf3 &outer, const BoundingBoxf3 &inner)
{
    if (! outer.defined || ! inner.defined)
        return false;
    for (int i = 0; i < 3; ++ i)
        if (outer.min(i) > inner.min(i) || outer.max(i) < inner.max(i))
            return false;
    const Vec3d outer_size = outer.max - outer.min;
    const Vec3d inner_size = inner.max - inner.min;
    return outer_size.prod() > inner_size.prod();
}

// Generalized winding number of `q` with respect to the triangle soup (V, F), by the
// Van Oosterom-Strackee formula: with a, b, c the corners of a facet taken relative to q, that
// facet subtends a signed solid angle of 2·atan2(det[a b c], |a||b||c| + (a·b)|c| + (b·c)|a| +
// (c·a)|b|). Summed over a closed, outward-oriented shell the solid angles come to 4π, so the
// halved form divided by 2π is +1 for a point inside and 0 for one outside; a shell wound the
// other way gives -1, which is why the caller takes the magnitude.
//
// This is the same arithmetic as igl::winding_number()'s single-point overload (igl/solid_angle.cpp
// divides by 2π for exactly this reason). It is spelled out here rather than included, because
// igl/winding_number.h is header-only in this tree and drags in the whole WindingNumberAABB
// hierarchy — some ninety headers, one of them <windows.h> — for a batch overload we never call.
double generalized_winding_number(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F, const Vec3d &q)
{
    // 2π. The winding number is the summed half-solid-angle over this.
    constexpr double two_pi = 6.28318530717958647692;

    double w = 0.;
    for (Eigen::Index f = 0; f < F.rows(); ++ f) {
        const Eigen::Index i0 = Eigen::Index(F(f, 0));
        const Eigen::Index i1 = Eigen::Index(F(f, 1));
        const Eigen::Index i2 = Eigen::Index(F(f, 2));
        const Vec3d a(V(i0, 0) - q.x(), V(i0, 1) - q.y(), V(i0, 2) - q.z());
        const Vec3d b(V(i1, 0) - q.x(), V(i1, 1) - q.y(), V(i1, 2) - q.z());
        const Vec3d c(V(i2, 0) - q.x(), V(i2, 1) - q.y(), V(i2, 2) - q.z());
        const double la = a.norm(), lb = b.norm(), lc = c.norm();
        const double denominator = la * lb * lc + a.dot(b) * lc + b.dot(c) * la + c.dot(a) * lb;
        // atan2(0, 0) is 0 on every platform this builds for, which is the right answer for a
        // facet degenerate enough that both arguments vanish: it subtends nothing.
        w += std::atan2(a.dot(b.cross(c)), denominator);
    }
    return w / two_pi;
}

} // namespace

std::vector<ShellOrientation> its_shell_orientations(const indexed_triangle_set &its,
                                                     size_t                      max_faces_for_winding,
                                                     std::string                *skipped_reason)
{
    std::vector<ShellOrientation> shells;
    const int                     face_count = int(its.indices.size());
    if (face_count == 0)
        return shells;

    const std::vector<MeshEdge> edges = its_mesh_edges(its);

    // ------------------------------------------------- edge-connected components, by union-find
    // static_cast rather than size_t(face_count): with the functional cast the whole statement
    // parses as the declaration of a function named parent (-Wvexing-parse).
    std::vector<int> parent(static_cast<size_t>(face_count));
    std::iota(parent.begin(), parent.end(), 0);
    auto root_of = [&parent](int x) {
        while (parent[size_t(x)] != x) {
            parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
            x                 = parent[size_t(x)];
        }
        return x;
    };
    for (const MeshEdge &edge : edges)
        for (size_t k = 1; k < edge.incident.size(); ++ k) {
            const int a = root_of(edge.incident[0].face);
            const int b = root_of(edge.incident[k].face);
            if (a != b)
                parent[size_t(b)] = a;
        }

    std::unordered_map<int, int> root_to_shell;
    std::vector<int>             shell_of(size_t(face_count), -1);
    for (int f = 0; f < face_count; ++ f) {
        const int root = root_of(f);
        auto      it   = root_to_shell.find(root);
        int       idx;
        if (it == root_to_shell.end()) {
            idx = int(shells.size());
            root_to_shell.emplace(root, idx);
            shells.emplace_back();
        } else {
            idx = it->second;
        }
        shell_of[size_t(f)] = idx;
        shells[size_t(idx)].faces.push_back(f);
    }

    // ----------------------------------------------------------- closedness, volume, bounding box
    std::vector<size_t> shell_edges(shells.size(), 0);
    std::vector<char>   shell_closed(shells.size(), 1);
    for (const MeshEdge &edge : edges) {
        const size_t s = size_t(shell_of[size_t(edge.incident[0].face)]);
        ++ shell_edges[s];
        if (edge.incident.size() != 2)
            shell_closed[s] = 0;
    }
    for (size_t s = 0; s < shells.size(); ++ s) {
        // A closed triangulated surface has exactly 3F/2 edges. A shell that also holds a facet
        // with a repeated vertex index fails this, because such a facet contributes no edges.
        if (shell_edges[s] * 2 != shells[s].faces.size() * 3)
            shell_closed[s] = 0;
        shells[s].closed = shell_closed[s] != 0;

        double volume = 0.;
        for (int f : shells[s].faces) {
            const stl_triangle_vertex_indices &tri = its.indices[size_t(f)];
            const Vec3d a = its.vertices[size_t(tri(0))].cast<double>();
            const Vec3d b = its.vertices[size_t(tri(1))].cast<double>();
            const Vec3d c = its.vertices[size_t(tri(2))].cast<double>();
            volume += a.dot(b.cross(c)) / 6.;
            shells[s].bbox.merge(a);
            shells[s].bbox.merge(b);
            shells[s].bbox.merge(c);
        }
        shells[s].volume = volume;
    }

    // --------------------------------------------------------------------------- nesting depth
    // Only shells whose bounding box is strictly inside another's can be nested, and that cheap
    // test is what separates a cavity from two solids that merely interpenetrate.
    std::vector<std::vector<int>> containers(shells.size());
    bool                          needs_winding = false;
    for (size_t i = 0; i < shells.size(); ++ i)
        for (size_t j = 0; j < shells.size(); ++ j)
            if (i != j && shells[j].closed && bbox_strictly_contains(shells[j].bbox, shells[i].bbox)) {
                containers[i].push_back(int(j));
                needs_winding = true;
            }

    const bool winding_affordable = its.indices.size() <= max_faces_for_winding;
    if (needs_winding && ! winding_affordable && skipped_reason != nullptr)
        *skipped_reason = "InvertedShell: nesting of " + std::to_string(shells.size()) + " shell(s) left undecided — the "
                          "winding-number test is capped at " + std::to_string(max_faces_for_winding) +
                          " facets and this mesh has " + std::to_string(its.indices.size()) + ". Shells that no other "
                          "shell encloses were still checked.";

    Eigen::MatrixXd              vertices;
    std::vector<Eigen::MatrixXi> shell_faces(shells.size());
    if (needs_winding && winding_affordable) {
        vertices.resize(Eigen::Index(its.vertices.size()), 3);
        for (size_t v = 0; v < its.vertices.size(); ++ v) {
            vertices(Eigen::Index(v), 0) = double(its.vertices[v].x());
            vertices(Eigen::Index(v), 1) = double(its.vertices[v].y());
            vertices(Eigen::Index(v), 2) = double(its.vertices[v].z());
        }
    }

    auto faces_of = [&its, &shells, &shell_faces](size_t j) -> const Eigen::MatrixXi & {
        Eigen::MatrixXi &F = shell_faces[j];
        if (F.rows() == 0) {
            F.resize(Eigen::Index(shells[j].faces.size()), 3);
            for (size_t k = 0; k < shells[j].faces.size(); ++ k) {
                const stl_triangle_vertex_indices &tri = its.indices[size_t(shells[j].faces[k])];
                F(Eigen::Index(k), 0) = tri(0);
                F(Eigen::Index(k), 1) = tri(1);
                F(Eigen::Index(k), 2) = tri(2);
            }
        }
        return F;
    };

    for (size_t i = 0; i < shells.size(); ++ i) {
        if (containers[i].empty()) {
            // Nothing encloses this shell's bounding box, so it is at depth 0 whatever the budget.
            shells[i].depth       = 0;
            shells[i].depth_known = true;
            continue;
        }
        if (! winding_affordable)
            continue;

        // Three sample points on the shell rather than one: a single vertex could happen to sit on
        // the enclosing surface, where the winding number is ambiguous. The median of the three is
        // robust to one such sample.
        const size_t n = shells[i].faces.size();
        Vec3d        samples[3];
        for (int s = 0; s < 3; ++ s) {
            const size_t                       face = shells[i].faces[size_t(s) * (n - 1) / 2];
            const stl_triangle_vertex_indices &tri  = its.indices[face];
            samples[s] = its.vertices[size_t(tri(s))].cast<double>();
        }

        int depth = 0;
        for (int j : containers[i]) {
            const Eigen::MatrixXi &F = faces_of(size_t(j));
            long long              rounded[3] = { 0, 0, 0 };
            for (int s = 0; s < 3; ++ s) {
                // Normalised so that a point inside a closed, consistently oriented shell comes
                // back as ±1 rather than ±4π; the sign follows the shell's winding, which is why
                // only the magnitude is used.
                const double wn = generalized_winding_number(vertices, F, samples[s]);
                rounded[s]      = std::llround(std::fabs(wn));
            }
            std::sort(rounded, rounded + 3);
            depth += int(rounded[1]);
        }
        shells[i].depth       = depth;
        shells[i].depth_known = true;
    }

    // --------------------------------------------------------------------------------- verdict
    for (ShellOrientation &shell : shells) {
        shell.inverted = false;
        if (! shell.closed || ! shell.depth_known)
            continue;
        // A shell that encloses nothing is a ZeroVolumeShell, reported on its own terms.
        if (std::abs(shell.volume) < 1e-12)
            continue;
        const bool should_be_negative = (shell.depth % 2) == 1;
        shell.inverted                = (shell.volume < 0.) != should_be_negative;
    }

    return shells;
}

bool check_self_intersections(const indexed_triangle_set &its, std::vector<MeshIssue> &out, std::string *error_message,
                              size_t max_faces)
{
    if (its.indices.empty())
        return true;

    if (its.indices.size() > max_faces) {
        if (error_message)
            *error_message = "SelfIntersection: not run — the mesh has " + std::to_string(its.indices.size()) +
                             " facets, past the limit of " + std::to_string(max_faces) +
                             " where the CGAL test stops being quick enough to run interactively.";
        return false;
    }

    const cgal::SelfIntersectionResult probe = cgal::self_intersections(its);
    if (! probe.ok) {
        if (error_message)
            *error_message = "SelfIntersection: " + probe.message;
        return false;
    }
    if (! probe.intersects)
        return true;

    MeshIssue issue;
    issue.kind     = IssueKind::SelfIntersection;
    issue.severity = Severity::Blocking;
    issue.count    = probe.pairs > 0 ? size_t(probe.pairs) : 1;
    issue.metric   = double(probe.pairs);
    issue.issue_id = "self-intersections-1";
    issue.bbox     = Slic3r::bounding_box(its);
    {
        std::vector<int> faces;
        faces.reserve(probe.face_pairs.size() * 2);
        for (const std::pair<int, int> &pair : probe.face_pairs) {
            faces.push_back(pair.first);
            faces.push_back(pair.second);
        }
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        // Empty while NocteCgal only answers the yes/no question; filled the day
        // PMP::self_intersections() is adopted, without any change here.
        store_capped(issue.face_ids, faces, default_reported_primitives);
    }
    issue.explanation = "The surface passes through itself: at least one pair of facets crosses. Inside and outside are "
                        "no longer well defined there, so a boolean or a shell offset can produce nonsense even though "
                        "the mesh looks closed. The exact number of crossing pairs is not counted — CGAL is only asked "
                        "the yes/no question, which is far cheaper — so this issue reports one occurrence.";
    out.push_back(std::move(issue));
    return true;
}

bool check_inverted_shell(const indexed_triangle_set &its, std::vector<MeshIssue> &out, std::string *error_message,
                          size_t max_faces_for_winding)
{
    if (its.indices.empty())
        return true;

    std::string                         skipped;
    const std::vector<ShellOrientation> shells = its_shell_orientations(its, max_faces_for_winding, &skipped);

    int index = 0;
    for (const ShellOrientation &shell : shells) {
        if (! shell.inverted)
            continue;

        MeshIssue issue;
        issue.kind     = IssueKind::InvertedShell;
        issue.severity = Severity::Blocking;
        issue.count    = shell.faces.size();
        issue.metric   = shell.volume;
        issue.issue_id = "inverted-shell-" + std::to_string(++ index);
        issue.bbox     = shell.bbox;
        store_capped(issue.face_ids, shell.faces, default_reported_primitives);
        issue.explanation = "A closed shell of " + std::to_string(shell.faces.size()) + " facet(s) is oriented "
                            "consistently but points the wrong way: it " +
                            (shell.depth % 2 == 1
                                 ? std::string("sits inside another shell, so it should face inwards as a cavity, and does not")
                                 : std::string("encloses a negative volume, so its normals face inwards instead of outwards")) +
                            ". Nothing is wrong with the shape; flipping the shell's facets fixes it without moving a "
                            "vertex.";
        out.push_back(std::move(issue));
    }

    // Anything found above is reported either way; a non-empty `skipped` only says that some shells
    // could not be judged.
    if (! skipped.empty()) {
        if (error_message)
            *error_message = skipped;
        return false;
    }
    return true;
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

    // ------------------------------------------------------------------- non-manifold vertices
    if (params.check_non_manifold_vertices) {
        std::vector<int> bad_vertices;
        std::vector<int> bad_faces;
        const size_t     count = its_non_manifold_vertices(its, bad_vertices, bad_faces);
        if (count > 0) {
            std::sort(bad_faces.begin(), bad_faces.end());
            bad_faces.erase(std::unique(bad_faces.begin(), bad_faces.end()), bad_faces.end());

            MeshIssue issue;
            issue.kind     = IssueKind::NonManifoldVertex;
            issue.severity = Severity::Blocking;
            issue.count    = count;
            issue.metric   = double(bad_faces.size());
            issue.issue_id = "nonmanifold-vertices-1";
            store_capped(issue.face_ids, bad_faces, cap);
            for (int v : bad_vertices)
                merge_vertex(issue.bbox, its, v);
            issue.explanation = std::to_string(count) + " vertex/vertices are pinch points: the facets around them fall "
                                "into more than one fan, so the surface touches itself at a single point without sharing "
                                "an edge. Splitting the vertex into one copy per fan separates them and moves nothing.";
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
    // Both checks append whatever they could establish and return false only when something was
    // left undecided, which is what `skipped_checks` is for.
    if (params.check_self_intersections) {
        std::string message;
        if (! check_self_intersections(its, result.issues, &message, params.max_faces_self_intersection))
            result.skipped_checks.push_back(message);
    } else {
        result.skipped_checks.push_back("SelfIntersection: not run (turned off in DiagnosticsParams).");
    }
    if (params.check_inverted_shell) {
        std::string message;
        if (! check_inverted_shell(its, result.issues, &message, params.max_faces_inverted_shell))
            result.skipped_checks.push_back(message);
    } else {
        result.skipped_checks.push_back("InvertedShell: not run (turned off in DiagnosticsParams).");
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
