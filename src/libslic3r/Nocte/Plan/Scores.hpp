// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The five physical terms the orientation engine ranks on, each in its own physical unit, plus the
// dimensionless combination step.
//
// The rule this file exists to enforce: a term is an estimate of a quantity the user can measure on
// a real print — cubic millimetres of support, millimetres of stair-step, seconds, newtons — and it
// is reported in that unit. Only the combination step is dimensionless, and there the weights are
// preferences the user can see and edit, never fitted constants. Orca's Orient.cpp does the
// opposite (28 undocumented constants, one of them divided by a field that is never assigned,
// Orient.cpp:479,483) and that is why we cannot explain its answers.
//
// Angle convention, shared with GeometryAnalysis.cpp:45-59:
//   0 deg  = vertical wall (normal horizontal),
//   90 deg = horizontal ceiling or floor (normal along Z).
// For a unit normal n under rotation R, the angle from the vertical is asin(|(R n) . z|).

#ifndef slic3r_Nocte_Plan_Scores_hpp_
#define slic3r_Nocte_Plan_Scores_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "libslic3r/Point.hpp"
#include "libslic3r/Nocte/Plan/PartInvariants.hpp"

namespace Slic3r {
namespace Nocte {

// How the support volume was obtained. The tier is carried in the result because tier 0 must never
// be presented to the user in grams: it is blind to self-support and overstates by a wide margin.
enum class SupportTier : uint8_t {
    FacetSweep = 0,  // milliseconds, ranking only
    SliceUnion = 1,  // seconds, the default; self-support aware
    FullDetect = 2,  // needs a PrintObject at posSlice; not reachable from this header
};

// "facet-sweep" / "slice-union" / "full-detect". These strings are written into the --nocte-plan
// JSON and into the project report, so they are a persisted format and belong beside the enum
// rather than in whichever file happens to serialise it. A tier that a reader cannot name is a tier
// a reader will ignore, and the whole point of carrying it is that a tier-0 number must never be
// mistaken for a tier-1 one.
const char *support_tier_name(SupportTier tier);

// Support volume, tier 1 — the top-down column carry
// --------------------------------------------------
// Support is a COLUMN under an overhang, from the overhang down to whatever it lands on. Its volume
// is therefore an area times a height, and the only way to get that from slices is to carry the
// support region downwards:
//
//     R = {}                                  // support region, one layer above the current one
//     for k = top .. 0:                       // TOP-DOWN. Bottom-up cannot work: at layer k you do
//         R = R union B_{k+1}                 // not yet know which overhangs above will need a
//         R = R \ S_k                         // column through it.
//         V_s += rho_s * area(R) * h
//
// with `S_k` the solid slice at layer k and `B_k` the downward-facing slab projection at layer k
// (`slice_mesh_slabs`' bottom output), i.e. the material that has nothing directly beneath it.
//
// The `\ S_k` step is what makes this self-support aware, and it is also what makes the column stop
// where the part itself is in the way — support resting on the model below, which is the correct
// behaviour when support is not restricted to the build plate.
//
// The trap this comment exists to close: summing `area(B_k \ union_{j<k} S_j) * h` bottom-up sums
// INTERFACE areas one layer thick. A flat overhang of area A at height H then scores `rho*A*h`
// instead of `rho*A*H` — off by the height of the part, and worse, off by a DIFFERENT factor for
// every candidate, since the height is exactly what the orientation changes. It would quietly
// invert the ranking on the one term everything else is ranked against.

struct ScoreParams
{
    double layer_height_mm        = 0.2;

    // There is deliberately no `line_width_mm`. The derivation makes the extrusion width
    // load-bearing in two of the three binding time bounds — the flow ceiling v <= Q_max/(h*w) and
    // the cooling floor v <= V_layer/(h*w*t_min) — but neither is computable here: no machine speed
    // and no minimum layer time reach this entry point, so Q_eff collapses to max_volumetric_speed
    // and the width cancels out. Declaring the field anyway would mean a user setting 0.8 and
    // watching the estimate not move. The width-carrying bounds land with the profile calculator,
    // where t_min and the machine limits are in scope.

    // Facets steeper than this from the vertical need support. Same convention as
    // AnalysisParams::overhang_threshold.
    double overhang_threshold_deg = 45.;

    // Facets within this much of horizontal are tops or bottoms, not staircase, and are excluded
    // from the cusp average. Without the exclusion the formula returns c -> h for a perfect flat
    // top, which is nonsense: a flat top has no staircase at all. Its quality is governed by
    // top_shell_layers and ironing.
    double flat_exclude_deg       = 5.;

    // Sparse fill fraction of the support structure. The solid column volume over-predicts the real
    // support by 3-8x; this is the factor that corrects it. Read from the support density of the
    // active process config, not guessed. Meaningless for tree supports, which need tier 2.
    double support_density        = 0.15;

    // mm^3/s. filament_max_volumetric_speed of the active filament (21 for Bambu PLA Basic on the
    // A1 0.4 nozzle, resolved from the shipped vendor profiles).
    double max_volumetric_speed   = 21.;

    // Seconds of overhead per layer: layer change, Z hop, and the acceleration of the first move.
    // This is the ONE calibrated constant of the model. It is measured as dt/dN between two already
    // sliced orientations of the same part, it is a property of the machine, and it belongs in the
    // printer profile. A guess here makes the time term a guess.
    double layer_overhead_s       = 2.;

    // In-plane tensile strength of the material, MPa, and the interlayer ratio k = sigma_z/sigma_xy.
    // k is a MEASURED material property; the literature gives 0.4-0.75 for PLA. Whatever ships must
    // carry its source, and the panel must say so.
    double sigma_xy_mpa           = 50.;
    double anisotropy_k           = 0.5;

    // Slice spacing for the load-direction section sweep, and the cap on the number of planes.
    double section_step_mm        = 0.5;
    int    section_max_planes     = 400;

    SupportTier support_tier      = SupportTier::SliceUnion;

    // There is deliberately no `support_on_build_plate_only` here. Restricting support to columns
    // that actually reach the bed means tracking each connected component of the support region down
    // the whole stack and discarding the ones that land on the model instead. That is a different
    // algorithm, not a flag, and it belongs in tier 2 where a real PrintObject already knows the
    // answer. A flag that silently did nothing would be exactly the defect this engine exists to
    // avoid: Orient.cpp divides by `area_projected`, a field nothing ever assigns.
};

// One candidate orientation, every field in its own unit. A field that could not be measured is
// left at its initialiser and flagged in `measured`, so a zero is never mistaken for a finding.
struct OrientScores
{
    double      support_volume_mm3      = 0.;
    SupportTier support_tier            = SupportTier::FacetSweep;

    // Cusp statistics over the VISIBLE, non-flat facets. All three are area-weighted.
    //
    // `cusp_mean_mm` is a CONDITIONAL mean — the excluded flat facets leave the denominator as well
    // as the numerator — so it answers "how rough is the rough part", not "how much roughness is
    // there". Two orientations can tie on it while one has a ninth of the staircase of the other.
    // It is what the combined score uses today.
    //
    // `cusp_error_mm3` is the extensive form, the surface-error integral sum(a_i * delta_i). It
    // separates the case the mean cannot. It is reported but NOT yet scored: which of the two
    // belongs in the objective is an open question that the batch harness settles against real
    // prints, and reporting both is how we get the data to settle it.
    //
    // `cusp_p95_mm` is the worst area a user actually notices. It drives the panel, the hard
    // constraint on a showcase face, and the computed layer height.
    double      cusp_mean_mm            = 0.;
    double      cusp_error_mm3          = 0.;
    double      cusp_p95_mm             = 0.;

    // False when no facet survived the visible-and-non-flat filter, so all three numbers above are
    // zero because nothing was measured — not because the surface is perfect. Zero is the BEST value
    // on this term. The case is not theoretical: an inverted mesh (every facet wound inward, an
    // ordinary broken STL) has a correct centroid and a positive surface area, so it passes every
    // other gate, yet every self-occlusion ray starts inside the surface and reports the whole part
    // invisible. Without this flag such a part scores perfect surface quality on every candidate and
    // the cusp term silently stops discriminating.
    bool        cusp_measured           = false;

    // Support touching visible surface. A hard constraint for signage, not a weighted term.
    // `contact_measured` is not decoration: visibility is unavailable when it was switched off or
    // when the mesh exceeds the facet budget — a scanned mesh, which is exactly the case where a
    // user cares about a showcase face. Without the flag a zero there reads as "no support on the
    // visible surface" and the hard constraint passes for every candidate, silently. Tier 0 never
    // computes contact at all, so it always reports false.
    //
    // It requires the visibility data to be USABLE, not merely present. An inverted mesh produces
    // `visibility_available == true` with every facet marked invisible; a zero contact then means
    // "the visibility data is useless", not "there is no support on any visible face", and the flag
    // must say so. At least one facet has to be visible.
    //
    // Granularity, stated because the field is easy to over-read: this is a whole-part total over
    // every visible overhang. The signage constraint is per-face — no support on THE showcase face —
    // and cannot be evaluated from this scalar alone. It is a sound necessary condition (zero here
    // implies zero there) and nothing more, until the panel carries the face selection.
    double      support_contact_mm2     = 0.;
    bool        contact_measured        = false;

    // False when the support sweep could not complete (slicing threw, or the slice count did not
    // match). Support volume then stays 0, which is the BEST possible value on that term, so a
    // consumer that ignores this flag would rank a part it failed to measure above every part it
    // measured honestly.
    bool        support_measured        = false;

    double      time_estimate_s         = 0.;
    int         layer_count             = 0;
    double      height_mm               = 0.;

    // Minimum section normal to the load direction, and the resulting force at failure. Zero when
    // no load direction was given.
    double      min_section_area_mm2    = 0.;
    double      failure_force_n         = 0.;

    // False when no load direction was given, and ALSO false when the section sweep was attempted
    // and failed. `min_section_area_along` returns 0 for seven distinct events — empty mesh, zero
    // direction, unusable Z range, degenerate height, empty plane list, a throw inside the slicer,
    // and "no plane had positive area" — and nothing else distinguishes them.
    //
    // Without this flag a failed sweep is laundered twice over. The engine reads the zero as the
    // named constraint `NoLoadSection`, so a measurement failure is reported to the user as their
    // own missing input; and the JSON writes `0.0` for a force, which its own comment says must
    // never happen because it reads as a part that fails under its own weight. A closed solid always
    // has a positive section, so on the CLI path — where a missing direction is refused before the
    // engine runs — a zero can essentially only mean the sweep broke.
    bool        section_measured        = false;

    // d_min / z_com: the part tips when lateral acceleration exceeds this times g. Isotropic
    // sqrt(area)/height would average a safe direction with an unsafe one, which is why the real
    // distance from the centre of mass to the nearest footprint hull edge is used instead.
    //
    // Two rules a caller has to know, because they are not the raw ratio:
    //  - It is CAPPED at 100. A part that tips only past 100 g is untippable by any real toolhead,
    //    and an uncapped outlier would pin the min-max span of the whole candidate set, flattening
    //    every genuine candidate to ~1 and turning the term binary. The cap is the term keeping its
    //    ability to discriminate, not a measurement.
    //  - A part that cannot be measured reports 0, the WORST value — a non-finite centre of mass, or
    //    a projected centre of mass falling outside the footprint hull, which means it is already
    //    tipping. Nothing unmeasurable is ever allowed to land on the favourable end.
    double      stability               = 0.;

    // BED CONTACT, from the first-layer slice — not the part's silhouette. For anything wider above
    // than at its base (a cap on a stem, a mushroom, a sphere, a T) the two are different objects,
    // and the silhouette flatters both constraints that read this: a 20x20 cap on a 4x4 stem has a
    // 400 mm^2 shadow and a 16 mm^2 footprint.
    //
    // `GeometryAnalysis::footprint_area` measures the same QUANTITY by a different construction: it
    // sums downward facets whose three vertices all sit within a tolerance of the lowest Z. The two
    // agree on a flat-bottomed part and disagree wherever the base is curved or coarsely tessellated
    // — a sphere on the bed gives 0 there and a small positive area here, because here it is the
    // area the first layer actually prints. Same intent, not the same definition. Neither is a
    // drop-in for the other, and saying they were the same thing would repeat, in a comment, the
    // mistake this field exists to correct.
    //
    // `footprint_area_mm2` is the RAW contact area and is what the adhesion constraint reads — a
    // ring's convex hull would overstate its adhesion badly. `footprint_hull_area_mm2` is the area
    // of the support polygon the tipping margin is measured against. Support material enlarges the
    // real support polygon and is deliberately not modelled: the number is the part's own footing.
    double      footprint_area_mm2      = 0.;
    double      footprint_hull_area_mm2 = 0.;

    // Exactly: the part invariants were usable AND the first-layer slice produced a footprint hull.
    // It is NOT a per-field guarantee — the cusp, support and contact terms carry their own flags
    // because each can fail on its own — so `measured == false` can mean "the cusp is perfectly good
    // but the first layer was empty". Read it as "the geometry this candidate rests on was usable",
    // and read the three specific flags for the terms.
    bool        measured                = false;
};

// Minimum cross-sectional area on planes NORMAL TO `dir`, in mm^2.
//
// This is orientation-invariant in the part frame, so it costs one sweep per (part, load
// direction) for the whole plan, not one per candidate. GeometryAnalysis' min_cross_section_area is
// the HORIZONTAL minimum, which is the load-bearing area only when the load runs along Z.
//
// Returns 0 for a degenerate mesh or a zero-length direction.
double min_section_area_along(const indexed_triangle_set &its, const Vec3d &dir, const ScoreParams &params);

// Scores one candidate. `rotation` maps object coordinates to build coordinates; the part is
// dropped to the bed internally, so the caller does not have to pre-translate it.
//
// PRECONDITION: `inv` must be the result of `precompute(its, ...)` on the SAME `its`. The two are
// read together — `inv.solid` supplies the slices while `its` and `inv`'s facet arrays supply the
// overhang sub-meshes — and a mismatched pair describes two different parts. The bounds checks keep
// that from being an out-of-bounds read; they cannot keep it from being a wrong answer, and there is
// deliberately no runtime defence, because a defence would suggest the pairing is optional.
//
// `load_dir_obj` is the load direction in OBJECT coordinates (zero = no strength term), and
// `min_section_mm2` is the value min_section_area_along() returned for it, passed in so the sweep
// is not repeated per candidate.
OrientScores evaluate(const indexed_triangle_set &its,
                      const PartInvariants       &inv,
                      const Transform3d          &rotation,
                      const ScoreParams          &params,
                      const Vec3d                &load_dir_obj,
                      double                      min_section_mm2);

// --- combination -------------------------------------------------------------------------------

// Dimensionless preferences. They are expected to sum to 1; `normalized()` enforces it. A zero
// weight drops the term entirely, which is how "this intent does not care about strength" is said.
struct ScoreWeights
{
    double support   = 0.25;
    double cusp      = 0.25;
    double time      = 0.20;
    double strength  = 0.15;
    double stability = 0.15;

    double sum() const { return support + cusp + time + strength + stability; }
    ScoreWeights normalized() const;
};

// Per-term indifference floor, in the term's own unit. NOT a numerical epsilon: it is the
// difference below which we declare two candidates equal on that term. Without it, min-max
// normalisation turns a 0.3% support difference into a decisive one.
struct ScoreEpsilons
{
    double support_mm3   = 500.;
    double cusp_mm       = 0.02;
    double time_s        = 60.;
    double force_frac    = 0.05;  // fraction of the best force in the candidate set
    double stability     = 0.1;
};

// Lower is better. Terms where more is better are inverted inside, and there are TWO of them:
// `failure_force_n` and `stability`. Stability is the lateral acceleration in g at which the part
// topples, so a larger value is safer; failing to invert it would make the engine prefer tippy
// orientations, which is the opposite of the constraint it is meant to serve.
//
// Hard constraints are applied by the caller as a FILTER over the candidate set before this runs,
// never as a large penalty: a penalty would distort the min-max range of every other term.
//
// **The caller must also drop candidates whose flags are false.** `combine()` reads the raw numbers
// and consults neither `measured` nor `support_measured`, `contact_measured` or `cusp_measured` — it
// cannot, because a filter that silently removed candidates would make the returned vector no longer
// line up with the input. A failed measurement leaves its term at zero, which is the BEST value on
// every term this function inverts, so a candidate we could not measure would otherwise outrank every
// candidate we measured honestly. That is the defect the flags exist to prevent, and this is the one
// place it can still happen.
//
// `out_normalized` receives the per-candidate dimensionless value of each term when non-null, in
// the field order (support, cusp, time, strength, stability), for the panel to show why. Each entry
// is in [0, 1] with 0 the best candidate on that term, already inverted where more is better, so a
// panel can render the row directly as a bar without knowing the sign of the underlying quantity.
// A term whose weight is zero contributes 0 to every row.
std::vector<double> combine(const std::vector<OrientScores> &candidates,
                            const ScoreWeights              &weights,
                            const ScoreEpsilons             &eps,
                            std::vector<std::array<double, 5>> *out_normalized);

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_Plan_Scores_hpp_
