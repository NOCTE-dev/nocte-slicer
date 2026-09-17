# HLSD — NØCTE subsystem overview

- **Date:** 2026-09-16
- **Status:** Living document
- **Related:** ADR-001 (fork + native 3mf), ADR-002 (networking under AGPL)

## Purpose

The NØCTE subsystem adds three first-class capabilities to the OrcaSlicer fork without disturbing upstream code: explainable mesh repair, geometry-derived per-part auto-tuning, and a NØCTE-owned LAN print path. The engine is wx-free and unit-testable; the GUI is a thin presentation layer. Native Bambu Studio 3mf output is a hard invariant of every feature (ADR-001).

## Directory layout

```
src/libslic3r/Nocte/          engine, no wx, testable
  NocteVersion.hpp
  MeshDiagnostics.{hpp,cpp}
  RepairOps.{hpp,cpp}  MeshRepair.{hpp,cpp}
  GeometryAnalysis.{hpp,cpp}
  AutoTune.{hpp,cpp}  Rules/RuleSet.{hpp,cpp}  Rules/rules_v1.cpp
  Report.{hpp,cpp}
  NocteCgal.cpp               compiled into the libslic3r_cgal target (GPL isolation, as upstream does)
src/slic3r/GUI/Nocte/         NocteRepairDialog, GLGizmoNocteRepair, NocteAutoTuneDialog, NocteBranding
src/slic3r/Utils/Nocte/       NocteLanPrinterAgent (FTPS + MQTT, Developer Mode) implementing IPrinterAgent
tests/libslic3r/test_nocte_*.cpp   Catch2
tests/data/nocte/             broken-mesh fixtures
tools/bbl-compat/             golden corpus + oracle.py + structdiff.py + roundtrip.py + report.html
tools/nocte/check_touchpoints.py   CI guard for the upstream touch-point allowlist
```

## Module designs

### MeshDiagnostics

```cpp
enum class IssueKind { DegenerateFacet, DuplicateVertex, NonManifoldEdge, NonManifoldVertex,
                       OpenBoundaryLoop, InvertedNormal, InvertedShell, SelfIntersection,
                       DisconnectedShell, ZeroVolumeShell, TinyFeature, ThinWall };
enum class Severity { Info, Warning, Blocking };
struct MeshIssue { IssueKind kind; Severity severity; size_t count; double metric;
                   std::vector<int> face_ids; std::vector<std::pair<int,int>> edges;
                   BoundingBoxf3 bbox; std::string explanation; std::string issue_id; };
struct DiagnosticsResult { TriangleMeshStats stats; std::vector<MeshIssue> issues;
                           int open_edges, shells; double volume; bool manifold; std::string summary; };
DiagnosticsResult diagnose(const indexed_triangle_set&, const DiagnosticsParams&);
```

Every issue carries a stable `issue_id`, so repair steps can name what they target and the report can show before/after.

### MeshRepair (explainable, step by step)

```cpp
enum class RepairOpKind { MergeVertices, RemoveDegenerateFaces, DuplicateNonManifoldVertices,
                          StitchBorders, OrientConsistently, FlipShell, RemoveTinyShells,
                          FillHolesCgal, NormalizeManifold, VoxelRemesh };
struct RepairOp { RepairOpKind kind; int tier; std::string title, rationale;
                  std::vector<std::string> targets_issue_ids; RepairOpParams params; bool lossy; };
struct MeshMetrics { int tris, open_edges, shells, self_intersections;
                     double volume, surface_area, hausdorff_est; };
struct RepairStepResult { bool succeeded; MeshMetrics before, after; std::vector<int> touched_faces;
                          std::string message; std::chrono::milliseconds duration; };
class RepairSession {          // snapshot stack, cap 8 [VERIFY memory on 10M-triangle meshes]
  const RepairPlan& plan() const;
  RepairStepResult preview(size_t i); RepairStepResult accept(size_t i); void reject(size_t i); void undo();
  const indexed_triangle_set& current() const; DiagnosticsResult diagnostics() const; NocteReport report() const;
};
```

Tiered fallbacks, with escalation visible to the user:

- **Tier 1 (topological):** `its_merge_vertices`, `its_remove_degenerate_faces`, `its_flip_triangles`, CGAL `stitch_borders`, `duplicate_non_manifold_vertices`, `triangulate_refine_and_fair_hole`.
- **Tier 2:** `MeshBoolean::cgal::repair(TriangleMesh&, RepairedMeshErrors*, std::string*)` (already returns a structured summary), then **Manifold v3.5.3** (Apache-2.0, new dependency in M2) for manifold normalisation by self-union.
- **Tier 3:** `OpenVDBUtils::mesh_to_grid` → `redistance_grid` → `grid_to_mesh`, voxel = min(bbox)/512 [VERIFY]; always `lossy = true` with a measured volume delta.

A cancellation token plus `tbb::parallel_for` over faces keeps large meshes responsive; AABB/KD indices are built once and shared.

### GeometryAnalysis

```cpp
struct PartFeatures { double volume, surface_area, hull_volume, solidity; Vec3d bbox_size, center_of_mass;
  double footprint_area, stability_ratio, tall_thin_ratio;
  double max_overhang_angle, overhang_area_fraction, steep_overhang_area;
  std::vector<double> thickness_hist; double min_wall_thickness, p05_wall_thickness;
  double max_bridge_span, min_cross_section_area, smallest_feature_size, mean_abs_curvature; int shell_count; };
PartFeatures analyze(const indexed_triangle_set&, const AnalysisParams&, const DynamicPrintConfig& ctx);
```

### AutoTune

```cpp
struct Recommendation { std::string rule_id, key, value, previous_value, reason;
                        std::vector<std::string> evidence;
                        ConfigScope scope /*Object|Volume|LayerRange*/; int volume_idx; double confidence; };
TuneResult tune(const PartFeatures&, const DynamicPrintConfig& base, const TuneProfile&);
```

`Rules/rules_v1.cpp` is a declarative table `Rule{id, doc, priority, match(features, cfg), emit(sink)}`. Verified Orca config keys for v1: `wall_loops`, `sparse_infill_density`, `sparse_infill_pattern`, `top_shell_layers`/`bottom_shell_layers`, `layer_height` (via `layer_config_ranges`), `support_type`, `support_threshold_angle`, `support_on_build_plate_only`, `bridge_speed`, `bridge_flow`, `slow_down_layer_time`/`slow_down_for_layer_cooling`/`slow_down_min_speed`, `enable_overhang_speed`, `tree_support_branch_angle`, `support_interface_top_layers`, `seam_position`, `brim_type`/`brim_width`, `elefant_foot_compensation`, `xy_hole_compensation`, `detect_thin_wall`.

**Invariant:** never silently overwrite a key the user already set in that scope; such a recommendation is emitted with low confidence and the note "user override present". All recommendations are opt-in and reversible.

### Report

`NocteReport{schema="nocte.report/1", nocte_version, diagnostics, steps, features, recs}` with `to_json()` (nlohmann, already in the tree) and `to_markdown()`. Persisted additively as `Metadata/nocte_report.json` inside the 3mf; Bambu ignores unknown `Metadata` entries [VERIFY with the oracle in M0].

## Reuse map (existing libslic3r facilities)

| Need | Reused facility |
|---|---|
| Topology counts, stats, repair bookkeeping | `src/libslic3r/TriangleMesh.hpp`: `its_num_open_edges`, `its_number_of_patches`, `its_split`, `its_volume`, `its_face_neighbors`, `VertexFaceIndex`, `TriangleMeshStats::manifold()`, `RepairedMeshErrors` |
| Manifoldness, boundaries, orientation, inverted shell, curvature, distance | libigl (`src/libigl`): `is_edge_manifold`, `is_vertex_manifold`, `boundary_loop`, `bfs_orient`, `connected_components`, `fast_winding_number`, `principal_curvature`, `signed_distance` |
| Self-intersections, hole filling with fairing, stitching, remesh | CGAL PMP via `MeshBoolean::cgal::` (incl. `MeshBoolean::cgal::repair`), called only from `NocteCgal.cpp` inside `libslic3r_cgal` |
| Voxel remesh fallback, SDF wall thickness | `OpenVDBUtils`: `mesh_to_grid`, `redistance_grid`, `grid_to_mesh` |
| Bridge spans, minimum cross-section | `slice_mesh()` + Clipper2 |
| Convex hull, solidity | `TriangleMesh::convex_hull_3d` (qhull) |
| Stability / tipping (M3) | Revive `Support/SupportSpotsGenerator` (PrusaSlicer 2.6, commented out in Orca main) |
| Adaptive layer height | `SlicingAdaptive` + `layer_config_ranges` |
| Parallelism, linear algebra | TBB, Eigen |

## GUI integration points

- **`NocteRepairDialog`** — `wxDataViewCtrl` over the `RepairPlan`: columns [✓ | step | reason | open edges before→after | Δ volume | lossy]; buttons Preview / Accept / Reject / Undo / Apply All.
- **`GLGizmoNocteRepair`** — modelled on `GLGizmoSimplify` (preview-mesh swap) plus `GLGizmoFdmSupports` (per-triangle colouring): filled holes, flipped faces and self-intersections each get a distinct diagnostic colour. Functional status colours, not brand colours.
- **`NocteAutoTuneDialog`** — list of `Recommendation` with checkbox, key, old→new, reason, evidence tooltip.
- **Entry points** — object context menu "NØCTE: Diagnose & Repair…" and "NØCTE: Auto-tune this part…", plus a top-level NØCTE menu (one hook in `GUI_ObjectList.cpp`, per the ADR-001 allowlist).

**Exact per-object override write path** (persistence into the 3mf is then free, via `bbs_3mf.cpp`):

```
plater->take_snapshot("NØCTE auto-tune")
  → ModelObject::config.set_key_value(...)  /  ModelVolume::config.set_key_value(...)
  → ObjectList::changed_object(obj_idx)
  → ObjectList::update_and_show_object_settings_item()
```

## Branding constraint

Anything brand-related — icons, splash, wordmark, About dialog, web assets, installer art — uses a **strictly black-and-white monochrome palette, no accent colours**. `SLIC3R_APP_NAME` is "NOCTE Slicer" (ASCII), display string "NØCTE Slicer"; `SLIC3R_APP_KEY` becomes `"NocteSlicer"` only at the end of M0, because it moves `%APPDATA%\<key>` and needs a first-run migration from `%APPDATA%\OrcaSlicer`. AGPL credits for OrcaSlicer / PrusaSlicer / Slic3r stay in `AboutDialog.cpp` (mandatory).

## Build and CI

The development machine has **no admin rights**: Visual Studio and Strawberry Perl cannot be installed (winget 1602); only CMake 4.4.3 and Ninja are available in user scope. So development and validation happen locally while **builds run in GitHub Actions** (Orca's `.github/workflows/build_all.yml` adapted to publish the portable Windows zip as an artifact), or on a machine with Visual Studio via `build_win.bat -d` / `-s`. `deps\build\OrcaSlicer_dep` is cached in CI. The tree lives at `C:\dev\nocte-slicer` (paths with spaces break Orca's deps/perl), reached by a junction from `Documents\NOCTE SLICER\nocte-slicer`, with `core.longpaths = true`.

## Milestones and verification

| Milestone | Scope | Verification |
|---|---|---|
| **M0 Foundation** (1 wk) | Toolchain, fork at upstream `main` (tag `nocte-base-2026-09-16`), first build, branding pass 1, `Nocte/` skeleton + CMake wiring, corpus 01–04 plus FDM-HUB 3mf, `oracle.py` v0, identity gate, ADR-001/002 | Branded exe starts; `ctest -R nocte` green; `oracle.py` PASS on an unmodified corpus file; identity gate logged in ADR-001 |
| **M1 Walking skeleton** (4 wks) | 8 diagnostic types + Tier 1 repairs + console report; GeometryAnalysis v0 (overhangs, thickness, hull); 5 rules; `NocteSlicer` tag; 8-file corpus; both dialogs, no 3D highlighting; `NocteLanPrinterAgent` v0 | Headless test loads `tests/data/nocte/broken_*.stl` and asserts issue kinds; NØCTE 3mf passes oracle (a)–(d); LAN send to a real Developer-Mode printer starts the print |
| **M2 MVP** (6 wks) | Per-step accept/reject + undo + highlight gizmo; Tier 2/3 fallbacks (Manifold dep); 15 rules with per-volume scope; `nocte_report.json`; 12-file corpus incl. MMU/AMS/multi-plate/multi-nozzle; LAN agent with live state and AMS mapping; Windows CI; installer | Scripted end-to-end: broken model → accept 3 of 5 steps → auto-tune → export → oracle (a)–(g) → **open in Bambu Studio and confirm the overrides in the object panel**, modifiers/painting/plates intact; `structdiff.py` clean on 12 files; LAN print with live status; full `ctest` green |
| **M3 Hardening** (8 wks) | TBB-parallel diagnostics; rules v2 (profile-aware); adaptive layer height; per-plate auto-tune; HTML report; upstream sync #2; signed installer; LAN fleet | 10M-triangle mesh under 30 s; sync branch merges cleanly with suite and oracle green |
| **M4 Full product** (10 wks) | Multi-vendor (Prusa/Klipper); 25+ rules validated on ≥20 physical prints; local accept/reject learning, no telemetry; docs + localisation; public AGPL repo | 20 before/after prints recorded in `docs/validation/`; second-vendor gcode validated in a simulator; clean build from the public repo using only published instructions; two clean upstream syncs; 8-hour session without a crash |
