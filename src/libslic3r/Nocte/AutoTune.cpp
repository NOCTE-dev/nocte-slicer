// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/AutoTune.hpp"

#include "libslic3r/Nocte/Rules/RuleSet.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

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

namespace {

// Confidence a recommendation is capped to when it would land on a key the user already set in the
// target scope. TuneProfile::min_confidence defaults to 0.5, so a capped recommendation is reported
// but never pre-selected.
constexpr double USER_OVERRIDE_CONFIDENCE = 0.2;

// Build the option a value string denotes, or nullptr when the string is not a legal value for that
// option. This is the guard that turns a stale enum string or a mistyped percentage into a failing
// unit test instead of a silently dropped or, worse, silently wrong print setting.
std::unique_ptr<ConfigOption> deserialized(const ConfigOptionDef &def, const std::string &value)
{
    std::unique_ptr<ConfigOption> opt;
    try {
        opt.reset(def.create_default_option());
        if (! opt || ! opt->deserialize(value))
            opt.reset();
    } catch (...) {
        opt.reset();
    }
    return opt;
}

// Canonical text of an option's value. Comparing two canonical forms is safer than comparing an
// author's spelling against a stored value ("5.00" and "5" are the same float) and safer than
// ConfigOption::operator==, which throws when the two concrete types disagree even though both
// describe the same coEnum.
std::string canonical(const ConfigOption &opt)
{
    try {
        return opt.serialize();
    } catch (...) {
        return std::string();
    }
}

} // namespace

// What counts as "a user-set value in that scope"
// ----------------------------------------------
// `base` is the config at the scope the recommendation targets. Orca's per-object and per-volume
// configs (ModelConfigObject / ModelConfigVolume) are sparse: they hold exactly the keys that were
// overridden at that scope and nothing else, inheriting everything else from the print preset. So
// "the key is present in base" is precisely "the user set this key here", and that is the test this
// function applies. A caller that passes a full print config instead — every key present — will see
// every recommendation treated as an override, which is the safe direction to be wrong in.
//
// previous_value is filled with the value that is in force today: the one from `base` when the key
// is present there, otherwise the preset default from print_config_def, so the dialog can always
// show what the recommendation would replace.
TuneResult tune(const PartFeatures &features, const DynamicPrintConfig &base, const TuneProfile &profile)
{
    TuneResult result;
    result.rules_version = rule_set_version();

    const std::vector<Rule> &table = rule_set_v1();

    // Higher priority first; ties keep the order the table declares them in.
    std::vector<const Rule *> ordered;
    ordered.reserve(table.size());
    for (const Rule &r : table)
        ordered.push_back(&r);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Rule *a, const Rule *b) { return a->priority > b->priority; });

    // (key, scope, volume_idx) already claimed by a higher-priority rule.
    std::set<std::tuple<std::string, int, int>> claimed;

    for (const Rule *rule : ordered) {
        if (! rule->match || ! rule->emit)
            continue;

        bool matched = false;
        try {
            matched = rule->match(features, base);
        } catch (...) {
            result.notes.emplace_back("Rule " + rule->id + " failed while testing its condition and was skipped.");
            continue;
        }
        if (! matched)
            continue;

        // Stage into a scratch result so a rule that throws half way through cannot leave a partial
        // set of recommendations behind.
        TuneResult staged;
        RuleSink   sink(staged);
        try {
            rule->emit(features, base, sink);
        } catch (...) {
            result.notes.emplace_back("Rule " + rule->id + " failed while emitting and was skipped.");
            continue;
        }

        for (std::string &note : staged.notes)
            result.notes.push_back(std::move(note));

        for (Recommendation &rec : staged.recommendations) {
            if (rec.rule_id.empty())
                rec.rule_id = rule->id;

            const ConfigOptionDef *def = print_config_def.get(rec.key);
            if (def == nullptr) {
                result.notes.emplace_back("Rule " + rec.rule_id + " proposed the unknown config key \"" + rec.key +
                                          "\"; the recommendation was dropped.");
                continue;
            }

            const std::unique_ptr<ConfigOption> proposed = deserialized(*def, rec.value);
            if (! proposed) {
                result.notes.emplace_back("Rule " + rec.rule_id + " proposed the value \"" + rec.value + "\" for \"" +
                                          rec.key + "\", which is not a legal value for that setting; the "
                                          "recommendation was dropped.");
                continue;
            }

            const auto claim = std::make_tuple(rec.key, int(rec.scope), rec.volume_idx);
            if (claimed.count(claim) > 0) {
                result.notes.emplace_back("Rule " + rec.rule_id + " also targets \"" + rec.key + "\" at " +
                                          to_string(rec.scope) +
                                          " scope, which a higher-priority rule already set; the recommendation was dropped.");
                continue;
            }

            const ConfigOption           *current = base.option(rec.key);
            const bool                    user_set = current != nullptr;
            std::unique_ptr<ConfigOption> fallback;
            if (current == nullptr && def->default_value) {
                fallback.reset(def->default_value->clone());
                current = fallback.get();
            }
            if (current != nullptr)
                rec.previous_value = canonical(*current);

            // Nothing to recommend when the setting already holds that value.
            if (current != nullptr && canonical(*proposed) == rec.previous_value)
                continue;

            if (user_set && ! profile.overwrite_user_values) {
                rec.confidence = (std::min)(rec.confidence, USER_OVERRIDE_CONFIDENCE);
                rec.evidence.emplace_back("user override present");
                result.notes.emplace_back("user override present: \"" + rec.key + "\" is already set to \"" +
                                          rec.previous_value + "\" at " + to_string(rec.scope) +
                                          " scope, so rule " + rec.rule_id +
                                          " is reported at low confidence rather than applied.");
            }

            // Below the profile's threshold a recommendation is still reported — the user can see
            // what was considered — but it is not pre-selected for application.
            if (rec.confidence < profile.min_confidence)
                result.notes.emplace_back("Rule " + rec.rule_id + " proposes \"" + rec.key +
                                          "\" below the confidence threshold of this profile; it is reported but not "
                                          "pre-selected.");

            claimed.insert(claim);
            result.recommendations.push_back(std::move(rec));
        }
    }

    if (result.recommendations.empty() && result.notes.empty())
        result.notes.emplace_back("No rule matched this part; the current settings are already appropriate.");

    return result;
}

bool tuning_available()
{
    return true;
}

} // namespace Nocte
} // namespace Slic3r
