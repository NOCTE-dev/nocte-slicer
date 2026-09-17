// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The v1 auto-tuning rule table.
//
// Scope discipline. Every key below is one Orca actually exposes as a per-object or per-region
// override, which is what SettingsFactory::get_options() (src/slic3r/GUI/GUI_Factories.cpp) hands
// the settings list: the keys of PrintRegionConfig always, plus the keys of PrintObjectConfig when
// the target is a whole object rather than a part. A rule that wants something outside those two
// configs — a filament or printer setting — has no legal recommendation to make and says so in a
// note instead. tune() re-checks every key against print_config_def and drops what does not
// deserialize, so an enum string that goes stale fails a unit test rather than a print.

#include "libslic3r/Nocte/Rules/RuleSet.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

// Fallback when the base config carries no nozzle diameter, which is the common case for an
// object-scope override config. 0.4 mm is Orca's stock nozzle.
constexpr double DEFAULT_NOZZLE_MM = 0.4;

// Mirrors AnalysisParams::overhang_threshold, in degrees from the vertical. R002 converts it into
// Orca's complementary slope convention when it recommends support_threshold_angle.
constexpr double OVERHANG_THRESHOLD_DEG = 45.;

// R002 wants a part to be meaningfully overhung before it proposes support at all.
constexpr double R002_MIN_ANGLE_DEG      = 55.;
constexpr double R002_MIN_AREA_FRACTION  = 0.02;
constexpr double R002_MIN_STEEP_AREA_MM2 = 50.;

// R003: a part this much taller than its narrowest horizontal extent, or whose footprint is small
// against the height of its centre of mass, is worth a brim.
constexpr double R003_TALL_THIN_RATIO = 4.;
constexpr double R003_STABILITY_RATIO = 1.;

// R004: a layer this small cools far slower than the print speed assumes.
constexpr double R004_MIN_CROSS_SECTION_MM2 = 100.;

// R005: "small and solid" — a part below this volume whose solidity is above the threshold is
// almost certainly a functional part rather than a display model.
constexpr double R005_MAX_VOLUME_MM3 = 5000.;
constexpr double R005_MIN_SOLIDITY   = 0.85;

std::string fixed2(double v) { return float_to_string_decimal_point(v, 2); }
std::string fixed1(double v) { return float_to_string_decimal_point(v, 1); }

// The extrusion width a wall actually lays down is a nozzle-derived quantity, and line_width is a
// coFloatOrPercent that may be a percentage of it. v1 uses the nozzle diameter itself as the
// stand-in, which is within a few percent of the stock 0.42 mm line width for a 0.4 nozzle.
double line_width_of(const DynamicPrintConfig &base)
{
    const ConfigOptionFloats *nozzle = base.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle != nullptr && ! nozzle->values.empty() && nozzle->values.front() > 0.)
        return nozzle->values.front();
    return DEFAULT_NOZZLE_MM;
}

Recommendation make_rec(const char       *rule_id,
                        const char       *key,
                        std::string       value,
                        ConfigScope       scope,
                        double            confidence,
                        std::string       reason,
                        std::vector<std::string> evidence)
{
    Recommendation rec;
    rec.rule_id    = rule_id;
    rec.key        = key;
    rec.value      = std::move(value);
    rec.scope      = scope;
    rec.confidence = confidence;
    rec.reason     = std::move(reason);
    rec.evidence   = std::move(evidence);
    return rec;
}

std::vector<Rule> build_rules()
{
    std::vector<Rule> rules;

    // ---------------------------------------------------------------- R001 -- thin walls
    {
        Rule r;
        r.id       = "R001-thin-walls";
        r.doc      = "Sections narrower than four extrusion widths cannot hold the default two wall "
                     "loops per side, so the walls are adjusted to fit the section.";
        r.priority = 80;
        r.match    = [](const PartFeatures &f, const DynamicPrintConfig &base) {
            const double lw = line_width_of(base);
            return f.p05_wall_thickness > 0. && f.p05_wall_thickness < 4. * lw;
        };
        r.emit = [](const PartFeatures &f, const DynamicPrintConfig &base, RuleSink &sink) {
            const double lw  = line_width_of(base);
            const double p05 = f.p05_wall_thickness;
            const std::vector<std::string> evidence {
                "p05_wall_thickness = " + fixed2(p05) + " mm",
                "min_wall_thickness = " + fixed2(f.min_wall_thickness) + " mm",
                "line width (nozzle diameter) = " + fixed2(lw) + " mm",
            };

            if (p05 < 2. * lw) {
                // Below two line widths not even one loop per side fits; the thin-wall detector is
                // what turns such a section into a single centred line instead of dropping it.
                sink.recommend(make_rec("R001-thin-walls", "detect_thin_wall", "1", ConfigScope::Object, 0.75,
                                        "The thinnest walls of this part are under two extrusion widths, so they would "
                                        "otherwise be left unprinted; thin-wall detection prints them as a single line.",
                                        evidence));
            } else {
                // Each wall loop lays one line on each side of the section, so n loops need
                // 2 * n * line_width. Flooring keeps the loops from over-filling and leaves the gap
                // fill to close what is left, rather than over-extruding into the middle.
                const int loops = (std::max)(1, int(std::floor(p05 / (2. * lw))));
                sink.recommend(make_rec("R001-thin-walls", "wall_loops", std::to_string(loops), ConfigScope::Object, 0.6,
                                        "The thinnest walls of this part fit " + std::to_string(loops) +
                                            " wall loop(s) per side; more loops than that would over-extrude into the section.",
                                        evidence));
            }
        };
        rules.push_back(std::move(r));
    }

    // ---------------------------------------------------------- R002 -- overhang supports
    {
        Rule r;
        r.id       = "R002-overhang-supports";
        r.doc      = "A part with a significant area of steep overhangs is given automatic support "
                     "and a threshold that actually covers the measured overhangs.";
        r.priority = 90;
        r.match    = [](const PartFeatures &f, const DynamicPrintConfig &) {
            if (f.max_overhang_angle > R002_MIN_ANGLE_DEG && f.overhang_area_fraction > R002_MIN_AREA_FRACTION)
                return true;
            return f.steep_overhang_area > R002_MIN_STEEP_AREA_MM2;
        };
        r.emit = [](const PartFeatures &f, const DynamicPrintConfig &, RuleSink &sink) {
            const std::vector<std::string> evidence {
                "max_overhang_angle = " + fixed1(f.max_overhang_angle) + " deg from vertical",
                "overhang_area_fraction = " + fixed2(100. * f.overhang_area_fraction) + " % of the surface",
                "steep_overhang_area = " + fixed1(f.steep_overhang_area) + " mm2 above 70 deg",
            };

            sink.recommend(make_rec("R002-overhang-supports", "enable_support", "1", ConfigScope::Object, 0.8,
                                    "This part has overhangs that cannot be printed in mid-air, so support is switched on.",
                                    evidence));

            // Tree support reaches a tall, narrow part's overhangs with far less material and far
            // less contact than a normal support block; a squat part is better served by normal.
            // Both strings come from the support_type enum (PrintConfig.cpp:6779-6782).
            const bool tall = f.tall_thin_ratio > 2.;
            sink.recommend(make_rec("R002-overhang-supports", "support_type", tall ? "tree(auto)" : "normal(auto)",
                                    ConfigScope::Object, 0.65,
                                    tall ? "The part is tall and narrow, where tree support reaches the overhangs with less "
                                           "material and less surface contact than a normal support block."
                                         : "The part is squat, where a normal support block is more predictable than tree support.",
                                    evidence));

            // support_threshold_angle is Orca's slope angle (90 = vertical wall) and support is
            // generated below it, which is the complement of the NØCTE overhang angle. Recommending
            // 90 - overhang_threshold makes Orca's support set match exactly the facets this
            // analysis counted as overhangs.
            const int threshold = int(std::lround(90. - OVERHANG_THRESHOLD_DEG));
            sink.recommend(make_rec("R002-overhang-supports", "support_threshold_angle", std::to_string(threshold),
                                    ConfigScope::Object, 0.6,
                                    "The support threshold is set to cover exactly the overhangs this analysis measured.",
                                    evidence));
        };
        rules.push_back(std::move(r));
    }

    // -------------------------------------------------------- R003 -- tall, thin stability
    {
        Rule r;
        r.id       = "R003-tall-thin-stability";
        r.doc      = "A part that is tall against its footprint is anchored with a brim so that it "
                     "does not tip or come loose part way up the print.";
        r.priority = 70;
        r.match    = [](const PartFeatures &f, const DynamicPrintConfig &) {
            if (f.footprint_area <= 0.)
                return false;
            if (f.tall_thin_ratio >= R003_TALL_THIN_RATIO)
                return true;
            return f.stability_ratio > 0. && f.stability_ratio < R003_STABILITY_RATIO;
        };
        r.emit = [](const PartFeatures &f, const DynamicPrintConfig &, RuleSink &sink) {
            const std::vector<std::string> evidence {
                "tall_thin_ratio = " + fixed2(f.tall_thin_ratio),
                "stability_ratio = " + fixed2(f.stability_ratio),
                "footprint_area = " + fixed1(f.footprint_area) + " mm2",
            };

            // "outer_only" is the brim_type enum value at PrintConfig.cpp:1866. An outer brim adds
            // the adhesion without a brim inside every hole, which would have to be dug out.
            sink.recommend(make_rec("R003-tall-thin-stability", "brim_type", "outer_only", ConfigScope::Object, 0.7,
                                    "The part is tall relative to its footprint, so an outer brim is added to hold it "
                                    "down; an outer-only brim leaves the interior of any hole clear.",
                                    evidence));

            // Scale the brim with how top-heavy the part is, then clamp it into a range that is
            // still worth peeling off by hand.
            const double width = (std::min)(10., (std::max)(3., 2. * f.tall_thin_ratio));
            sink.recommend(make_rec("R003-tall-thin-stability", "brim_width", float_to_string_decimal_point(width, 2),
                                    ConfigScope::Object, 0.65,
                                    "The brim is widened in proportion to how tall the part is against its narrowest "
                                    "horizontal extent.",
                                    evidence));
        };
        rules.push_back(std::move(r));
    }

    // -------------------------------------------- R004 -- small cross section, cooling only
    {
        Rule r;
        r.id       = "R004-small-cross-section-cooling";
        r.doc      = "Reports layers whose cross section is too small to cool at print speed. The "
                     "fix lives in the filament preset, so this rule only reports.";
        r.priority = 60;
        r.match    = [](const PartFeatures &f, const DynamicPrintConfig &) {
            return f.min_cross_section_area > 0. && f.min_cross_section_area < R004_MIN_CROSS_SECTION_MM2;
        };
        r.emit = [](const PartFeatures &f, const DynamicPrintConfig &, RuleSink &sink) {
            // Deliberately no recommendation. The controls that would help — slow_down_layer_time,
            // slow_down_for_layer_cooling, slow_down_min_speed and the fan curve — are all filament
            // options, and Orca has no per-object or per-region override for a filament option:
            // SettingsFactory::get_options() offers only PrintRegionConfig and PrintObjectConfig
            // keys. Nothing in either config cools a small layer (wall and infill counts change how
            // much is printed, not how long the layer takes), so v1 reports the finding and leaves
            // the fix to the filament preset rather than emitting a recommendation that cannot be
            // applied at this scope.
            sink.note("R004-small-cross-section-cooling: the smallest cross section of this part is " +
                      fixed1(f.min_cross_section_area) +
                      " mm2, which may not cool before the next layer lands on it. The remedy — minimum layer time "
                      "and its slow-down speed — is a filament setting and cannot be overridden per object, so adjust "
                      "it in the filament preset, or print more than one copy at a time.");
        };
        rules.push_back(std::move(r));
    }

    // ------------------------------------------------ R005 -- small solid part, strength
    {
        Rule r;
        r.id       = "R005-solid-small-part-strength";
        r.doc      = "A small, near-convex part is almost always a functional one, and is given more "
                     "infill and more walls than the display-model defaults.";
        r.priority = 50;
        r.match    = [](const PartFeatures &f, const DynamicPrintConfig &) {
            return f.volume > 0. && f.volume < R005_MAX_VOLUME_MM3 && f.solidity > R005_MIN_SOLIDITY;
        };
        r.emit = [](const PartFeatures &f, const DynamicPrintConfig &, RuleSink &sink) {
            const std::vector<std::string> evidence {
                "volume = " + fixed1(f.volume) + " mm3",
                "solidity = " + fixed2(f.solidity),
                "hull_volume = " + fixed1(f.hull_volume) + " mm3",
            };

            // sparse_infill_density is a coPercent, so the value string carries the sign.
            const int density = f.volume < 1000. ? 30 : 25;
            sink.recommend(make_rec("R005-solid-small-part-strength", "sparse_infill_density",
                                    std::to_string(density) + "%", ConfigScope::Object, 0.6,
                                    "The part is small and nearly solid, so the extra infill costs little time but "
                                    "makes it noticeably stronger.",
                                    evidence));
            sink.recommend(make_rec("R005-solid-small-part-strength", "wall_loops", "3", ConfigScope::Object, 0.55,
                                    "An extra wall loop adds more strength to a small part than the same material "
                                    "spent on infill.",
                                    evidence));
        };
        rules.push_back(std::move(r));
    }

    return rules;
}

} // namespace

const std::vector<Rule> &rule_set_v1()
{
    static const std::vector<Rule> rules = build_rules();
    return rules;
}

} // namespace Nocte
} // namespace Slic3r
