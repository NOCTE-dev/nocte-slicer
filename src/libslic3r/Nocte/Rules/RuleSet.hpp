// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The auto-tuning rule table. A rule is declarative: an id, a sentence of documentation, a
// priority, a predicate over the measured features plus the current config, and an emitter that
// writes recommendations into a sink. Keeping rules data rather than code is what lets the report
// cite a rule id and the GUI remember a user's accept/reject per rule.
//
// The table itself is Rules/rules_v1.cpp; this header carries only the shapes it is written in.
//
// A rule's emit() writes candidate recommendations; it is tune() that validates the key against
// print_config_def, fills previous_value, resolves conflicts by priority and applies the
// user-override invariant. A rule may emit a note without any recommendation when it has a finding
// to report but no legal key at object or region scope to act on it.

#ifndef slic3r_Nocte_Rules_RuleSet_hpp_
#define slic3r_Nocte_Rules_RuleSet_hpp_

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Nocte/AutoTune.hpp"
#include "libslic3r/Nocte/GeometryAnalysis.hpp"

namespace Slic3r {

class DynamicPrintConfig;

namespace Nocte {

// What a rule writes into. Wrapping TuneResult keeps a rule from reordering or dropping what
// another rule emitted.
class RuleSink
{
public:
    explicit RuleSink(TuneResult &out) : m_out(out) {}

    void recommend(Recommendation rec) { m_out.recommendations.push_back(std::move(rec)); }
    void note(std::string text) { m_out.notes.push_back(std::move(text)); }

private:
    TuneResult &m_out;
};

struct Rule
{
    // Stable across releases; it is persisted with the user's accept/reject history.
    std::string id;
    // One sentence explaining what the rule is for, shown as the tooltip in the tuning dialog.
    std::string doc;
    // Higher wins when two rules touch the same key in the same scope.
    int         priority = 0;

    std::function<bool(const PartFeatures &, const DynamicPrintConfig &)>              match;
    std::function<void(const PartFeatures &, const DynamicPrintConfig &, RuleSink &)>  emit;
};

// The v1 table, defined in Rules/rules_v1.cpp. The returned reference is to a function-local static
// built on first use and never mutated afterwards, so it is safe to read from several threads.
const std::vector<Rule> &rule_set_v1();

inline std::string rule_set_version()
{
    return "nocte.rules/1";
}

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_Rules_RuleSet_hpp_
