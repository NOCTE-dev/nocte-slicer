# NØCTE Slicer — implementation status

Living document (the HLSD describes the design as it stands and carries no status markers, per `AGENTS.md`).

Base: OrcaSlicer upstream `main` @ `6b0e190e64` (tag `nocte-base-2026-09-16`). Updated: 2026-09-21.

## CI

- First full `Build all` run on the fork (workflow_dispatch, 2026-09-17): green on Windows x64, Linux x86_64/aarch64, macOS arm64/x86_64 and Flatpak. Windows deps built in 55 min, slicer in 1 h 24 min, unit tests 5 min. Artifact `nocte-slicer-windows-x64` produced. `NOCTE checks` green.
- Block 2 (PR #2, merged 2026-09-17, run 35241116400): Windows x64 build 33 min with the deps cache and a warm ccache, unit tests 6 min, 943/943 passed. The first attempt failed after 12 min on one `-Wvexing-parse` error: every Clang build here compiles with `/W4 -Werror` (root `CMakeLists.txt:549-598`), so a warning in new code stops the build. Pull-request and push builds cover Windows x64 only: on this fork the Linux, macOS and Flatpak jobs are guarded to `workflow_dispatch` (`build_all.yml`), so new code is not compiled with GCC or AppleClang until someone dispatches `Build all` by hand. Block 2 was dispatched on `main` as run 35246417361: green on Windows x64, Linux x86_64/aarch64, macOS arm64/x86_64 and universal, and both Flatpaks, with the unit tests passing on Windows, Linux and macOS. With the deps caches valid the compiled platforms finish in about an hour; Flatpak x86_64 builds everything from scratch and takes about 2 h 30 min.
- Block 3 (ADR-003): PR #3 (offline core, installer safety) and PR #4 (offline surface, identity, rename, migration; 126 files) both compiled and passed the Windows unit tests on their first attempt after two compile-by-reading reviews each. The all-platform dispatch after PR #4 (run 35562545128) found the Linux and macOS deps caches evicted and rebuilt them.
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

## Product (ADR-003, since 2026-09-21)

**Offline by default.** No cloud agent is created and no cloud provider can be instantiated (`NetworkAgentFactory`); the Bambu plugin agent is not registered. Version-check and profile-update URLs are empty, `stealth_mode` and `hide_login_side_panel` default on, `sync_system_preset` off, vendor `url` fields blank. Removed from the interface: the Preferences Online tab, Help → Network Test / Check for Updates, Sync Presets, the plugin hint and download dialogs, the login funnel, the stealth escape, the publish dialog, the OrcaCloud notifications, the model-mall pages. The Device, Multi-device and device-calibration tabs are not created as pages and `show_device()` returns early; the Calibration menu stays. Measured on the PR #4 binary: over 90 s from a cold start `nocte-slicer.exe` opens no outbound socket; the only connections belong to `msedgewebview2.exe` (Microsoft's WebView2 runtime, which renders the home page) — silencing that runtime's own traffic through its browser arguments is a follow-up.

**Identity.** Monochrome palette through the central points (`StateColor::gDarkColors`, `ColorRGBA::ORCA()`, ImGui, toolbar tints, 3D background, `global.css`, the hosted web dialogs) with the greys in `GUI/Nocte/NocteTheme.hpp`; `BitmapCache` recolours both the `fill=` and the `style=` forms of the brand colours at load. Dark by default on every platform; `GUI_App` no longer overwrites the setting from the system appearance at start (`GUI_Utils.cpp:315-320` still does on a live theme change — follow-up). `SLIC3R_APP_FULL_NAME` is `NOCTE Slicer`, the window title reads `… - NØCTE Slicer`, home page rewritten (44 kB, no accounts), NØCTE menu next to Help. Wizard: welcome → printers → filaments → finish, printers filtered to Bambu Lab and Custom in the page script. Residue: ~116 teal literals in `src/`, all grey in dark mode (they route through `gDarkColors`) but teal in light mode, mostly in the Bambu device pages.

**Rename and migration.** Runtime key `NocteSlicer` (`%APPDATA%\NocteSlicer`, `NocteSlicer.conf`, `NocteSlicer.mo` for 23 languages) through `SLIC3R_RUNTIME_APP_KEY`; the CMake `SLIC3R_APP_KEY` stays `OrcaSlicer` for the Linux AppImage and macOS bundle templates. Windows launcher `nocte-slicer.exe`; `OrcaSlicer.dll`, target names and the unix binary unchanged. Installer with its own package name and registry key and no uninstall-before-install step. First start: a native prompt offers to import an OrcaSlicer data directory; verified with the PR #4 binary and `APPDATA` pointed at a fixture (32 files, SHA-256 manifest identical before and after; `user/**`, `shapes/` and a key-scrubbed `NocteSlicer.conf` copied; `cache/`, `log/`, the machine id not copied; no prompt on the second start; "start fresh" writes only the marker). The prompt is Windows-only for now (Linux/macOS start fresh and log). No real `%APPDATA%\OrcaSlicer` exists on the development machine, so the fixture came from a data directory the block-2 binary had written. Open: the Windows exe still has no version resource; `build_win.bat` prints the old launcher name; `AGENTS.md:71` names the old catalog; generic (PrusaSlicer-format) 3mf exports say `Application = NocteSlicer-…`; users with a custom `APPDATA` environment variable get the migration computed from the variable while wx uses the shell folder.

**Language.** The catalogs ship; English is the default from PR #5 on (`language = "en"` seeded), Spanish selectable in Preferences.

## LAN printing

Protocol validated in Python against a real A1 in Developer Mode (`tools/nocte/lan`, measured sequence in ADR-002). PR #5 carries the C++ agent (`src/slic3r/Utils/Nocte/`: MQTT over `boost::asio::ssl` at QoS 0, FTPS through libcurl, `IPrinterAgent` registered as `nocte-lan` and aliased as `bbl`, printing behind an `allow_print` flag nothing sets, and a headless probe under `NOCTE_LAN_PROBE=1`) together with the GUI panels under `src/slic3r/GUI/Nocte/`.
