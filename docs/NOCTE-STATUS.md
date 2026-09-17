# NØCTE Slicer — implementation status

Living document (the HLSD describes the design as it stands and carries no status markers, per `AGENTS.md`).

Base: OrcaSlicer upstream `main` @ `6b0e190e64` (tag `nocte-base-2026-09-16`). Updated: 2026-09-17.

## CI

- First full `Build all` run on the fork (workflow_dispatch, 2026-09-17): green on Windows x64, Linux x86_64/aarch64, macOS arm64/x86_64 and Flatpak. Windows deps built in 55 min, slicer in 1 h 24 min, unit tests 5 min. Artifact `nocte-slicer-windows-x64` produced. `NOCTE checks` green.
- Block 2 (PR #2, merged 2026-09-17, run 35241116400): Windows x64 build 33 min with the deps cache and a warm ccache, unit tests 6 min, 943/943 passed. The first attempt failed after 12 min on one `-Wvexing-parse` error: every Clang build here compiles with `/W4 -Werror` (root `CMakeLists.txt:549-598`), so a warning in new code stops the build. A pull-request build covers Windows x64 only; Linux, macOS and Flatpak build on the push to `main`.
- Known: the initial push to the empty repository did not fire `push` workflows; manual dispatch works and later pushes trigger normally. Any push to a pull-request branch restarts `Build all`, whatever files it touches, because the path filter is evaluated against the whole pull-request diff.

## Bambu Studio compatibility

- First native round trip (2026-09-17): CI-built binary exported an STL to 3mf with A1 profiles; Bambu Studio 02.08.02.61 CLI loaded it (`Success.`), sliced it and re-exported it with object/part/plate/instance intact.
- The 3mf now carries `<metadata name="NocteSlicer">` next to the unchanged `Application = BambuStudio-<SLIC3R_VERSION>`, plus `Metadata/nocte_report.json`; both are left out of a minimal published 3mf.
- Synthetic corpus `tools/bbl-compat/corpus/nocte-cli/01–04` (single cube, modifier with per-part overrides, negative part, support enforcer and blocker), written end to end by the NØCTE CLI from run 35241116400: 4/4 pass oracle criteria (a)(c)(d)(e) against Bambu Studio 02.08.02.61; (b)(f)(g) are skipped for lack of a plaintext log, paint data and an AMS list. Subtypes and per-part overrides survive both the NØCTE reader and writer and Bambu Studio's re-export.
- Identity gate measured (ADR-001): with `Application = NocteSlicer-…` or `OrcaSlicer-…` the Bambu Studio CLI discards the project settings and crashes with an access violation on `--export-3mf`; the gate is the `BambuStudio-` prefix.
- CLI: a Bambu Studio 02.08.02 project no longer exits `-24` (checked with the old and the new binary on the same file). The `-18` fix is covered by its unit test only; no corpus file carries the `-1` values yet.
- Open, inherited from upstream and reproduced by Bambu Studio itself: the per-part `matrix` metadata of a multi-part object is compounded again on every save (`tools/bbl-compat/NOTES.md` §7.4). Geometry is unaffected.

## Engine (`src/libslic3r/Nocte/`)

**Implemented and unit-tested**

- `MeshDiagnostics` — degenerate facets, duplicate vertices, open boundary loops, non-manifold edges and vertices, inconsistent orientation, disconnected shells, zero-volume shells, self-intersections (CGAL, with a facet budget) and inverted shells (signed volume plus nesting depth from a generalized winding number, so a cavity is not reported). Checks that are over budget or switched off are listed in `skipped_checks`.
- `NocteCgal` — the only NØCTE source that sees CGAL; compiled into `libslic3r_cgal`, no CGAL type in its header.
- `MeshRepair` — `RepairSession` with preview / accept / reject / undo. Operations that run: `RemoveDegenerateFaces`, `MergeVertices`, `OrientConsistently`, `RemoveTinyShells`, `StitchBorders`, `DuplicateNonManifoldVertices`, `FillHolesCgal`, `FlipShell`.
- `GeometryAnalysis` — volume, area, hull and solidity, centre of mass, overhang angle and area, footprint, stability and tall-thin ratios, wall thickness from inward rays, minimum cross-section.
- `AutoTune` with `Rules/rules_v1.cpp` (`nocte.rules/1`) — five rules, limited to keys that are legal per-object or per-part overrides; every recommendation must exist in `print_config_def` and deserialize; a value the user already set in that scope is never overwritten.
- `Report`, `ProjectReport`, `BblCompat`.

**Planned only**

- `NormalizeManifold` (tier 2, needs the Manifold dependency) and `VoxelRemesh` (tier 3).
- `TinyFeature` and `ThinWall` diagnostics; `max_bridge_span`, `smallest_feature_size` and `mean_abs_curvature` in the analysis.
- A listing of the intersecting facet pairs (today the check reports that the mesh self-intersects, not where).

## Branding

`SLIC3R_APP_NAME` is `NOCTE Slicer`; icons, splash and About logos are replaced in place in pure black and white; the About dialog names NØCTE and keeps every upstream credit. Not done: `SLIC3R_APP_KEY` and the data-dir migration, the exe file name, installer and packaging identifiers. The Windows exe has no version resource in either CI build, so the product name does not show in its file properties yet.

## LAN printing

Protocol validated in Python against a real A1 in Developer Mode (`tools/nocte/lan`, measured sequence in ADR-002). The C++ agent under `src/slic3r/Utils/Nocte/` and the GUI layer under `src/slic3r/GUI/Nocte/` do not exist yet.
