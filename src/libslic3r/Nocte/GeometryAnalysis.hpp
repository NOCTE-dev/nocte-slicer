// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Geometry-derived part features, the input the auto-tuning rules match on. Declaration only in
// M0: the struct is fixed so the report and the rule table can be written against it, while the
// measurements themselves land in M1.

#ifndef slic3r_Nocte_GeometryAnalysis_hpp_
#define slic3r_Nocte_GeometryAnalysis_hpp_

#include <string>
#include <vector>

#include "libslic3r/Point.hpp"

struct indexed_triangle_set;

namespace Slic3r {

class DynamicPrintConfig;

namespace Nocte {

struct PartFeatures
{
    double              volume                  = 0.;
    double              surface_area            = 0.;
    double              hull_volume             = 0.;
    // volume / hull_volume. 1 for a convex part, small for a lattice.
    double              solidity                = 0.;
    Vec3d               bbox_size               = Vec3d::Zero();
    Vec3d               center_of_mass          = Vec3d::Zero();

    double              footprint_area          = 0.;
    // footprint radius over centre-of-mass height; below 1 the part is tipping-prone.
    double              stability_ratio         = 0.;
    // max(bbox height / min horizontal extent).
    double              tall_thin_ratio         = 0.;

    double              max_overhang_angle      = 0.;
    double              overhang_area_fraction  = 0.;
    double              steep_overhang_area     = 0.;

    // Wall-thickness histogram, uniform bins over [0, max thickness].
    std::vector<double> thickness_hist;
    double              min_wall_thickness      = 0.;
    double              p05_wall_thickness      = 0.;

    double              max_bridge_span         = 0.;
    double              min_cross_section_area  = 0.;
    double              smallest_feature_size   = 0.;
    double              mean_abs_curvature      = 0.;
    int                 shell_count             = 0;
};

struct AnalysisParams
{
    // Slice spacing for the bridge-span and cross-section measurements (mm).
    double slice_step          = 0.2;
    // Facets steeper than this count towards the overhang measures (degrees from horizontal).
    double overhang_threshold  = 45.;
    // Number of bins in thickness_hist.
    int    thickness_bins      = 32;
    // Cap on the sample count of the wall-thickness probe, to bound the cost on large meshes.
    int    max_thickness_samples = 20000;
};

// TODO(M1): implement. Overhangs and curvature come from the facet normals, thickness from an SDF
// probe (OpenVDBUtils), hull_volume from TriangleMesh::convex_hull_3d, bridge spans and minimum
// cross section from slice_mesh() + Clipper2. Returns a zero-initialised PartFeatures for now, so
// callers and the report can already be written and tested against the final shape of the struct.
PartFeatures analyze(const indexed_triangle_set &its, const AnalysisParams &params, const DynamicPrintConfig &ctx);

// True once analyze() actually measures something. Lets the GUI and the report say "not analysed"
// instead of presenting zeros as findings.
bool analysis_available();

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_GeometryAnalysis_hpp_
