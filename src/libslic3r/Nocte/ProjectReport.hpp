// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The project-level companion of Nocte::NocteReport: a cheap inventory of what a saved project
// contains, written additively into the 3mf as Metadata/nocte_report.json. Bambu Studio ignores
// unknown Metadata entries (ADR-001), so the file is invisible to the native path.
//
// Deliberately dependency-light: no mesh analysis, no diagnostics, nothing that would make saving
// a project any slower. The per-mesh findings live in Nocte::NocteReport and are surfaced through
// the "reports" array once the repair pipeline persists them.

#ifndef slic3r_Nocte_ProjectReport_hpp_
#define slic3r_Nocte_ProjectReport_hpp_

#include <string>

// Schema identifier of the project-level report. Sibling of NOCTE_REPORT_SCHEMA
// (Nocte/NocteVersion.hpp), versioned independently of it.
#define NOCTE_PROJECT_REPORT_SCHEMA "nocte.project-report/1"

// Path of the report inside the 3mf archive.
#define NOCTE_PROJECT_REPORT_FILE "Metadata/nocte_report.json"

namespace Slic3r {

class Model;

namespace Nocte {

// The single switch that decides whether a save writes the report at all, so the feature can be
// turned off in one line while the identity metadata stays.
bool project_report_enabled();

// The report for `model`, as a JSON string. `indent` < 0 emits the compact form, as
// nlohmann::json::dump() defines it. Never throws on odd object names: invalid UTF-8 is replaced.
std::string project_report_json(const Model &model, int indent = 2);

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_ProjectReport_hpp_
