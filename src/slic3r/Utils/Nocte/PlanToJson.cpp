#include "PlanToJson.hpp"

// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The two rules stated in the header are enforced in exactly two places, and they are marked RULE 1
// and RULE 2 below so that a reviewer can find them without reading the rest:
//
//   RULE 1 — `measured_number()`. Every quantity goes through it with the flag that governs it, and
//            a false flag yields JSON null. Nothing in this file writes a bare double.
//   RULE 2 — `support_tier_name()` and the `support_mm3` key. The support volume appears once, in
//            cubic millimetres, with the tier that produced it beside it, and there is no gram
//            anywhere in this file. docs/HLSD/nocte-planning-engine.md §8 is the reason.
//
// The document is built with nlohmann::json and dumped once, as MeshInspect.cpp does, rather than
// printed field by field. That is not a style preference: a hand-printed document has to get every
// separator right in every loop, and a trailing comma in the rejected-candidate array would make
// the file unparseable in exactly the case — a part that failed — where its contents matter most.

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/NocteVersion.hpp"
#include "libslic3r/Nocte/Plan/OrientEngine.hpp"
#include "libslic3r/Nocte/Plan/PartInvariants.hpp"
#include "libslic3r/Nocte/Plan/PlanIntent.hpp"
#include "libslic3r/Nocte/Plan/Scores.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace Slic3r {
namespace Nocte {

namespace {

using json = nlohmann::json;

// Schema identifier of this document. It sits here rather than in NocteVersion.hpp only because
// this PR does not own that header; it belongs beside NOCTE_REPORT_SCHEMA.
const char *const PLAN_SCHEMA = "nocte.plan/1";

// Decimal places, expressed as the factor a value is rounded against. Three decimals for anything
// in millimetres, cubic millimetres, square millimetres, seconds or newtons — a micron and a
// millisecond are already past what the model can defend. Six for the dimensionless terms, whose
// whole range is [0, 1] and where three decimals would collapse a genuine ordering into a tie.
constexpr double DEC3 = 1000.;
constexpr double DEC6 = 1000000.;

// The same floor PlanIntent.cpp uses to decide whether a direction was given at all. A load
// direction is either present or absent, and both files have to agree on which, or the JSON would
// report a strength number the engine never computed.
constexpr double DIR_MIN_LENGTH = 1e-9;

// Rounded to 1/factor, and never negative zero. A value like -1e-17 rounds to -0.0, which nlohmann
// prints as "-0.0": a sign on nothing, which makes two plans of the same orientation diff as changed
// and invites a reader to wonder which way a zero points. -0.0 == 0.0 is true, so the test below
// catches it and hands back a plain zero.
double rounded(double v, double factor)
{
    const double r = std::round(v * factor) / factor;
    return r == 0. ? 0. : r;
}

// RULE 1. The one place a physical quantity becomes JSON.
//
// A false flag emits null, never 0. Every term the ranking inverts treats 0 as the BEST possible
// value (Scores.hpp, `combine()`: "A failed measurement leaves its term at zero, which is the BEST
// value on every term this function inverts"), so a support volume of 0 written for a candidate
// whose support sweep threw reads as a candidate that needs no support at all. null cannot be
// misread that way by a human or by a harness, and it is what JSON has for "no value".
//
// A non-finite value is treated the same. It is not a measurement either, and nlohmann would write
// it as null regardless — doing it here makes that deliberate rather than incidental.
json measured_number(double value, bool measured, double factor)
{
    if (! measured || ! std::isfinite(value))
        return json(nullptr);
    return json(rounded(value, factor));
}

json measured_int(int value, bool measured)
{
    if (! measured)
        return json(nullptr);
    return json(value);
}

json to_json_rounded(const Vec3d &v, double factor)
{
    return json::array({ rounded(v.x(), factor), rounded(v.y(), factor), rounded(v.z(), factor) });
}

// Null for a direction that was not given, so it reads the same way as an unmeasured quantity does
// everywhere else in this document rather than as a vector that happens to be at the origin. Used
// for the root intent and for every object alike, so the same absence prints the same way in both.
json direction_json(const Vec3d &v)
{
    if (! (v.norm() > DIR_MIN_LENGTH))
        return json(nullptr);
    return to_json_rounded(v, DEC6);
}

// RULE 2, and the reason Scores.hpp's `support_tier_name()` (Scores.hpp:45) is called rather than
// spelled out here. The tier is reported beside the volume, in every candidate, because the two are
// only meaningful together: tier 0 is blind to self-support and overstates by a wide margin
// (Scores.hpp on SupportTier), so "1200 mm^3" means different things on the two tiers. Neither is a
// mass, and this file never converts one into grams: §8 of the planning-engine HLSD lists the
// support density factor among the numbers that are not yet calibrated, so a gram figure here would
// be a fabricated precision attached to the single number every other term is ranked against.
//
// The tier names are persisted in this document, so they live beside the enum they name. A spelling
// that lives in whichever file happens to serialise it is a data format defined in a serialiser,
// and the next serialiser would define it differently.

// The rotation, twice over, and deliberately not as a matrix. A 3x3 matrix is unreadable in a diff
// and a user cannot check it against the part in front of them; an axis and an angle in degrees is
// something a person can compare with "I would have laid it on its back", which is the whole point
// of a plan that explains itself. The quaternion is the exact form a caller applies, the axis-angle
// is the form a human reads, and they are two spellings of one rotation rather than two numbers
// that could disagree — the axis-angle is derived from the quaternion, not measured separately.
//
// The sign of a quaternion is free (q and -q are the same rotation) and Eigen's choice depends on
// which matrix element happened to be largest, so it is pinned here. Without that, two runs that
// found the same orientation could print different-looking rotations and a diff of two plans would
// show a change that is not one.
//
// The rule is "the first component, in the order w, x, y, z, that does not PRINT as zero is
// positive" — decided on the rounded value, the one a reader sees. Pinning w >= 0 alone is not
// enough: every half turn has w = 0 in exact arithmetic and +-1e-17 in practice, so the sign of w
// would be round-off and the half turn would print as either of its two spellings from run to run.
json rotation_json(const Transform3d &t)
{
    const Matrix3d     m = t.linear();
    Eigen::Quaterniond q(m);
    q.normalize();
    const double order[4] = { q.w(), q.x(), q.y(), q.z() };
    for (const double c : order) {
        const double shown = rounded(c, DEC6);
        if (shown != 0.) {
            if (shown < 0.)
                q.coeffs() *= -1.;
            break;
        }
    }
    // Eigen's AngleAxis flips the axis when w < 0, so a w of -1e-17 left over from the rule above
    // would print the axis of a half turn against the sign the quaternion was just given. A w that
    // prints as zero is therefore made non-negative: a change of at most 5e-7 in one component, a
    // turn of about 1e-6 rad, below both the six decimals of the quaternion and the three of the angle.
    if (rounded(q.w(), DEC6) == 0.)
        q.w() = std::abs(q.w());

    // Eigen maps the identity quaternion to angle 0 about (1, 0, 0), so "no rotation" always prints
    // the same way rather than as an arbitrary axis.
    const Eigen::AngleAxisd aa(q);

    json out = json::object();
    // x, y, z, w — the storage order of Eigen's coefficient vector, named in the key so it can
    // never be read as w, x, y, z by a consumer that assumes the other convention.
    out["quaternion_xyzw"] = json::array({ rounded(q.x(), DEC6), rounded(q.y(), DEC6),
                                           rounded(q.z(), DEC6), rounded(q.w(), DEC6) });
    out["axis"]            = to_json_rounded(Vec3d(aa.axis()), DEC6);
    out["angle_deg"]       = rounded(Geometry::rad2deg(aa.angle()), DEC3);
    return out;
}

json scores_json(const OrientScores &s, bool load_given)
{
    // Which flag governs which number. Three of the four groups are stated by Scores.hpp directly:
    //   support_measured -> support_volume_mm3
    //   contact_measured -> support_contact_mm2
    //   cusp_measured    -> the three cusp fields
    //   measured         -> "the geometry this candidate rests on was usable", i.e. the invariants
    //                       were usable AND the first-layer slice produced a footprint hull, which
    //                       is exactly what the footprint areas and the stability ratio are built
    //                       from (Scores.cpp: they are assigned only under `footprint_usable`).
    //
    // The time, layer-count and height fields have no flag of their own. They are computed from the
    // rotated Z range, which fails only when nothing measurable reached the function at all, and
    // `measured` is the narrowest published statement of that, so they follow it. The choice is
    // conservative in the only direction that is safe: it can write null where a height was in fact
    // known, which loses information, and it cannot write a number that was never measured. It also
    // costs nothing in the ranking, because the engine drops every candidate with `measured ==
    // false` before it normalises (OrientEngine.hpp, step 2), so this can only ever null the fields
    // of a candidate that is already in the `rejected` array.
    //
    // Stability is the one term where 0 is the WORST value rather than the best, so a missing flag
    // there would not flatter the candidate. It is still written as null when unmeasured: "we could
    // not measure whether it tips" and "it tips at the slightest touch" are different findings, and
    // a batch harness averaging the second over parts we never measured would be measuring itself.
    json out = json::object();

    // RULE 2: cubic millimetres, with the tier beside it. Never a mass.
    out["support_mm3"]         = measured_number(s.support_volume_mm3, s.support_measured, DEC3);
    out["support_tier"]        = support_tier_name(s.support_tier);
    out["support_contact_mm2"] = measured_number(s.support_contact_mm2, s.contact_measured, DEC3);

    out["cusp_mean_mm"]        = measured_number(s.cusp_mean_mm, s.cusp_measured, DEC6);
    out["cusp_p95_mm"]         = measured_number(s.cusp_p95_mm, s.cusp_measured, DEC6);
    out["cusp_error_mm3"]      = measured_number(s.cusp_error_mm3, s.cusp_measured, DEC3);

    out["time_s"]              = measured_number(s.time_estimate_s, s.measured, DEC3);
    out["layer_count"]         = measured_int(s.layer_count, s.measured);
    out["height_mm"]           = measured_number(s.height_mm, s.measured, DEC3);

    // Both strength fields are zero when no load direction was given (Scores.hpp), and that zero is
    // an absence, not a finding: a part nobody declared a load for has no failure force, and
    // printing 0 N would read as a part that fails under its own weight.
    //
    // They are governed by `section_measured`, the flag Scores.hpp gives them, and not by `measured`:
    // the section sweep is independent of the first-layer slice, so a candidate can have a good
    // footprint and a failed sweep — the zero that must never print as 0 N — or the reverse.
    const bool strength_known  = load_given && s.section_measured;
    out["min_section_mm2"]     = measured_number(s.min_section_area_mm2, strength_known, DEC3);
    out["failure_force_n"]     = measured_number(s.failure_force_n, strength_known, DEC3);

    out["stability"]           = measured_number(s.stability, s.measured, DEC6);
    out["footprint_mm2"]       = measured_number(s.footprint_area_mm2, s.measured, DEC3);
    out["footprint_hull_mm2"]  = measured_number(s.footprint_hull_area_mm2, s.measured, DEC3);

    return out;
}

json flags_json(const OrientScores &s)
{
    // Every flag, unabridged, and not folded into a single "ok". They fail independently — a part
    // can have a perfectly measured cusp and an empty first layer — and a reader who sees a null
    // above needs to know which of the five is responsible without guessing.
    json out = json::object();
    out["measured"]         = s.measured;
    out["support_measured"] = s.support_measured;
    out["contact_measured"] = s.contact_measured;
    out["cusp_measured"]    = s.cusp_measured;
    out["section_measured"] = s.section_measured;
    return out;
}

json candidate_json(const OrientCandidate &c, bool load_given)
{
    json out = json::object();
    out["source"]   = candidate_source_name(c.source);
    out["rotation"] = rotation_json(c.rotation);
    out["scores"]   = scores_json(c.scores, load_given);
    out["flags"]    = flags_json(c.scores);

    // `terms` and `score` are dimensionless and come from the min-max normalisation over the
    // SURVIVING set (OrientEngine.hpp, steps 3-4), so they exist only for a candidate that survived
    // the filter. A rejected candidate keeps the struct's initialiser, all five terms at 0 and a
    // combined score of 0 — which is the best possible row in this document. Writing those zeros
    // would put every rejected candidate at the top of any sort a reader applies, so they are null
    // by the same rule as an unmeasured quantity: the normalisation was not run, therefore there is
    // no value to report.
    if (c.accepted()) {
        json terms = json::object();
        // Field order fixed by OrientCandidate::terms: support, cusp, time, strength, stability.
        // Named here rather than emitted as a bare array of five numbers, because a reader of the
        // JSON has no header to tell them which index is which.
        terms["support"]   = rounded(c.terms[0], DEC6);
        terms["cusp"]      = rounded(c.terms[1], DEC6);
        terms["time"]      = rounded(c.terms[2], DEC6);
        terms["strength"]  = rounded(c.terms[3], DEC6);
        terms["stability"] = rounded(c.terms[4], DEC6);
        out["terms"]       = std::move(terms);
        out["score"]       = rounded(c.score, DEC6);
        out["rejected"]    = json(nullptr);
    } else {
        out["terms"]    = json(nullptr);
        out["score"]    = json(nullptr);
        out["rejected"] = reject_reason_name(c.rejected);
    }
    return out;
}

json intent_json(const PlanIntent &intent)
{
    const ScoreWeights    w = weights_for(intent.kind);
    const PlanConstraints k = constraints_for(intent);

    json weights = json::object();
    weights["support"]   = rounded(w.support, DEC6);
    weights["cusp"]      = rounded(w.cusp, DEC6);
    weights["time"]      = rounded(w.time, DEC6);
    weights["strength"]  = rounded(w.strength, DEC6);
    weights["stability"] = rounded(w.stability, DEC6);

    json constraints = json::object();
    constraints["min_stability"]          = rounded(k.min_stability, DEC6);
    constraints["min_footprint_mm2"]      = rounded(k.min_footprint_mm2, DEC3);
    constraints["require_load_section"]   = k.require_load_section;
    constraints["require_showcase_up"]    = k.require_showcase_up;
    constraints["require_showcase_clean"] = k.require_showcase_clean;

    json out = json::object();
    out["name"]                 = intent_name(intent.kind);
    out["load_dir_obj"]         = direction_json(intent.load_dir_obj);
    out["showcase_normal_obj"]  = direction_json(intent.showcase_normal_obj);
    out["showcase_tol_deg"]     = rounded(intent.showcase_tol_deg, DEC3);
    out["showcase_support_free"] = intent.showcase_support_free;
    // Null when the intent has everything it needs; otherwise the name of the input the user still
    // owes us. It is reported rather than defaulted, because a default would answer the question
    // the user was asked precisely because we cannot answer it (PlanIntent.hpp).
    const char *missing         = intent.missing_input();
    out["missing_input"]        = missing != nullptr ? json(missing) : json(nullptr);
    out["weights"]              = std::move(weights);
    out["constraints"]          = std::move(constraints);
    return out;
}

// The settings every number above depends on, written into the document so that a plan read a month
// from now can be reproduced rather than guessed at. `layer_overhead_s` in particular is the one
// calibrated constant of the time model (Scores.hpp), and a time estimate is not interpretable
// without the value that produced it.
json params_json(const OrientParams &params)
{
    const ScoreParams &sp = params.scores;

    json scores = json::object();
    scores["layer_height_mm"]        = rounded(sp.layer_height_mm, DEC6);
    scores["overhang_threshold_deg"] = rounded(sp.overhang_threshold_deg, DEC3);
    scores["flat_exclude_deg"]       = rounded(sp.flat_exclude_deg, DEC3);
    scores["support_density"]        = rounded(sp.support_density, DEC6);
    scores["max_volumetric_speed"]   = rounded(sp.max_volumetric_speed, DEC3);
    scores["layer_overhead_s"]       = rounded(sp.layer_overhead_s, DEC3);
    scores["sigma_xy_mpa"]           = rounded(sp.sigma_xy_mpa, DEC3);
    scores["anisotropy_k"]           = rounded(sp.anisotropy_k, DEC6);
    scores["section_step_mm"]        = rounded(sp.section_step_mm, DEC3);
    scores["section_max_planes"]     = sp.section_max_planes;
    scores["support_tier"]           = support_tier_name(sp.support_tier);

    json out = json::object();
    out["angular_merge_deg"]    = rounded(params.angular_merge_deg, DEC3);
    out["max_candidates"]       = params.max_candidates;
    out["include_axis_aligned"] = params.include_axis_aligned;
    out["max_results"]          = params.max_results;
    out["scores"]               = std::move(scores);
    return out;
}

// THE FRAME THIS FILE PLANS IN, stated once because everything below depends on it.
//
// `PlanIntent.hpp` fixes both of the intent's vectors as OBJECT coordinates, and that is the right
// contract for the engine: the engine is handed a mesh and two directions and has no idea what an
// instance is. This file is the boundary where an instance exists, so this file is where object
// coordinates become the frame the engine is actually given, and it has to move BOTH the mesh and
// the directions or it moves the part out from under them.
//
// The mesh gets the instance's rotation, scale and mirror, but not its offset. Two reasons, and the
// review accepted both: the scores are in physical units, so an instance scaled 2x prints eight
// times the support volume and a plan measured on the unscaled mesh would be a plan of a different
// part; and the engine's `Current` candidate is "the orientation the part arrived in" — the
// identity — which is only true of the orientation the user is looking at on the plate. Every
// rotation this document reports is therefore relative to the part as it currently sits, which is
// also the form in which a caller applies it. The offset is dropped because the engine drops the
// part to the bed itself (Scores.hpp on `evaluate`), so where the instance sits on the plate
// changes nothing it measures.
//
// ONLY `instances.front()` is used. An object whose instances sit at different rotations is planned
// as if they all matched the first one, which is wrong for the others and is not fixable here: this
// file has no way to know which instance the user means. That belongs with the panel, which does.
Transform3d instance_matrix(const ModelObject &mo)
{
    if (mo.instances.empty() || mo.instances.front() == nullptr)
        return Transform3d::Identity();
    return mo.instances.front()->get_matrix_no_offset();
}

TriangleMesh object_mesh(const ModelObject &mo, const Transform3d &inst)
{
    TriangleMesh mesh = mo.raw_mesh();
    // fix_left_handed: a mirrored instance has a negative determinant, which inverts every facet's
    // winding and therefore every outward normal. The whole engine reads normals — the overhang
    // test, the visibility ray, the signed volume — so a mirrored part planned without this would
    // have its overhangs and its upward faces exactly swapped.
    mesh.transform(inst, true);
    return mesh;
}

// A DIRECTION through the instance: the linear part, nothing else. A load direction is a vector
// between two points of the part, so it travels the way the points do.
//
// A zero in gives a zero out, untouched: zero means "not given", and "not given" must survive a
// change of frame unchanged or an intent the user left blank would arrive somewhere else as an
// intent they filled in badly.
Vec3d mapped_direction(const Transform3d &inst, const Vec3d &v)
{
    if (! (v.norm() > DIR_MIN_LENGTH))
        return Vec3d::Zero();
    const Vec3d  mapped = inst.linear() * v;
    const double len    = mapped.norm();
    // A singular instance matrix — a zero scale on an axis — can collapse a real direction to
    // nothing, and an axis-collapsing scale can send it to a non-finite value. Neither is a
    // direction any more, so it degrades to "not given" rather than being passed on as noise: the
    // strength fields then come out null, which is true, instead of measuring a sweep along a
    // vector that means nothing.
    if (! mapped.allFinite() || ! (len > DIR_MIN_LENGTH))
        return Vec3d::Zero();
    return mapped / len;
}

// A SURFACE NORMAL through the instance: the inverse transpose, which is not the same matrix once
// scale or mirror is involved. Squash a part to half its height and a 45-degree face's normal does
// not tilt the way a direction lying in that face does; using the linear part for a normal tilts
// the showcase face off the surface it belongs to and protects a face the user did not pick.
// `inst_matrix.matrix().block(0,0,3,3).inverse().transpose()` is the same construction
// LayOnFace.cpp:86 uses to carry --ground-face-normal's object-coordinate normals into the instance
// frame, which is the sibling option of this one.
//
// The inverse transpose is also exactly consistent with `object_mesh`'s `fix_left_handed`. Raw
// transform of a triangle maps its normal by det(M) * M^-T, so a mirrored instance (det < 0) comes
// out reversed; flipping the winding puts it back, leaving the mesh's outward normals at M^-T n.
// Mapping the showcase normal by M^-T therefore lands it on the same side of the same surface as
// the facets the engine will compare it against.
Vec3d mapped_normal(const Transform3d &inst, const Vec3d &n)
{
    if (! (n.norm() > DIR_MIN_LENGTH))
        return Vec3d::Zero();
    const Matrix3d normal_matrix = inst.linear().inverse().transpose();
    const Vec3d    mapped        = normal_matrix * n;
    const double   len           = mapped.norm();
    // A singular instance matrix makes the inverse non-finite, which `allFinite()` catches. Same
    // reading as above: not a normal any more, so not given.
    if (! mapped.allFinite() || ! (len > DIR_MIN_LENGTH))
        return Vec3d::Zero();
    return mapped / len;
}

// The intent as the engine will actually receive it. Both vectors are mapped, each by its own
// matrix, and the result is what gets planned AND what the object reports, so the document can
// never describe a frame other than the one it planned in.
PlanIntent intent_in_instance_frame(const PlanIntent &intent, const Transform3d &inst)
{
    PlanIntent out = intent;
    out.load_dir_obj        = mapped_direction(inst, intent.load_dir_obj);
    out.showcase_normal_obj = mapped_normal(inst, intent.showcase_normal_obj);
    return out;
}

json object_json(const ModelObject &mo, size_t index, const std::string &fallback_source,
                 const PlanIntent &intent, const OrientParams &params)
{
    json out = json::object();
    // Index within model.objects, which is what --nocte-plan's sibling options address an object
    // by. It is NOT a stable identity across loads: the stable `oid` is owned by the object-identity
    // work and is deliberately not invented here, because two files disagreeing about what an oid
    // is would be worse than one file not having it.
    out["index"]  = index;
    out["name"]   = mo.name;
    out["source"] = mo.input_file.empty() ? (fallback_source.empty() ? json(nullptr) : json(fallback_source))
                                          : json(mo.input_file);
    // The intent is repeated per object, not only at the root, so that a single object lifted out of
    // this document still says what it was ranked for.
    out["intent"] = intent_name(intent.kind);

    // Mesh and intent are moved into the instance frame TOGETHER. Moving only the mesh is the defect
    // this pairing exists to prevent: with a rotated instance — ordinary in a 3mf, impossible in a
    // freshly loaded STL, which is why it hides — `--nocte-showcase 0,0,1` would go on protecting
    // the object-frame +Z face while the engine measured a part that had turned underneath it, and
    // the plan would be wrong in a way that still looked entirely reasonable.
    const Transform3d           inst    = instance_matrix(mo);
    const PlanIntent            planned = intent_in_instance_frame(intent, inst);
    const TriangleMesh          mesh    = object_mesh(mo, inst);
    const indexed_triangle_set &its     = mesh.its;
    out["triangle_count"] = its.indices.size();

    // The frame the numbers below are in, and the two vectors as the engine received them, so a
    // reader can check the mapping rather than trust it. The root `intent` block keeps the
    // object-frame values the user typed; these are those values after the instance.
    const bool load_given = planned.load_dir_obj.norm() > DIR_MIN_LENGTH;
    json directions = json::object();
    directions["frame"]           = "instance";
    directions["load_dir"]        = direction_json(planned.load_dir_obj);
    directions["showcase_normal"] = direction_json(planned.showcase_normal_obj);
    out["directions"] = std::move(directions);

    OrientResult result;
    try {
        const PartInvariants inv    = precompute(its, PrecomputeParams());
        const CancelFn       cancel = []() -> bool { return false; };
        // A non-empty predicate is passed even though nothing cancels a CLI run: `CancelFn` is a
        // std::function, and calling an empty one throws std::bad_function_call.
        result = plan_orientation(its, inv, planned, params, cancel);
    } catch (...) {
        // precompute() and plan_orientation() both promise not to throw on user geometry. This
        // catches what is left — an allocation failure on a mesh too large for the machine — so
        // that one object cannot cost the whole batch its report. `result` keeps its initialiser,
        // which is `ok == false`, so the object is reported as unplanned rather than as planned
        // with no candidates.
        result = OrientResult();
    }

    out["ok"]        = result.ok;
    out["cancelled"] = result.cancelled;
    out["considered"] = result.considered;
    // True when the hard constraints emptied the candidate set, in which case `blocking_reason`
    // names the constraint that did it and `best` holds the current orientation as a fallback.
    out["degenerate"] = result.degenerate;
    out["blocking_reason"] = result.blocking_reason == RejectReason::None
                                 ? json(nullptr)
                                 : json(reject_reason_name(result.blocking_reason));
    // How many rejected candidates carry each reason, every reason named even at zero so the key set
    // is the same in every document. `blocking_reason` is only the largest of these, and when two
    // constraints conflict — the face can point up only if the part stands on an edge — the largest
    // is half the story: the tally is what shows the other constraint emptied the rest of the set.
    json rejections = json::object();
    for (size_t slot = 1; slot < REJECT_REASON_COUNT; ++ slot)
        rejections[reject_reason_name(static_cast<RejectReason>(slot))] = result.rejection_counts[slot];
    out["rejections"] = std::move(rejections);

    json best = json::array();
    for (const OrientCandidate &c : result.best)
        best.push_back(candidate_json(c, load_given));
    out["best"] = std::move(best);

    json rejected = json::array();
    for (const OrientCandidate &c : result.rejected)
        rejected.push_back(candidate_json(c, load_given));
    out["rejected"] = std::move(rejected);

    return out;
}

} // namespace

void plan_to_json(const Model &model, const std::vector<std::string> &source_paths,
                  const PlanIntent &intent, const OrientParams &params, std::ostream &out)
{
    // Whether a load direction was given is decided per object, in object_json(), on the direction
    // AFTER the instance transform — that is the one the engine was handed, and a singular instance
    // can turn a given direction into none.
    json objects = json::array();
    for (size_t i = 0; i < model.objects.size(); ++ i) {
        const ModelObject *mo = model.objects[i];
        if (mo == nullptr)
            continue;
        // One input file merged into one object is the common case, and then the object's own
        // `input_file` is usually set. When it is not, a single input file is an unambiguous
        // fallback and anything else is a guess, so the field goes null instead.
        const std::string fallback = source_paths.size() == 1 ? source_paths.front() : std::string();
        objects.push_back(object_json(*mo, i, fallback, intent, params));
    }

    json root = json::object();
    root["schema"]        = PLAN_SCHEMA;
    root["nocte_version"] = NOCTE_VERSION;
    root["generator"]     = NOCTE_APP_NAME;
    root["sources"]       = source_paths;
    root["note"]          = "Every quantity names its unit in its key. A number is null when the "
                            "measurement flag that governs it is false: null means NOT MEASURED, and "
                            "0 is the best possible value on every ranked term, so the two must never "
                            "be confused. Support is reported in mm^3 only and is never a mass - the "
                            "support density factor is uncalibrated (HLSD nocte-planning-engine.md "
                            "section 8). Rotations map the part as it currently sits on the plate "
                            "into the planned orientation; the engine drops the part to the bed "
                            "itself, so no translation is reported. terms and score are "
                            "dimensionless, lower is better, and they are meaningful only as a rank "
                            "within one object's surviving candidates.";
    root["intent"]        = intent_json(intent);
    root["params"]        = params_json(params);
    root["objects"]       = std::move(objects);

    // Object names and file paths are arbitrary bytes, and dump() throws on invalid UTF-8 by
    // default. Replace such sequences with U+FFFD so the output is always valid JSON, as
    // MeshInspect.cpp does.
    out << root.dump(2, ' ', false, json::error_handler_t::replace) << std::endl;
}

} // namespace Nocte
} // namespace Slic3r
