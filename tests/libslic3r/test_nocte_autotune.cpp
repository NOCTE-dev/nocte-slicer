// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/AutoTune.hpp"
#include "libslic3r/Nocte/GeometryAnalysis.hpp"
#include "libslic3r/Nocte/Rules/RuleSet.hpp"

using namespace Slic3r;
using namespace Slic3r::Nocte;

using Catch::Matchers::WithinAbs;

namespace {

PartFeatures features_of(const indexed_triangle_set &its)
{
    const AnalysisParams     params;
    const DynamicPrintConfig ctx;
    return analyze(its, params, ctx);
}

indexed_triangle_set thin_plate()  { return its_make_cube(20., 20., 0.6); }
indexed_triangle_set tall_pillar() { return its_make_cube(5., 5., 60.); }
indexed_triangle_set plain_cube()  { return its_make_cube(20., 20., 20.); }

// A 20 mm cap on a 4 mm stem: a 90 deg overhang 10 mm above the bed.
indexed_triangle_set cube_on_a_stem()
{
    indexed_triangle_set mesh = its_make_cube(4., 4., 10.);
    indexed_triangle_set cap  = its_make_cube(20., 20., 4.);
    its_translate(cap, Vec3f(-8.f, -8.f, 10.f));
    its_merge(mesh, cap);
    return mesh;
}

const Recommendation *find_rec(const TuneResult &result, const std::string &key)
{
    for (const Recommendation &rec : result.recommendations)
        if (rec.key == key)
            return &rec;
    return nullptr;
}

bool any_note_contains(const TuneResult &result, const std::string &needle)
{
    for (const std::string &note : result.notes)
        if (note.find(needle) != std::string::npos)
            return true;
    return false;
}

// The keys v1 is allowed to recommend: every key of PrintRegionConfig (always overridable) or of
// PrintObjectConfig (overridable on a whole object), as SettingsFactory::get_options() offers them.
const std::set<std::string> &allowed_keys()
{
    static const std::set<std::string> keys {
        // Region scope.
        "wall_loops", "sparse_infill_density", "sparse_infill_pattern",
        "top_shell_layers", "bottom_shell_layers", "bridge_flow", "detect_thin_wall",
        // Object scope.
        "enable_support", "support_type", "support_threshold_angle", "support_on_build_plate_only",
        "support_interface_top_layers", "tree_support_branch_angle", "seam_position",
        "brim_type", "brim_width", "elefant_foot_compensation", "xy_hole_compensation",
    };
    return keys;
}

TuneResult tune_mesh(const indexed_triangle_set &its)
{
    const DynamicPrintConfig base;
    const TuneProfile        profile;
    return tune(features_of(its), base, profile);
}

} // namespace

TEST_CASE("nocte auto-tune: the v1 rule table is complete and well formed", "[NocteAutoTune]")
{
    REQUIRE(tuning_available());
    REQUIRE(rule_set_version() == "nocte.rules/1");

    const std::vector<Rule> &rules = rule_set_v1();
    REQUIRE(rules.size() == 5);

    std::set<std::string> ids;
    for (const Rule &r : rules) {
        REQUIRE_FALSE(r.id.empty());
        REQUIRE_FALSE(r.doc.empty());
        REQUIRE(r.priority > 0);
        REQUIRE(bool(r.match));
        REQUIRE(bool(r.emit));
        ids.insert(r.id);
    }
    // Rule ids are persisted with the user's accept/reject history, so they have to be unique.
    REQUIRE(ids.size() == rules.size());
}

TEST_CASE("nocte auto-tune: a plain cube needs no changes", "[NocteAutoTune]")
{
    const TuneResult result = tune_mesh(plain_cube());

    REQUIRE(result.rules_version == "nocte.rules/1");
    REQUIRE(result.recommendations.empty());
    REQUIRE_FALSE(result.notes.empty());
}

TEST_CASE("nocte auto-tune: an empty part produces no recommendations", "[NocteAutoTune]")
{
    const indexed_triangle_set empty;
    const TuneResult           result = tune_mesh(empty);

    REQUIRE(result.recommendations.empty());
}

TEST_CASE("nocte auto-tune: a thin plate gets thin-wall detection", "[NocteAutoTune]")
{
    const TuneResult result = tune_mesh(thin_plate());

    const Recommendation *rec = find_rec(result, "detect_thin_wall");
    REQUIRE(rec != nullptr);
    REQUIRE(rec->rule_id == "R001-thin-walls");
    REQUIRE(rec->value == "1");
    REQUIRE(rec->previous_value == "0");
    REQUIRE(rec->confidence >= 0.5);
    REQUIRE_FALSE(rec->reason.empty());
    REQUIRE_FALSE(rec->evidence.empty());
}

TEST_CASE("nocte auto-tune: a tall pillar gets a brim", "[NocteAutoTune]")
{
    const TuneResult result = tune_mesh(tall_pillar());

    const Recommendation *type = find_rec(result, "brim_type");
    REQUIRE(type != nullptr);
    REQUIRE(type->rule_id == "R003-tall-thin-stability");
    REQUIRE(type->value == "outer_only");
    REQUIRE(type->scope == ConfigScope::Object);

    const Recommendation *width = find_rec(result, "brim_width");
    REQUIRE(width != nullptr);
    REQUIRE(std::stod(width->value) > 0.);
}

TEST_CASE("nocte auto-tune: an overhanging part gets support", "[NocteAutoTune]")
{
    const TuneResult result = tune_mesh(cube_on_a_stem());

    const Recommendation *enable = find_rec(result, "enable_support");
    REQUIRE(enable != nullptr);
    REQUIRE(enable->rule_id == "R002-overhang-supports");
    REQUIRE(enable->value == "1");

    // The cap is wide, not tall, so R002 proposes "normal(auto)" - which is also the stock default
    // of support_type (PrintConfig.cpp: set_default_value(new ConfigOptionEnum<SupportType>(
    // stNormalAuto))), so tune() drops it as a recommendation that would change nothing (the rule
    // for that is covered by "a recommendation equal to the current value is skipped"). Tree
    // support would differ from the default and survive, so the absence of a support_type
    // recommendation is what proves the squat branch was taken.
    REQUIRE(find_rec(result, "support_type") == nullptr);

    // NØCTE measures the overhang angle from the vertical; Orca's threshold is the complement.
    const Recommendation *threshold = find_rec(result, "support_threshold_angle");
    REQUIRE(threshold != nullptr);
    REQUIRE(threshold->value == "45");
}

TEST_CASE("nocte auto-tune: a small solid part gets more infill and walls", "[NocteAutoTune]")
{
    const TuneResult result = tune_mesh(thin_plate());

    const Recommendation *density = find_rec(result, "sparse_infill_density");
    REQUIRE(density != nullptr);
    REQUIRE(density->rule_id == "R005-solid-small-part-strength");
    // sparse_infill_density is a coPercent, so the value string carries the sign.
    REQUIRE(density->value.back() == '%');

    const Recommendation *loops = find_rec(result, "wall_loops");
    REQUIRE(loops != nullptr);
    REQUIRE(loops->value == "3");
}

TEST_CASE("nocte auto-tune: a small cross section is reported without a recommendation", "[NocteAutoTune]")
{
    // The layer-time controls that would fix this are filament settings, which Orca cannot override
    // per object, so R004 reports the finding rather than emitting a key it could not apply.
    const TuneResult result = tune_mesh(tall_pillar());

    REQUIRE(any_note_contains(result, "R004-small-cross-section-cooling"));
    for (const Recommendation &rec : result.recommendations)
        REQUIRE(rec.rule_id != "R004-small-cross-section-cooling");
}

TEST_CASE("nocte auto-tune: every recommendation names a real key with a legal value", "[NocteAutoTune]")
{
    const std::vector<indexed_triangle_set> parts { thin_plate(), tall_pillar(), cube_on_a_stem(), plain_cube() };

    for (size_t i = 0; i < parts.size(); ++ i) {
        const TuneResult result = tune_mesh(parts[i]);
        for (const Recommendation &rec : result.recommendations) {
            INFO("part " << i << ", rule " << rec.rule_id << ", key " << rec.key << ", value " << rec.value);
            REQUIRE_FALSE(rec.rule_id.empty());
            REQUIRE_FALSE(rec.reason.empty());
            REQUIRE_FALSE(rec.evidence.empty());
            REQUIRE(allowed_keys().count(rec.key) == 1);

            const ConfigOptionDef *def = print_config_def.get(rec.key);
            REQUIRE(def != nullptr);

            // The value string has to round-trip through the option it targets, which is what
            // catches a stale enum name or a percentage that lost its sign.
            const std::unique_ptr<ConfigOption> opt(def->create_default_option());
            REQUIRE(opt.get() != nullptr);
            REQUIRE(opt->deserialize(rec.value));
        }
    }
}

TEST_CASE("nocte auto-tune: all five rules are exercised by the fixtures", "[NocteAutoTune]")
{
    std::set<std::string> fired;
    const std::vector<indexed_triangle_set> parts { thin_plate(), tall_pillar(), cube_on_a_stem() };
    for (const indexed_triangle_set &its : parts) {
        const TuneResult result = tune_mesh(its);
        for (const Recommendation &rec : result.recommendations)
            fired.insert(rec.rule_id);
        if (any_note_contains(result, "R004-small-cross-section-cooling"))
            fired.insert("R004-small-cross-section-cooling");
    }

    for (const Rule &r : rule_set_v1()) {
        INFO("rule " << r.id);
        REQUIRE(fired.count(r.id) == 1);
    }
}

TEST_CASE("nocte auto-tune: a value the user already set is never silently overwritten", "[NocteAutoTune]")
{
    // A key present in the scope's config is a key the user set there.
    DynamicPrintConfig base;
    base.set_deserialize_strict("detect_thin_wall", "0");

    const PartFeatures features = features_of(thin_plate());

    TuneProfile profile;
    REQUIRE_FALSE(profile.overwrite_user_values);

    const TuneResult guarded = tune(features, base, profile);
    const Recommendation *rec = find_rec(guarded, "detect_thin_wall");
    REQUIRE(rec != nullptr);
    REQUIRE(rec->previous_value == "0");
    // Reported, but below min_confidence, so it is never pre-selected.
    REQUIRE(rec->confidence < profile.min_confidence);
    REQUIRE(any_note_contains(guarded, "user override present"));

    profile.overwrite_user_values = true;
    const TuneResult overwriting = tune(features, base, profile);
    const Recommendation *forced = find_rec(overwriting, "detect_thin_wall");
    REQUIRE(forced != nullptr);
    REQUIRE(forced->confidence >= profile.min_confidence);
}

TEST_CASE("nocte auto-tune: a recommendation equal to the current value is skipped", "[NocteAutoTune]")
{
    DynamicPrintConfig base;
    base.set_deserialize_strict("brim_type", "outer_only");

    const TuneResult result = tune(features_of(tall_pillar()), base, TuneProfile());

    REQUIRE(find_rec(result, "brim_type") == nullptr);
    // The rest of the rule still applies.
    REQUIRE(find_rec(result, "brim_width") != nullptr);
}

TEST_CASE("nocte auto-tune: a recommendation records the value it would replace", "[NocteAutoTune]")
{
    const TuneResult result = tune_mesh(tall_pillar());

    const Recommendation *width = find_rec(result, "brim_width");
    REQUIRE(width != nullptr);
    // brim_width is absent from the empty scope config, so previous_value is the preset default.
    REQUIRE_THAT(std::stod(width->previous_value), WithinAbs(0., 1e-9));
    REQUIRE(width->value != width->previous_value);
}
