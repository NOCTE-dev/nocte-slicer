# bbl-compat — observed Bambu Studio CLI behaviour

Harness for checking that NØCTE Slicer stays byte-for-structure compatible with
Bambu Studio's 3MF dialect.

| file | role |
| --- | --- |
| `inspect3mf.py` | library + CLI: static summary of one 3MF |
| `structdiff.py` | library + CLI: structural diff of two 3MF |
| `oracle.py` | library + CLI: run Bambu Studio against one 3MF, verdict on criteria (a)–(g) |
| `roundtrip.py` | CLI: run the oracle over the whole corpus, write `report/summary.{json,md}` |
| `smoke_export.ps1` | one STL → NØCTE 3mf → oracle: the minimal end-to-end check |
| `make_corpus.ps1` / `make_corpus.py` | generate `corpus/nocte-cli/` with the NØCTE CLI — see §7 |
| `corpus/` | test material — see `corpus/README.md` and `corpus/fdmhub/SOURCES.md` |
| `corpus_manifest.json` | pinned inventory (`bambu_studio_version`, `orca_base`, sha256 per file) |
| `_work/` | scratch: temp 3mf, oracle workdirs, ad-hoc reports (gitignored) |

Quick start:

```bash
python inspect3mf.py corpus/fdmhub/P4_FLUJO_A1.3mf
python structdiff.py corpus/fdmhub/P9_BARRIDO_Z_PETG_A1.gcode.3mf \
                     corpus/fdmhub/P9_BARRIDO_Z2_PETG_A1.gcode.3mf
python oracle.py corpus/fdmhub/P4_FLUJO_A1.3mf --workdir /tmp/w --bs-version 02.08.02.61
python roundtrip.py --static-only --bs-version 02.08.02.61
```

---

## Findings from the 2026-09-16 smoke test

Environment: Windows 11 Pro 26200, Python 3.12.10, Bambu Studio **02.08.02.61** at
`C:\Program Files\Bambu Studio\bambu-studio.exe`. One single real invocation was
made, through `oracle.py`, on `corpus/fdmhub/P4_FLUJO_A1.3mf` (12.5 kB, 1 object,
1 `normal_part`, 1 plate, 1 `model_instance`):

```
bambu-studio.exe --info --debug 5 --outputdir <workdir> <file>
```

### 1. It does **not** hang and does **not** open a window

Exit code `0`, wall clock **1.25 s**, no GUI window, no dialog. Launched with
`creationflags=subprocess.CREATE_NO_WINDOW`. Two unrelated Bambu Studio GUI
instances happened to be running at the time and the CLI run neither reused nor
disturbed them: it did its own work (`result.json` written with `prepare_time:
40`). No kill was needed.

### 2. `--debug 5` produces **no log text at all** — the big surprise

`stdout` length **0**, `stderr` length **0**. `bambu-studio.exe` is linked as a
*GUI subsystem* binary on Windows, so it never writes to the pipes it is handed,
whatever `--debug` level is passed. No `*.log` appeared in `--outputdir` either.

Its real log goes to `%APPDATA%\BambuStudio\log\studio_<date>_<pid>_enc.log.N`,
and since the `_enc` naming was introduced that file is **encrypted**: a plaintext
`BEGIN_HEADER { ... "enc_version": "1.0.0.0" } END_HEADER` block followed by a
ciphertext body. Confirmed by reading the bytes directly.

**Consequence for the oracle design.** The brief assumed
`oracle = exit code + log text + structure of the re-export`. The log leg is
simply not available with a stock Bambu Studio 02.08.02.61, so:

- criteria **(b)** ("not from Bambu Lab" / "load geometry data only") and **(d)**
  ("newer version" warning) report `skip`, not a vacuous `PASS`, when no log text
  was captured. A silent PASS there would have been the worst outcome: it would
  have hidden the non-native import path rather than caught it;
- `oracle.py` grew `--log-file` and `--log-dir` so the markers *can* be checked
  when a plaintext log exists — which is the normal case for a NØCTE Slicer debug
  build, or any Orca-derived binary built as a console subsystem app. Files whose
  header contains `enc_version` are recognised and skipped with a reason rather
  than fed to the matcher as mojibake;
- two static fallbacks were added so the criteria are not purely decorative:
  **(b)** *fails* when `Application` lacks the `BambuStudio-` prefix (the
  non-native path is then unavoidable, no log needed), and **(d)** *fails* when
  `--bs-version` shows the file was written by a build newer than the one under
  test (this is exactly what triggers Bambu Studio's warning).

### 3. `result.json` is the real machine-readable verdict

Undocumented in the brief but far more useful than the exit code: the CLI writes
`result.json` into `--outputdir`. Verbatim, from the smoke run:

```json
{
    "error_string": "Success.",
    "export_time": 0,
    "layer_height": 0.0,
    "plate_index": 0,
    "prepare_time": 40,
    "return_code": 0,
    "sparse_infill_density": 0.0,
    "upward_compatible_machine": [
        "Bambu Lab H2D 0.4 nozzle",
        "Bambu Lab H2D Pro 0.4 nozzle",
        "Bambu Lab H2S 0.4 nozzle",
        "Bambu Lab P2S 0.4 nozzle",
        "Bambu Lab H2C 0.4 nozzle",
        "Bambu Lab X2D 0.4 nozzle",
        "Bambu Lab A2L 0.4 nozzle"
    ],
    "wall_loops": 0
}
```

`oracle.py` now reads it after every phase and folds `return_code` into criterion
(a) alongside the process exit code; `error_string` is printed and stored, and
`roundtrip.py` carries a `rc` column for it. A stale `result.json` is deleted
before the first phase so one phase cannot inherit another's verdict.

Note that `--info` reports `layer_height`, `sparse_infill_density` and
`wall_loops` as `0` — those are slice outputs, only populated by a `--slice` run.
`upward_compatible_machine` is a genuinely useful extra signal: it is the profile
compatibility list Bambu Studio computed for the file.

### 4. Windows exit codes arrive unsigned

The documented negative return codes (`-13` failed 3mf export, `-18` invalid
parameter value in the 3mf, `-100` unrepairable mesh) come back from Windows as
unsigned 32-bit values (`-13` → `4294967283`). `oracle.py` normalises them in
`_normalize_exit_code` before comparing, so the report shows `-18`, not
`4294967278`.

### 5. Not yet observed

Only one real invocation was allowed, so these remain **assumptions**, not
verified facts, and are flagged as such in the code:

- that `--slice 0 --export-3mf out.3mf` behaves as documented (never run here);
- the exact wording of the non-native warning ("The 3mf is not from Bambu Lab,
  load geometry data only") — the matcher looks for the two lowercase substrings
  `not from bambu lab` and `load geometry data only`, which should survive minor
  rewording;
- the wording of the "newer version" warning — matched loosely via `newer
  version`, `newer 3mf`, `higher version`;
- whether a `--slice` run of a *non-native* file returns `0`, `-13` or `-18`.

The first real `--slice` sweep should be done with
`python roundtrip.py --slice --bs-version 02.08.02.61` and the resulting
`report/summary.md` diffed against this list.

---

## Other things worth knowing about the corpus

- Every FDM-HUB file is native (`Application` = `BambuStudio-02.08.02.60` or
  `-02.04.00.70`), so the *non*-native path has no fixture yet. Producing one is
  easy: save any project from OrcaSlicer or PrusaSlicer, or rewrite the
  `Application` metadata of a copy.
- Several `*.gcode.3mf` exports carry slice data with **no geometry whatsoever**
  (`3D/Objects/` absent, `model_settings.config` with plates but zero objects) —
  `plantilla.gcode.3mf`, `P9_BARRIDO_*`, `P4_FLUJO_A1.gcode.3mf`. `_PERFIL_ADD1300.3mf`
  is settings-only. These are the degenerate inputs a loader trips over.
- `PRUEBA_FDMHUB.3mf` is the only fixture using the production extension
  (`3D/Objects/object_1.model` reached through `p:path`), so it is the one that
  proves `inspect3mf` aggregates objects across `.model` parts.
- No corpus file has any paint attribute and none has an `<ams_list>`, so criteria
  (f) and (g) are `skip` for the whole current corpus. They only come alive with
  `corpus/bambu-studio/05_mmu_painted_4ams`, which has to be authored by hand.

## 6. Findings from the first CI-built binary (2026-09-17, run 35180882793)

Binary: `nocte-slicer-windows-x64` artifact (`orca-slicer.exe`, base upstream main @ 6b0e190e64,
`SoftFever_VERSION 2.5.0-dev`, `SLIC3R_VERSION 02.08.01.55`). Headless CLI works on a machine
without admin rights; it writes nothing to stdout except `Slic3r::CLI::run found error, exit` on
failure. Use PowerShell `Start-Process ... -PassThru` to read the true (negative) exit code; Git Bash
shows it masked.

- **`-24 CLI_FILE_VERSION_NOT_SUPPORTED` on every Bambu Studio 02.08.02.6x project.**
  `src/OrcaSlicer.cpp` (~L1761) compares `SoftFever_VERSION` (2.5.x) against the *Bambu Studio*
  version parsed from the 3mf `Application` string (2.8.x) as if they were the same numbering, so
  any file from BS >= 2.6 is "newer". Bypass: `--allow-newer-file`. For NØCTE this check must be
  rewritten to compare against `SLIC3R_VERSION` (the Bambu-compatible version) — candidate
  touch point, to be decided in M1.
- **`-18 CLI_INVALID_VALUES_IN_3MF` on Bambu Studio 02.08.02 projects.** BS writes
  `raft_first_layer_expansion = -1` and `tree_support_wall_count = -1` (meaning "auto"); Orca's
  option ranges are `[0, +inf)` and `[0, 2]`, so the strict CLI validation rejects the project.
  The GUI path applies substitutions instead. Implication for the oracle: round trips of
  BS-authored projects through the NØCTE CLI need either relaxed validation or a
  `-1 -> auto` migration in `PrintConfigDef::handle_legacy()` (M1 decision). Implication for
  export: NØCTE must never write `-1` for these keys unless the target BS version accepts it.
- Corpus files with `Application` 02.04.00.70 pass the version check but are sliced `.gcode.3mf`
  files with no geometry, so they are not usable as export inputs.
- **`-13 CLI_EXPORT_3MF_ERROR` when `--export-3mf` gets an absolute path.** Both Orca and Bambu
  Studio concatenate the value onto `--outputdir`; pass a bare file name. This is the -13 that
  FDM-HUB hit with Bambu Studio 02.04 and worked around by patching `.gcode.3mf` files by hand.
  `oracle.py` now passes `reexport.3mf` and Bambu Studio 02.08.02.61 exports fine.
- Packaged builds ship profiles as `resources/profiles/<Vendor>.opc` bundles; the CLI
  `--load-settings` / `--load-filaments` want the JSON files, which live in the source tree
  (`resources/profiles/BBL/{machine,process,filament}/*.json`).

### First native round trip (2026-09-17)

`tools/bbl-compat/smoke_export.ps1`: STL -> NØCTE 3mf (A1 0.4 nozzle, 0.20mm Standard, Bambu PLA
Basic) -> `oracle.py --slice` against Bambu Studio 02.08.02.61.

| crit | result | note |
|---|---|---|
| (a) | PASS | `--info` and `--slice 0 --export-3mf` both `return_code 0`, `Success.` |
| (b) | skip | no plaintext log (encrypted studio log) |
| (c) | PASS | `Application = BambuStudio-02.08.01.55` |
| (d) | PASS | file version <= 02.08.02.61 |
| (e) | PASS | object, `normal_part`, per-part keys, `plater_id`, one `model_instance` preserved |
| (f)/(g) | skip | test file has no paint attributes / `ams_list` (needs corpus 05) |

Observed and to be studied in M1: Bambu Studio's re-export drops 266 `project_settings` keys and
adds 105 (Orca-only keys are discarded, BS-only keys added), changes 13 values, and rewrites
`3D/3dmodel.model` (39 canonicalised diff lines, e.g. item transform / UUIDs). None of that affects
native detection, but the auto-tune overrides must only use keys Bambu Studio keeps.

---

## 7. The synthetic `corpus/nocte-cli` corpus, written by the NØCTE CLI (2026-09-17)

`make_corpus.ps1` / `make_corpus.py` generate four projects that carry every part subtype Bambu
Studio knows, end-to-end through the NØCTE writer. Binary used for the first run:
`C:\dev\nocte-builds\run-35180882793` (base + M0, `SLIC3R_VERSION 02.08.01.55`).

```
powershell -File tools\bbl-compat\make_corpus.ps1                      # newest run-* build
powershell -File tools\bbl-compat\make_corpus.ps1 -SlicerDir C:\dev\nocte-builds\run-<newid>
powershell -File tools\bbl-compat\make_corpus.ps1 -SlicerDir <dir> -Oracle -KeepWork
```

### 7.1 Three stages, because the CLI has no "add a modifier" switch

| stage | what happens |
|---|---|
| A | `orca-slicer.exe --load-assemble-list <json> --load-settings ... --load-filaments ... --outputdir W --export-3mf stageA.3mf` |
| B | Python rewrites **only** `Metadata/model_settings.config` inside that archive: each extra `<part>` gets its real `subtype` and its per-part `<metadata key=... value=.../>` overrides |
| C | `orca-slicer.exe --export-3mf stageC.3mf stageB.3mf` — the corpus entry is this file, so it is the NØCTE writer's own output |

`--load-assemble-list` is the **only** CLI door to a 3MF with one object made of several parts:
every entry of a plate that shares the same `assemble_index > 0` is folded into one `ModelObject`
whose volumes are the STLs (`merge_or_add_object`, `src/OrcaSlicer.cpp:802`). Its JSON schema is in
`src/OrcaSlicer.hpp` (`JSON_ASSEMPLE_*`): `plates[] { plate_name, need_arrange, objects[] { path,
count, filaments[], assemble_index[], pos_x[], pos_y[], pos_z[], print_params{}, height_ranges[] } }`.

Quirks measured while getting this to work:

- the assemble list **must not** be combined with input model files or with any transform option
  (`src/OrcaSlicer.cpp:1706`); it is the whole input;
- the JSON must be UTF-8 **without a BOM** — the CLI feeds it straight to nlohmann/json, and
  PowerShell's `Out-File -Encoding utf8` writes a BOM that makes the parse fail;
- `need_arrange: false` plus explicit `pos_x/pos_y = 128` keeps the result deterministic and puts
  the object on the A1 bed centre; without a position the object lands in the bed corner;
- `filaments` is mandatory on every object entry (read with `.at()`, no default);
- `print_params` / `assembled_params` set **object**-level config, never per-volume config, and the
  switch has no notion of a volume *type* — that is why stage B exists.

Dead ends, so nobody repeats them:

- `--assemble` (a `CLITransformConfigDef` bool) is a transform applied to loaded input files and
  cannot be combined with `--load-assemble-list`;
- `--clone-objects` / `--repetitions` only multiply whole objects;
- `--metadata-name` / `--metadata-value` write 3MF *document* metadata (Title, Designer, ...), not
  per-part settings;
- `C:\dev\nocte-builds\help.txt` is **0 bytes** — the CLI writes no help to a redirected stdout, so
  the option table has to be read from `PrintConfig.cpp` (`CLIActionsConfigDef`,
  `CLITransformConfigDef`, `CLIMiscConfigDef`, lines ~11836-12360).

### 7.2 The four cases

Every case is one object on plate 1, one `model_instance`, A1 0.4 nozzle / 0.20mm Standard /
Bambu PLA Basic, meshes of 12 triangles each (684-byte binary STLs kept next to the 3MF):

| case | parts | per-part overrides |
|---|---|---|
| `01_single_cube` | `normal_part` | – |
| `02_modifier_block` | `normal_part` + `modifier_part` | `sparse_infill_density=35%`, `wall_loops=5` |
| `03_negative_part` | `normal_part` + `negative_part` | – |
| `04_support_enforcer_blocker` | `normal_part` + `support_enforcer` + `support_blocker` | – |

The two override keys were chosen because Bambu Studio's own `result.json` reports
`sparse_infill_density` and `wall_loops`, so they are unambiguously part of its dialect (§6: BS drops
Orca-only keys on re-export). Verified: **both survive Bambu Studio's re-export unchanged.**

### 7.3 Round-trip results (old binary, run-35180882793)

- **NØCTE reader → NØCTE writer (stage B → stage C): every `subtype` and every per-part override
  survives**, for all four cases, on all 2-3 parts per object.
- **Bambu Studio 02.08.02.61 `--slice 0 --export-3mf`: same — subtypes, per-part config keys and
  values, `plater_id` and the `model_instance` count all come back unchanged.**

| file | (a) | (b) | (c) | (d) | (e) | (f) | (g) |
|---|---|---|---|---|---|---|---|
| `01_single_cube.3mf` | PASS | skip | PASS | PASS | PASS | skip | skip |
| `02_modifier_block.3mf` | PASS | skip | PASS | PASS | PASS | skip | skip |
| `03_negative_part.3mf` | PASS | skip | PASS | PASS | PASS | skip | skip |
| `04_support_enforcer_blocker.3mf` | PASS | skip | PASS | PASS | PASS | skip | skip |

(b) skips because there is still no plaintext log; (f)/(g) skip because no case has paint attributes
or an `<ams_list>` — that is corpus case 05, which has to be authored in Bambu Studio by hand.

### 7.4 Finding: the per-part `matrix` metadata is re-compounded on every save

Measured on `02_modifier_block`, whose object has two volumes:

| writer | `<part>` `matrix` translation | `<component transform>` |
|---|---|---|
| NØCTE, stage A (fresh from STL) | `128 128 10` | `128 128 10` |
| NØCTE, stage C (1 load+save) | `256 256 20` | `128 128 10` |
| NØCTE, 2nd load+save | `384 384 30` | `128 128 10` |
| Bambu Studio 02.08.02.61 re-export of stage C | `384 384 30` | `128 128 10` |

Cause, identical in both slicers because it is upstream code:

- reader `bbs_3mf.cpp:5157` — `volume->source.transform = Transformation(<matrix metadata>)`;
- reader `bbs_3mf.cpp:5163` — `volume->set_transformation(component_transform * ...)`;
- writer `bbs_3mf.cpp:8024` — `matrix = volume->get_matrix() * volume->source.transform.get_matrix()`.

So `M(n+1) = V * M(n)` where `V` is the volume's component transform. It is stable only when
`V == I`, which is the case for a single-volume object (the placement then lives in the build
`<item>` transform) — that is why `01_single_cube` does not drift and `02`/`03`/`04` do.

Consequences:

- geometry is **not** affected: the authoritative placement is `<component transform>` / build
  `<item transform>` in `3D/3dmodel.model`, and those are byte-identical across all writers;
  `source.transform` only feeds "reload from disk" / source-file reconciliation;
- it is **not** a NØCTE regression — Bambu Studio's own 02.08.02.61 writer does exactly the same
  step, so any BS project with a modifier has the same drift;
- `structdiff.py` now classifies a per-part `matrix` value change as severity `layout`
  (category `model_settings/part_recompounded`, see `RECOMPOUNDED_PART_KEYS`) instead of
  `unexpected`, so criterion (e) is not failed by a difference Bambu Studio itself produces. Key
  *loss* is still `unexpected`, and the `3D/3dmodel.model` transforms are still compared;
- M1 candidate fix: either stop multiplying by `source.transform` in the writer, or stop seeding
  `source.transform` from the stored matrix in the reader. Whichever, the corpus must be
  regenerated afterwards — after the fix `matrix` will simply equal the component transform.

### 7.5 Other measured details

- Stage C does **not** need `--allow-newer-file` for our own output on this binary (tested:
  exit 0 without it). It is passed anyway so the generator keeps working against a build where the
  §6 `-24` gate behaves differently.
- The generated files are **not byte-reproducible**: `3D/3dmodel.model` carries per-save `p:UUID`
  values and the archive carries timestamps. Structure, subtypes, overrides and sizes are stable;
  the `sha256` in `corpus_manifest.json` therefore changes on every regeneration, by design.
- Total size of `corpus/nocte-cli`: 4 x ~15.5 kB of 3MF (62 018 B) plus 7 STLs of 684 B — about
  67 kB, well inside the 300 kB budget.
- `corpus_manifest.json` is rewritten by the generator over the **whole** corpus (origins preserved)
  and gains `nocte_cli_slicer_build` plus, per generated file, `slicer_build`, `nocte_tag` and
  `nocte_report`.
- `inspect3mf.py` now reports `nocte_tag` / `nocte_tag_present` (the `<metadata name="NocteSlicer">`
  value in `3D/3dmodel.model`) and `nocte_report_present` (`Metadata/nocte_report.json`). With the
  old binary both are absent, which is why each `<case>.expected.json` records them as
  `"required_from_build": "block2"` rather than as a hard requirement.
- Full sweep `python roundtrip.py --slice --bs-version 02.08.02.61`: 5/23 files pass. The four
  `nocte-cli` cases and `fdmhub/PRUEBA_FDMHUB.3mf` pass; every other `fdmhub` project fails (e)
  with `part config keys differ (lost: [], gained: ['source_file', 'source_object_id',
  'source_offset_*', 'source_volume_id'])` — Bambu Studio *adds* the source-file keys that the
  FDM-HUB originals do not carry — and the `*.gcode.3mf` files fail (a) because they have no
  geometry. Both are pre-existing and unrelated to this corpus.

---

## 8. Identity gate: V1 vs V2, measured (2026-09-17)

For ADR-001. **V2** = the file as NØCTE writes it today:
`<metadata name="Application">BambuStudio-02.08.01.55</metadata>` (plus, from the block2 build, a
separate `<metadata name="NocteSlicer">` tag and `Metadata/nocte_report.json`). **V1** = a byte-wise
identical copy whose `Application` value alone was rewritten to `NocteSlicer-0.1.0`.

Method: a work-dir-only script rewrote the `Application` metadata of `01_single_cube.3mf` and
`02_modifier_block.3mf`, then ran `oracle.py --slice --bs-version 02.08.02.61` on both.
Bambu Studio 02.08.02.61, Windows 11. The V1 files live only in `tools/bbl-compat/_work/`.

### 8.1 The headline: V1 makes Bambu Studio crash on export

| run | V2 | V1 |
|---|---|---|
| `--info` | exit 0 | exit 0 (`result.json` says `return_code 0`, `Success.`) |
| `--slice 0` | exit 0, `plate_1.gcode` + `result.json` | exit 0, `plate_1.gcode` + `result.json` |
| `--slice 0 --export-3mf re.3mf` | exit 0, 3MF written | **exit `-1073741819` = `0xC0000005` ACCESS_VIOLATION**, no `result.json`, no 3MF |

Reproduced identically on both cases. A V1-identity project is therefore a **one-way street**: Bambu
Studio will slice it, but it crashes the moment the user saves or exports the project back to 3MF.

Controls, all on `01_single_cube`, which rule out the re-zipping and the particular vendor name:

| `Application` value | `--slice --export-3mf` |
|---|---|
| `BambuStudio-02.08.01.55` (identical re-zip, value untouched) | exit 0, re-export produced, (e) PASS |
| `BambuStudio-02.08.01.55-NocteSlicer` (prefix kept, tag appended) | exit 0, re-export produced, (e) PASS |
| `OrcaSlicer-2.5.0` | **exit `0xC0000005`** |
| `NocteSlicer-0.1.0` | **exit `0xC0000005`** |

The crash is caused by the **absence of the `BambuStudio-` prefix**, not by the string "NocteSlicer"
and not by the zip rewriting. A *suffix* after the version is harmless.

### 8.2 What the non-native path silently changes (V1 `--slice 0`, no export)

Same input geometry, same project settings embedded in the 3MF:

| signal | V2 (native) | V1 (geometry-only import) |
|---|---|---|
| `sparse_infill_density` in `result.json` | `15.0` (the project's own value) | `20.0` — **Bambu Studio's default; the embedded project settings were discarded** |
| filament | `filament_id ""`, `total_used_g 3.68` | `filament_id "unknown"`, `total_used_g 0.0` |
| object bbox on the bed | `x 118, y 118` (as placed, centre 128/128) | `x 90, y 90` — **re-arranged from scratch** |
| object bbox size | `20.0 x 20.0 x 20.0` | `20.0 x 20.0 x 19.999998` — **the instance transform was baked into the mesh in float** |
| `plate_1.gcode` | 422 501 B | 192 322 B (different tool paths, different md5) |
| prediction | 1 172 s | 1 711 s |

Note that `--info` and `--slice` both still return 0 with `error_string "Success."` for V1, so the
exit code alone is **not** a detector — only the export crash and the value differences above are.

### 8.3 Conclusion for ADR-001

1. V1 (`Application = NocteSlicer-...`) is not merely "degraded interoperability": it *crashes*
   Bambu Studio 02.08.02.61 on `--export-3mf`, and before that it discards the project settings,
   loses the filament identity, re-arranges the plate and bakes the instance transform into the
   mesh with float error.
2. V2 keeps every one of those: identical placement, identical settings, identical filament, and a
   clean re-export whose objects, parts, subtypes, per-part overrides, plates and instances all
   match (§7.3).
3. The gate is exactly the `BambuStudio-` **prefix** of the `Application` value. Everything after it
   is free, and a separate `<metadata name="NocteSlicer">` tag plus an extra archive entry
   (`Metadata/nocte_report.json`) are both invisible to Bambu Studio — confirmed by the suffix
   control above and by (c)/(e) passing on every corpus file.

## 9. The block 2 binary (2026-09-17, run 35241116400)

First build that writes the NØCTE identity and carries the CLI fixes. Artifact unpacked at
`C:\dev\nocte-builds\run-35241116400\`.

- **Corpus regenerated** with `make_corpus.ps1 -SlicerDir C:\dev\nocte-builds\run-35241116400 -Oracle`.
  `inspect3mf.py` reports, for all four `corpus/nocte-cli` files, `Application = BambuStudio-02.08.01.55`,
  `NocteSlicer = 0.1.0-dev` and `Metadata/nocte_report.json` present. The manifest records
  `slicer_build 35241116400`.
- **Oracle, Bambu Studio 02.08.02.61:** 4/4 `VERDICT: PASS` on (a)(c)(d)(e), `return_code 0`,
  `Success.`; (b)(f)(g) skipped for the reasons in §7.3. Subtypes and the per-part overrides survive
  Bambu Studio's re-export. This is the measurement §8.3 point 3 lacked: the files that were loaded,
  sliced and re-exported here do carry the tag and the extra archive entry.
- **`-24` is gone.** `orca-slicer.exe corpus/fdmhub/P4_FLUJO_A1.3mf --outputdir W --export-3mf out.3mf`
  (`Application = BambuStudio-02.08.02.60`, no `--allow-newer-file`): run 35180882793 exits `-24`
  and writes nothing; run 35241116400 exits `0` and writes `out.3mf`.
- **`-18` is only covered by the unit test** (`[NocteBblCompat]`): `P4_FLUJO_A1.3mf` does not carry
  the `-1` values, so no corpus file exercises `sanitize_bbl_config` through the CLI yet. A project
  saved by Bambu Studio 02.08.02 with tree supports on auto is the fixture to add.
- **No version resource in the exe.** `orca-slicer.exe` and `OrcaSlicer.dll` have empty
  ProductName / FileDescription / FileVersion in both this build and run 35180882793, so the
  `SLIC3R_APP_NAME` change is not visible in the file properties. The icon did change (the exe went
  from 278 kB to 79 kB with the new `.ico`). Inherited from upstream's clang-cl build; not
  investigated yet.
