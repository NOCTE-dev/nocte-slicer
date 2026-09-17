// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/MeshRepair.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace Slic3r {
namespace Nocte {

std::string to_string(RepairOpKind kind)
{
    switch (kind) {
    case RepairOpKind::MergeVertices:                return "MergeVertices";
    case RepairOpKind::RemoveDegenerateFaces:        return "RemoveDegenerateFaces";
    case RepairOpKind::DuplicateNonManifoldVertices: return "DuplicateNonManifoldVertices";
    case RepairOpKind::StitchBorders:                return "StitchBorders";
    case RepairOpKind::OrientConsistently:           return "OrientConsistently";
    case RepairOpKind::FlipShell:                    return "FlipShell";
    case RepairOpKind::RemoveTinyShells:             return "RemoveTinyShells";
    case RepairOpKind::FillHolesCgal:                return "FillHolesCgal";
    case RepairOpKind::NormalizeManifold:            return "NormalizeManifold";
    case RepairOpKind::VoxelRemesh:                  return "VoxelRemesh";
    }
    return "Unknown";
}

std::string to_string(RepairStepState state)
{
    switch (state) {
    case RepairStepState::Pending:  return "Pending";
    case RepairStepState::Applied:  return "Applied";
    case RepairStepState::Rejected: return "Rejected";
    case RepairStepState::Failed:   return "Failed";
    }
    return "Unknown";
}

int RepairPlan::index_of(RepairOpKind kind) const
{
    for (size_t i = 0; i < this->ops.size(); ++ i)
        if (this->ops[i].kind == kind)
            return int(i);
    return -1;
}

namespace {

const char *op_title(RepairOpKind kind)
{
    switch (kind) {
    case RepairOpKind::MergeVertices:                return "Merge coincident vertices";
    case RepairOpKind::RemoveDegenerateFaces:        return "Remove degenerate facets";
    case RepairOpKind::DuplicateNonManifoldVertices: return "Split non-manifold vertices";
    case RepairOpKind::StitchBorders:                return "Stitch matching borders";
    case RepairOpKind::OrientConsistently:           return "Orient facets consistently";
    case RepairOpKind::FlipShell:                    return "Flip an inside-out shell";
    case RepairOpKind::RemoveTinyShells:             return "Remove tiny stray shells";
    case RepairOpKind::FillHolesCgal:                return "Fill remaining holes";
    case RepairOpKind::NormalizeManifold:            return "Normalise to a manifold solid";
    case RepairOpKind::VoxelRemesh:                  return "Rebuild the surface from a voxel grid";
    }
    return "Unknown operation";
}

std::string join(const std::vector<std::string> &items)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++ i) {
        if (i)
            out += ", ";
        out += items[i];
    }
    return out;
}

std::string fixed(double value, int decimals = 3)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    return oss.str();
}

double facet_area(const indexed_triangle_set &its, size_t face_id)
{
    const stl_triangle_vertex_indices &tri = its.indices[face_id];
    const Vec3d a = its.vertices[tri(0)].cast<double>();
    const Vec3d b = its.vertices[tri(1)].cast<double>();
    const Vec3d c = its.vertices[tri(2)].cast<double>();
    return 0.5 * (b - a).cross(c - a).norm();
}

// Drops facets that reference the same vertex twice (what its_remove_degenerate_faces() removes)
// and, in the same pass, facets whose area is below `area_eps` — collinear slivers that the
// upstream helper keeps but that diagnose() reports. Keeps `properties` in step with `indices`.
int remove_degenerate_facets(indexed_triangle_set &its, double area_eps, std::vector<int> &removed_faces)
{
    const bool has_properties = its.properties.size() == its.indices.size();

    size_t write = 0;
    for (size_t read = 0; read < its.indices.size(); ++ read) {
        const stl_triangle_vertex_indices &tri = its.indices[read];
        const bool repeated = tri(0) == tri(1) || tri(1) == tri(2) || tri(0) == tri(2);
        if (repeated || facet_area(its, read) < area_eps) {
            removed_faces.push_back(int(read));
            continue;
        }
        if (write != read) {
            its.indices[write] = its.indices[read];
            if (has_properties)
                its.properties[write] = its.properties[read];
        }
        ++ write;
    }

    const int removed = int(its.indices.size() - write);
    if (removed > 0) {
        its.indices.erase(its.indices.begin() + write, its.indices.end());
        if (has_properties)
            its.properties.erase(its.properties.begin() + write, its.properties.end());
    }
    return removed;
}

// Signed volume of a subset of facets around the origin, honouring a per-facet flip flag.
double patch_signed_volume(const indexed_triangle_set &its, const std::vector<int> &patch, const std::vector<char> &flip)
{
    double volume = 0.;
    for (int f : patch) {
        const stl_triangle_vertex_indices &tri = its.indices[f];
        Vec3d a = its.vertices[tri(0)].cast<double>();
        Vec3d b = its.vertices[tri(1)].cast<double>();
        Vec3d c = its.vertices[tri(2)].cast<double>();
        if (flip[f])
            std::swap(b, c);
        volume += a.dot(b.cross(c)) / 6.;
    }
    return volume;
}

// Makes every connected patch of `its` internally consistent, then flips the whole patch if that
// leaves it enclosing a negative volume, so normals end up pointing outwards. Returns the number of
// facets flipped and records them in `flipped`.
//
// Best effort by construction: on a surface that cannot be oriented at all (a Klein-bottle-like
// mesh, or one whose non-manifold edges branch), the traversal keeps the first assignment it made
// for a facet and the contradiction stays. diagnose() will still report the leftover edges, which
// is what escalates the plan to a tier-2 operation.
int orient_consistently(indexed_triangle_set &its, std::vector<int> &flipped)
{
    const int face_count = int(its.indices.size());
    if (face_count == 0)
        return 0;

    std::vector<std::vector<std::pair<int, bool>>> adjacency(face_count);
    for (const MeshEdge &edge : its_mesh_edges(its)) {
        // Only an edge shared by exactly two facets carries an orientation constraint.
        if (edge.incident.size() != 2)
            continue;
        const int  f0            = edge.incident[0].face;
        const int  f1            = edge.incident[1].face;
        const bool inconsistent  = edge.incident[0].forward == edge.incident[1].forward;
        adjacency[f0].emplace_back(f1, inconsistent);
        adjacency[f1].emplace_back(f0, inconsistent);
    }

    std::vector<char> visited(face_count, 0);
    std::vector<char> flip(face_count, 0);
    std::vector<int>  patch;
    std::vector<int>  stack;

    for (int seed = 0; seed < face_count; ++ seed) {
        if (visited[seed])
            continue;
        patch.clear();
        stack.clear();
        visited[seed] = 1;
        flip[seed]    = 0;
        stack.push_back(seed);
        while (! stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            patch.push_back(f);
            for (const std::pair<int, bool> &neighbour : adjacency[f]) {
                if (visited[neighbour.first])
                    continue;
                visited[neighbour.first] = 1;
                flip[neighbour.first]    = char(flip[f] ^ (neighbour.second ? 1 : 0));
                stack.push_back(neighbour.first);
            }
        }
        if (patch_signed_volume(its, patch, flip) < 0.)
            for (int f : patch)
                flip[f] = char(flip[f] ^ 1);
    }

    for (int f = 0; f < face_count; ++ f)
        if (flip[f]) {
            std::swap(its.indices[f](1), its.indices[f](2));
            flipped.push_back(f);
        }
    return int(flipped.size());
}

// Splits the mesh and drops the shells that are negligible next to the largest one, then merges the
// survivors back. Never drops everything: if every shell qualifies, the largest is kept.
int remove_tiny_shells(indexed_triangle_set &its, const RepairOpParams &params, std::vector<int> &dropped_facet_counts)
{
    std::vector<indexed_triangle_set> parts = its_split(its);
    if (parts.size() < 2)
        return 0;

    std::vector<double> volumes(parts.size(), 0.);
    double              largest = 0.;
    for (size_t i = 0; i < parts.size(); ++ i) {
        volumes[i] = std::abs(double(its_volume(parts[i])));
        largest    = std::max(largest, volumes[i]);
    }

    std::vector<char> keep(parts.size(), 1);
    size_t            kept = 0;
    for (size_t i = 0; i < parts.size(); ++ i) {
        bool tiny = largest > 0. && volumes[i] < params.tiny_shell_volume_fraction * largest;
        if (params.tiny_shell_max_faces > 0 && int(parts[i].indices.size()) <= params.tiny_shell_max_faces)
            tiny = true;
        keep[i] = tiny ? 0 : 1;
        kept += keep[i] ? 1 : 0;
    }
    if (kept == 0) {
        const size_t best = size_t(std::max_element(volumes.begin(), volumes.end()) - volumes.begin());
        keep[best] = 1;
    }

    int                  dropped = 0;
    indexed_triangle_set merged;
    for (size_t i = 0; i < parts.size(); ++ i) {
        if (keep[i]) {
            its_merge(merged, parts[i]);
        } else {
            ++ dropped;
            dropped_facet_counts.push_back(int(parts[i].indices.size()));
        }
    }
    if (dropped > 0)
        its = std::move(merged);
    return dropped;
}

} // namespace

MeshMetrics compute_metrics(const indexed_triangle_set &its)
{
    MeshMetrics metrics;
    metrics.tris       = int(its.indices.size());
    metrics.open_edges = int(its_num_open_edges(its));
    metrics.shells     = its.indices.empty() ? 0 : int(its_number_of_patches(its));
    metrics.volume     = double(its_volume(its));

    double area = 0.;
    for (size_t f = 0; f < its.indices.size(); ++ f)
        area += facet_area(its, f);
    metrics.surface_area = area;

    // self_intersections stays at -1: counting them needs CGAL, which this target does not link.
    return metrics;
}

RepairPlan plan_from(const DiagnosticsResult &diagnostics, const RepairOpParams &defaults)
{
    RepairPlan plan;

    auto ids_of = [&diagnostics](IssueKind kind) {
        std::vector<std::string> ids;
        for (const MeshIssue &issue : diagnostics.issues)
            if (issue.kind == kind)
                ids.push_back(issue.issue_id);
        return ids;
    };

    auto add = [&plan, &defaults](RepairOpKind kind, int tier, bool lossy, bool implemented,
                                  std::vector<std::string> targets, const std::string &why) {
        RepairOp op;
        op.kind              = kind;
        op.tier              = tier;
        op.lossy             = lossy;
        op.implemented       = implemented;
        op.title             = op_title(kind);
        op.params            = defaults;
        op.targets_issue_ids = std::move(targets);
        op.rationale         = op.targets_issue_ids.empty()
                                   ? why
                                   : "Targets " + join(op.targets_issue_ids) + ". " + why;
        plan.ops.push_back(std::move(op));
    };

    // ------------------------------------------------------------------ tier 1, topological
    if (diagnostics.has(IssueKind::DegenerateFacet))
        add(RepairOpKind::RemoveDegenerateFaces, 1, false, true, ids_of(IssueKind::DegenerateFacet),
            "Facets that enclose no area carry no surface; dropping them first keeps every later step "
            "from tripping over edges of zero length.");

    if (diagnostics.has(IssueKind::DuplicateVertex))
        add(RepairOpKind::MergeVertices, 1, false, true, ids_of(IssueKind::DuplicateVertex),
            "Vertices that sit on the same position keep neighbouring facets from sharing an edge. "
            "Merging them is lossless and usually closes most of the open edges on its own.");

    if (diagnostics.has(IssueKind::InvertedNormal))
        add(RepairOpKind::OrientConsistently, 1, false, true, ids_of(IssueKind::InvertedNormal),
            "Neighbouring facets disagree about which side is outside. Re-orienting the surface moves no "
            "vertex, so the shape is unchanged.");

    if (diagnostics.has(IssueKind::InvertedShell))
        add(RepairOpKind::FlipShell, 1, false, false, ids_of(IssueKind::InvertedShell),
            "The shell is consistently oriented but inside out. Not implemented in M0.");

    if (diagnostics.has(IssueKind::NonManifoldEdge))
        add(RepairOpKind::DuplicateNonManifoldVertices, 1, false, false, ids_of(IssueKind::NonManifoldEdge),
            "Edges shared by more than two facets have to be split before the surface can be closed. "
            "Needs CGAL PMP duplicate_non_manifold_vertices (M1).");

    if (diagnostics.has(IssueKind::OpenBoundaryLoop)) {
        const std::vector<std::string> loops = ids_of(IssueKind::OpenBoundaryLoop);
        add(RepairOpKind::StitchBorders, 1, false, false, loops,
            "Borders that already match up geometrically can be sewn together without adding geometry. "
            "Needs CGAL PMP stitch_borders (M1).");
        add(RepairOpKind::FillHolesCgal, 1, false, false, loops,
            "What is left after stitching is a genuine hole and has to be triangulated and faired. "
            "Needs CGAL PMP triangulate_refine_and_fair_hole (M1).");
    }

    if (diagnostics.has(IssueKind::DisconnectedShell))
        add(RepairOpKind::RemoveTinyShells, 1, true, true, ids_of(IssueKind::DisconnectedShell),
            "Shells that are negligible next to the main body are export debris and print as specks. "
            "Dropping them removes geometry, so the step is marked lossy.");

    // ------------------------------------------------------------------------- tier 2, rebuild
    const bool needs_rebuild = ! diagnostics.manifold ||
                               diagnostics.has(IssueKind::ZeroVolumeShell) ||
                               diagnostics.has(IssueKind::SelfIntersection);
    if (needs_rebuild) {
        std::vector<std::string> targets = ids_of(IssueKind::ZeroVolumeShell);
        for (std::string &id : ids_of(IssueKind::SelfIntersection))
            targets.push_back(std::move(id));
        for (std::string &id : ids_of(IssueKind::OpenBoundaryLoop))
            targets.push_back(std::move(id));
        add(RepairOpKind::NormalizeManifold, 2, false, false, std::move(targets),
            "Fallback for whatever tier 1 could not close: MeshBoolean::cgal::repair, then a Manifold "
            "self-union. Needs the Manifold dependency (M2).");
    }

    // ----------------------------------------------------------------------- tier 3, resample
    if (needs_rebuild)
        add(RepairOpKind::VoxelRemesh, 3, true, false, ids_of(IssueKind::OpenBoundaryLoop),
            "Last resort: rasterise to an SDF and re-extract the surface. Always lossy, so it only runs "
            "when the user asks for it after tier 2 failed. Needs OpenVDBUtils (M3).");

    std::stable_sort(plan.ops.begin(), plan.ops.end(),
                     [](const RepairOp &l, const RepairOp &r) { return l.tier < r.tier; });
    return plan;
}

RepairSession::RepairSession(indexed_triangle_set mesh, const DiagnosticsParams &params)
    : m_mesh(std::move(mesh)), m_diag_params(params)
{
    m_initial_diagnostics = diagnose(m_mesh, m_diag_params);
    m_diagnostics         = m_initial_diagnostics;
    m_plan                = plan_from(m_diagnostics);
    m_states.assign(m_plan.size(), RepairStepState::Pending);
    m_results.assign(m_plan.size(), RepairStepResult());
}

RepairStepResult RepairSession::run(RepairOpKind kind, const RepairOpParams &params, indexed_triangle_set &mesh) const
{
    const auto started = std::chrono::steady_clock::now();

    RepairStepResult result;
    result.before = compute_metrics(mesh);

    switch (kind) {
    case RepairOpKind::RemoveDegenerateFaces: {
        const int removed = remove_degenerate_facets(mesh, params.degenerate_area_eps, result.touched_faces);
        result.succeeded  = true;
        result.message    = removed > 0 ? "Removed " + std::to_string(removed) + " degenerate facet(s)."
                                        : "No degenerate facets left to remove.";
        break;
    }
    case RepairOpKind::MergeVertices: {
        const int merged = its_merge_vertices(mesh);
        result.succeeded = true;
        result.message   = merged > 0 ? "Merged " + std::to_string(merged) + " coincident vertex/vertices."
                                      : "No coincident vertices left to merge.";
        break;
    }
    case RepairOpKind::OrientConsistently: {
        const int flipped = orient_consistently(mesh, result.touched_faces);
        result.succeeded  = true;
        result.message    = flipped > 0 ? "Flipped " + std::to_string(flipped) + " facet(s) so the surface is oriented "
                                          "consistently outwards."
                                        : "The surface was already oriented consistently.";
        break;
    }
    case RepairOpKind::RemoveTinyShells: {
        std::vector<int> dropped_facet_counts;
        const int        dropped = remove_tiny_shells(mesh, params, dropped_facet_counts);
        result.succeeded = true;
        if (dropped > 0) {
            int facets = 0;
            for (int n : dropped_facet_counts)
                facets += n;
            // Facet indices are rebuilt by the split/merge, so touched_faces would be meaningless here.
            result.message = "Dropped " + std::to_string(dropped) + " shell(s) totalling " + std::to_string(facets) +
                             " facet(s), below " + fixed(params.tiny_shell_volume_fraction * 100., 4) +
                             "% of the largest shell's volume.";
        } else {
            result.message = "No shell was small enough to drop.";
        }
        break;
    }
    default:
        result.succeeded = false;
        result.message   = std::string(op_title(kind)) + " (" + to_string(kind) + ") is not implemented in M0.";
        break;
    }

    result.after    = compute_metrics(mesh);
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return result;
}

RepairStepResult RepairSession::preview(size_t i)
{
    RepairStepResult result;
    if (i >= m_plan.size()) {
        result.message = "Step " + std::to_string(i) + " is not part of the plan.";
        return result;
    }
    indexed_triangle_set candidate = m_mesh;
    return this->run(m_plan.ops[i].kind, m_plan.ops[i].params, candidate);
}

RepairStepResult RepairSession::accept(size_t i)
{
    RepairStepResult result;
    if (i >= m_plan.size()) {
        result.message = "Step " + std::to_string(i) + " is not part of the plan.";
        return result;
    }

    indexed_triangle_set candidate = m_mesh;
    result = this->run(m_plan.ops[i].kind, m_plan.ops[i].params, candidate);
    if (result.succeeded) {
        this->push_snapshot(i);
        m_mesh        = std::move(candidate);
        m_diagnostics = diagnose(m_mesh, m_diag_params);
        m_states[i]   = RepairStepState::Applied;
    } else {
        m_states[i] = RepairStepState::Failed;
    }
    m_results[i] = result;
    return result;
}

void RepairSession::reject(size_t i)
{
    if (i < m_states.size())
        m_states[i] = RepairStepState::Rejected;
}

void RepairSession::push_snapshot(size_t step)
{
    m_snapshots.push_back(m_mesh);
    m_snapshot_steps.push_back(step);
    while (m_snapshots.size() > max_snapshots) {
        m_snapshots.pop_front();
        m_snapshot_steps.pop_front();
    }
}

void RepairSession::undo()
{
    if (m_snapshots.empty())
        return;
    m_mesh = std::move(m_snapshots.back());
    m_snapshots.pop_back();

    const size_t step = m_snapshot_steps.back();
    m_snapshot_steps.pop_back();
    if (step < m_states.size()) {
        m_states[step]  = RepairStepState::Pending;
        m_results[step] = RepairStepResult();
    }

    m_diagnostics = diagnose(m_mesh, m_diag_params);
}

RepairStepState RepairSession::state_of(size_t i) const
{
    return i < m_states.size() ? m_states[i] : RepairStepState::Pending;
}

NocteReport RepairSession::report() const
{
    NocteReport report;
    report.diagnostics = m_initial_diagnostics;

    bool any_applied = false;
    for (size_t i = 0; i < m_plan.size(); ++ i) {
        ReportStep step;
        step.op     = m_plan.ops[i];
        step.result = m_results[i];
        step.state  = m_states[i];
        any_applied = any_applied || step.state == RepairStepState::Applied;
        report.steps.push_back(std::move(step));
    }
    if (any_applied)
        report.diagnostics_after = m_diagnostics;

    return report;
}

} // namespace Nocte
} // namespace Slic3r
