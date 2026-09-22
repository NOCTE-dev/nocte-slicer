// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The one-off measurement pass behind PartInvariants.hpp. Every quantity produced here is invariant
// under rotation, so the orientation engine pays for it once per part instead of once per candidate;
// what a candidate then costs is a walk over the flat facet arrays this file fills in.
//
// Frame: `its` arrives in object coordinates (Z up, millimetres) and nothing here rotates, translates
// or copies it, so the facet arrays come out parallel to `its.indices`. As in GeometryAnalysis.cpp
// the file deliberately leans on nothing that libslic3r links conditionally: no CGAL, no OpenVDB, no
// libigl beyond what AABBMesh already pulls in. It also never throws on user geometry — a mesh it
// cannot measure comes back as a zeroed struct with `valid == false`, matching Nocte::analyze().

#include "libslic3r/Nocte/Plan/PartInvariants.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

// The signed tetrahedra of an open shell or a flat sheet cancel to rounding noise rather than to an
// exact zero, so the enclosed volume is only believable above a floor. The sum below is six times the
// volume, and this floor corresponds to a sixth of a billionth of a cubic millimetre — some fourteen
// orders of magnitude under the volume of a single extruded line, and therefore never a real part.
constexpr double MIN_SIGNED_VOLUME6 = 1e-9;

// A tetrahedron is the smallest solid there is: four vertices in and four facets out. Below either
// figure the input is a point, a segment or a sheet, qhull would throw on it, and the part has no
// hull whose facet normals could seed a candidate orientation.
constexpr size_t MIN_HULL_VERTICES = 4;
constexpr size_t MIN_HULL_FACETS   = 4;

// Facets handed to one TBB worker in the visibility pass. A single ray is a descent of an AABB tree
// costing a few microseconds, which is the same order as the cost of scheduling a task, so one facet
// per task would spend as long in the scheduler as in the geometry. A couple of hundred facets buys
// that back while still cutting a part of a few thousand facets into enough tasks to fill every core.
constexpr size_t VISIBILITY_GRAIN_SIZE = 256;

// Every triangle index in range. Nothing downstream checks: its_unnormalized_normal() indexes
// `its.vertices` through the triangle straight away, and measure_volume() below does the same, so a
// corrupt index is an out-of-bounds READ rather than the `valid == false` the header promises for a
// mesh we cannot measure. One pass here, before anything dereferences an index, buys that promise
// for every reader of the arrays this file fills.
bool indices_in_range(const indexed_triangle_set &its)
{
    const size_t n_vertices = its.vertices.size();
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int k = 0; k < 3; ++ k) {
            const int v = tri(k);
            if (v < 0 || static_cast<size_t>(v) >= n_vertices)
                return false;
        }
    return true;
}

// Builds `inv.solid`: the input with its zero-area facets dropped and its vertices remapped, so
// that facets which shared a vertex in the source still share one here. The sharing is the point —
// the slab slicer chains loops through face neighbours and a mesh of unwelded triangles would not
// chain — and the filtering is the other point, because slice_mesh_slabs asserts at
// TriangleMeshSlicer.cpp:2232 that no triangle has two coincident vertices.
//
// It is built HERE, once per part, because the filter reads nothing that a rotation can change.
// Doing it inside the candidate loop would copy every vertex and every index once per orientation,
// which is the first thing this subsystem's header criticises Orca's Orient.cpp for.
//
// Note: Scores.cpp carries a predicate-driven `submesh_where()` with the same remap. It cannot be
// shared without an internal header for the subsystem, because the sub-meshes it builds are keyed
// on the ORIGINAL facet index — a facet's entry in facet_normal and facet_visible — which the
// filtering here renumbers. Any change to one of the two belongs in both.
void build_solid(const indexed_triangle_set &its, PartInvariants &inv)
{
    const size_t face_count = (std::min)(its.indices.size(), inv.facet_area.size());
    std::vector<int> remap(its.vertices.size(), -1);
    for (size_t i = 0; i < face_count; ++ i) {
        if (! (inv.facet_area[i] > 0.f))
            continue;
        const stl_triangle_vertex_indices &tri = its.indices[i];
        stl_triangle_vertex_indices        dst;
        for (int k = 0; k < 3; ++ k) {
            const size_t src  = static_cast<size_t>(tri(k));  // range already checked by the caller
            int         &slot = remap[src];
            if (slot < 0) {
                slot = static_cast<int>(inv.solid.vertices.size());
                inv.solid.vertices.emplace_back(its.vertices[src]);
            }
            dst(k) = slot;
        }
        inv.solid.indices.emplace_back(dst);
    }
}

// Fills facet_area, facet_normal and surface_area, and returns how many facets carried real area.
size_t accumulate_facets(const indexed_triangle_set &its, PartInvariants &inv)
{
    const size_t face_count = its.indices.size();
    inv.facet_area.assign(face_count, 0.f);
    inv.facet_normal.assign(face_count, Vec3f(0.f, 0.f, 0.f));

    size_t measured = 0;
    for (size_t i = 0; i < face_count; ++ i) {
        // its_unnormalized_normal() yields the outward normal scaled by twice the facet area, so a
        // single call gives both the area and the direction, and degenerate facets are recognised by
        // a zero length instead of producing a NaN normal. Such a facet keeps the area 0 and the
        // zero normal it was initialised with, which is the contract every consumer tests for.
        const Vec3d  un  = its_unnormalized_normal(its, i).cast<double>();
        const double len = un.norm();
        if (! (len > 0.))
            continue;
        const double area = 0.5 * len;
        inv.facet_area[i]   = float(area);
        inv.facet_normal[i] = (un / len).cast<float>();
        inv.surface_area   += area;
        ++ measured;
    }
    return measured;
}

// Volume-weighted centroid and enclosed volume from the signed tetrahedra spanned by each facet and
// the origin, the same method as GeometryAnalysis.cpp::compute_center_of_mass. Returns false when the
// mesh encloses no measurable volume, in which case the caller falls back to the bounding-box centre.
bool measure_volume(const indexed_triangle_set &its, double &out_volume, Vec3d &out_center)
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
    // The finiteness test has to come FIRST and cannot be folded into the magnitude test. A single
    // NaN vertex makes vol6 NaN, and `std::abs(NaN) < MIN_SIGNED_VOLUME6` is false — every
    // comparison against a NaN is — so the magnitude test alone would report success and hand back
    // a NaN centroid. That centroid reaches the stability term, where `NaN > COM_HEIGHT_MIN_MM` is
    // also false, and the part is scored as untippable: the BEST possible value on that term, on a
    // mesh we could not measure at all. accumulate_facets() above is already NaN-safe through its
    // `!(len > 0.)` test; this pass was the one that was not.
    if (! std::isfinite(vol6) || std::abs(vol6) < MIN_SIGNED_VOLUME6)
        return false;
    // Each tetrahedron's centroid is (a + b + c + origin) / 4. The sign of vol6 cancels between the
    // numerator and the denominator, so an inside-out mesh still reports the right centroid.
    const Vec3d center = acc / (4. * vol6);
    if (! center.allFinite())
        // A finite vol6 with a non-finite moment sum: the individual tetrahedra overflowed even
        // though their signed volumes cancelled. There is no centroid to defend here either.
        return false;
    out_center = center;
    // The volume does not survive that inversion: a mesh whose facets all wind the wrong way gives a
    // negative sum. Rather than take the magnitude — which would quietly bless a broken mesh — the
    // value is clamped at 0, the reading the header promises and the one the scores treat as "no
    // usable solid". The centroid is kept because it is still correct.
    out_volume = (std::max)(0., vol6 / 6.);
    return true;
}

// Convex hull of the part and its volume. A hull that cannot be built is not a failure: the caller
// carries on with an empty hull and a zero volume.
void measure_hull(const indexed_triangle_set &its, PartInvariants &inv)
{
    if (its.vertices.size() < MIN_HULL_VERTICES)
        return;
    // qhull needs a genuinely three-dimensional point cloud. A cloud that is flat in any one axis
    // makes it throw, and its_convex_hull() would hand back an empty mesh anyway.
    const Vec3d extent = inv.bbox.size();
    if (extent.x() <= EPSILON || extent.y() <= EPSILON || extent.z() <= EPSILON)
        return;
    try {
        indexed_triangle_set hull = its_convex_hull(its);
        if (hull.indices.size() < MIN_HULL_FACETS)
            return;
        // The hull comes out of qhull with a consistent winding, but not a guaranteed one, so the
        // magnitude is taken here; unlike the part itself an inverted hull tells us nothing.
        inv.hull_volume = std::abs(double(its_volume(hull)));
        inv.hull        = std::move(hull);
    } catch (...) {
        inv.hull.clear();
        inv.hull_volume = 0.;
    }
}

// Self-occlusion flags. A facet is visible when a ray leaving it along its own outward normal escapes
// the part, so that nothing of the part itself stands between that facet and the outside world.
void measure_visibility(const indexed_triangle_set &its, const PrecomputeParams &params, PartInvariants &inv)
{
    const size_t face_count = its.indices.size();
    if (! params.measure_visibility || face_count > params.visibility_facet_budget)
        return;

    try {
        const AABBMesh aabb(its);
        inv.facet_visible.assign(face_count, uint8_t(0));
        tbb::parallel_for(tbb::blocked_range<size_t>(0, face_count, VISIBILITY_GRAIN_SIZE),
            [&its, &params, &aabb, &inv](const tbb::blocked_range<size_t> &range) {
                // Each iteration writes its own index of facet_visible and nothing else, so the loop
                // needs no synchronisation.
                for (size_t i = range.begin(); i < range.end(); ++ i) {
                    // A degenerate facet has no direction to shoot along and stays at 0.
                    if (! (inv.facet_area[i] > 0.f))
                        continue;
                    const Vec3d n = inv.facet_normal[i].cast<double>();

                    const stl_triangle_vertex_indices &tri = its.indices[i];
                    const Vec3d centroid = (its.vertices[tri(0)].cast<double>() +
                                            its.vertices[tri(1)].cast<double>() +
                                            its.vertices[tri(2)].cast<double>()) / 3.;

                    // Lifting the origin clear of the surface stops the ray from re-hitting the facet
                    // it has just left, which would otherwise read as an occlusion on every facet of
                    // a watertight mesh.
                    const Vec3d origin = centroid + params.visibility_ray_eps_mm * n;
                    if (! aabb.query_ray_hit(origin, n).is_hit())
                        inv.facet_visible[i] = uint8_t(1);
                }
            });
        inv.visibility_available = true;
    } catch (...) {
        // An AABB tree we could not build, or a ray cast that threw, leaves no partial answer worth
        // keeping: an empty facet_visible is read as "treat every facet as visible".
        inv.facet_visible.clear();
        inv.visibility_available = false;
    }
}

} // namespace

PartInvariants precompute(const indexed_triangle_set &its, const PrecomputeParams &params)
{
    PartInvariants inv;
    if (its.vertices.empty() || its.indices.empty())
        return inv;
    if (! indices_in_range(its))
        // A triangle pointing outside the vertex array. Everything below would read out of bounds,
        // so the mesh is refused the same way an empty one is: a zeroed struct with valid == false.
        return inv;

    const size_t measured_facets = accumulate_facets(its, inv);
    if (measured_facets == 0 || ! (inv.surface_area > 0.))
        // Nothing here carries area: a cloud of collapsed triangles, not a part. The header promises
        // a zeroed struct rather than a half-filled one, so the facet arrays go back too.
        return PartInvariants{};

    inv.bbox = bounding_box(its);

    if (! measure_volume(its, inv.volume, inv.center_of_mass)) {
        // An open shell has a surface but no interior. Its bounding-box centre is the only centre we
        // can defend, and it is what the stability term expects to be handed.
        inv.volume         = 0.;
        inv.center_of_mass = inv.bbox.center();
    }

    measure_hull(its, inv);
    measure_visibility(its, params, inv);
    build_solid(its, inv);

    // The finiteness gate sits below every pass, so that it covers every number a consumer reads
    // rather than only the three that happened to be computed above it. `bbox` in particular is not
    // optional: it is what the scores drop the part to the bed with, and a NaN vertex referenced by
    // no triangle at all still poisons it while leaving the volume perfectly finite. `hull_volume`
    // is here for the same reason — it is produced by qhull, after the earlier passes.
    //
    // A struct that reaches the scores carrying a NaN is worse than no struct. It does not fail
    // loudly: every comparison against a NaN is false, so it simply lands on whichever branch the
    // code falls through to, and those branches are chosen to be the safe ones for a MEASURED part.
    //
    // `solid` is checked too, because the header promises it is populated whenever `valid` is true.
    // A consumer that had to decide between `solid` and the original mesh is a consumer that can
    // get the decision wrong silently.
    if (! inv.bbox.min.allFinite() || ! inv.bbox.max.allFinite() ||
        ! inv.center_of_mass.allFinite() ||
        ! std::isfinite(inv.volume) || ! std::isfinite(inv.surface_area) ||
        ! std::isfinite(inv.hull_volume) ||
        inv.solid.indices.empty())
        return PartInvariants{};

    inv.valid = true;
    return inv;
}

} // namespace Nocte
} // namespace Slic3r
