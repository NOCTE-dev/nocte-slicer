// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Everything about a part that does NOT change when the part is rotated, measured once.
//
// The orientation engine scores tens of candidate rotations. Orca's Orient.cpp copies the whole
// mesh twice per candidate (Orient.cpp:329,348) and recomputes the volume, the hull and the surface
// area every time, none of which a rotation can change. Here those are measured once and a
// candidate costs one pass over the facet arrays with two dot products per facet.
//
// Frame: `its` is in object coordinates, Z up, millimetres. Facet arrays are parallel to
// `its.indices`, so facet i of the mesh is entry i here.

#ifndef slic3r_Nocte_Plan_PartInvariants_hpp_
#define slic3r_Nocte_Plan_PartInvariants_hpp_

#include <cstdint>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace Nocte {

struct PrecomputeParams
{
    // Cast the self-occlusion ray for the visibility flags. Off gives a zeroed `facet_visible` and
    // `visibility_available == false`, which the scores read as "treat every facet as visible".
    bool   measure_visibility     = true;
    // A facet counts as visible when a ray from its centroid along its own normal escapes the mesh.
    // The ray starts this far off the surface so it cannot re-hit the facet it left.
    double visibility_ray_eps_mm  = 1e-3;
    // Above this facet count the visibility pass is skipped rather than run: it is O(F log F) and a
    // scanned mesh can carry millions of facets. Skipping sets `visibility_available = false`.
    size_t visibility_facet_budget = 400000;
};

struct PartInvariants
{
    // Parallel to its.indices. A degenerate facet has area 0 and a zero normal; every consumer must
    // skip it by testing the area, never by testing the normal.
    std::vector<float> facet_area;    // mm^2
    std::vector<Vec3f> facet_normal;  // unit outward normal, object frame

    // 1 when the facet is on the outside of the part and not occluded by the part itself.
    // Empty when `visibility_available` is false.
    std::vector<uint8_t> facet_visible;
    bool                 visibility_available = false;

    double        surface_area   = 0.;   // mm^2
    double        volume         = 0.;   // mm^3, signed volume of the closed mesh, clamped at 0
    Vec3d         center_of_mass = Vec3d::Zero();  // object frame; bbox centre for a zero-volume mesh
    BoundingBoxf3 bbox;                  // object frame

    // Convex hull of the part. Its facet normals are the second source of candidate orientations
    // (the first is lay_on_face_planes), and its volume gives the solidity.
    indexed_triangle_set hull;
    double               hull_volume = 0.;  // mm^3

    // The input with its degenerate facets removed and its vertices remapped, which is what every
    // slicing call must be handed: slice_mesh_slabs asserts on a triangle with two coincident
    // vertices, and a mesh carrying exactly that is the case this engine exists to handle.
    //
    // It lives here because it does not depend on the candidate. Building it inside the per-
    // candidate scoring loop would copy the whole mesh once per orientation — which is the first
    // thing this file's header criticises Orca's Orient.cpp for doing.
    //
    // Always populated when `valid` is true, even if nothing had to be removed — a consumer must
    // never have to decide between this and the original, because that is a decision it can get
    // wrong silently.
    indexed_triangle_set solid;

    // True once the part carried enough geometry to measure. A degenerate or empty mesh yields a
    // zeroed struct with `valid == false` rather than an exception, matching Nocte::analyze().
    bool valid = false;

    size_t facet_count() const { return facet_area.size(); }
};

// Measures `its` once. Never throws on a degenerate mesh: it returns `valid == false`.
PartInvariants precompute(const indexed_triangle_set &its, const PrecomputeParams &params);

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_Plan_PartInvariants_hpp_
