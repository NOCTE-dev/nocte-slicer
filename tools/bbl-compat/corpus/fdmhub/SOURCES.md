# `corpus/fdmhub` — provenance

Real production files **copied** (never moved) out of the FDM-HUB tree on
2026-09-16. Origin root: `C:\Users\yosef_torres\Documents\FDM-HUB`.

All of them were written by Bambu Studio, so all are predicted *native*
(`Application` starts with `BambuStudio-`). Sizes and `sha256` are pinned in
`../../corpus_manifest.json`.

## Project files (geometry, no slice data)

| file | origin (relative to FDM-HUB) | written by | description |
| --- | --- | --- | --- |
| `P1_TORRE_PLA_A1.3mf` | `calibracion/probetas/P1_TORRE_PLA_A1.3mf` | 02.08.02.60 | Temperature tower probe, A1 profile. 1 object / 1 `normal_part` / 1 plate. Largest of the probe projects (106 kB). |
| `P1_TORRE_PLA_H2D.3mf` | `calibracion/probetas/P1_TORRE_PLA_H2D.3mf` | 02.08.02.60 | Same tower on an H2D profile — the A1/H2D pair is the printer-profile skew case. |
| `P2_HUMEDAD_PETG_w1_A1.3mf` | `calibracion/probetas/P2_HUMEDAD_PETG_w1_A1.3mf` | 02.08.02.60 | Filament-moisture witness probe, PETG, single-wall variant. |
| `P3_RETRACCION_0p6_A1.3mf` | `calibracion/probetas/P3_RETRACCION_0p6_A1.3mf` | 02.08.02.60 | Retraction tower for a 0.6 mm nozzle. |
| `P4_FLUJO_A1.3mf` | `calibracion/probetas/P4_FLUJO_A1.3mf` | 02.08.02.60 | Single-wall flow probe. Smallest project in the corpus (12.5 kB) — the default pick for a quick oracle run. |
| `P5_PRIMERA_CAPA_H2D.3mf` | `calibracion/probetas/P5_PRIMERA_CAPA_H2D.3mf` | 02.08.02.60 | First-layer patch, H2D profile. |
| `P6_FLUJO_PETG_1p00_A1.3mf` | `calibracion/probetas/P6_FLUJO_PETG_1p00_A1.3mf` | 02.08.02.60 | Flow-ratio sweep member (PETG, ratio 1.00). |
| `P7_SOPORTE_PLA_arbol_0p20_A1.3mf` | `calibracion/probetas/P7_SOPORTE_PLA_arbol_0p20_A1.3mf` | 02.08.02.60 | Tree-support probe, 0.20 mm layer — carries support-related per-object overrides. |
| `P8_PLANCHA_70_CRYOGRIP_PETG_A1.3mf` | `calibracion/probetas/P8_PLANCHA_70_CRYOGRIP_PETG_A1.3mf` | 02.08.02.60 | Bed-adhesion patch, Cryogrip plate, PETG, 70 °C. |
| `P10_SOPORTE_HIBRIDO_A1.3mf` | `calibracion/probetas/P10_SOPORTE_HIBRIDO_A1.3mf` | 02.08.02.60 | Hybrid-support probe. |
| `PRUEBA_FDMHUB.3mf` | `informes/PRUEBA_FDMHUB.3mf` | 02.08.02.60 | The FDM-HUB end-to-end test part. The only file here that uses the **production extension**: geometry lives in `3D/Objects/object_1.model` and `3D/3dmodel.model` references it through `p:path`, so the object count (2) is split across two `.model` parts. Also carries `cut_information.xml` and `filament_sequence.json`. |
| `_PERFIL_ADD1300.3mf` | `calibracion/perfiles/_PERFIL_ADD1300.3mf` | 02.08.02.60 | **Settings-only** project: `project_settings.config` + `model_settings.config` with a plate but **no geometry at all** (no `<object>`, no thumbnails). The degenerate-input case. |

## Sliced exports (`*.gcode.3mf`, printer-bound)

These are what Bambu Studio sends to a printer: slice data, thumbnails,
`plate_N.gcode` + its `plate_N.gcode.md5`, and in most cases **no geometry**
(`3D/Objects/` absent, `model_settings.config` has plates but no objects). A
loader that assumes every 3MF has meshes breaks on exactly these.

| file | origin (relative to FDM-HUB) | written by | description |
| --- | --- | --- | --- |
| `plantilla.gcode.3mf` | `informes/plantilla.gcode.3mf` | **02.04.00.70** | The FDM-HUB report template export, 671 kB with a 2.6 MB embedded G-code. Written by an **older Bambu Studio**, so it is the version-skew regression case. Also has `Metadata/_rels/model_settings.config.rels`, `cut_information.xml` and two `filament_settings_N.config` parts. |
| `PRUEBA_FDMHUB.gcode.3mf` | `informes/PRUEBA_FDMHUB.gcode.3mf` | **02.04.00.70** | Sliced counterpart of `PRUEBA_FDMHUB.3mf`, also from the older build. |
| `ADD-1281-Locker_ID_Tag_Rev.00_plate_1.gcode.3mf` | `datos/gcode/A4/ADD-1281-Locker_ID_Tag_Rev.00_plate_1.gcode.3mf` | 02.08.02.60 | Real production job (locker ID tags). The only multi-plate file in the corpus: `plater_id` 1, 2 and 3 in `model_settings.config` but only plate 1's G-code exported. 1.8 MB. |
| `P4_FLUJO_A1.gcode.3mf` | `datos/gcode/A4/P4_FLUJO_A1.gcode.3mf` | **02.04.00.70** | Sliced flow probe from the older build; mixes a 02.04 `3dmodel.model` with a 02.08 `plate_1.gcode`. |
| `P9_BARRIDO_Z_PETG_A1.gcode.3mf` | `datos/gcode/A4/P9_BARRIDO_Z_PETG_A1.gcode.3mf` | 02.08.02.60 | Z-offset sweep, pass 1. |
| `P9_BARRIDO_Z2_PETG_A1.gcode.3mf` | `datos/gcode/A4/P9_BARRIDO_Z2_PETG_A1.gcode.3mf` | 02.08.02.60 | Z-offset sweep, pass 2. |
| `P9_BARRIDO_Z3_PETG_A1.gcode.3mf` | `datos/gcode/A4/P9_BARRIDO_Z3_PETG_A1.gcode.3mf` | 02.08.02.60 | Z-offset sweep, pass 3. The three passes are near-identical, which makes them the cheap `structdiff` fixture. |

## Source meshes

Copied when the probe's STL could be identified next to its `.3mf`; they let a
test rebuild a project from scratch instead of only re-reading one.

| file | origin (relative to FDM-HUB) | description |
| --- | --- | --- |
| `P3_RETRACCION_0p6.stl` | `calibracion/probetas/P3_RETRACCION_0p6.stl` | Mesh behind `P3_RETRACCION_0p6_A1.3mf`. |
| `P5_PRIMERA_CAPA.stl` | `calibracion/probetas/P5_PRIMERA_CAPA.stl` | Mesh behind `P5_PRIMERA_CAPA_H2D.3mf`. 3 kB, the smallest mesh available. |
| `P8_PLANCHA_70.stl` | `calibracion/probetas/P8_PLANCHA_70.stl` | Mesh behind `P8_PLANCHA_70_CRYOGRIP_PETG_A1.3mf`. 684 B. |
| `P10_SOPORTE_HIBRIDO.stl` | `calibracion/probetas/P10_SOPORTE_HIBRIDO.stl` | Mesh behind `P10_SOPORTE_HIBRIDO_A1.3mf`. |
| `PRUEBA_FDMHUB.stl` | `informes/PRUEBA_FDMHUB.stl` | Mesh behind `PRUEBA_FDMHUB.3mf`. |

No STL was found for `P1`, `P2`, `P4`, `P6`, `P7` under a name derivable from the
project's, so none was copied for those.

## What was deliberately **not** copied

Never staged, at any point:

- `.env`, `.env.example`, `.credenciales.md`
- anything under `config/` with a `.json` extension
- anything matching `datos/clave_sesion*` or `datos/cli_sesion*`
- any file carrying a printer access code, LAN code, serial or session token

Also skipped while selecting:

- `datos/gcode/A4/ADD-1294-…Clamping_Fixture….gcode.3mf` and
  `auto_cali_for_user_param.gcode.gcode.3mf` — 0 bytes, not valid archives
- every `datos/gcode/A4/*.gcode.3mf` above 30 MB (none actually hit the cap; the
  largest candidate, `ADD-1354_GT02_CALIDAD.gcode.3mf` at 8.0 MB, was simply not
  among the smallest picks)
- the `camara-A4-*.jpg` / `*.png` photo material in `informes/`, irrelevant here

## Credential scan

Every copied `.3mf` was reopened and each textual entry (`.model`, `.config`,
`.xml`, `.json`, `.rels`, `.gcode`, `.md5`, `.txt`) searched for `access_code`,
`codigo`, `bblp` (case-insensitive) and for bare 8-digit numbers. A second pass
scanned the raw bytes of **every** entry for `bblp|access|passw|secret|token|
dev_id|device_id|printer_serial`, IPv4 addresses and MAC addresses.

Result: **0 keyword matches, 0 credentials, 0 files removed.** The 8-digit
matches that do occur are all benign and were reviewed individually:

- firmware/profile date stamps inside `change_filament_gcode` and machine G-code
  (`20250206`, `20251031`, `20260513`);
- vertex coordinate digit runs inside `3D/Objects/object_1.model`;
- `error_code="10018003"` on a `<warning>` in `slice_info.config`;
- `<metadata name="DesignProfileId">87228090</metadata>` in `PRUEBA_FDMHUB.3mf`
  — a public MakerWorld design id, not a secret.

The IPv4 regex matched only Bambu Studio version strings (`02.08.02.60`,
`02.04.00.70`, `1.10.1.50`).

**Re-run the scan after adding any file here**, e.g.:

```bash
python -c "
import glob, re, zipfile
kw = re.compile(rb'access_code|codigo|bblp|passw|secret|token|dev_id|printer_serial', re.I)
d8 = re.compile(rb'(?<!\d)\d{8}(?!\d)')
for f in sorted(glob.glob('*.3mf')):
    with zipfile.ZipFile(f) as z:
        for n in z.namelist():
            data = z.read(n)
            for m in kw.finditer(data):
                print('KEYWORD', f, n, m.group(0))
            for m in d8.finditer(data):
                ctx = data[max(0, m.start()-60):m.end()+60]
                if re.search(rb'access|passw|token|secret|serial|clave|lan_', ctx, re.I):
                    print('CODE?', f, n, m.group(0))
"
```
