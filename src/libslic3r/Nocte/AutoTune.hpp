// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Per-part auto-tuning: turns PartFeatures into a list of opt-in, reversible config
// recommendations. Declaration only in M0; the rule table arrives in M1 (Rules/rules_v1.cpp).
//
// Invariant: a key the user already set in the target scope is never silently overwritten. Such a
// recommendation is emitted with low confidence and the note "user override present".

#ifndef slic3r_Nocte_AutoTune_hpp_
#define slic3r_Nocte_AutoTune_hpp_

#include <string>
#include <vector>

#include "libslic3r/Nocte/GeometryAnalysis.hpp"

namespace Slic3r {

class DynamicPrintConfig;

namespace Nocte {

enum class ConfigScope { Object, Volume, LayerRange };

std::string to_string(ConfigScope scope);

struct Recommendation
{
    // Identifies the rule that produced this, so a user's accept/reject can be remembered per rule.
    std::string              rule_id;
    // Orca config key, e.g. "wall_loops".
    std::string              key;
    std::string              value;
    std::string              previous_value;
    // One sentence, phrased for a user rather than a developer.
    std::string              reason;
    // Measured quantities backing the reason, e.g. "max_overhang_angle = 62 deg".
    std::vector<std::string> evidence;
    ConfigScope              scope      = ConfigScope::Object;
    // Index into ModelObject::volumes when scope == Volume, otherwise -1.
    int                      volume_idx = -1;
    // 0..1. Anything below TuneProfile::min_confidence is reported but not pre-selected.
    double                   confidence = 0.;
};

struct TuneProfile
{
    // "conservative" | "balanced" | "aggressive". Selects the rule priorities that apply.
    std::string id             = "balanced";
    double      min_confidence = 0.5;
    // When false, a key the user already set in the target scope yields a low-confidence
    // recommendation carrying the "user override present" note instead of a plain one.
    bool        overwrite_user_values = false;
};

struct TuneResult
{
    std::vector<Recommendation> recommendations;
    // Messages that are not tied to a single key: skipped rules, missing measurements, overrides.
    std::vector<std::string>    notes;
    // Version of the rule table that produced this result.
    std::string                 rules_version;
};

// Runs the rule table against the measured features. `base` is the config at the scope the
// recommendations target, and it is read two ways: as the context a rule matches on, and as the
// record of what the user already set there — a key present in `base` is a user override, because
// Orca's per-object and per-volume configs hold only the keys overridden at that scope.
//
// Every recommendation a rule emits is checked against print_config_def before it is accepted: an
// unknown key, or a value string that does not deserialize for that option, is dropped with a note.
// Recommendations that would not change anything are skipped, and when two rules target the same
// key in the same scope the higher-priority rule wins.
TuneResult tune(const PartFeatures &features, const DynamicPrintConfig &base, const TuneProfile &profile);

// True once tune() actually evaluates rules.
bool tuning_available();

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_AutoTune_hpp_
