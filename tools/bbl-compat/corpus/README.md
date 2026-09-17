# bbl-compat corpus

Test material for the Bambu Studio 3MF compatibility harness in `tools/bbl-compat`.
Everything here is input data: no tool in this directory writes into the corpus.

Two independent halves:

| directory | origin | what it proves |
| --- | --- | --- |
| `bambu-studio/` | authored by hand in Bambu Studio **02.08.02.61** | that NØCTE Slicer reads every feature Bambu Studio can write |
| `fdmhub/` | copied verbatim from the FDM-HUB production tree | that NØCTE Slicer does not regress on files that already exist in production |

`../corpus_manifest.json` pins the whole inventory (`bambu_studio_version`,
`orca_base`, and one record per file with its `sha256`).

---

## 1. `bambu-studio/` — the authored reference corpus

Layout, one directory per case:

```
corpus/bambu-studio/NN_<name>/
    project.3mf         required  - saved with "File > Save project as..."
    sliced.gcode.3mf    optional  - the same project after "Slice plate" + export
    expected.json       required  - manifest of what the harness must find
```

The directories already exist (each with a `.gitkeep`); the `.3mf` files have to
be **authored manually in Bambu Studio 02.08.02.61**, because nothing else can
produce a genuinely native file: Bambu Studio only takes its native import path
when `3D/3dmodel.model` carries
`<metadata name="Application">BambuStudio-…</metadata>`. A file written by any
other slicer is loaded as geometry only, with instances split and instance
transforms baked into the mesh — which is exactly the failure mode this corpus
exists to detect, so it must never be how the reference files are made.

### The twelve cases

| directory | what to build in Bambu Studio |
| --- | --- |
| `01_single_cube` | one cube, default profile. The minimal baseline: one object, one `normal_part`, one plate, one `model_instance`. |
| `02_modifier_block` | a cube plus a child part set to **Modifier**, with at least one overridden setting on the modifier. Exercises `subtype="modifier_part"` and per-part `<metadata key>`. |
| `03_negative_part` | a cube with a child part set to **Negative part** boring a hole through it. Exercises `subtype="negative_part"`. |
| `04_support_enforcer_blocker` | one object carrying both a **Support enforcer** and a **Support blocker** child part. Exercises `support_enforcer` and `support_blocker` in the same object. |
| `05_mmu_painted_4ams` | an object colour-painted across **4 AMS slots**, sliced. Exercises `paint_color` on `<triangle>`, the filament map, and `<ams_list>` in `slice_info.config`. Add some support painting and a seam so `paint_supports` and `paint_seam` are present too. |
| `06_multiplate_3` | three plates, each with objects, at least one object placed as **several instances**. Exercises the `plater_id` set and the `<model_instance>` count — the canary for "Bambu Studio split my instances". |
| `07_per_object_settings` | two objects with different per-object overrides (layer height, wall loops, infill, a different filament/extruder). Exercises per-object `<metadata key>` in `model_settings.config`. |
| `08_cut_object` | an object cut with the **Cut tool** (keep both halves, with connectors). Exercises `Metadata/cut_information.xml` and the cut ids in `model_settings.config`. |
| `09_text_svg` | an object carrying an embossed **Text** feature and an imported **SVG** feature. Exercises the text/SVG metadata blobs Bambu Studio stores per part. |
| `10_multinozzle_h2d` | a project on an **H2D** profile using both nozzles, with parts assigned to different extruders, sliced. Exercises multi-nozzle `nozzle_diameter`, filament maps and `filament_sequence.json`. |
| `11_layer_ranges` | an object with **height-range modifiers** (two or three ranges with different layer heights). Exercises `Metadata/layer_config_ranges.xml`. |
| `12_custom_gcode_per_layer` | a sliced plate with **custom G-code / pause / filament change** markers on several layers. Exercises `Metadata/custom_gcode_per_layer.xml`. |

### `expected.json`

A hand-written manifest of the invariants for that case. Keys mirror the
`inspect3mf` summary so they can be compared field by field:

```json
{
  "case": "04_support_enforcer_blocker",
  "authored_with": "BambuStudio-02.08.02.61",
  "notes": "one cube, one support enforcer, one support blocker",
  "project.3mf": {
    "predicted_native": true,
    "object_count": 1,
    "part_count": 3,
    "subtypes": { "normal_part": 1, "support_enforcer": 1, "support_blocker": 1 },
    "plater_ids": ["1"],
    "model_instance_count": 1,
    "paint_attrs_present": [],
    "entries_required": ["3D/3dmodel.model", "Metadata/model_settings.config"]
  }
}
```

Record only what is genuinely invariant for the case. Anything volatile
(`version`, `name`, `from` in `project_settings.config`, `p:UUID` values,
timestamps) is ignored by the harness and must not appear here.

### Authoring checklist

1. Use Bambu Studio **02.08.02.61** exactly. A file saved by a newer build makes
   the harness expect a "newer version" warning (criterion *d*).
2. Keep the meshes tiny — a corpus file over a few hundred kB slows every run and
   the cases are about structure, not geometry.
3. Save `project.3mf` **before** slicing, then slice and export
   `sliced.gcode.3mf` so the pair differs only by the slice data.
4. Strip anything personal: no printer bound in the project, no AMS serials, no
   project name carrying a customer name. `05_mmu_painted_4ams` needs AMS *slots*,
   not a real printer.
5. Confirm the file before committing it:
   ```
   python tools/bbl-compat/inspect3mf.py corpus/bambu-studio/NN_case/project.3mf
   ```
   then fill `expected.json` from what it prints.

---

## 2. `fdmhub/` — the production regression corpus

Real files copied (never moved) out of `C:\Users\yosef_torres\Documents\FDM-HUB`.
See `fdmhub/SOURCES.md` for the per-file origin and description.

These are *not* feature-complete: they are mostly single-object calibration
probes and printer-bound `*.gcode.3mf` exports. Their value is that they are real,
that several were written by an older build (`BambuStudio-02.04.00.70`) and so
exercise the version-skew path, and that some `*.gcode.3mf` files carry slice data
with **no geometry at all** (`3D/Objects/` absent), which is a shape the loader
must survive.

Nothing in `fdmhub/` may contain credentials. The staging pass scanned every
copied archive's textual entries for `access_code`, `codigo`, `bblp` and bare
8-digit numbers; see `fdmhub/SOURCES.md` for the result. Re-run that scan after
adding a file.

---

## Running the harness over the corpus

```bash
# fast, no slicer involved: only the static criteria are decided
python tools/bbl-compat/roundtrip.py --static-only --bs-version 02.08.02.61

# launch Bambu Studio once per file (--info only)
python tools/bbl-compat/roundtrip.py --bs-version 02.08.02.61

# the full round trip: slice, re-export, structural diff (slow)
python tools/bbl-compat/roundtrip.py --slice --bs-version 02.08.02.61
```

Reports land in `tools/bbl-compat/report/summary.json` and `summary.md`.

## Regenerating `corpus_manifest.json`

Run from `tools/bbl-compat/` after adding, replacing or removing any corpus file:

```bash
python -c "
import datetime, json, pathlib
from inspect3mf import inspect_3mf, sha256_file
corpus = pathlib.Path('corpus')
old = json.loads(pathlib.Path('corpus_manifest.json').read_text(encoding='utf-8'))
origins = {f['path']: f.get('origin') for f in old['files']}
files = []
for p in sorted(corpus.rglob('*')):
    if not p.is_file() or p.suffix.lower() not in ('.3mf', '.stl'):
        continue
    rel = p.relative_to(corpus).as_posix()
    rec = {'path': rel, 'size': p.stat().st_size, 'sha256': sha256_file(p),
           'origin': origins.get(rel)}
    if p.suffix.lower() == '.3mf':
        s = inspect_3mf(p); ms = s['model_settings']; si = s['slice_info']
        rec.update(application=s['application'], predicted_native=s['predicted_native'],
                   is_sliced=s['is_sliced'], objects=s['object_count'],
                   parts=ms.get('part_count'), subtypes=ms.get('subtype_counts'),
                   plater_ids=ms.get('plater_ids'),
                   model_instances=ms.get('model_instance_count'),
                   paint_attrs=s['paint_attrs_present'],
                   ams_list_count=si.get('ams_list_count', 0))
    files.append(rec)
old.update(generated=datetime.datetime.now().isoformat(timespec='seconds'),
           file_count=len(files), files=files)
pathlib.Path('corpus_manifest.json').write_text(
    json.dumps(old, indent=2, ensure_ascii=False), encoding='utf-8')
print('wrote corpus_manifest.json with', len(files), 'files')
"
```

`bambu_studio_version` and `orca_base` are edited by hand, only when the pinned
Bambu Studio build or the OrcaSlicer base tag actually changes.
