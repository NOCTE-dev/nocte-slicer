// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Step-by-step, explainable mesh repair. A plan is derived from the diagnostics, every step names
// the issue ids it targets, and the user accepts or rejects each one; a bounded snapshot stack
// makes accepting reversible. The session is wx-free and includes no CGAL header of its own: the
// operations that need CGAL go through Nocte/NocteCgal.hpp, whose implementation is compiled into
// the libslic3r_cgal target. Tier 2 and tier 3 are planned but not yet executable.

#ifndef slic3r_Nocte_MeshRepair_hpp_
#define slic3r_Nocte_MeshRepair_hpp_

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/MeshDiagnostics.hpp"
#include "libslic3r/Nocte/RepairOps.hpp"
#include "libslic3r/Nocte/Report.hpp"

namespace Slic3r {
namespace Nocte {

// Cheap enough to call before and after every step; surface_area is exact. self_intersections is
// filled with 0 or 1 on meshes small enough for the CGAL test to be quick and stays at -1 on the
// rest, because the step runs it four times per accept.
MeshMetrics compute_metrics(const indexed_triangle_set &its);

// Derives the ordered repair plan from a diagnosis. Steps come out sorted by tier, and each one
// carries a rationale naming the issue ids it addresses. Operations this build cannot run yet are
// still emitted, with `implemented == false`, so the user can see the escalation path.
RepairPlan plan_from(const DiagnosticsResult &diagnostics, const RepairOpParams &defaults = RepairOpParams());

class RepairSession
{
public:
    // Snapshots are full meshes, so the stack is bounded.
    // [VERIFY] memory behaviour on 10M-triangle meshes (M3).
    static constexpr size_t max_snapshots = 8;

    explicit RepairSession(indexed_triangle_set mesh, const DiagnosticsParams &params = DiagnosticsParams());

    const RepairPlan       &plan() const { return m_plan; }
    // Runs the step on a copy and reports what it would change. The session is left untouched.
    RepairStepResult        preview(size_t i);
    // Runs the step, pushes a snapshot first, and re-diagnoses on success.
    RepairStepResult        accept(size_t i);
    void                    reject(size_t i);
    // Restores the mesh from before the most recent accepted step.
    void                    undo();
    bool                    can_undo() const { return ! m_snapshots.empty(); }

    const indexed_triangle_set &current() const { return m_mesh; }
    // The diagnosis the plan was built from, refreshed after every accepted step and every undo.
    DiagnosticsResult       diagnostics() const { return m_diagnostics; }
    NocteReport             report() const;

    RepairStepState         state_of(size_t i) const;
    const std::vector<RepairStepResult> &results() const { return m_results; }

private:
    RepairStepResult        run(RepairOpKind kind, const RepairOpParams &params, indexed_triangle_set &mesh) const;
    void                    push_snapshot(size_t step);

    indexed_triangle_set            m_mesh;
    DiagnosticsParams               m_diag_params;
    // The diagnosis the session opened with. The report shows it next to the current one.
    DiagnosticsResult               m_initial_diagnostics;
    DiagnosticsResult               m_diagnostics;
    // The plan is built once, at construction, and its indices stay valid for the session's life:
    // the GUI holds on to them. Diagnostics are refreshed, the plan is not.
    RepairPlan                      m_plan;
    std::vector<RepairStepState>    m_states;
    std::vector<RepairStepResult>   m_results;
    std::deque<indexed_triangle_set> m_snapshots;
    // Accepted step indices, newest last; parallel to m_snapshots so undo() knows what to unmark.
    std::deque<size_t>              m_snapshot_steps;
};

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_MeshRepair_hpp_
