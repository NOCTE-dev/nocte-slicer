// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/AutoTune.hpp"

#include "libslic3r/Nocte/Rules/RuleSet.hpp"

namespace Slic3r {
namespace Nocte {

std::string to_string(ConfigScope scope)
{
    switch (scope) {
    case ConfigScope::Object:     return "Object";
    case ConfigScope::Volume:     return "Volume";
    case ConfigScope::LayerRange: return "LayerRange";
    }
    return "Unknown";
}

// TODO(M1): evaluate rule_set_v1() against the features and the base config, honouring
// TuneProfile::overwrite_user_values and the "user override present" invariant.
TuneResult tune(const PartFeatures & /* features */, const DynamicPrintConfig & /* base */, const TuneProfile & /* profile */)
{
    TuneResult result;
    result.rules_version = rule_set_version();
    result.notes.emplace_back("Auto-tuning is not implemented in M0; no recommendations were produced.");
    return result;
}

bool tuning_available()
{
    return false;
}

} // namespace Nocte
} // namespace Slic3r
