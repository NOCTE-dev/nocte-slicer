# NØCTE Slicer — implementation status

Living document (the HLSD describes the design as it stands and carries no status markers, per `AGENTS.md`).

Base: OrcaSlicer upstream `main` @ `6b0e190e64` (tag `nocte-base-2026-09-16`). Updated: 2026-09-21.

## CI

- First full `Build all` run on the fork (workflow_dispatch, 2026-09-17): green on Windows x64, Linux x86_64/aarch64, macOS arm64/x86_64 and Flatpak. Windows deps built in 55 min, slicer in 1 h 24 min, unit tests 5 min. Artifact `nocte-slicer-windows-x64` produced. `NOCTE checks` green.
- Block 2 (PR #2, merged 2026-09-17, run 35241116400): Windows x64 build 33 min with the deps cache and a warm ccache, unit tests 6 min, 943/943 passed. The first attempt failed after 12 min on one `-Wvexing-parse` error: every Clang build here compiles with `/W4 -Werror` (root `CMakeLists.txt:549-598`), so a warning in new code stops the build. Pull-request and push builds cover Windows x64 only: on this fork the Linux, macOS and Flatpak jobs are guarded to `workflow_dispatch` (`build_all.yml`), so new code is not compiled with GCC or AppleClang until someone dispatches `Build all` by hand. Block 2 was dispatched on `main` as run 35246417361: green on Windows x64, Linux x86_64/aarch64, macOS arm64/x86_64 and universal, and both Flatpaks, with the unit tests passing on Windows, Linux and macOS. With the deps caches valid the compiled platforms finish in about an hour; Flatpak x86_64 builds everything from scratch and takes about 2 h 30 min.
- Block 3 (ADR-003): PR #3 (offline core, installer safety), PR #4 (offline surface, identity, rename, migration; 126 files) and PR #5 (repair and auto-tune panels, LAN agent; 5000 lines) all compiled and passed the 943 Windows unit tests on their first attempt after two compile-by-reading reviews each. The all-platform dispatch after PR #4 (run 35562545128) found the Linux and macOS deps caches evicted and rebuilt them; it was cancelled by the next dispatch while its last three jobs (Flatpak unit tests, publish) were still running. The dispatch after PR #5 (run 35570424053, `main` @ `70bba341ca`) is green on Windows x64, Linux x86_64/aarch64, macOS arm64/x86_64/universal and both Flatpaks, with the unit tests passing on Windows, Linux, macOS and inside both Flatpaks — the first time the panels and the LAN agent met GCC and AppleClang. Cost to know: that run rebuilt the deps on every platform, Windows included, because saving four fresh Linux/macOS deps caches pushed the Windows one out of the 10 GB budget; the next Windows PR build pays ~2.5 h until the caches settle. Whether all-platform dispatches should save deps caches for Linux/macOS at all is an open CI decision.
- Known: the initial push to the empty repository did not fire `push` workflows; manual dispatch works and later pushes trigger normally. Any push to a pull-request branch restarts `Build all`, whatever files it touches, because the path filter is evaluated against the whole pull-request diff.

## Bambu Studio compatibility

- First native round trip (2026-09-17): CI-built binary exported an STL to 3mf with A1 profiles; Bambu Studio 02.08.02.61 CLI loaded it (`Success.`), sliced it and re-exported it with object/part/plate/instance intact.
- The 3mf now carries `<metadata name="NocteSlicer">` next to the unchanged `Application = BambuStudio-<SLIC3R_VERSION>`, plus `Metadata/nocte_report.json`; both are left out of a minimal published 3mf.
- Synthetic corpus `tools/bbl-compat/corpus/nocte-cli/01–04` (single cube, modifier with per-part overrides, negative part, support enforcer and blocker), written end to end by the NØCTE CLI from run 35241116400: 4/4 pass oracle criteria (a)(c)(d)(e) against Bambu Studio 02.08.02.61; (b)(f)(g) are skipped for lack of a plaintext log, paint data and an AMS list. Subtypes and per-part overrides survive both the NØCTE reader and writer and Bambu Studio's re-export.
- Identity gate measured (ADR-001): with `Application = NocteSlicer-…` or `OrcaSlicer-…` the Bambu Studio CLI discards the project settings and crashes with an access violation on `--export-3mf`; the gate is the `BambuStudio-` prefix.
- CLI: a Bambu Studio 02.08.02 project no longer exits `-24` (checked with the old and the new binary on the same file). The `-18` fix is covered by its unit test only; no corpus file carries the `-1` values yet.
- Open, inherited from upstream and reproduced by Bambu Studio itself: the per-part `matrix` metadata of a multi-part object is compounded again on every save (`tools/bbl-compat/NOTES.md` §7.4). Geometry is unaffected.

## Planning engine (ADR-004, block 4, since 2026-09-22)

The block that creates the actual advantage over Bambu Studio: the user declares what a part is
**for** — ornament, functional by strength, functional by appearance (signage), draft — and the
engine ranks orientations, derives a process profile and offers modifiers defined relative to the
part. The derivation is `docs/HLSD/nocte-planning-engine.md` and it is normative: a number that
cannot be traced to it does not reach a user.

Eight PRs: A invariants and scores, B orientation engine and `--nocte-plan`, C stable object id and
the report reader, D the plan panel, E intent-aware rules, F computed presets, G parametric
modifiers, H LAN printing. Orca's `Orient.cpp` is left untouched and the NØCTE engine is written
beside it, for the reasons in ADR-004 §1.

**PR-A (in flight).** `Nocte/Plan/PartInvariants` measures once what a rotation cannot change
(facet areas and normals, volume, centroid, hull, self-occlusion visibility), so a candidate costs
one pass over flat arrays instead of the two whole-mesh copies `Orient.cpp` makes per candidate.
`Nocte/Plan/Scores` implements the five terms in their own physical units and the dimensionless
combination. 27 test cases.

Five specification errors were caught during PR-A, all of the same family — a parameter or formula
that silently does nothing, or measures a quantity that looks right and is not. None was found by
reading the specification; each was found by tracing a formula through a concrete part:

1. The support volume summed **interface** areas one layer thick instead of **column** volumes. A
   flat overhang of area A at height H scored `ρ·A·h` instead of `ρ·A·H`: wrong by the height of the
   part, and the height is exactly what the orientation changes, so it could invert the ranking on
   the term every other term is ranked against. Replaced by the top-down carry.
2. `support_on_build_plate_only` could not be honoured in that formulation and was deleted rather
   than left as a flag that provably does nothing.
3. `overhang_threshold_deg` was read by tier 0 and silently ignored by tier 1, the default path,
   because `slice_mesh_slabs` returns every downward-facing facet regardless of angle. Tier 1 now
   slices a sub-mesh filtered to the steep facets. A sphere on the bed was over-charged by
   `2 + 2√2 = 4.83`; a flat ceiling cannot expose this, which is why the test suite uses the sphere.
4. The footprint was the part's **silhouette** (`project_mesh` over the whole Z range) instead of its
   **bed contact**. A 20 × 20 mm cap on a 4 × 4 mm stem reported 400 mm² instead of 16 mm², giving
   `S = 0.88` where the truth is `S = 0.18` — it passed the `S ≥ 0.35` gate it should have failed,
   and a sphere resting on a point scored as stable as a cube. `GeometryAnalysis.cpp:93-103` already
   defined footprint as first-layer contact, so the fork was shipping two meanings of one word in one
   subsystem. Every stability fixture in the test suite was prismatic from the bed up, where the two
   definitions coincide, which is why it survived.

5. The cusp term returned three zeros — the **best** value on that axis — when no facet survived the
   visible-and-non-flat filter, with no way for a caller to tell that apart from a perfect surface.
   Reachable from an ordinary inverted STL: the centroid is still correct so the part passes every
   other gate, but every self-occlusion ray starts inside the surface and reports the whole part
   invisible, so the term silently stops discriminating on every candidate.

A related discipline the reviews tightened: any field that can be a *silent* zero now carries a flag
(`support_measured`, `contact_measured`, `cusp_measured`), because zero is the best possible score on
those terms, and a part we failed to measure must never outrank one we measured honestly. The
combination step cannot enforce this itself without desynchronising its output from its input, so the
obligation is written into its contract and into the HLSD, at the layer that owns it.

The reviews also found that all 34 `evaluate()` calls in the test file passed the identity transform,
so the orientation engine's reason to exist — ranking rotations — was never exercised through the
scoring entry point. Five distinct mutations of the rotation handling would have passed the entire
suite, including dropping the object-to-build-frame rotation of the load direction, which the file's
strongest physics assertion depends on.

The calibration that is still owed: the support density factor against real support grams, the
per-layer time overhead `t_layer`, and `k = σ_z/σ_xy`. Until those are measured, support volume ranks
candidates but is not quoted in grams, and the tier that produced a number is carried in the result.

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

## Product (ADR-003, since 2026-09-21)

**Offline by default.** No cloud agent is created and no cloud provider can be instantiated (`NetworkAgentFactory`); the Bambu plugin agent is not registered. Version-check and profile-update URLs are empty, `stealth_mode` and `hide_login_side_panel` default on, `sync_system_preset` off, vendor `url` fields blank. Removed from the interface: the Preferences Online tab, Help → Network Test / Check for Updates, Sync Presets, the plugin hint and download dialogs, the login funnel, the stealth escape, the publish dialog, the OrcaCloud notifications, the model-mall pages. The Device, Multi-device and device-calibration tabs are not created as pages and `show_device()` returns early; the Calibration menu stays. Measured on the PR #4 binary: over 90 s from a cold start `nocte-slicer.exe` opens no outbound socket; the only connections belong to `msedgewebview2.exe` (Microsoft's WebView2 runtime, which renders the home page) — silencing that runtime's own traffic through its browser arguments is a follow-up.

**Identity.** Monochrome palette through the central points (`StateColor::gDarkColors`, `ColorRGBA::ORCA()`, ImGui, toolbar tints, 3D background, `global.css`, the hosted web dialogs) with the greys in `GUI/Nocte/NocteTheme.hpp`; `BitmapCache` recolours both the `fill=` and the `style=` forms of the brand colours at load. Dark by default on every platform; `GUI_App` no longer overwrites the setting from the system appearance at start (`GUI_Utils.cpp:315-320` still does on a live theme change — follow-up). `SLIC3R_APP_FULL_NAME` is `NOCTE Slicer`, the window title reads `… - NØCTE Slicer`, home page rewritten (44 kB, no accounts), NØCTE menu next to Help. Wizard: welcome → printers → filaments → finish, printers filtered to Bambu Lab and Custom in the page script. Residue: ~116 teal literals in `src/`, all grey in dark mode (they route through `gDarkColors`) but teal in light mode, mostly in the Bambu device pages.

**Rename and migration.** Runtime key `NocteSlicer` (`%APPDATA%\NocteSlicer`, `NocteSlicer.conf`, `NocteSlicer.mo` for 23 languages) through `SLIC3R_RUNTIME_APP_KEY`; the CMake `SLIC3R_APP_KEY` stays `OrcaSlicer` for the Linux AppImage and macOS bundle templates. Windows launcher `nocte-slicer.exe`; `OrcaSlicer.dll`, target names and the unix binary unchanged. Installer with its own package name and registry key and no uninstall-before-install step. First start: a native prompt offers to import an OrcaSlicer data directory; verified with the PR #4 binary and `APPDATA` pointed at a fixture (32 files, SHA-256 manifest identical before and after; `user/**`, `shapes/` and a key-scrubbed `NocteSlicer.conf` copied; `cache/`, `log/`, the machine id not copied; no prompt on the second start; "start fresh" writes only the marker). The prompt is Windows-only for now (Linux/macOS start fresh and log). No real `%APPDATA%\OrcaSlicer` exists on the development machine, so the fixture came from a data directory the block-2 binary had written. Open: the Windows exe still has no version resource; `build_win.bat` prints the old launcher name; `AGENTS.md:71` names the old catalog; generic (PrusaSlicer-format) 3mf exports say `Application = NocteSlicer-…`; users with a custom `APPDATA` environment variable get the migration computed from the variable while wx uses the shell folder.

**Language.** The catalogs ship; English is the default from PR #5 on (`language = "en"` seeded), Spanish selectable in Preferences.

## LAN printing

Protocol validated in Python against a real A1 in Developer Mode (`tools/nocte/lan`, measured sequence in ADR-002). The C++ agent is in the tree since PR #5 (`src/slic3r/Utils/Nocte/`: MQTT over `boost::asio::ssl` at QoS 0, FTPS through libcurl, `IPrinterAgent` registered as `nocte-lan` and aliased as `bbl`, printing behind an `allow_print` flag nothing sets, and a headless probe under `NOCTE_LAN_PROBE=1` that writes a redacted checklist to `nocte_lan_probe.txt`). The probe of the PR #5 binary ran as designed (credentials only through the environment, redaction, exit code 1 on failure) but the printer was off at the time, so the hardware checklist — CONNACK, SUBACK, first report, `get_version`, NLST, STOR of a generated `.gcode.3mf`, DELE — is still to be recorded. The Device tab stays hidden until the agent shows live state; sending a print is a separate decision.

## Panels (`src/slic3r/GUI/Nocte/`, since PR #5)

NØCTE menu next to Help and two entries in the object/part context menu. `DiagnoseDialog` lists the issues and the repair plan of the selected part, previews steps and applies the checked ones through one `RepairSession`, writing the mesh back once inside one snapshot; `AutoTuneDialog` lists the recommendations for the selected scope and writes the checked ones as object or part overrides inside one snapshot. Both are `DPIDialog`s; there is no 3D highlighting yet. Strings in English with Spanish entries in `NocteSlicer_es.po`. The click-by-click manual test is in `docs/block3/W4-report.md` §7 and has not been run yet.
