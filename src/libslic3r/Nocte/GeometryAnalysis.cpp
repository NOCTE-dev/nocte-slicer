// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// v0 of the geometry measurements. Everything here runs on a plain indexed_triangle_set in object
// coordinates (Z up, millimetres) and uses only facilities that libslic3r links unconditionally:
// no CGAL, no OpenVDB, no libigl beyond what AABBMesh already pulls in.

#include "libslic3r/Nocte/GeometryAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

// A vertex is "on the bed" when it sits this close to the minimum Z of the bounding box.
constexpr double BED_EPS_MM = 1e-4;

// Wall-thickness histogram range. Fixed rather than data-driven so that two parts produce
// comparable histograms; anything at or above the top lands in the last bin.
constexpr double THICKNESS_HIST_MAX_MM = 10.;

// Facets at or above this overhang angle are counted into steep_overhang_area.
constexpr double STEEP_OVERHANG_DEG = 70.;

// Upper bound on the number of slicing planes used for min_cross_section_area, so that a tall part
// does not turn the analysis into a full slicing job. The step is coarsened to stay under it.
constexpr int MAX_SLICES = 200;

// Fraction of the layer stack ignored at the top and at the bottom of the part. The extreme layers
// of a closed mesh are tangent to the surface and their area collapses to nearly nothing, which
// would swamp the real minimum cross section.
constexpr double CROSS_SECTION_TRIM = 0.02;

// Overhang angle convention
// ------------------------
// The overhang angle of a facet is the angle between the facet and the vertical:
//   0 deg  = a vertical wall (facet normal is horizontal),
//   90 deg = a horizontal ceiling (facet normal points straight down).
// For a unit outward normal n of a downward-facing facet (n.z() < 0) that is
//   angle = asin(-n.z()).
// A facet needs support when angle > AnalysisParams::overhang_threshold.
//
// Orca's `support_threshold_angle` (PrintConfig.cpp:7102) uses the complementary convention — it is
// a *slope* angle where 90 deg is a vertical wall, and support is generated for overhangs whose
// slope is *below* the threshold. The two are related by
//   support_threshold_angle = 90 - overhang_threshold
// so the NØCTE default of 45 deg maps onto an Orca threshold of 45 deg. Rules/rules_v1.cpp relies
// on this identity when it recommends a threshold.
double overhang_angle_deg(double nz)
{
    const double s = (std::min)(1., (std::max)(-1., -nz));
    return std::asin(s) * 180. / PI;
}

struct FacetSums
{
    double surface_area   = 0.;
    double overhang_area  = 0.;
    double steep_area     = 0.;
    double max_angle      = 0.;
    double footprint_area = 0.;
};

FacetSums accumulate_facets(const indexed_triangle_set &its, const AnalysisParams &params, double min_z)
{
    FacetSums sums;
    for (size_t i = 0; i < its.indices.size(); ++ i) {
        // its_unnormalized_normal() yields the outward normal scaled by twice the facet area, so a
        // single call gives both the area and the direction, and degenerate facets are recognised
        // by a zero length instead of producing a NaN normal.
        const Vec3d  un  = its_unnormalized_normal(its, i).cast<double>();
        const double len = un.norm();
        if (! (len > 0.))
            continue;
        const double area = 0.5 * len;
        const Vec3d  n    = un / len;
        sums.surface_area += area;

        if (! (n.z() < 0.))
            continue;

        const stl_triangle_vertex_indices &tri = its.indices[i];
        bool on_bed = true;
        for (int k = 0; k < 3; ++ k)
            if (double(its.vertices[tri(k)].z()) - min_z > BED_EPS_MM) {
                on_bed = false;
                break;
            }
        if (on_bed) {
            // First-layer contact: the facet projected onto the bed plane.
            sums.footprint_area += area * (-n.z());
            continue;
        }

        const double angle = overhang_angle_deg(n.z());
        sums.max_angle = (std::max)(sums.max_angle, angle);
        if (angle > params.overhang_threshold)
            sums.overhang_area += area;
        if (angle > STEEP_OVERHANG_DEG)
            sums.steep_area += area;
    }
    return sums;
}

// Volume-weighted centroid from the signed tetrahedra spanned by each facet and the origin.
// Returns false when the mesh encloses no measurable volume, in which case the caller falls back to
// the bounding-box centre.
bool compute_center_of_mass(const indexed_triangle_set &its, Vec3d &out)
{
    double vol6 = 0.;
    Vec3d  acc  = Vec3d::Zero();
    for (const stl_triangle_vertex_indices &tri : its.indices) {
        const Vec3d a = its.vertices[tri(0)].cast<double>();
        const Vec3d b = its.vertices[tri(1)].cast<double>();
        const Vec3d c = its.vertices[tri(2)].cast<double>();
        const double v = a.dot(b.cross(c)); // six times the signed tetrahedron volume
        vol6 += v;
        acc  += v * (a + b + c);
    }
    if (std::abs(vol6) < 1e-9)
        return false;
    // Each tetrahedron's centroid is (a + b + c + origin) / 4.
    out = acc / (4. * vol6);
    return true;
}

void measure_hull(const indexed_triangle_set &its, const Vec3d &bbox_size, PartFeatures &f)
{
    // qhull needs a genuinely three-dimensional point cloud; a flat or nearly flat input makes it
    // throw, and its_convex_hull() would then hand back an empty mesh anyway.
    if (its.vertices.size() < 4)
        return;
    if (bbox_size.x() <= EPSILON || bbox_size.y() <= EPSILON || bbox_size.z() <= EPSILON)
        return;
    try {
        const indexed_triangle_set hull = its_convex_hull(its);
        if (hull.indices.size() < 4)
            return;
        f.hull_volume = std::abs(double(its_volume(hull)));
        if (f.hull_volume > EPSILON)
            f.solidity = f.volume / f.hull_volume;
    } catch (...) {
        f.hull_volume = 0.;
        f.solidity    = 0.;
    }
}

void measure_thickness(const indexed_triangle_set &its, const AnalysisParams &params, PartFeatures &f)
{
    const int bins = (std::max)(1, params.thickness_bins);
    f.thickness_hist.assign(size_t(bins), 0.);

    const size_t n_faces = its.indices.size();
    if (n_faces == 0)
        return;

    const size_t cap    = size_t((std::max)(1, params.max_thickness_samples));
    // Deterministic stride sampling: no RNG, so two runs on the same mesh agree exactly.
    const size_t stride = (n_faces + cap - 1) / cap;

    std::vector<double> thickness;
    thickness.reserve((n_faces + stride - 1) / stride);

    try {
        const AABBMesh aabb(its);
        for (size_t i = 0; i < n_faces; i += stride) {
            const Vec3d  un  = its_unnormalized_normal(its, i).cast<double>();
            const double len = un.norm();
            if (! (len > 0.))
                continue;
            const Vec3d n = un / len;

            const stl_triangle_vertex_indices &tri = its.indices[i];
            const Vec3d centroid = (its.vertices[tri(0)].cast<double>() +
                                    its.vertices[tri(1)].cast<double>() +
                                    its.vertices[tri(2)].cast<double>()) / 3.;

            // Start just inside the solid and shoot along the inward normal; the first hit is the
            // facet on the far side of the wall.
            const Vec3d origin = centroid - BED_EPS_MM * n;
            const AABBMesh::hit_result hit = aabb.query_ray_hit(origin, -n);
            if (! hit.is_hit())
                continue;
            const double d = hit.distance();
            if (! std::isfinite(d) || d <= 0.)
                continue;
            thickness.push_back(d + BED_EPS_MM);
        }
    } catch (...) {
        thickness.clear();
    }

    if (thickness.empty())
        return;

    std::sort(thickness.begin(), thickness.end());
    f.min_wall_thickness = thickness.front();
    // The 5th percentile is the headline number: a handful of stray rays that slip through a seam
    // cannot drag it to zero the way the plain minimum can.
    const double last = double(thickness.size()) - 1.;
    const size_t idx  = size_t(std::floor(0.05 * last));
    f.p05_wall_thickness = thickness[(std::min)(idx, thickness.size() - 1)];

    const double bin_width = THICKNESS_HIST_MAX_MM / double(bins);
    for (double t : thickness) {
        size_t b = size_t(std::floor(t / bin_width));
        if (b >= size_t(bins))
            b = size_t(bins) - 1;
        f.thickness_hist[b] += 1.;
    }
}

void measure_cross_section(const indexed_triangle_set &its, const AnalysisParams &params, double min_z, double height, PartFeatures &f)
{
    if (height <= EPSILON)
        return;

    double step = (std::max)(params.slice_step, 1e-3);
    if (height / step > double(MAX_SLICES))
        step = height / double(MAX_SLICES);

    std::vector<float> zs;
    zs.reserve(size_t(height / step) + 2);
    for (double z = min_z + 0.5 * step; z < min_z + height; z += step)
        zs.push_back(float(z));
    if (zs.empty())
        return;

    try {
        const std::vector<ExPolygons> layers = slice_mesh_ex(its, zs, MeshSlicingParamsEx{});
        const size_t trim = size_t(double(layers.size()) * CROSS_SECTION_TRIM);
        double       best = 0.;
        for (size_t i = trim; i + trim < layers.size(); ++ i) {
            // ExPolygon::area() is in scaled coordinates squared; two unscale steps give mm².
            const double a = unscale<double>(unscale<double>(Slic3r::area(layers[i])));
            if (a <= 0.)
                continue;
            if (best == 0. || a < best)
                best = a;
        }
        f.min_cross_section_area = best;
    } catch (...) {
        f.min_cross_section_area = 0.;
    }
}

} // namespace

PartFeatures analyze(const indexed_triangle_set &its, const AnalysisParams &params, const DynamicPrintConfig & /* ctx */)
{
    PartFeatures f;
    if (its.vertices.empty() || its.indices.empty())
        return f;

    const BoundingBoxf3 bbox = bounding_box(its);
    f.bbox_size = bbox.size();

    f.volume      = std::abs(double(its_volume(its)));
    f.shell_count = int(its_number_of_patches(its));

    const FacetSums sums = accumulate_facets(its, params, bbox.min.z());
    f.surface_area   = sums.surface_area;
    f.footprint_area = sums.footprint_area;
    f.max_overhang_angle     = sums.max_angle;
    f.steep_overhang_area    = sums.steep_area;
    f.overhang_area_fraction = f.surface_area > EPSILON ? sums.overhang_area / f.surface_area : 0.;

    if (! compute_center_of_mass(its, f.center_of_mass))
        f.center_of_mass = bbox.center();

    measure_hull(its, f.bbox_size, f);

    // sqrt(footprint_area) is the side of the square with the same first-layer contact area, i.e. a
    // stand-in for the footprint radius. Divided by the height of the centre of mass above the bed
    // it is dimensionless: below 1 the part is taller than it is wide at its base and tips easily.
    const double com_height = f.center_of_mass.z() - bbox.min.z();
    if (f.footprint_area > 0.)
        f.stability_ratio = com_height > EPSILON ? std::sqrt(f.footprint_area) / com_height : 1e6;

    const double min_horizontal = (std::min)(f.bbox_size.x(), f.bbox_size.y());
    if (min_horizontal > EPSILON)
        f.tall_thin_ratio = f.bbox_size.z() / min_horizontal;

    measure_thickness(its, params, f);
    measure_cross_section(its, params, bbox.min.z(), f.bbox_size.z(), f);

    // TODO(M2): max_bridge_span, smallest_feature_size and mean_abs_curvature. Bridge spans need
    // the per-layer difference between consecutive slices (slice_mesh_slabs + Clipper2), the
    // smallest feature an erosion sweep over the same slices, and the curvature a per-vertex
    // cotangent Laplacian. None of the three is robust enough to ship in v0, so they stay at 0 and
    // the rule table does not read them.

    return f;
}

bool analysis_available()
{
    return true;
}

} // namespace Nocte
} // namespace Slic3r
