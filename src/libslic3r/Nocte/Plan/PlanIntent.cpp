// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The intent table behind PlanIntent.hpp: five rows of weights, the hard constraints each intent
// adds, and the two spellings that move an intent in and out of a file.
//
// There is no physics in this file and there is deliberately nothing to tune. Every number here is a
// PREFERENCE — "an ornament is judged on its surface, a draft on the clock" — stated once, in one
// table, where a user can read it and argue with it. The physical quantities the preferences weigh
// are measured in Scores.cpp, in their own units, and none of them change when a row below changes:
// editing a row re-ranks the candidates and does nothing else. That separation is the whole reason
// the engine can explain an answer, and it only survives if this file stays a table.
//
// The rows are the ones tabulated in docs/HLSD/nocte-planning-engine.md §7. They are written out to
// three decimals and then pushed through ScoreWeights::normalized(), so the "sums to 1" property is
// a consequence of the code rather than a claim in a comment that a later edit can quietly break.

#include "libslic3r/Nocte/Plan/PlanIntent.hpp"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>

#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Nocte/Plan/Scores.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

// "Not given" for a direction is the zero vector, and it is tested as a LENGTH against a floor
// rather than as `== Vec3d::Zero()`. These vectors arrive from a CLI parse, from a 3mf round trip
// through a decimal string, and from a normal a panel computed by subtracting two nearly equal
// points; any of those can produce 1e-17 where the user meant zero, and an exact comparison would
// then accept a direction with no direction in it. The floor is far below any normalised vector
// (length 1) and far above the noise those three paths produce.
constexpr double DIR_MIN_LENGTH = 1e-9;

// A NaN component makes norm() NaN, and `NaN > DIR_MIN_LENGTH` is false, so a corrupt direction is
// reported as missing rather than used. That is the direction the error has to fall in: a missing
// input is a question put back to the user, a NaN one is a silently wrong plan.
bool has_direction(const Vec3d &v) { return v.norm() > DIR_MIN_LENGTH; }

std::string lowercased(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
        // The cast through unsigned char is required: std::tolower is undefined for a negative int,
        // and a plain char is signed here, so any byte over 0x7F in a mis-encoded name would be UB.
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

struct IntentName
{
    PartIntent  kind;
    const char *name;
};

// THE PERSISTED SPELLINGS. These strings are written into nocte_report.json and read back out of
// it, and they are what the --nocte-intent CLI value is matched against, so they are a stable data
// format and not a display string: renaming one silently changes the intent of every saved project
// that carries the old spelling, because parse_intent() would then reject it and the caller would
// fall back to Unspecified — a different set of weights and a different answer. A user-facing label
// (translated, capitalised, "Functional — strength") belongs in the panel, keyed off the enum.
const IntentName INTENT_NAMES[] = {
    { PartIntent::Unspecified,        "unspecified"         },
    { PartIntent::Ornament,           "ornament"            },
    { PartIntent::FunctionalStrength, "functional-strength" },
    { PartIntent::FunctionalVisual,   "functional-visual"   },
    { PartIntent::Draft,              "draft"               },
};

// The five spellings as one list, built from the table rather than written out again, so that the
// error message a user reads cannot drift from the set parse_intent() actually accepts. A message
// that offers a name the parser rejects is worse than no message.
std::string intent_name_list()
{
    std::string out;
    for (const IntentName &entry : INTENT_NAMES) {
        if (! out.empty())
            out += ", ";
        out += entry.name;
    }
    return out;
}

// The CLI option keys, as PrintConfig.cpp declares them (PrintConfig.cpp:12182, 12191, 12199).
// Underscored here and hyphenated on the command line: --nocte-load-dir arrives as nocte_load_dir.
// The messages below name the hyphenated form, because that is what the user typed.
const char *const OPT_INTENT   = "nocte_intent";
const char *const OPT_LOAD_DIR = "nocte_load_dir";
const char *const OPT_SHOWCASE = "nocte_showcase";

// `option<T>()` (Config.hpp:2656) returns null both for an option this config does not carry and
// for one carrying the wrong type, and an absent option is exactly "not given". `opt_string()`
// (Config.hpp:2993) cannot be used here: it dereferences that null without checking it, so a
// config assembled without the --nocte-* definitions would crash rather than plan.
std::string option_string(const DynamicPrintConfig &config, const char *key)
{
    const ConfigOptionString *opt = config.option<ConfigOptionString>(key);
    return opt != nullptr ? opt->value : std::string();
}

// "x,y,z" to a unit vector.
//
// The accepted form and the diagnostic are deliberately identical to --ground-face-normal's
// (OrcaSlicer.cpp:4957): three numbers, commas, nothing else, all of it consumed. A user who has
// learnt one of these options has learnt all of them, and a second syntax for the same thing is a
// second thing to get wrong.
//
// NORMALISED HERE, on the way in, so that no downstream code has to ask whether it was handed a
// direction or a length. The engine dots these against facet normals, where a length of 12 changes
// no ranking but makes every intermediate number in a report unreadable, and a caller that
// normalised again "just in case" would have to decide what to do with a zero — which is the
// decision this function exists to make once.
bool parse_direction(const std::string &text, const char *option_name, Vec3d &out, std::string &error)
{
    Vec3d v(0., 0., 0.);
    int   consumed = 0;
    if (std::sscanf(text.c_str(), "%lf,%lf,%lf%n", &v.x(), &v.y(), &v.z(), &consumed) != 3 ||
        consumed != int(text.size()) || ! v.allFinite()) {
        error = std::string(option_name) + " expects three comma-separated numbers, as \"x,y,z\"; got \"" + text + "\"";
        return false;
    }

    const double len = v.norm();
    if (! (len > DIR_MIN_LENGTH)) {
        // Rejected rather than stored. A stored zero is indistinguishable from "not given", and the
        // two are not the same event: "not given" is a question we put back to the user, while a
        // zero the user typed is a mistake only we can see. Silently conflating them would let
        // `--nocte-load-dir 0,0,0` reach the engine as an intent with no load direction at all, and
        // the user would be told they forgot an option they did not forget.
        error = std::string(option_name) + " is a direction and cannot have zero length; got \"" + text + "\"";
        return false;
    }

    out = v / len;
    return true;
}

} // namespace

bool parse_intent(const std::string &name, PartIntent &out)
{
    const std::string key = lowercased(name);
    for (const IntentName &entry : INTENT_NAMES) {
        if (key == entry.name) {
            out = entry.kind;
            return true;
        }
    }
    // `out` is deliberately left untouched on failure, so a caller that ignores the return value
    // keeps whatever default it had rather than receiving a guess. Guessing an intent re-weights
    // every candidate and changes the winner, which is exactly the kind of silent decision this
    // engine exists not to make.
    return false;
}

const char *intent_name(PartIntent intent)
{
    for (const IntentName &entry : INTENT_NAMES)
        if (entry.kind == intent)
            return entry.name;
    // Unreachable for any enumerator, since the table above covers all five. It is still written,
    // and written as a valid spelling rather than as "unknown": a value cast in from outside the
    // enum must not be able to produce a token that parse_intent() would later reject, which would
    // turn a corrupt in-memory value into an unreadable project file.
    return "unspecified";
}

bool parse_plan_options(const DynamicPrintConfig &config, PlanIntent &out, std::string &error)
{
    error.clear();

    // Built into a local and copied out only on success, so a caller that ignores the return value
    // cannot end up holding an intent half-assembled from a malformed command line — which would be
    // the worst of both outcomes: a plan that ran, and ran on something the user did not ask for.
    PlanIntent intent;

    const std::string intent_text = option_string(config, OPT_INTENT);
    // An empty value is "not given" and leaves the balanced Unspecified weights, the same reading
    // the two directions get below. An unknown NAME is a different event and is an error: it means
    // the user asked for something this build does not have, and quietly ranking their part as
    // Unspecified would answer a question they believed they had already answered.
    if (! intent_text.empty() && ! parse_intent(intent_text, intent.kind)) {
        error = "unknown --nocte-intent \"" + intent_text + "\"; expected one of: " + intent_name_list();
        return false;
    }

    const std::string load_text = option_string(config, OPT_LOAD_DIR);
    if (! load_text.empty() && ! parse_direction(load_text, "--nocte-load-dir", intent.load_dir_obj, error))
        return false;

    const std::string showcase_text = option_string(config, OPT_SHOWCASE);
    if (! showcase_text.empty() && ! parse_direction(showcase_text, "--nocte-showcase", intent.showcase_normal_obj, error))
        return false;

    // `showcase_tol_deg` and `showcase_support_free` have no CLI options yet and keep their
    // defaults (5 degrees, and support on the read face is fatal). They are judgements about a
    // particular sign rather than about the part, so they belong to the panel that selects the
    // face; adding flags for them before that exists would fix their meaning in a persisted
    // command line before we know what it should be.
    //
    // Whether the intent is COMPLETE is not decided here. `missing_input()` answers that, and the
    // caller acts on it: a missing load direction is fatal to a plan and merely irrelevant to a
    // report that lists what was asked for, and this function cannot tell which of the two it is
    // being called for.
    out = intent;
    return true;
}

const char *PlanIntent::missing_input() const
{
    // Only two intents require an input, and each requires exactly one. Every other intent is
    // complete on its own, so a zero direction there is not an omission and must not be reported as
    // one: an ornament has no load direction because it carries no load.
    if (kind == PartIntent::FunctionalStrength && ! has_direction(load_dir_obj))
        return "load direction";
    if (kind == PartIntent::FunctionalVisual && ! has_direction(showcase_normal_obj))
        return "showcase face";
    return nullptr;
}

ScoreWeights weights_for(PartIntent intent)
{
    ScoreWeights w;
    switch (intent) {
    case PartIntent::Ornament:
        // Looked at, never loaded. Surface is what the part is for, so the cusp term dominates;
        // strength is dropped entirely, which a zero weight says exactly (Scores.hpp: "A zero
        // weight drops the term entirely").
        w.support   = 0.30;
        w.cusp      = 0.45;
        w.time      = 0.15;
        w.strength  = 0.00;
        w.stability = 0.10;
        break;

    case PartIntent::FunctionalStrength:
        // The part carries a load in a direction the user gave us. Half the weight goes to the
        // strength term and the cusp term is nearly dropped: a bracket that holds is not improved
        // by being smooth, and letting surface quality outvote the load path is how a part that
        // looks right snaps.
        w.support   = 0.20;
        w.cusp      = 0.05;
        w.time      = 0.10;
        w.strength  = 0.50;
        w.stability = 0.15;
        break;

    case PartIntent::FunctionalVisual:
        // Signage: one face is read and it must be flawless, but the part is still a part. The
        // showcase face is protected by the HARD constraints below, not by this weight — a weight
        // can be outvoted and a constraint cannot — so the cusp weight here governs the rest of the
        // surface, and the strength term stays in the mix at a low weight.
        w.support   = 0.25;
        w.cusp      = 0.40;
        w.time      = 0.10;
        w.strength  = 0.10;
        w.stability = 0.15;
        break;

    case PartIntent::Draft:
        // Printed to be thrown away: the only thing that matters is when it comes off the plate.
        // The cusp weight is zero, not small — a draft's surface is not a consideration at all, and
        // saying so with a zero is how the panel can show the term greyed out rather than tiny.
        w.support   = 0.30;
        w.cusp      = 0.00;
        w.time      = 0.60;
        w.strength  = 0.00;
        w.stability = 0.10;
        break;

    case PartIntent::Unspecified:
    default:
        // No question was put to the user, so no preference may be invented: every term carries
        // weight and none dominates. `default` shares this row so that a value cast in from outside
        // the enum lands on the balanced set rather than on an uninitialised one.
        w.support   = 0.25;
        w.cusp      = 0.25;
        w.time      = 0.20;
        w.strength  = 0.15;
        w.stability = 0.15;
        break;
    }

    // Normalised on the way out, so "the weights sum to 1" is enforced by the code rather than by
    // arithmetic done in a comment. Each row above already sums to 1, which makes this a no-op
    // today and a safety net the moment a row is edited.
    return w.normalized();
}

PlanConstraints constraints_for(const PlanIntent &intent)
{
    PlanConstraints c;

    // Every intent, including Draft: a part that tips is not a cheap print, it is a failed one plus
    // a crash. The floor is the same for all five because it is a property of the machine's
    // acceleration, not of what the part is for.
    c.min_stability = 0.35;

    // `min_footprint_mm2` is deliberately left at 0 (no requirement). Bed adhesion depends on the
    // material, the plate and the first-layer settings, none of which reach this header, so any
    // number here would be a guess presented as a constraint — and a hard constraint is the one
    // place a guess cannot be argued with, because it removes candidates before they are ranked.

    switch (intent.kind) {
    case PartIntent::FunctionalStrength:
        // A candidate with no section across the load direction has no load path at all. That is
        // not a low score, it is a different part, so it is filtered rather than penalised.
        c.require_load_section = true;
        break;

    case PartIntent::FunctionalVisual:
        // The read face must point up, within `showcase_tol_deg`, which the engine reads from the
        // intent itself.
        c.require_showcase_up = true;
        // Whether support landing on that face is fatal is the user's call, and it is the one
        // constraint here that is switchable: a sign that will be sanded is a different job from a
        // sign that comes off the plate finished. It follows the flag rather than overriding it.
        c.require_showcase_clean = intent.showcase_support_free;
        break;

    case PartIntent::Unspecified:
    case PartIntent::Ornament:
    case PartIntent::Draft:
    default:
        // Stability only. Nothing else about these intents can disqualify an orientation outright.
        break;
    }

    return c;
}

} // namespace Nocte
} // namespace Slic3r
