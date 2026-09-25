// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// What the part is FOR, and what that implies about how to rank its orientations.
//
// This is the decision that orders the whole engine. The five score terms measure physical
// quantities that do not depend on anybody's opinion; the intent supplies the dimensionless
// preferences over them, and the hard constraints that a candidate must satisfy to be considered at
// all. Weights are preferences the user can see and edit. They are never fitted constants, and no
// physics lives here — if a number in this file changes, the ranking changes and nothing else does.

#ifndef slic3r_Nocte_Plan_PlanIntent_hpp_
#define slic3r_Nocte_Plan_PlanIntent_hpp_

#include <cstdint>
#include <string>

#include "libslic3r/Point.hpp"
#include "libslic3r/Nocte/Plan/Scores.hpp"

namespace Slic3r {

// Forward declared rather than included. This header is pulled in by the scoring and the
// orientation code, none of which reads a config, and PrintConfig.hpp is one of the heaviest
// headers in the tree; only parse_plan_options() below needs the type, and only by reference. Same
// treatment as CustomGCode.hpp:10 and Nocte/AutoTune.hpp:19.
class DynamicPrintConfig;

namespace Nocte {

enum class PartIntent : uint8_t {
    Unspecified = 0,     // balanced; no question asked of the user
    Ornament,            // looked at, never loaded
    FunctionalStrength,  // carries a load in a direction the user gives
    FunctionalVisual,    // signage: one face is read, and it must be flawless
    Draft,               // a shape check, printed to be thrown away
};

// Parsed from the CLI and from the project report. The accepted spellings are exactly the five
// `intent_name` returns — "unspecified", "ornament", "functional-strength", "functional-visual",
// "draft" — matched CASE-INSENSITIVELY, because they are typed on a command line.
//
// Returns false on an unknown name and leaves `out` untouched, rather than falling back to
// Unspecified. A silent fallback would re-weight every candidate without saying so, and the user
// would read a ranking that answers a question they did not ask.
bool        parse_intent(const std::string &name, PartIntent &out);

// The inverse, and the spelling written to nocte_report.json. Stable: it is persisted.
const char *intent_name(PartIntent intent);

struct PlanIntent
{
    PartIntent kind = PartIntent::Unspecified;

    // OBJECT coordinates, unit length. Zero means "not given", which is not an error for every
    // intent: only FunctionalStrength requires a load direction and only FunctionalVisual requires
    // a showcase normal. An intent that requires one and does not have it is reported by
    // `missing_input()`, never silently defaulted — a default here would answer a question the user
    // was asked precisely because we cannot answer it.
    Vec3d  load_dir_obj        = Vec3d::Zero();
    Vec3d  showcase_normal_obj = Vec3d::Zero();

    // How close the showcase face must come to pointing straight up, in degrees.
    double showcase_tol_deg    = 5.;
    // Whether support touching the showcase face disqualifies a candidate outright.
    bool   showcase_support_free = true;

    // Null when the intent has everything it needs; otherwise exactly "load direction" for
    // FunctionalStrength without one, or "showcase face" for FunctionalVisual without one. The
    // strings are fixed because the CLI refuses to plan on them and names the option to supply.
    const char *missing_input() const;
};

// Builds a PlanIntent from the --nocte-* CLI options. Returns false and fills `error` with a
// message meant for a user rather than a developer. It does NOT check missing_input(): whether a
// missing load direction is fatal is the caller's decision, and the CLI refuses there.
bool parse_plan_options(const DynamicPrintConfig &config, PlanIntent &out, std::string &error);

// The weights for an intent. Always normalised, so they sum to 1.
//
// The table is HERE, in the contract, and not only in the .cpp, for two reasons: a test can pin it
// without reading the implementation it is testing, and a reader can see what changing an intent
// actually does. These are preferences, not physics — every one of them is arguable, and none of
// them is fitted to anything. The physics is in Scores.hpp and does not move when a row here does.
//
//   Intent              support  cusp  time  strength  stability
//   Unspecified          0.25    0.25  0.20    0.15      0.15
//   Ornament             0.30    0.45  0.15    0.00      0.10
//   FunctionalStrength   0.20    0.05  0.10    0.50      0.15
//   FunctionalVisual     0.25    0.40  0.10    0.10      0.15
//   Draft                0.30    0.00  0.60    0.00      0.10
//
// A zero is deliberate and is how "this intent does not care about that" is said: an ornament is
// never loaded, and a draft is thrown away before anyone looks at its surface.
//
// Mirrors docs/HLSD/nocte-planning-engine.md section 7. If the two ever disagree, the HLSD is the
// document a person reads and this is the one the code obeys, so fix both in the same commit.
ScoreWeights weights_for(PartIntent intent);

// The hard constraints for an intent. A candidate failing any of these is removed from the set
// BEFORE the scores are normalised — never penalised — because a penalty distorts the min-max range
// of every other term.
struct PlanConstraints
{
    // Minimum d_min/z_com. A part below this tips under ordinary toolhead acceleration.
    double min_stability          = 0.35;
    // Minimum bed contact in mm^2, or 0 for no requirement.
    double min_footprint_mm2      = 0.;
    // FunctionalStrength: the load-bearing section must exist.
    bool   require_load_section   = false;
    // FunctionalVisual: the showcase face must point up within the tolerance, and carry no support.
    bool   require_showcase_up    = false;
    bool   require_showcase_clean = false;
};

PlanConstraints constraints_for(const PlanIntent &intent);

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_Plan_PlanIntent_hpp_
