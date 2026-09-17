// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Vocabulary of the repair pipeline: the operations a plan can contain, the parameters they take,
// the metrics used to show what a step changed, and the result of running one. Split out from
// MeshRepair.hpp so the report and the GUI can describe a plan without pulling in the session.

#ifndef slic3r_Nocte_RepairOps_hpp_
#define slic3r_Nocte_RepairOps_hpp_

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace Nocte {

// Tier 1 is topological and lossless, tier 2 rebuilds the surface, tier 3 resamples it.
// Escalation is always visible to the user, and every tier-3 operation is lossy by construction.
enum class RepairOpKind {
    MergeVertices,
    RemoveDegenerateFaces,
    DuplicateNonManifoldVertices,
    StitchBorders,
    OrientConsistently,
    FlipShell,
    RemoveTinyShells,
    FillHolesCgal,
    NormalizeManifold,
    VoxelRemesh
};

std::string to_string(RepairOpKind kind);

struct RepairOpParams
{
    // RemoveDegenerateFaces: facets below this area are dropped as well as facets that reference
    // the same vertex index twice. Keep it aligned with DiagnosticsParams::degenerate_area_eps.
    double degenerate_area_eps        = 1e-8;
    // RemoveTinyShells: a shell is tiny when |volume| is below this fraction of the largest shell's.
    double tiny_shell_volume_fraction = 1e-3;
    // RemoveTinyShells: a shell with at most this many facets is tiny regardless of its volume.
    // Zero disables the facet criterion.
    int    tiny_shell_max_faces       = 0;
    // FillHolesCgal (M1): holes longer than this are left alone. Zero means no limit.
    double hole_max_perimeter         = 0.;
    // VoxelRemesh (M3): zero means min(bbox) / 512.
    double voxel_size                 = 0.;
};

struct RepairOp
{
    RepairOpKind             kind  = RepairOpKind::MergeVertices;
    int                      tier  = 1;
    std::string              title;
    // Why this step is in the plan, naming the issue ids it addresses.
    std::string              rationale;
    std::vector<std::string> targets_issue_ids;
    RepairOpParams           params;
    // The step changes geometry the user modelled, not just its topology.
    bool                     lossy = false;
    // False for operations the plan can propose but this build cannot run yet. accept() on such a
    // step fails with a message rather than silently doing nothing.
    bool                     implemented = false;
};

struct MeshMetrics
{
    int    tris              = 0;
    int    open_edges        = 0;
    int    shells            = 0;
    // -1 when not measured. Counting self-intersections needs CGAL, which M0 does not call.
    int    self_intersections = -1;
    double volume            = 0.;
    double surface_area      = 0.;
    // Estimated one-sided Hausdorff distance to the mesh the session started from. Zero for every
    // lossless step; only the tier-3 fallbacks are expected to report a non-zero value (M3).
    double hausdorff_est     = 0.;
};

struct RepairStepResult
{
    bool                      succeeded = false;
    MeshMetrics               before;
    MeshMetrics               after;
    std::vector<int>          touched_faces;
    std::string               message;
    std::chrono::milliseconds duration{0};
};

enum class RepairStepState { Pending, Applied, Rejected, Failed };

std::string to_string(RepairStepState state);

struct RepairPlan
{
    std::vector<RepairOp> ops;

    bool   empty() const { return ops.empty(); }
    size_t size() const { return ops.size(); }
    // Index of the first operation of that kind, or -1.
    int    index_of(RepairOpKind kind) const;
    bool   contains(RepairOpKind kind) const { return index_of(kind) >= 0; }
};

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_RepairOps_hpp_
