// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The explainable-repair artefact: everything that was diagnosed, everything that was done about
// it, and why. Persisted additively as Metadata/nocte_report.json inside the 3mf, which Bambu
// Studio ignores (ADR-001), and rendered as Markdown for the console and the PR-style summaries.

#ifndef slic3r_Nocte_Report_hpp_
#define slic3r_Nocte_Report_hpp_

#include <optional>
#include <string>
#include <vector>

#include "libslic3r/Nocte/AutoTune.hpp"
#include "libslic3r/Nocte/GeometryAnalysis.hpp"
#include "libslic3r/Nocte/MeshDiagnostics.hpp"
#include "libslic3r/Nocte/NocteVersion.hpp"
#include "libslic3r/Nocte/RepairOps.hpp"

namespace Slic3r {
namespace Nocte {

// One entry of the repair log: the operation as it was planned, plus what happened to it.
struct ReportStep
{
    RepairOp         op;
    RepairStepResult result;
    RepairStepState  state = RepairStepState::Pending;
};

struct NocteReport
{
    std::string              schema        = NOCTE_REPORT_SCHEMA;
    std::string              nocte_version = NOCTE_VERSION;
    std::string              app_name      = NOCTE_APP_NAME;
    // Free-form label of what was inspected, e.g. the object name. Optional.
    std::string              subject;

    DiagnosticsResult        diagnostics;
    // Diagnostics re-run after the last accepted step, when there was one.
    std::optional<DiagnosticsResult> diagnostics_after;
    std::vector<ReportStep>  steps;

    std::optional<PartFeatures> features;
    std::vector<Recommendation> recs;

    // `indent` < 0 emits the compact form, as nlohmann::json::dump() defines it.
    std::string to_json(int indent = 2) const;
    std::string to_markdown() const;
};

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_Report_hpp_
