# bbl-compat — observed Bambu Studio CLI behaviour

Harness for checking that NØCTE Slicer stays byte-for-structure compatible with
Bambu Studio's 3MF dialect.

| file | role |
| --- | --- |
| `inspect3mf.py` | library + CLI: static summary of one 3MF |
| `structdiff.py` | library + CLI: structural diff of two 3MF |
| `oracle.py` | library + CLI: run Bambu Studio against one 3MF, verdict on criteria (a)–(g) |
| `roundtrip.py` | CLI: run the oracle over the whole corpus, write `report/summary.{json,md}` |
| `corpus/` | test material — see `corpus/README.md` and `corpus/fdmhub/SOURCES.md` |
| `corpus_manifest.json` | pinned inventory (`bambu_studio_version`, `orca_base`, sha256 per file) |

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
