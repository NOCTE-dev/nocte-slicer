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

The GUI additions live in `src/slic3r/GUI/Nocte/` and hook into upstream through the interface in `NocteUi.hpp`, so the two sides can be written independently:

- **`DiagnoseDialog`** (`DPIDialog`) — runs `diagnose()` and `plan_from()` on the selected part (or the first model part of the selected object), lists the issues (kind, severity, occurrences, explanation) in a `wxDataViewListCtrl` and the plan in a `wxCheckListBox`; Preview / Apply selected / Undo last operate on one `RepairSession`. Apply writes the mesh back exactly once, inside one snapshot, with the sequence `ObjectList::smooth_mesh()` uses (`save_painting`, `set_mesh`, `restore_painting`, convex hull and bounding-box invalidation, `changed_mesh`); after that the panel is read-only and Edit → Undo is the way back.
- **`AutoTuneDialog`** — `analyze()` on the plated mesh (instance and volume transforms applied, since the analysis expects Z-up object coordinates) and `tune()` against the config of the selected scope (volume config for a part, object config for an object — that config is what `tune()` reads as the user-override record). Recommendations show current → proposed, reason and evidence; Apply writes the checked ones through `print_config_def` (`create_default_option()` + `deserialize`) with `ModelConfig::set_key_value`, inside one snapshot taken right before the first write, then `changed_object()` and the settings-item refresh.
- **`NocteMenu`** — `create_main_menu()` builds the NØCTE menu that `MainFrame` appends next to Help in both menu branches; `append_object_menu_items()` adds the two entries to the object and part context menus from one hook in `ObjectList::show_context_menu()`, idempotently, since those menus are long-lived singletons.
- **No gizmo.** 3D highlighting of repairs would need the gizmo registry (`GLGizmosManager`), which is not a touch point; it stays deferred.

## Offline policy

The application creates no cloud agent: `NetworkAgentFactory::create_agent_from_config()` returns a `NetworkAgent` without one and `create_cloud_agent()` returns null for every provider, so every login, sync and token path in `NetworkAgent` degrades to a null-guarded no-op. The Bambu plugin agent is not registered. The remaining online surface is switched off by defaults in `AppConfig` (empty version-check and profile-update URLs, `stealth_mode`, `hide_login_side_panel`, no cloud provider, `sync_system_preset` off, blank vendor `url` fields) and by marked early returns at the entry points a user could still reach (Help entries, Sync Presets, plugin prompts, login funnel, stealth escape, publish dialog, model-mall pages). The Device, Multi-device and device-calibration tabs are constructed but never added as pages, and `show_device()` returns before re-inserting them; the Calibration menu is slicer functionality and stays. `GUI/Nocte/OfflinePolicy.hpp` names the policy for code that wants to ask.

## Data directory migration

`GUI/Nocte/DataDirMigration` runs from the CLI entry point before `GUI_App` exists (the instance lock and the first `AppConfig` read happen in the `GUI_App` constructor). It skips under `--datadir` and in portable mode, computes the NØCTE and OrcaSlicer directories the same way wx does for the current platform, and, when the NØCTE one has no configuration and no declined marker while the OrcaSlicer one has a configuration, asks once through a native message box (Windows; other platforms start fresh and log). Import copies `user/**`, `shapes/` and `SVG/`, and rewrites the JSON configuration without the file-association, desktop-integration, network-plugin, cloud, sync, token and theme keys, so the fork's defaults apply. Runtime directories (`cache/`, `log/`, `plugins/`, `system/`, `ota/`, the machine id, `user_backup-*`) are not copied. The source is never opened for writing. A failure leaves `MIGRATION_INCOMPLETE.txt` and the declined marker, so the prompt does not return; the manual path is File → Import → Import Configs. `DataDirMigrationRules` holds the pure include/exclude and key rules, mirrored by `tools/nocte/migration_dry_run.py`.

## LAN Developer Mode agent

`src/slic3r/Utils/Nocte/` implements ADR-002 from the sequence measured in `tools/nocte/lan` (ADR-002 §Measured): `NocteMqttClient` is a minimal MQTT 3.1.1 client over `boost::asio::ssl` (TLS 1.2, no certificate verification because the printer's certificate names its serial, no SNI; CONNECT/CONNACK, one exact subscription, PUBLISH at QoS 0 only because the printer never acknowledges QoS 1, PINGREQ every 30 s, full remaining-length encoding, one `io_context` thread that survives handler exceptions and drains stale handlers on restart); `NocteFtps` is implicit FTPS on 990 through the vendored libcurl, with STOR success decided by re-listing because the server omits the final 226; `NocteLanPrinterAgent` implements `IPrinterAgent` over the two and forwards the printer's reports verbatim to the GUI through `queue_on_main_fn` (the GUI already parses Bambu's JSON). It is registered as `nocte-lan` and, until `GUI_App::resolve_printer_agent_id()` returns it for Bambu presets, also as `bbl`, sharing one instance. Printing is behind `allow_print`, which defaults to false and nothing sets; the headless probe (`NOCTE_LAN_PROBE=1` with the printer's IP, serial and access code in the environment) exercises everything except printing and writes a redacted checklist to `nocte_lan_probe.txt`.

**Exact per-object override write path** (persistence into the 3mf is then free, via `bbs_3mf.cpp`):

```
plater->take_snapshot("NØCTE auto-tune")
  → ModelObject::config.set_key_value(...)  /  ModelVolume::config.set_key_value(...)
  → ObjectList::changed_object(obj_idx)
  → ObjectList::update_and_show_object_settings_item()
```

## Branding

Anything brand-related — icons, splash, wordmark, About dialog, web assets, installer art — uses a **strictly black-and-white monochrome palette, no accent colours**; grey appears only as anti-aliasing. The mark is a ring enclosing an `N`; the wordmark is `NØCTE` in a condensed uniform-stroke geometric sans; the company is NØCTE Engineering and the tagline "Building the Impossible". AGPL credits for OrcaSlicer / BambuStudio / PrusaSlicer / Slic3r stay in `AboutDialog.cpp` (mandatory) — the fork's own identity is added around them, never in place of them.

### Application name

`SLIC3R_APP_NAME` is `"NOCTE Slicer"` (ASCII; `version.inc`), display string "NØCTE Slicer". CMake interpolates it into `libslic3r_version.h`, the Windows `.rc` `ProductName`/`FileDescription`/`InternalName` and the macOS `Info.plist`. At runtime it reaches window titles, the HTTP user agent, log lines and the G-code header `; generated by <name>`; the header is written and detected through the same macro (`Config.cpp`, `GCodeProcessor.cpp`), so the space in the name cannot desynchronise them. Nothing keys a file path, registry key, single-instance mutex, URL scheme or `.desktop` entry off it — those use the app key, which is split in two (ADR-003): `SLIC3R_RUNTIME_APP_KEY` (`NocteSlicer`) becomes the `SLIC3R_APP_KEY` macro and names the data directory, the configuration file and the translation catalog domain, so the catalogs are `NocteSlicer.pot` / `NocteSlicer_<lang>.po` / `NocteSlicer.mo` and the gettext scripts follow; the CMake variable `SLIC3R_APP_KEY` stays `OrcaSlicer` for the Linux AppImage script, the macOS `Info.plist` and the FHS share directory, whose renames are deferred with the rest of the Linux/macOS packaging identity. The Windows launcher is `nocte-slicer.exe`; it loads `OrcaSlicer.dll` by a name compiled into the launcher source, which is not a touch point, so the library, its export, the CMake project and target names and the unix binary keep their names.

### Assets

The app-identity images under `resources/images/` are replaced in place, keeping upstream's filenames, pixel sizes and formats, because the C++ (`AboutDialog`, `MainFrame`, `MsgDialog`, `ConfigWizard`, `TroubleshootDialog`, `SendSystemInfoDialog`, `DesktopIntegrationDialog`), CMake and the packaging scripts reference those names. `tools/nocte/branding/make_assets.py` regenerates all of them from the brand originals, which it fetches from the user's private repos into a gitignored cache; only derived assets are committed. Every constant in the generator is a measurement of the original artwork, and the icons are *re-drawn* at each target size rather than downsampled, because the mark's ring is 6.7 % of its diameter and vanishes below about 48 px otherwise. The SVGs are emitted from the same geometry model and use only `rect`, `circle`, `ellipse`, `line`, `polygon` and `path` — nanosvg, which renders them, has no `<text>` — in `#000000`/`#FFFFFF`, the only two colours `BitmapCache::load_svg` never rewrites for dark mode.

Light and dark variants exist wherever upstream had them (`*_about`, `*_about_dark`, `splash_logo`, `splash_logo_dark`, `*_horizontal_light`, `*_horizontal_dark`), so the mark is drawn in the colour opposite the surface it lands on. The splash screen and the Troubleshoot header are purely resource-driven and need no code change.

### About dialog

The dialog keeps the upstream credit paragraphs, the upstream copyright notice and the whole "License Info" library list unchanged, and adds ahead of them: the product name and "by NØCTE Engineering — Building the Impossible"; an explicit "NØCTE Slicer is based on OrcaSlicer and is released under the same licence, the GNU Affero General Public License, version 3"; and the trademark reservation "NØCTE and the NØCTE logo are trademarks of NØCTE Engineering; the AGPL licence does not grant trademark rights." Those lines are brand and legal text and are deliberately not translated. The dialog also carries its own display-name strings, because `SLIC3R_APP_FULL_NAME` lives in `libslic3r.h`, which is not an allowed touch point. Non-ASCII literals are written as explicit UTF-8 byte escapes (`"N\xC3\x98" "CTE"`), so they do not depend on the compiler's source charset and are not swallowed by `\x`'s greedy hex parsing.

### Palette

The monochrome palette is applied at the central points rather than at every call site: `StateColor::gDarkColors` (the light→dark map every `StateColor`-based widget resolves through), `ColorRGB/RGBA::ORCA()` (the named accent of the gizmos, canvas and ImGui overlays), the ImGui palette in `ImGuiWrapper`, the toolbar sprite tints in `GLTexture`, the 3D background in `GLCanvas3D`, the design tokens in `resources/web/include/global.css` and the accent the hosted web dialogs receive from `WebViewHostDialog`. The greys are named once in `GUI/Nocte/NocteTheme.hpp`. Where the colour is a mark (radio dot, progress bar, icon) the accent is white on dark and near-black on light; where it fills a button it is a grey surface, because upstream paints button text near-white. `BitmapCache::load_svg` rewrites the brand colours in both the `fill="…"` and the `style="…"` spelling at load time, which is what reaches the bulk of the icon set; the few icons loaded raw by `GLGizmosManager` are recoloured on disk.

### Deferred

The `resources/images/OrcaSlicer*` filenames, the MSIX/flatpak/AppImage/`.desktop` identifiers and the macOS bundle name, `build_win.bat`'s printed launcher name, the version resource of the Windows executable (absent in every CI build so far), the upstream logo variants nothing references (`OrcaSlicer_gradient.svg`, `OrcaSlicer_gradient_narrow.svg`, `OrcaSlicer_gray.svg`, `studio_logo.svg`), the remaining teal literals in the Bambu device pages, the light-mode home page, and installer artwork.

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
