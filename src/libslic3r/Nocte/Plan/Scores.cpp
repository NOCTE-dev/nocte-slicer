// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The five physical terms of the orientation engine, measured. Everything here runs on a plain
// indexed_triangle_set in object coordinates (Z up, millimetres) plus the invariants measured once
// by PartInvariants, and uses only facilities that libslic3r links unconditionally: the triangle
// mesh slicer, Clipper and the 2D convex hull.
//
// Two conventions are load-bearing throughout and are shared with GeometryAnalysis.cpp:45-59:
//   * the angle of a facet is measured from the VERTICAL, so 0 deg is a wall and 90 deg a ceiling,
//     and for a unit normal n under the rotation R it is asin(|(R n) . z|);
//   * libslic3r polygon areas are in scaled coordinates squared, so two unscale steps give mm^2.
//
// Every constant in this file is named, and its comment gives the physical reason it exists. The
// fork exists because Orca's Orient.cpp carries 28 tuned constants with no such reason.

#include "libslic3r/Nocte/Plan/Scores.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

// Fraction of the plane stack ignored at the top and at the bottom of the section sweep. The
// extreme planes of a closed mesh are tangent to the surface and their area collapses to nearly
// nothing, which would swamp the real minimum cross section. The FRACTION is the same as
// GeometryAnalysis.cpp's CROSS_SECTION_TRIM, and for the same reason, but that is all the two
// sweeps share: the spacing differs (section_step_mm 0.5 against slice_step 0.2) and so does the
// plane cap (section_max_planes 400 against MAX_SLICES 200). On anything but a prismatic part the
// two sweeps therefore land on different planes and their minima are NOT comparable — this one is
// the minimum normal to the load direction, that one the horizontal minimum, and they answer
// different questions anyway.
constexpr double SECTION_TRIM_FRACTION = 0.02;

// Floor on every vertical spacing this file uses, and it has two jobs. The smaller one is the
// section sweep: below a micrometre the plane count is dominated by floating point noise rather
// than by geometry, and the sweep would not terminate in any useful time. The load-bearing one is
// the LAYER HEIGHT, which multiplies the cusp term and divides the time term — at a layer height of
// zero the cusp of every facet is exactly 0, the best possible value on that axis for every
// candidate, so that term stops discriminating silently while the time term diverges.
constexpr double MIN_SLICE_STEP_MM = 1e-3;

// The bound-first rule for std::min and std::max
// ----------------------------------------------
// std::max(a, b) is `a < b ? b : a` and std::min(a, b) is `b < a ? b : a`. Every comparison against
// a NaN is false, so both return their FIRST argument whenever the second is a NaN. A clamp
// therefore only clamps when the KNOWN-FINITE bound comes first: max(FLOOR, x) yields FLOOR for a
// NaN x and min(CEILING, x) yields CEILING, while the reversed spellings hand the NaN straight
// through. An escaped NaN does not then blow up, which is exactly the danger — a clamp spelled
// min(1., max(0., NaN)) yields 0, and 0 is the BEST value on every term. combine() therefore scores
// a non-finite value as 1, the worst, before any clamp can see it; the rest of the file keeps NaN
// from escaping in the first place. So: every clamp in this file puts the bound first, and every
// accumulator fold puts the running accumulator, which is known finite, first.

// Upper bound on the number of layer planes the support sweep will slice. A 300 mm part at a 0.1 mm
// layer height is 3000 layers, and the sweep runs once per candidate orientation; past this count
// the sweep is coarsened by an integer factor and that same coarsened step is the height increment
// the column carry multiplies each layer's support area by, so V_s keeps its units exactly and only
// its vertical resolution drops.
constexpr int SUPPORT_MAX_LAYERS = 600;

// A direction shorter than this is treated as "no direction given" rather than normalised, which
// would amplify round-off into an arbitrary axis.
constexpr double DIR_MIN_NORM = 1e-9;

// Below this height the centre of mass sits on the bed and the tipping ratio d_min / z_com
// diverges.
constexpr double COM_HEIGHT_MIN_MM = 1e-4;

// Ceiling on the reported tipping ratio. A part that tips only past a hundred times gravity does
// not tip, and every value above this one says the same thing, so nothing is lost by reporting the
// cap. What would be lost by reporting the true value is the whole stability term: combine() uses a
// min-max range, and a single candidate at 1e6 pins the span there and flattens every genuine
// candidate — 0.2 against 0.9 — into the bottom 1e-6 of the range. The term would go binary and
// stop discriminating. An infinity does this too; a large finite number does it just as thoroughly.
constexpr double STABILITY_MAX = 100.;

// Ceiling on any layer or plane count derived from a height divided by a layer height. A part a
// metre tall at a ten-micrometre layer is a hundred thousand layers, already an order of magnitude
// past anything an FDM machine prints; past this the input is not a part but a broken transform.
// The clamp is not tuning, it is what makes the double-to-int conversion DEFINED: converting a NaN
// or a value beyond INT_MAX is undefined behaviour, and on x86 it yields INT_MIN, which a later
// "must be at least 1" clamp would silently turn into a plausible-looking 1.
constexpr double MAX_LAYER_COUNT = 1e6;

// Slack, in LAYERS, taken off a height-over-layer-height ratio before it is rounded up. A 20 mm part
// that a rotation built from cos(PI/2) = 6.1e-17 leaves 20.000000000000004 mm tall is 100.00000000000002
// layers, and a bare ceil() prints it as 101: one phantom layer, two seconds of phantom time, and a
// candidate that differs from its unrotated twin by nothing but round-off.
//
// Double round-off alone would be cleared by 1e-9 of a layer, but it is not the largest error here.
// Vertices are floats (~6e-8 relative), and a candidate that lays a hull face down is built from a
// normal computed in float, so the face rests up to ~1e-7 rad off flat: a 20 mm cube laid on a face
// that way is 20.000002 mm tall, 100.00001 layers, and 1e-9 would still hand it a 101st. Across the
// A1's 256 mm build height at a 0.08 mm layer the same two effects reach about 4e-4 of a layer. The
// slack is therefore 1e-2 of a layer, a factor of 25 above that; and it costs nothing real, because
// 1e-2 of even a 0.2 mm layer is 2 micrometres, a sliver no FDM profile can print as a layer at all.
// It is a slack on the ratio, in layers, because that is the unit of the decision being made.
constexpr double LAYER_COUNT_ROUNDOFF = 1e-2;

// Two vectors are treated as (anti)parallel when the magnitude of their dot product is within this
// of one. At that point the cross product that a from-to quaternion needs has collapsed into
// round-off and an explicit axis must be chosen instead.
constexpr double PARALLEL_DOT_EPS = 1e-12;

// Share of the surviving facet area that the reported cusp percentile sits at.
constexpr double CUSP_PERCENTILE = 0.95;

// Degrees per radian, spelled out once.
constexpr double RAD_TO_DEG = 180. / PI;

// The angle of a facet from the vertical, in degrees, given the Z component of its unit normal in
// the build frame. Identical to GeometryAnalysis.cpp's overhang_angle_deg for downward facets; the
// absolute value extends it to upward facets, which the cusp term also needs.
double facet_angle_deg(double nz_build)
{
    const double s = (std::min)(1., std::abs(nz_build));
    return std::asin(s) * RAD_TO_DEG;
}

// Distance from `p` to the segment `a`-`b`, all in millimetres.
double point_segment_distance(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  ab = b - a;
    const double l2 = ab.squaredNorm();
    if (l2 <= 0.)
        return (p - a).norm();
    double t = (p - a).dot(ab) / l2;
    t = (std::min)(1., (std::max)(0., t));
    return (p - (a + t * ab)).norm();
}

// The rotation that takes `dir` onto +Z. Eigen's from-two-vectors construction needs a cross
// product, which vanishes when the two directions are parallel or antiparallel, so both ends are
// handled explicitly: parallel is the identity and antiparallel is a half turn about X, which is a
// valid choice because any axis orthogonal to Z will do.
Transform3d rotation_dir_to_z(const Vec3d &dir_unit)
{
    Transform3d t   = Transform3d::Identity();
    const double dot = dir_unit.z();
    if (dot >= 1. - PARALLEL_DOT_EPS)
        return t;
    if (dot <= -1. + PARALLEL_DOT_EPS) {
        t.rotate(Eigen::AngleAxisd(PI, Vec3d::UnitX()));
        return t;
    }
    t.rotate(Eigen::Quaterniond::FromTwoVectors(dir_unit, Vec3d::UnitZ()));
    return t;
}

// Sample planes centred in each slab of thickness `step` over `height`, starting at `z_min`. The
// first plane sits half a step above the bottom so that no plane is ever tangent to a cap.
//
// PRECONDITION: the caller must already have capped the plane count by coarsening `step`. This
// function does not, and deliberately so — the cap is a policy that differs between the two call
// sites (`section_max_planes` for the section sweep, SUPPORT_MAX_LAYERS for the support carry) and
// neither the height nor the step alone can express it here. What this function does guarantee is
// that a non-finite argument produces an empty result rather than an unbounded loop.
std::vector<float> sample_planes(double z_min, double height, double step)
{
    std::vector<float> zs;
    if (! std::isfinite(z_min) || ! std::isfinite(height) || ! std::isfinite(step))
        return zs;
    if (height <= EPSILON || step <= 0.)
        return zs;
    const double count = std::floor(height / step) + 2.;
    if (count > 0. && count < MAX_LAYER_COUNT)
        zs.reserve(static_cast<size_t>(count));
    for (double z = z_min + 0.5 * step; z < z_min + height; z += step)
        zs.push_back(static_cast<float>(z));
    return zs;
}

// How many entries of the parallel facet arrays may be read. The three arrays are documented as
// parallel to `its.indices`, but a caller can hand us a PartInvariants measured from a different
// mesh, and reading past the end of one of them would be an out-of-bounds read rather than the
// degraded-but-safe answer the header promises. Folding all three into one minimum, in one place,
// is what keeps every consumer honest about it.
//
// It bounds the FACET index only. The three vertex indices inside a facet are a separate question
// and this says nothing about them: anything that dereferences `its.vertices[tri(k)]` has to test
// them itself. precompute() rejects a mesh whose triangles point out of range, but the whole reason
// this function exists is that the `its` reaching evaluate() need not be the one precompute() saw.
size_t usable_facet_count(const indexed_triangle_set &its, const PartInvariants &inv)
{
    return (std::min)((std::min)(its.indices.size(), inv.facet_area.size()), inv.facet_normal.size());
}

// Whether the visibility flags are worth reading at all: present, long enough, and carrying at
// least one visible facet.
//
// The last test is the one that is easy to leave out. An inverted mesh — every facet wound inward,
// an ordinary broken STL — passes the self-occlusion pass cleanly and comes back with
// `visibility_available == true` and every single facet marked invisible, because each ray starts
// just inside the surface and travels inward, so each one hits. Everything downstream that reads
// "visible" then measures the empty set and reports zero, and zero is the favourable end of both
// terms that depend on it. Present is not the same as usable.
bool visibility_usable(const PartInvariants &inv, size_t n_faces)
{
    if (! inv.visibility_available || inv.facet_visible.size() < n_faces)
        return false;
    for (size_t i = 0; i < n_faces; ++ i)
        if (inv.facet_visible[i] != 0)
            return true;
    return false;
}

// The three vertex indices of facet `i` all address a vertex of `its`. See usable_facet_count().
bool facet_vertices_in_range(const indexed_triangle_set &its, size_t i)
{
    const stl_triangle_vertex_indices &tri = its.indices[i];
    for (int k = 0; k < 3; ++ k) {
        const int v = tri(k);
        if (v < 0 || static_cast<size_t>(v) >= its.vertices.size())
            return false;
    }
    return true;
}

// The range of the mesh along `row_z`, offset by `z_shift`, skipping any vertex whose projection is
// not finite. The skip is not defensive decoration: a NaN is STICKY through std::min, because
// min(a, b) returns `b < a ? b : a` and every comparison against a NaN is false. One NaN vertex read
// first would therefore pin both ends of the range at NaN, and the candidate would be scored on a
// NaN height that no later clamp can recover. Returns false when no vertex carried a finite height.
bool z_range_along(const indexed_triangle_set &its,
                   const Vec3d                &row_z,
                   double                      z_shift,
                   double                     &out_min,
                   double                     &out_max)
{
    bool first = true;
    for (const Vec3f &v : its.vertices) {
        const double z = row_z.dot(v.cast<double>()) + z_shift;
        if (! std::isfinite(z))
            continue;
        if (first) {
            out_min = z;
            out_max = z;
            first   = false;
        } else {
            out_min = (std::min)(out_min, z);
            out_max = (std::max)(out_max, z);
        }
    }
    return ! first;
}

// ExPolygon / Polygon areas are in scaled coordinates squared. Two unscale steps give mm^2.
double area_mm2(const Polygons &polys)
{
    return unscale<double>(unscale<double>(Slic3r::area(polys)));
}

double area_mm2(const ExPolygons &polys)
{
    return unscale<double>(unscale<double>(Slic3r::area(polys)));
}

// The sub-mesh of the facets a predicate accepts, with the vertex indices remapped so that facets
// which shared a vertex in the source still share it here. Sharing matters: the slab slicer chains
// loops through face neighbours, and a mesh of unwelded triangles would not chain.
//
// Handing slice_mesh_slabs() an open, partial sub-mesh is its documented use rather than an abuse
// of it: TriangleMeshSlicer.hpp:101 states that the triangle set the function processes may not
// cover the whole top or bottom surface.
//
// The degenerate-area skip lives here, once, so that no predicate has to remember it. A degenerate
// facet has zero area and a zero normal; PartInvariants guarantees it is recognised by the area and
// never by the normal, and the slab slicer expects non-degenerate triangles.
template<typename FacetPredicate>
indexed_triangle_set submesh_where(const indexed_triangle_set &its,
                                   const PartInvariants       &inv,
                                   FacetPredicate              accept)
{
    indexed_triangle_set out;
    const size_t n_faces = usable_facet_count(its, inv);
    if (n_faces == 0)
        return out;

    std::vector<int> remap(its.vertices.size(), -1);
    for (size_t i = 0; i < n_faces; ++ i) {
        if (! (inv.facet_area[i] > 0.f))
            continue;
        if (! accept(i))
            continue;
        // A facet pointing outside the vertex array is dropped, not a reason to throw the rest of
        // the mesh away: the same rule tier 0 applies, so one corrupt triangle costs one triangle.
        if (! facet_vertices_in_range(its, i))
            continue;
        const stl_triangle_vertex_indices &tri = its.indices[i];
        stl_triangle_vertex_indices        dst;
        for (int k = 0; k < 3; ++ k) {
            const int src = tri(k);
            int &slot = remap[static_cast<size_t>(src)];
            if (slot < 0) {
                slot = static_cast<int>(out.vertices.size());
                out.vertices.emplace_back(its.vertices[static_cast<size_t>(src)]);
            }
            dst(k) = slot;
        }
        out.indices.emplace_back(dst);
    }
    return out;
}

// One candidate's cusp statistics, all area-weighted.
struct CuspResult
{
    double mean  = 0.;   // mm,   conditional area-weighted mean over the surviving facets
    double error = 0.;   // mm^3, the extensive form: sum(a_i * delta_i) over the same facets
    double p95   = 0.;   // mm

    // False when no facet survived the visible-and-non-flat filter, so the three zeros above mean
    // "nothing was measured" and not "the surface is perfect". Zero is the BEST value on this term
    // in combine(), which is why the distinction cannot be left to the reader.
    bool   measured = false;
};

struct CuspSample
{
    double cusp = 0.;
    double area = 0.;
};

// The cusp height of a facet is the layer height times the cosine of the angle between the facet
// normal and the build direction, i.e. h * |n' . z|: a wall has no staircase and a ceiling has a
// full layer of it. Facets within flat_exclude_deg of horizontal are dropped because the formula
// returns c -> h for a perfect flat top, which is nonsense — a flat top has no staircase at all,
// its quality is governed by the top shell and by ironing.
CuspResult measure_cusp(const indexed_triangle_set &its,
                        const PartInvariants       &inv,
                        const Vec3d                &row_z,
                        const ScoreParams          &params,
                        double                      h)
{
    CuspResult result;
    const size_t n_faces = usable_facet_count(its, inv);
    if (n_faces == 0)
        return result;

    const bool   use_visibility = inv.visibility_available && inv.facet_visible.size() >= n_faces;
    const double flat_limit_deg = 90. - params.flat_exclude_deg;

    std::vector<CuspSample> samples;
    samples.reserve(n_faces);
    double total_area     = 0.;
    double area_cusp_sum  = 0.;

    for (size_t i = 0; i < n_faces; ++ i) {
        const double area = double(inv.facet_area[i]);
        if (! (area > 0.))
            continue;
        if (use_visibility && inv.facet_visible[i] == 0)
            continue;
        const double nz  = row_z.dot(inv.facet_normal[i].cast<double>());
        const double phi = facet_angle_deg(nz);
        if (phi > flat_limit_deg)
            continue;
        const double cusp = h * std::abs(nz);
        samples.push_back(CuspSample{cusp, area});
        total_area    += area;
        area_cusp_sum += area * cusp;
    }

    if (! (total_area > 0.))
        return result;

    // The two forms of the same sum. `mean` divides by the surviving area, so the excluded flat
    // facets leave the denominator as well as the numerator and it answers "how rough is the rough
    // part". `error` keeps the sum itself and is the one that can tell a small rough patch from a
    // large one.
    //
    // `error` is the SURFACE-ERROR INTEGRAL, area times peak deviation, and it is not the volume of
    // material the staircase displaces. It is off from that volume by two factors that both matter:
    // a sawtooth of peak delta displaces about a*delta/2, not a*delta, and `a` here is the slanted
    // facet area rather than its projection. Calling it displaced material would overstate by
    // roughly two. It is a comparable error measure between candidates, in mm^3, and nothing more.
    result.mean     = area_cusp_sum / total_area;
    result.error    = area_cusp_sum;
    result.measured = true;

    // The percentile is taken over AREA, not over facet count: one huge facet and a thousand slivers
    // describe the same surface, and a count percentile would be decided by the slivers.
    std::sort(samples.begin(), samples.end(),
              [](const CuspSample &l, const CuspSample &r) { return l.cusp < r.cusp; });
    const double target = CUSP_PERCENTILE * total_area;
    double       acc    = 0.;
    result.p95 = samples.back().cusp;
    for (const CuspSample &s : samples) {
        acc += s.area;
        if (acc >= target) {
            result.p95 = s.cusp;
            break;
        }
    }
    return result;
}

// Tier 0. Each overhanging facet is charged a column of support reaching from the bed to its
// centroid: V ~= sum a_i * sin(phi_i) * z_i, where a_i * sin(phi_i) is the facet's horizontal
// projection. That product is an area times a height, which is the same physical quantity tier 1
// measures, so the two tiers are directly comparable and differ in exactly one thing: tier 0 is
// blind to self-support, and charges a facet standing directly above solid material the whole column
// down to the bed anyway. That is why the tier is recorded in the result and why tier 0 must never
// be shown to the user in grams.
double support_volume_facet_sweep(const indexed_triangle_set &its,
                                  const PartInvariants       &inv,
                                  const Vec3d                &row_z,
                                  double                      z_offset,
                                  const ScoreParams          &params)
{
    const size_t n_faces = usable_facet_count(its, inv);
    double       volume  = 0.;
    for (size_t i = 0; i < n_faces; ++ i) {
        const double area = double(inv.facet_area[i]);
        if (! (area > 0.))
            continue;
        const double nz = row_z.dot(inv.facet_normal[i].cast<double>());
        if (! (nz < 0.))
            continue;
        const double phi = facet_angle_deg(nz);
        if (phi <= params.overhang_threshold_deg)
            continue;
        // This is the one place in the file that dereferences a vertex index without having gone
        // through submesh_where(), which refuses such a facet. usable_facet_count() bounds the
        // facet index and says nothing about the three indices inside it.
        if (! facet_vertices_in_range(its, i))
            continue;

        const stl_triangle_vertex_indices &tri = its.indices[i];
        const Vec3d centroid = (its.vertices[tri(0)].cast<double>() +
                                its.vertices[tri(1)].cast<double>() +
                                its.vertices[tri(2)].cast<double>()) / 3.;
        const double z_centroid = row_z.dot(centroid) + z_offset;
        if (! (z_centroid > 0.))
            continue;
        // sin(phi) is exactly -nz here, because phi was defined as asin(-nz).
        volume += area * (-nz) * z_centroid;
    }
    return params.support_density * volume;
}

struct SliceUnionResult
{
    double support_volume_mm3 = 0.;
    double contact_mm2        = 0.;
    bool   ok                 = false;
};

// Tier 1, the top-down column carry. Support is a COLUMN under an overhang, so its volume is an
// area times a height, and the region that needs one has to be carried DOWNWARDS through the stack:
//
//     R = {};   for k = top .. 0:   R = (R union B_{k+1}) \ S_k;   V_s += rho_s * area(R) * h
//
// `S_k` is the solid slice of the WHOLE part at plane k, and `B_{k+1}` the downward-facing slab
// projection at the plane above it, taken from a sub-mesh of the facets that actually need support:
// downward-facing in the build frame and steeper than `overhang_threshold_deg`. Only the overhang
// source is filtered; the part that gets in a column's way is still the whole part. The `\ S_k` step
// is what makes the estimate self-support aware, and it is also what stops a column where the part
// itself is in the way, so that support rests on the model below it rather than always on the bed.
//
// The direction is not a preference. Bottom-up cannot work: at plane k the overhangs that will need
// a column through it are all above, and have not been seen yet.
//
// Why `B` must be filtered, and why the cap below cannot show it: the slab bottom output carries
// EVERY facet with a downward component, a 10-degree draft face as readily as a horizontal ceiling.
// Take a sphere resting on the bed. Every facet of the lower hemisphere faces downwards, so all of
// them would enter B; `S_k` is a disc whose radius shrinks going down, so the annuli carried from
// above always fall outside it and `\ S_k` removes nothing. An unfiltered carry therefore charges a
// support ring from the equator all the way to the plate, where a real slicer supports only below
// the 45-degree parallel — and the same happens under every tapered boss, chamfer and fillet. The
// cap-on-a-stem case below is blind to this, because a horizontal cap sits at 90 degrees and passes
// any threshold. With the filter in place tier 0 and tier 1 apply the SAME threshold, so the only
// remaining difference between them is self-support awareness, which is what the tier field claims.
//
// The case that pins the formula: a 20 x 20 mm cap at z = 10 carried by a 4 x 4 mm stem. The cap's
// downward projection is 400 - 16 = 384 mm^2, and S_k removes only the 16 mm^2 of stem at every
// plane underneath it, so the carry runs 384 mm^2 down the whole ten millimetres and
// V_s = 0.15 * 384 * 10 = 576 mm^3. Summing interface areas one layer thick instead would have
// returned 0.15 * 384 * 0.2 = 11.5 mm^3, short by the height of the part — and short by a DIFFERENT
// factor for every candidate, since the height is exactly what the orientation changes.
//
// B_0 is never added to the region: the part has been dropped to the bed, so the downward faces of
// the lowest slab rest on the build plate and carry no column.
SliceUnionResult support_volume_slice_union(const indexed_triangle_set &solid_mesh,
                                            const indexed_triangle_set &overhangs,
                                            const indexed_triangle_set &overhangs_visible,
                                            const Transform3d          &trafo,
                                            const std::vector<float>   &zs,
                                            double                      step,
                                            const ScoreParams          &params)
{
    SliceUnionResult out;
    if (zs.empty())
        return out;

    // The whole body is guarded, not just the slicing. libslic3r's Clipper wrappers throw
    // clipperException on coordinates that overflow the scaled integer range, so to_polygons(),
    // union_(), diff() and area() in the carry below are every bit as able to throw as the slicer
    // is, and the header promises the engine never throws on user geometry.
    try {
        MeshSlicingParamsEx slicing_params;
        slicing_params.trafo = trafo;

        std::vector<ExPolygons> solid = slice_mesh_ex(solid_mesh, zs, slicing_params);
        std::vector<Polygons>   downward;
        std::vector<Polygons>   downward_visible;
        if (! overhangs.indices.empty())
            slice_mesh_slabs(overhangs, zs, trafo, nullptr, &downward, nullptr, [](){});
        if (! overhangs_visible.indices.empty())
            slice_mesh_slabs(overhangs_visible, zs, trafo, nullptr, &downward_visible, nullptr, [](){});

        if (solid.size() != zs.size())
            return SliceUnionResult();

        // Two different things that a single size test used to conflate.
        //
        // An EMPTY sub-mesh is a measured zero: no facet of the part passed the overhang threshold,
        // nothing needs support, and no slab call was made. A slab call that DID run and came back
        // with the wrong number of layers is a failure, and has to be reported as one — reading it
        // as "no support" would launder a failed measurement into the best possible score on this
        // term, which is precisely the treatment slice_mesh_ex gets two lines above.
        const bool have_overhangs = ! overhangs.indices.empty();
        const bool have_visible   = ! overhangs_visible.indices.empty();
        if (have_overhangs && downward.size() != zs.size())
            return SliceUnionResult();
        if (have_visible && downward_visible.size() != zs.size())
            return SliceUnionResult();
        if (! have_overhangs && ! have_visible) {
            out.ok = true;
            return out;
        }

        Polygons region;   // R, the support region carried down from the plane above
        double   volume  = 0.;
        double   contact = 0.;
        // Reverse iteration over an unsigned index: the post-decrement in the condition leaves k at
        // zs.size()-1 on the first pass and terminates before k would wrap around.
        for (size_t k = zs.size(); k-- > 0; ) {
            const Polygons solid_here = to_polygons(solid[k]);
            if (k + 1 < zs.size()) {
                if (have_overhangs)
                    region = union_(region, downward[k + 1]);
                if (have_visible) {
                    // Support touches the part where a VISIBLE overhang of the slab above is not
                    // already resting on solid material at this plane. The source sub-mesh is
                    // visible AND past the overhang threshold, because a shallow draft face on the
                    // showcase side is not support touching the visible surface — no support is
                    // generated there at all. This is an interface and not a column, so it is
                    // counted once where it occurs and is never carried downwards. It is a
                    // PROJECTED area: for an overhang at angle phi the real contacted surface is
                    // larger by 1/sin(phi), which matters only if the number is ever quoted to a
                    // user as finished surface rather than used as a ranking or a hard constraint.
                    const double av = area_mm2(diff(downward_visible[k + 1], solid_here));
                    if (av > 0.)
                        contact += av;
                }
            }
            region = diff(region, solid_here);
            const double a = area_mm2(region);
            if (a > 0.)
                volume += a * step;
        }

        out.support_volume_mm3 = params.support_density * volume;
        out.contact_mm2        = contact;
        out.ok                 = true;
    } catch (...) {
        return SliceUnionResult();
    }
    return out;
}

} // namespace

const char *support_tier_name(SupportTier tier)
{
    // These three spellings are written into the --nocte-plan JSON and into the project report, so
    // they are a persisted format: change one and every document already produced stops matching.
    switch (tier) {
    case SupportTier::FacetSweep: return "facet-sweep";
    case SupportTier::SliceUnion: return "slice-union";
    case SupportTier::FullDetect: return "full-detect";
    }
    // Unreachable for a valid enumerator, and deliberately not an assert: a tier we cannot name is
    // a reason to say so in the output, not to bring down a run that has already measured a part.
    return "unknown";
}

double min_section_area_along(const indexed_triangle_set &its, const Vec3d &dir, const ScoreParams &params)
{
    if (its.vertices.empty() || its.indices.empty())
        return 0.;
    const double dir_norm = dir.norm();
    if (! (dir_norm > DIR_MIN_NORM))
        return 0.;

    const Transform3d trafo = rotation_dir_to_z(dir / dir_norm);
    const Vec3d       row_z = trafo.linear().transpose() * Vec3d::UnitZ();

    double z_min = 0.;
    double z_max = 0.;
    if (! z_range_along(its, row_z, 0., z_min, z_max))
        return 0.;
    const double height = z_max - z_min;
    if (height <= EPSILON)
        return 0.;

    const int    max_planes = (std::max)(1, params.section_max_planes);
    double       step       = (std::max)(MIN_SLICE_STEP_MM, params.section_step_mm);
    if (height / step > double(max_planes))
        step = height / double(max_planes);

    const std::vector<float> zs = sample_planes(z_min, height, step);
    if (zs.empty())
        return 0.;

    MeshSlicingParamsEx slicing_params;
    slicing_params.trafo = trafo;

    try {
        const std::vector<ExPolygons> layers = slice_mesh_ex(its, zs, slicing_params);
        const size_t trim  = static_cast<size_t>(double(layers.size()) * SECTION_TRIM_FRACTION);
        double       best  = 0.;
        bool         found = false;
        for (size_t i = trim; i + trim < layers.size(); ++ i) {
            const double a = area_mm2(layers[i]);
            if (a <= 0.)
                continue;
            if (! found || a < best) {
                best  = a;
                found = true;
            }
        }
        return found ? best : 0.;
    } catch (...) {
        return 0.;
    }
}

OrientScores evaluate(const indexed_triangle_set &its,
                      const PartInvariants       &inv,
                      const Transform3d          &rotation,
                      const ScoreParams          &params,
                      const Vec3d                &load_dir_obj,
                      double                      min_section_mm2)
{
    OrientScores scores;
    scores.support_tier = params.support_tier == SupportTier::FacetSweep ? SupportTier::FacetSweep
                                                                        : SupportTier::SliceUnion;
    if (its.vertices.empty() || its.indices.empty())
        return scores;

    // `rotation` is a rigid body rotation, so the third row of its linear part maps a point or a
    // unit normal in object coordinates straight onto its Z component in the build frame. That one
    // dot product is the whole per-facet cost of a candidate.
    const Vec3d row_z = rotation.linear().transpose() * Vec3d::UnitZ();
    const double z_translation = rotation.translation().z();

    double z_min = 0.;
    double z_max = 0.;
    if (! z_range_along(its, row_z, z_translation, z_min, z_max))
        // Not one vertex landed at a finite height, so there is nothing to drop to the bed and
        // nothing to measure. `measured` stays false and every field keeps its initialiser.
        return scores;
    scores.height_mm = z_max - z_min;

    // Drop to the bed: everything Z-dependent below is measured from the build plate, not from
    // wherever the caller's rotation happened to leave the part.
    Transform3d trafo = rotation;
    trafo.pretranslate(Vec3d(0., 0., -z_min));
    const double z_offset = z_translation - z_min;

    // The layer height is floored ONCE, here, and every term is handed the same value. Two
    // different clamps of it is not a style question: with layer_height_mm at 0 an unfloored cusp
    // term returns identically 0 — the best possible value on that axis, for every candidate, so
    // the term silently stops discriminating — while the time term divides by it and explodes.
    // Floor first, per the rule above: a NaN layer height must come out of this as the floor, not
    // as a NaN that reaches all three cusp fields and normalises to the best score in combine().
    const double h = (std::max)(MIN_SLICE_STEP_MM, params.layer_height_mm);

    // --- cusp ---------------------------------------------------------------------------------
    const CuspResult cusp = measure_cusp(its, inv, row_z, params, h);
    scores.cusp_mean_mm   = cusp.mean;
    scores.cusp_error_mm3 = cusp.error;
    scores.cusp_p95_mm    = cusp.p95;
    scores.cusp_measured  = cusp.measured;

    // --- support ------------------------------------------------------------------------------
    // Tier 2 needs a PrintObject advanced to posSlice, which this entry point does not have. It
    // falls back to tier 1 and reports tier 1, so that a caller reading `support_tier` can never
    // believe a full detection was run.
    if (params.support_tier == SupportTier::FacetSweep) {
        // The facet sweep is a bounded arithmetic pass with no slicing and nothing that can fail,
        // so its answer is always a measurement. It computes no contact area at all, which is why
        // contact_measured stays false on this tier however the visibility flags came out.
        scores.support_volume_mm3 = support_volume_facet_sweep(its, inv, row_z, z_offset, params);
        scores.support_tier       = SupportTier::FacetSweep;
        scores.support_measured   = true;
    } else if (scores.height_mm > EPSILON) {
        // The coarsening factor is taken from the UNCLAMPED plane count. Clamping first and then
        // dividing would defeat the cap it is supposed to enforce: at a true count of 1e7 planes,
        // a factor computed from a 1e6 clamp comes out at 1667, and 1e7 / 1667 is 6000 planes —
        // ten times what SUPPORT_MAX_LAYERS promises. Nothing here is narrowed to an int, so no
        // clamp is needed for the conversion either; `height_mm` is finite by construction and
        // `step` is at least MIN_SLICE_STEP_MM, so the ratio is finite.
        double       step       = h;
        const double raw_planes = std::ceil(scores.height_mm / step - LAYER_COUNT_ROUNDOFF);
        if (raw_planes > double(SUPPORT_MAX_LAYERS)) {
            const double factor = std::ceil(raw_planes / double(SUPPORT_MAX_LAYERS));
            step = h * factor;
        }
        const std::vector<float> zs = sample_planes(0., scores.height_mm, step);

        // A facet needs support when it faces downwards in the build frame and stands steeper than
        // the threshold — the same two tests tier 0 applies, so that the two tiers charge the same
        // geometry. The predicate depends on the rotation, so this sub-mesh is rebuilt per candidate;
        // that is one O(F) pass in front of a slab pass that is already O(F log F).
        const size_t n_faces = usable_facet_count(its, inv);
        // USABLE, not merely available: see visibility_usable(). On an inverted mesh the flags are
        // present and every one of them says "invisible", and a contact area measured over the
        // empty set would otherwise be reported as a measured zero.
        const bool   use_visibility = visibility_usable(inv, n_faces);

        const auto is_overhang = [&inv, &row_z, &params](size_t i) {
            const double nz = row_z.dot(inv.facet_normal[i].cast<double>());
            return nz < 0. && facet_angle_deg(nz) > params.overhang_threshold_deg;
        };

        const indexed_triangle_set overhangs = submesh_where(its, inv, is_overhang);
        indexed_triangle_set       overhangs_visible;
        if (use_visibility)
            overhangs_visible = submesh_where(its, inv, [&inv, &is_overhang](size_t i) {
                return inv.facet_visible[i] != 0 && is_overhang(i);
            });

        // S_k comes from `inv.solid`, the same mesh the footprint slice below uses. Dropping the
        // zero-area facets cannot change a cross section — they contribute no segment to a slice
        // plane — so this is the same solid, measured once instead of once per candidate.
        const SliceUnionResult su = support_volume_slice_union(inv.solid, overhangs, overhangs_visible,
                                                               trafo, zs, step, params);
        scores.support_tier     = SupportTier::SliceUnion;
        scores.support_measured = su.ok;
        if (su.ok) {
            scores.support_volume_mm3 = su.support_volume_mm3;
            // Without USABLE visibility flags we cannot say which of that contact area sits under a
            // surface the user will look at, and a zero here means "not measured", never "none".
            // With them, a zero is a measurement: there is no support on any visible face. The
            // distinction rests on visibility_usable(), not on visibility_available, because the
            // flags can be present and unanimously wrong.
            if (use_visibility) {
                scores.support_contact_mm2 = su.contact_mm2;
                scores.contact_measured    = true;
            }
        }
    }

    // --- time ---------------------------------------------------------------------------------
    // The part's own extruded volume is orientation-invariant, so it shifts every candidate by the
    // same amount and cancels out of the ranking. What actually separates candidates here is
    // layer_count * layer_overhead_s plus the support volume, which is why layer_overhead_s is the
    // one constant of the model that has to be measured on the machine rather than guessed.
    //
    // The count is clamped as a double before it is narrowed. Converting a NaN or a value past
    // INT_MAX to int is undefined behaviour; on x86 it produces INT_MIN, and a bare "at least 1"
    // clamp afterwards would turn that into a perfectly plausible single-layer part whose time
    // estimate is then quietly wrong rather than visibly absurd.
    //
    // The ratio is rounded up with LAYER_COUNT_ROUNDOFF of slack, so a height that is a whole number
    // of layers plus round-off is that whole number and not one more.
    {
        double n_layers = std::ceil(scores.height_mm / h - LAYER_COUNT_ROUNDOFF);
        if (! std::isfinite(n_layers) || n_layers < 1.)
            n_layers = 1.;
        scores.layer_count = static_cast<int>((std::min)(MAX_LAYER_COUNT, n_layers));
    }
    {
        // No machine speed reaches this function, so the effective flow is the filament's own
        // volumetric ceiling: Q_eff = min(h * w * v_max, max_volumetric_speed) collapses to the
        // second term.
        const double q_eff    = (std::max)(0., params.max_volumetric_speed);
        const double extruded = (std::max)(0., inv.volume) + scores.support_volume_mm3;
        const double flow_s   = q_eff > 0. ? extruded / q_eff : 0.;
        scores.time_estimate_s = flow_s + double(scores.layer_count) * params.layer_overhead_s;
    }

    // --- strength -----------------------------------------------------------------------------
    // An FDM part is transversely isotropic: it carries sigma_xy in the layer plane and only
    // k * sigma_xy across the layers. With d the load direction in the build frame, d.z^2 is the
    // share of the load taken across the layers, so the allowable stress interpolates between the
    // two. mm^2 * MPa is exactly newtons, with no conversion factor.
    const double load_norm = load_dir_obj.norm();
    if (load_norm > DIR_MIN_NORM) {
        const Vec3d  d         = (rotation.linear() * load_dir_obj).normalized();
        const double sigma_z   = params.anisotropy_k * params.sigma_xy_mpa;
        const double dz2       = (std::min)(1., d.z() * d.z());
        const double sigma_all = sigma_z + (params.sigma_xy_mpa - sigma_z) * (1. - dz2);
        scores.min_section_area_mm2 = (std::max)(0., min_section_mm2);
        scores.failure_force_n      = scores.min_section_area_mm2 * sigma_all;
        // A load direction was given, so the sweep was attempted. It is believable only if it came
        // back with a positive section: min_section_area_along() returns 0 for seven distinct
        // failures as well as for "no load given", and a closed solid always has a positive section
        // normal to any direction. So a zero here means the sweep broke, not that the part is
        // infinitely weak, and the flag is what stops that being reported as the user's own missing
        // input or written into the JSON as a force of zero newtons.
        scores.section_measured = scores.min_section_area_mm2 > 0.;
    }

    // --- footprint and stability ----------------------------------------------------------------
    // The footprint is BED CONTACT — the first-layer slice — and NOT the part's silhouette.
    // project_mesh() (TriangleMeshSlicer.cpp:2422) slices at {-1e10, 1e10} and unions the two ends,
    // so what it returns is the shadow of the whole part. For anything wider above than at its base
    // the two are different objects and the shadow flatters every constraint that reads this: the
    // 20x20 cap on a 4x4 stem casts a 400 mm^2 shadow over a 16 mm^2 footing, which turns a tipping
    // ratio of 0.18 into 0.88 and walks a part that must be rejected straight through the gate. A
    // ball resting on the bed scores as steadily as a cube, on a point contact.
    //
    // The slice is taken from `inv.solid`, the degenerate-filtered mesh PartInvariants built once
    // for the whole part, not from the raw input and not from a copy made per candidate: the filter
    // reads nothing a rotation can change. Filtering matters most for the two slice_mesh_slabs()
    // calls in the support carry, which assert at TriangleMeshSlicer.cpp:2232 that no triangle has
    // two coincident vertices — those are fed by submesh_where(), which applies the same filter —
    // and the footprint slice uses the same mesh so that the solid it measures is the one the carry
    // subtracts.
    //
    // The tipping margin is then the real distance from the centre of mass to the nearest edge of
    // the hull of that contact, not an isotropic sqrt(area) / height: a long flat part is safe
    // across its length and marginal across its width, and averaging the two hides the marginal one.
    ExPolygons first_layer;
    Polygon    hull;
    try {
        if (! inv.solid.indices.empty()) {
            MeshSlicingParamsEx fp_params;
            fp_params.trafo = trafo;
            const std::vector<float>      fp_z { static_cast<float>(0.5 * h) };
            const std::vector<ExPolygons> fp = slice_mesh_ex(inv.solid, fp_z, fp_params);
            if (! fp.empty()) {
                first_layer = fp.front();
                hull        = Geometry::convex_hull(first_layer);
            }
        }
    } catch (...) {
        first_layer.clear();
        hull = Polygon();
    }

    const bool footprint_usable = hull.points.size() >= 3;
    if (footprint_usable) {
        // Two areas, because two constraints read them and they are not the same number. The RAW
        // first-layer area is what adhesion depends on — a ring's convex hull would claim the whole
        // disc and overstate its grip on the plate — while the HULL area is the support polygon the
        // tipping margin is measured against. Support material would enlarge the real support
        // polygon and is deliberately not modelled here: this is the part's own footing.
        scores.footprint_area_mm2      = std::abs(area_mm2(first_layer));
        scores.footprint_hull_area_mm2 = std::abs(unscale<double>(unscale<double>(hull.area())));

        const Vec3d com   = trafo * inv.center_of_mass;
        const Vec2d com2d = Vec2d(com.x(), com.y());

        // A centre of mass that is not finite leaves `stability` at 0. That direction matters: 0 is
        // the WORST value on this term, and the alternative — falling through the comparisons, all
        // of which are false against a NaN, and landing on the untippable ceiling — would hand the
        // best possible stability score to the one part we failed to measure.
        if (com.allFinite()) {
            // The hull is convex, so the centre of mass is inside exactly when every edge sees it
            // on the same side. Testing the sign rather than the winding keeps this independent of
            // the orientation the hull builder happens to produce.
            bool   positive = false;
            bool   negative = false;
            double d_min    = 0.;
            bool   have_min = false;
            const size_t n_pts = hull.points.size();
            for (size_t i = 0; i < n_pts; ++ i) {
                const Point &pa = hull.points[i];
                const Point &pb = hull.points[(i + 1) % n_pts];
                const Vec2d  a(unscale<double>(pa.x()), unscale<double>(pa.y()));
                const Vec2d  b(unscale<double>(pb.x()), unscale<double>(pb.y()));
                const double cross = (b.x() - a.x()) * (com2d.y() - a.y()) -
                                     (b.y() - a.y()) * (com2d.x() - a.x());
                if (cross > 0.)
                    positive = true;
                else if (cross < 0.)
                    negative = true;
                const double dist = point_segment_distance(com2d, a, b);
                if (! have_min || dist < d_min) {
                    d_min    = dist;
                    have_min = true;
                }
            }

            if (positive && negative) {
                // The projected centre of mass falls outside the footprint: the part is already
                // tipping and no lateral acceleration at all is needed to topple it.
                scores.stability = 0.;
            } else {
                const double z_com = com.z();
                double       ratio = 0.;
                if (z_com < 0.) {
                    // The part was dropped to the bed, so its centre of mass cannot be below it.
                    // An inverted-winding mesh can still produce one, and that is a measurement
                    // failure, not an infinitely stable part: it goes to 0, the worst value, with
                    // the rest of the unmeasurable cases. The old test sent it to the ceiling.
                    ratio = 0.;
                } else if (z_com <= COM_HEIGHT_MIN_MM) {
                    // The centre of mass is ON the bed. There is no lever arm to tip about at all,
                    // which is the one case that genuinely earns the cap.
                    ratio = STABILITY_MAX;
                } else {
                    ratio = d_min / z_com;
                }
                scores.stability = std::isfinite(ratio) ? (std::min)(STABILITY_MAX, ratio) : 0.;
            }
        }
    }

    scores.measured = inv.valid && footprint_usable;
    return scores;
}

// --- combination -------------------------------------------------------------------------------

ScoreWeights ScoreWeights::normalized() const
{
    const double s = this->sum();
    ScoreWeights out;
    if (! (s > 0.)) {
        // A set of weights that does not sum to anything positive states no preference at all, and
        // an equal split is the only answer that does not silently invent one.
        out.support   = 0.2;
        out.cusp      = 0.2;
        out.time      = 0.2;
        out.strength  = 0.2;
        out.stability = 0.2;
        return out;
    }
    out.support   = support / s;
    out.cusp      = cusp / s;
    out.time      = time / s;
    out.strength  = strength / s;
    out.stability = stability / s;
    return out;
}

std::vector<double> combine(const std::vector<OrientScores> &candidates,
                            const ScoreWeights              &weights,
                            const ScoreEpsilons             &eps,
                            std::vector<std::array<double, 5>> *out_normalized)
{
    std::vector<double> result;
    if (out_normalized)
        out_normalized->clear();
    if (candidates.empty())
        return result;

    const size_t n = candidates.size();
    result.assign(n, 0.);
    if (out_normalized)
        out_normalized->assign(n, std::array<double, 5>{{0., 0., 0., 0., 0.}});

    const ScoreWeights w = weights.normalized();

    // The force indifference floor is a FRACTION of the best force in this candidate set, not an
    // absolute newton value: the same 5 N difference is decisive on a 20 N bracket and irrelevant
    // on a 2 kN one.
    //
    // Only finite forces are folded in. A NaN would be dropped by the bound-first spelling anyway,
    // but a +inf would not, and an infinite best force makes the floor infinite and flattens every
    // measured force in the set to the same value.
    double best_force = 0.;
    for (const OrientScores &c : candidates)
        if (std::isfinite(c.failure_force_n))
            best_force = (std::max)(best_force, c.failure_force_n);

    // Raw values, all oriented so that LOWER IS BETTER before normalisation.
    //  * support, cusp and time are costs already: they are taken as they are.
    //  * strength is inverted by negating the failure force, because more force is better.
    //  * stability is inverted for exactly the same reason: it is the lateral acceleration in g at
    //    which the part topples, so a larger value is a safer part. The header states the rule as
    //    "terms where more is better are inverted inside", and names both of them.
    // Because every raw value is already oriented this way, each entry written to `out_normalized`
    // lands in [0, 1] with 0 on the best candidate for that term, whatever the sign of the physical
    // quantity behind it, and a term whose weight is zero leaves its column at 0 for every row.
    const std::array<double, 5> term_weights = {{ w.support, w.cusp, w.time, w.strength, w.stability }};
    const std::array<double, 5> term_eps     = {{ eps.support_mm3,
                                                  eps.cusp_mm,
                                                  eps.time_s,
                                                  eps.force_frac * best_force,
                                                  eps.stability }};

    std::array<std::vector<double>, 5> raw;
    for (size_t t = 0; t < 5; ++ t) {
        if (! (term_weights[t] > 0.))
            continue;   // a zero weight drops the term entirely: no range, no contribution
        raw[t].reserve(n);
        for (const OrientScores &c : candidates) {
            double v = 0.;
            if (t == 0)
                v = c.support_volume_mm3;
            else if (t == 1)
                // The area-weighted mean is the surface error integrated over the part, which is
                // the quantity the weight is a preference about. cusp_p95_mm is reported alongside
                // it for the panel and for hard constraints, and is not combined here.
                v = c.cusp_mean_mm;
            else if (t == 2)
                v = c.time_estimate_s;
            else if (t == 3)
                v = -c.failure_force_n;
            else
                v = -c.stability;
            raw[t].push_back(v);
        }
    }

    for (size_t t = 0; t < 5; ++ t) {
        if (raw[t].empty())
            continue;

        // The range is taken over the FINITE values only. A NaN is sticky through min and max in
        // one argument order and ignored in the other, and an infinity would pin one end of the
        // span and flatten every real value against it; either way the range would stop describing
        // the candidates that were actually measured. Non-finite values are scored separately below.
        bool   have_range = false;
        double lo         = 0.;
        double hi         = 0.;
        for (double v : raw[t]) {
            if (! std::isfinite(v))
                continue;
            if (! have_range) {
                lo         = v;
                hi         = v;
                have_range = true;
            } else {
                lo = (std::min)(lo, v);
                hi = (std::max)(hi, v);
            }
        }

        // The span is compared against the indifference floor, never against a numerical epsilon,
        // and below the floor the candidates are declared EQUAL on this term: every one of them
        // normalises to 0. That is what the floor means — "the difference below which we do not
        // care" (HLSD §6) — and dividing by max(span, floor) alone does not deliver it: it only
        // shrinks a sub-floor ordering, so 1000 against 1200 mm^3 of support would still come out
        // 0 and 0.4 and still decide a ranking the floor says it must not. Bound first, so a NaN
        // floor comes out as 0 and declares nothing equal that is not.
        const double floor_eps   = (std::max)(0., term_eps[t]);
        const bool   indifferent = have_range && (hi - lo) < floor_eps;
        double den = (std::max)(floor_eps, hi - lo);
        if (! (den > 0.))
            den = 1.;
        for (size_t i = 0; i < n; ++ i) {
            const double v = raw[t][i];
            double       s = 0.;
            if (! std::isfinite(v))
                // A value that is not a number is not a measurement, and it gets the WORST value on
                // the term rather than the best. The clamp below cannot be trusted with it:
                // max(0., NaN) is 0 — the best — which is exactly the direction this file forbids.
                s = 1.;
            else if (indifferent)
                s = 0.;
            else
                s = (std::min)(1., (std::max)(0., (v - lo) / den));
            result[i] += term_weights[t] * s;
            if (out_normalized)
                (*out_normalized)[i][t] = s;
        }
    }

    return result;
}

} // namespace Nocte
} // namespace Slic3r
