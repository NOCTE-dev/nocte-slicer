// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/Report.hpp"

#include <iomanip>
#include <sstream>

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

using json = nlohmann::json;

json to_json(const BoundingBoxf3 &bbox)
{
    if (! bbox.defined)
        return nullptr;
    return json{
        { "min", { bbox.min.x(), bbox.min.y(), bbox.min.z() } },
        { "max", { bbox.max.x(), bbox.max.y(), bbox.max.z() } }
    };
}

json to_json(const TriangleMeshStats &stats)
{
    return json{
        { "facets",      stats.number_of_facets },
        { "volume",      stats.volume },
        { "parts",       stats.number_of_parts },
        { "open_edges",  stats.open_edges },
        { "min",         { stats.min.x(), stats.min.y(), stats.min.z() } },
        { "max",         { stats.max.x(), stats.max.y(), stats.max.z() } },
        { "size",        { stats.size.x(), stats.size.y(), stats.size.z() } }
    };
}

json to_json(const MeshIssue &issue)
{
    json edges = json::array();
    for (const std::pair<int, int> &e : issue.edges)
        edges.push_back(json::array({ e.first, e.second }));

    return json{
        { "issue_id",    issue.issue_id },
        { "kind",        to_string(issue.kind) },
        { "severity",    to_string(issue.severity) },
        { "count",       issue.count },
        { "metric",      issue.metric },
        { "explanation", issue.explanation },
        { "face_ids",    issue.face_ids },
        { "edges",       edges },
        { "bbox",        to_json(issue.bbox) }
    };
}

json to_json(const DiagnosticsResult &diag)
{
    json issues = json::array();
    for (const MeshIssue &issue : diag.issues)
        issues.push_back(to_json(issue));

    return json{
        { "stats",          to_json(diag.stats) },
        { "open_edges",     diag.open_edges },
        { "shells",         diag.shells },
        { "volume",         diag.volume },
        { "manifold",       diag.manifold },
        { "summary",        diag.summary },
        { "skipped_checks", diag.skipped_checks },
        { "issues",         issues }
    };
}

json to_json(const MeshMetrics &metrics)
{
    return json{
        { "tris",               metrics.tris },
        { "open_edges",         metrics.open_edges },
        { "shells",             metrics.shells },
        { "self_intersections", metrics.self_intersections },
        { "volume",             metrics.volume },
        { "surface_area",       metrics.surface_area },
        { "hausdorff_est",      metrics.hausdorff_est }
    };
}

json to_json(const RepairOp &op)
{
    return json{
        { "kind",              to_string(op.kind) },
        { "tier",              op.tier },
        { "title",             op.title },
        { "rationale",         op.rationale },
        { "targets_issue_ids", op.targets_issue_ids },
        { "lossy",             op.lossy },
        { "implemented",       op.implemented }
    };
}

json to_json(const ReportStep &step)
{
    return json{
        { "op",            to_json(step.op) },
        { "state",         to_string(step.state) },
        { "succeeded",     step.result.succeeded },
        { "message",       step.result.message },
        { "duration_ms",   step.result.duration.count() },
        { "touched_faces", step.result.touched_faces.size() },
        { "before",        to_json(step.result.before) },
        { "after",         to_json(step.result.after) }
    };
}

json to_json(const PartFeatures &f)
{
    return json{
        { "volume",                 f.volume },
        { "surface_area",           f.surface_area },
        { "hull_volume",            f.hull_volume },
        { "solidity",               f.solidity },
        { "bbox_size",              { f.bbox_size.x(), f.bbox_size.y(), f.bbox_size.z() } },
        { "center_of_mass",         { f.center_of_mass.x(), f.center_of_mass.y(), f.center_of_mass.z() } },
        { "footprint_area",         f.footprint_area },
        { "stability_ratio",        f.stability_ratio },
        { "tall_thin_ratio",        f.tall_thin_ratio },
        { "max_overhang_angle",     f.max_overhang_angle },
        { "overhang_area_fraction", f.overhang_area_fraction },
        { "steep_overhang_area",    f.steep_overhang_area },
        { "thickness_hist",         f.thickness_hist },
        { "min_wall_thickness",     f.min_wall_thickness },
        { "p05_wall_thickness",     f.p05_wall_thickness },
        { "max_bridge_span",        f.max_bridge_span },
        { "min_cross_section_area", f.min_cross_section_area },
        { "smallest_feature_size",  f.smallest_feature_size },
        { "mean_abs_curvature",     f.mean_abs_curvature },
        { "shell_count",            f.shell_count }
    };
}

json to_json(const Recommendation &rec)
{
    return json{
        { "rule_id",        rec.rule_id },
        { "key",            rec.key },
        { "value",          rec.value },
        { "previous_value", rec.previous_value },
        { "reason",         rec.reason },
        { "evidence",       rec.evidence },
        { "scope",          to_string(rec.scope) },
        { "volume_idx",     rec.volume_idx },
        { "confidence",     rec.confidence }
    };
}

std::string fixed(double value, int decimals = 3)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    return oss.str();
}

} // namespace

std::string NocteReport::to_json(int indent) const
{
    json root{
        { "schema",        this->schema },
        { "nocte_version", this->nocte_version },
        { "app_name",      this->app_name },
        { "subject",       this->subject },
        { "diagnostics",   Nocte::to_json(this->diagnostics) }
    };

    root["diagnostics_after"] = this->diagnostics_after ? Nocte::to_json(*this->diagnostics_after) : json(nullptr);

    json steps = json::array();
    for (const ReportStep &step : this->steps)
        steps.push_back(Nocte::to_json(step));
    root["steps"] = steps;

    root["features"] = this->features ? Nocte::to_json(*this->features) : json(nullptr);

    json recs = json::array();
    for (const Recommendation &rec : this->recs)
        recs.push_back(Nocte::to_json(rec));
    root["recommendations"] = recs;

    return root.dump(indent);
}

std::string NocteReport::to_markdown() const
{
    std::ostringstream md;

    // The display name is a u8"" literal, which is char8_t-based from C++20 on; the cast keeps this
    // streaming whichever standard libslic3r is compiled with.
    md << "# " << reinterpret_cast<const char *>(NOCTE_APP_DISPLAY_NAME) << " report\n\n";
    md << "- Schema: `" << this->schema << "`\n";
    md << "- Engine: " << this->app_name << " " << this->nocte_version << "\n";
    if (! this->subject.empty())
        md << "- Subject: " << this->subject << "\n";
    md << "\n";

    md << "## Diagnostics\n\n";
    md << this->diagnostics.summary << "\n\n";
    md << "| Facets | Shells | Volume (mm3) | Open edges | Manifold |\n";
    md << "|---|---|---|---|---|\n";
    md << "| " << this->diagnostics.stats.number_of_facets
       << " | " << this->diagnostics.shells
       << " | " << fixed(this->diagnostics.volume)
       << " | " << this->diagnostics.open_edges
       << " | " << (this->diagnostics.manifold ? "yes" : "no") << " |\n\n";

    if (this->diagnostics.issues.empty()) {
        md << "No issues found.\n\n";
    } else {
        md << "| Issue | Kind | Severity | Count | Explanation |\n";
        md << "|---|---|---|---|---|\n";
        for (const MeshIssue &issue : this->diagnostics.issues)
            md << "| `" << issue.issue_id << "` | " << to_string(issue.kind) << " | " << to_string(issue.severity)
               << " | " << issue.count << " | " << issue.explanation << " |\n";
        md << "\n";
    }

    if (! this->diagnostics.skipped_checks.empty()) {
        md << "### Checks not run\n\n";
        for (const std::string &skipped : this->diagnostics.skipped_checks)
            md << "- " << skipped << "\n";
        md << "\n";
    }

    md << "## Repair steps\n\n";
    if (this->steps.empty()) {
        md << "No repair steps were planned.\n\n";
    } else {
        md << "| # | Tier | Step | State | Open edges | Volume (mm3) | Lossy | Why |\n";
        md << "|---|---|---|---|---|---|---|---|\n";
        for (size_t i = 0; i < this->steps.size(); ++ i) {
            const ReportStep &step = this->steps[i];
            md << "| " << (i + 1)
               << " | " << step.op.tier
               << " | " << step.op.title
               << " | " << to_string(step.state)
               << " | " << step.result.before.open_edges << " -> " << step.result.after.open_edges
               << " | " << fixed(step.result.before.volume) << " -> " << fixed(step.result.after.volume)
               << " | " << (step.op.lossy ? "yes" : "no")
               << " | " << step.op.rationale << " |\n";
        }
        md << "\n";
        for (const ReportStep &step : this->steps)
            if (! step.result.message.empty())
                md << "- **" << step.op.title << "**: " << step.result.message << "\n";
        md << "\n";
    }

    if (this->diagnostics_after) {
        md << "## Diagnostics after repair\n\n";
        md << this->diagnostics_after->summary << "\n\n";
    }

    if (this->features) {
        md << "## Geometry\n\n";
        md << "| Metric | Value |\n|---|---|\n";
        md << "| Volume (mm3) | " << fixed(this->features->volume) << " |\n";
        md << "| Surface area (mm2) | " << fixed(this->features->surface_area) << " |\n";
        md << "| Solidity | " << fixed(this->features->solidity) << " |\n";
        md << "| Max overhang angle (deg) | " << fixed(this->features->max_overhang_angle, 1) << " |\n";
        md << "| Min wall thickness (mm) | " << fixed(this->features->min_wall_thickness) << " |\n\n";
    }

    if (! this->recs.empty()) {
        md << "## Recommendations\n\n";
        md << "| Rule | Key | Old | New | Scope | Confidence | Reason |\n";
        md << "|---|---|---|---|---|---|---|\n";
        for (const Recommendation &rec : this->recs)
            md << "| `" << rec.rule_id << "` | `" << rec.key << "` | " << rec.previous_value << " | " << rec.value
               << " | " << to_string(rec.scope) << " | " << fixed(rec.confidence, 2) << " | " << rec.reason << " |\n";
        md << "\n";
    }

    return md.str();
}

} // namespace Nocte
} // namespace Slic3r
