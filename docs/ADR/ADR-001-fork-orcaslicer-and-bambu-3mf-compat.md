# ADR-001 — Fork OrcaSlicer and preserve native Bambu Studio 3mf compatibility

- **Status:** Accepted
- **Date:** 2026-09-16
- **Deciders:** NØCTE Engineering

## Context

NØCTE Slicer is a C++ 3D-printing slicer (dark branding, Ø wordmark, tagline "Building the Impossible") whose goal is to beat Bambu Studio at analysis and control. Three features are first-class from day one — there is no "features later" phase:

1. **Explainable repair** of missing faces / damaged geometry: what was repaired, why, step by step, with accept/reject. Not a black-box auto-fix.
2. **Per-part auto-tuning** derived from real geometry, written as per-object / per-part overrides that Bambu Studio displays natively.
3. **.3mf export that Bambu Studio treats as native** (modifiers, multi-material/AMS, plates, proprietary metadata). This requirement is **non-negotiable**.

Fixed business constraints: open source under AGPL-3.0, revenue from brand and services, no proprietary modules. Bambu Lab printers first, multi-vendor later.

## Decision

### 1. Fork OrcaSlicer

- Canonical upstream: `https://github.com/OrcaSlicer/OrcaSlicer.git` (the `SoftFever/OrcaSlicer` URL redirects here). Remote name `upstream`; `origin` is the NØCTE repository. A pristine `upstream-main` branch is kept fast-forward-only.
- **Starting point: upstream `main` at commit `6b0e190e64` (2026-09-16), tagged `nocte-base-2026-09-16`.** The plan originally named the stable tag `v2.4.2`, but the measured delta rules it out for our purpose: `v2.4.2` writes 3mf with `SLIC3R_VERSION 02.06.00.51`, two Bambu Studio generations behind the installed 02.08.02.61, while `main` writes `02.08.01.55` and carries 19 commits (394 insertions) in `src/libslic3r/Format/bbs_3mf.cpp` for the multi-nozzle era, plus 42 changed CI/deps files that match today's hosted runners. Native compatibility outweighs release stability at M0; the base is pinned by tag so it is still a fixed point.
- **Sync cadence:** monthly and on every upstream tag, via `sync/upstream-YYYY-MM` branches, merge (never rebase). Gate: full `ctest` plus the `tools/bbl-compat` oracle.
- **Isolation rule:** all NØCTE code lives in NØCTE-owned directories (`src/libslic3r/Nocte/`, `src/slic3r/GUI/Nocte/`, `src/slic3r/Utils/Nocte/`). Every edit inside an upstream file is wrapped in `// NOCTE-BEGIN <ticket>` / `// NOCTE-END`, and `tools/nocte/check_touchpoints.py` fails CI when a marker appears outside the allowlist. Upstream files are never reformatted.

**Upstream touch-point allowlist (complete):** `README.md` (replaced; upstream copy kept at `docs/UPSTREAM-README-OrcaSlicer.md`), `version.inc`, `CMakeLists.txt`, `src/libslic3r/CMakeLists.txt`, `src/slic3r/CMakeLists.txt`, `tests/libslic3r/CMakeLists.txt`, `src/libslic3r/Format/bbs_3mf.cpp`, `src/slic3r/GUI/Plater.cpp`, `src/slic3r/GUI/MainFrame.cpp`, `src/slic3r/GUI/GUI_ObjectList.cpp` (one menu hook only), `src/slic3r/GUI/AboutDialog.cpp`, `src/OrcaSlicer.cpp` (strings only; no rename in M0), `src/slic3r/Utils/NetworkAgentFactory.*` (register the LAN agent), `resources/*`, and the Windows `.rc` resources [VERIFY exact path].

### 2. Options considered

| Criterion | A: fork OrcaSlicer | A': fork Bambu Studio directly | B: engine from scratch |
|---|---|---|---|
| Time to a branded working build | 1–2 weeks (toolchain + deps 1–3 h, slicer 30–60 min) | Comparable to A | 0 weeks for "hello world", **2–4 years** for parity (Arachne, tree supports, wipe tower, Bambu gcode, calibrations, MMU painting, 3D GUI) |
| Native Bambu 3mf compatibility | Inherited: `src/libslic3r/Format/bbs_3mf.cpp` is literally the same writer as Bambu Studio; Orca already opens as native in Bambu Studio | Also inherited, by definition | Re-implement ~30 archive files (XML/JSON/gcode with header and md5) with no public spec, and chase every Bambu release (every 2–3 months) |
| Maintenance | Monthly upstream merge; conflicts only if we touch upstream files (mitigated by isolated directories) | Single-vendor upstream, none of the multi-vendor groundwork we need later | Everything is ours; nobody else fixes bugs; no community |
| License | AGPL-3.0 for the whole binary; no dual licensing (hundreds of authors, no CLA); the NØCTE brand is still protectable as a trademark (§7(e)) | Same AGPL-3.0 constraint | Full freedom (MIT/proprietary) — but Orca/Bambu `resources/profiles` live inside an AGPL repo, so copying them into a proprietary product is a risk; values would have to be re-derived |
| Specific legal risk | The proprietary network plugin `BambuNetworkLibrary.dll` (downloaded at runtime) is under a public Software Freedom Conservancy dispute (May 2026) — **never redistribute it under the NØCTE brand** (see ADR-002) | Inherits the same plugin risk, more tightly coupled | None inherited, but LAN sending to Bambu still requires Developer Mode |
| Differentiation | All effort goes to the 3 features | Loses Orca's `IPrinterAgent` / `NetworkAgentFactory` groundwork needed for multi-vendor | Years rebuilding what already exists before differentiating anything |

Option A is chosen. The critical requirement is covered *by construction*; the chosen business model (AGPL + services) removes B's only real advantage (closing the source); Orca already vendors libigl, CGAL (isolated in the `libslic3r_cgal` target), OpenVDB, Eigen, TBB, Clipper2 and qhull — nearly everything explainable repair and geometric analysis need — plus `IPrinterAgent` / `ICloudServiceAgent` / `NetworkAgentFactory` abstractions that give a clean seam for our own LAN agent.

### 3. 3mf identity

The default written by NØCTE Slicer is:

- `Application = "BambuStudio-<SLIC3R_VERSION>"` (exactly what OrcaSlicer writes), with `SLIC3R_VERSION` kept in the `02.08.xx.xx` shape (a hard requirement of Orca's `AGENTS.md`; it feeds the `Application` string);
- plus a NØCTE tag `<metadata name="NocteSlicer"><ver></metadata>`;
- plus an additive `Metadata/nocte_report.json`.

NØCTE is therefore identifiable inside the file without losing native mode. The variant `Application = "NocteSlicer-<ver>"` (V1) remains only as a hidden advanced option.

**Source-code evidence** (bambulab/BambuStudio master, `src/libslic3r/Format/bbs_3mf.cpp`): `_handle_end_metadata()` L4234 sets `m_is_bbl_3mf = true` **only** when `Application` starts with `"BambuStudio-"` (version = `substr(12)`). `BambuStudio:3mfVersion` (=1) only sets `m_version`; its detection branch is commented out. On the non-native path (`!m_is_bbl_3mf`) the loader splits multi-instance objects, bakes transforms into the mesh, and names objects from the filename. `Plater.cpp` warns "The 3mf is not from Bambu Lab, load geometry data only" when the config is empty; `Newer3mfVersionDialog` fires when the file version exceeds the app version; `is_bbl_vendor_config()` requires a `printer_model` from vendor BBL; `check_project_config()` requires `extruder_type.size() == nozzle_diameter.size()`.

**Empirical evidence from NØCTE's own FDM-HUB tool** (`calibracion/APRENDIDO.md` §1ter L113–149; `calibracion/herramientas/armar_probeta.py:82-106`): with any `Application` value not starting with "BambuStudio", Studio treats the 3mf as an imported mesh — loose mesh, `filament_id=unknown`, 0.00 g, no AMS macros (`T1000` absent) — even though slicing reports *Success*. FDM-HUB therefore uses `APP_POR_OMISION = "BambuStudio-02.08.02.60"`. V1 is thus already measured as a failure; V2 is the default.

## Consequences

**Positive:** native Bambu 3mf support on day one; a mature slicing engine, GUI and profile set; permissive mesh-processing libraries already vendored; a clean extension point for printer agents; effort concentrated on the three differentiating features.

**Negative:** the whole binary is AGPL-3.0 (no proprietary modules, no dual licensing); a permanent monthly upstream-merge cost; CGAL's GPL surface must stay confined to `libslic3r_cgal` (NØCTE's CGAL calls live in `Nocte/NocteCgal.cpp`); Bambu format drift must be tracked release by release; the `Application` string cannot carry the NØCTE name in the default profile, so in-file branding is carried by the `NocteSlicer` metadata tag instead.

## Verification

In M0 the `tools/bbl-compat` oracle runs both identity variants (V1 and V2) purely to record the logs in this ADR, and re-confirms that `Metadata/nocte_report.json` is additive and ignored on reload [VERIFY with the oracle in M0]. Bambu Studio has no `--validate`; the oracle is exit code plus log, running `bambu-studio.exe --info --debug 5 --outputdir <tmp> <file>` and then `--load-settings ... --slice 0 --export-3mf`. The Bambu Studio version under test is pinned in `corpus_manifest.json` (currently 02.08.02.61).

"Detected as native" criteria:

- **(a)** exit code 0.
- **(b)** the log does not contain "not from Bambu Lab, load geometry data only".
- **(c)** with `--debug 5`, config keys are loaded (⇒ `m_is_bbl_3mf`).
- **(d)** `Newer3mfVersionDialog` is not triggered.
- **(e)** Bambu's re-export preserves object and part counts, every `subtype`, every per-part `<metadata key>`, `plater_id` and the `model_instance` mapping, and neither splits instances nor bakes transforms.
- **(f)** the `paint_color` / `paint_supports` / `paint_seam` triangle attributes are byte-identical.
- **(g)** the `ams_list` in `slice_info.config` is preserved.

Reference for (e): part subtypes are `normal_part`, `negative_part` (not `negative_volume`), `modifier_part`, `support_enforcer`, `support_blocker` (`Model.cpp` L3614).
