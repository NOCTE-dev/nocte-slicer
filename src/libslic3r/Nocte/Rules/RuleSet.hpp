// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The auto-tuning rule table. A rule is declarative: an id, a sentence of documentation, a
// priority, a predicate over the measured features plus the current config, and an emitter that
// writes recommendations into a sink. Keeping rules data rather than code is what lets the report
// cite a rule id and the GUI remember a user's accept/reject per rule.
//
// Header-only in M0. The table itself is Rules/rules_v1.cpp (M1).

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

// TODO(M1): populate in Rules/rules_v1.cpp. Empty until then, so tune() has a well-defined result.
inline const std::vector<Rule> &rule_set_v1()
{
    static const std::vector<Rule> rules;
    return rules;
}

inline std::string rule_set_version()
{
    return "nocte.rules/0";
}

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_Rules_RuleSet_hpp_
