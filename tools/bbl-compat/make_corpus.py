#!/usr/bin/env python3
r"""Generate the synthetic ``corpus/nocte-cli`` 3MF corpus with the NOCTE writer.

Why this exists
---------------
``corpus/bambu-studio/`` has to be authored by hand in Bambu Studio. This module
produces the *other* half: four small projects that exercise every part subtype
Bambu Studio knows, written **end to end by the NOCTE Slicer CLI**, so that the
harness can prove our reader and our writer round-trip subtypes and per-part
overrides and that Bambu Studio 02.08.02.61 still loads the result natively.

The CLI has no "add a modifier" switch, so each case is built in three stages:

A. **assemble** - ``--load-assemble-list <json>`` is the only CLI door to a 3MF
   with *one object made of several parts*: every entry of a plate that carries
   the same ``assemble_index`` > 0 is merged into one ``ModelObject`` whose
   volumes are the STLs (``merge_or_add_object`` in ``src/OrcaSlicer.cpp``).
   Every volume comes out as ``subtype="normal_part"``; the switch takes no
   volume type and no per-volume config.
B. **patch** - ``Metadata/model_settings.config`` inside the stage A archive is
   rewritten: each extra ``<part>`` gets its real ``subtype`` and its per-part
   ``<metadata key=... value=.../>`` overrides. Nothing else in the archive is
   touched, so the geometry stays exactly what the NOCTE writer produced.
C. **re-export** - the patched file is loaded back into the NOCTE CLI and
   written out again with ``--export-3mf``. The corpus entry is *that* file, so
   it is the NOCTE writer's own output and it proves reader+writer preserve the
   subtypes and the overrides. If stage C were to lose them, the stage B file is
   kept instead and ``written_by`` in ``<case>.expected.json`` says so.

Stage A and stage C both go through ``Start-Process``-equivalent plain
``subprocess`` calls; exit codes are normalised from Windows' unsigned form.

Usage::

    python make_corpus.py [--slicer-dir C:\dev\nocte-builds\run-<id>]
                          [--out corpus/nocte-cli] [--work _work/make_corpus]
                          [--bs-version 02.08.02.61] [--no-manifest] [--keep-work]

Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect3mf import inspect_3mf, sha256_file  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BBL_PROFILES = REPO / "resources" / "profiles" / "BBL"
BUILDS_ROOT = Path(r"C:\dev\nocte-builds")

DEFAULT_PRINTER = "Bambu Lab A1 0.4 nozzle"
DEFAULT_PROCESS = "0.20mm Standard @BBL A1"
DEFAULT_FILAMENT = "Bambu PLA Basic @BBL A1"
DEFAULT_BS_VERSION = "02.08.02.61"

#: Bed centre of the A1 (256 x 256). Parts are modelled around the origin and
#: placed here so Bambu Studio can actually slice the plate.
BED_CENTRE = (128.0, 128.0, 0.0)

MODEL_SETTINGS_ENTRY = "Metadata/model_settings.config"
NOCTE_TAG_NAME = "NocteSlicer"
NOCTE_REPORT_ENTRY = "Metadata/nocte_report.json"

#: The build from which the NOCTE tag and the project report are mandatory.
NOCTE_REQUIRED_FROM_BUILD = "block2"

STL_HEADER = b"NOCTE bbl-compat synthetic corpus"


# ---------------------------------------------------------------------------
# Tiny STL authoring (12 triangles per box, 684 bytes on disk)
# ---------------------------------------------------------------------------

def box_triangles(
    x0: float, y0: float, z0: float, x1: float, y1: float, z1: float
) -> list[tuple[tuple[float, float, float], ...]]:
    """Return the 12 outward-facing triangles of an axis aligned box."""
    v = [
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    ]
    faces = [
        (0, 2, 1), (0, 3, 2),   # bottom (-z)
        (4, 5, 6), (4, 6, 7),   # top    (+z)
        (0, 1, 5), (0, 5, 4),   # front  (-y)
        (1, 2, 6), (1, 6, 5),   # right  (+x)
        (2, 3, 7), (2, 7, 6),   # back   (+y)
        (3, 0, 4), (3, 4, 7),   # left   (-x)
    ]
    return [(v[a], v[b], v[c]) for a, b, c in faces]


def _normal(tri: tuple[tuple[float, float, float], ...]) -> tuple[float, float, float]:
    """Unit normal of one triangle (right-hand rule over its vertex order)."""
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    length = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
    return (nx / length, ny / length, nz / length)


def write_binary_stl(path: Path, tris: list[tuple[tuple[float, float, float], ...]]) -> None:
    """Write a deterministic binary STL (fixed header, no timestamps)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as fh:
        fh.write(STL_HEADER.ljust(80, b"\0")[:80])
        fh.write(struct.pack("<I", len(tris)))
        for tri in tris:
            fh.write(struct.pack("<3f", *_normal(tri)))
            for point in tri:
                fh.write(struct.pack("<3f", *point))
            fh.write(b"\0\0")


# ---------------------------------------------------------------------------
# The four cases
# ---------------------------------------------------------------------------

#: A part: (stl stem, box extents, subtype, per-part config overrides).
#: Only keys Bambu Studio itself keeps on re-export may appear in the overrides
#: (NOTES.md section 6: BS drops Orca-only keys). ``sparse_infill_density`` and
#: ``wall_loops`` are reported by Bambu Studio's own ``result.json``, so they are
#: unambiguously part of its dialect.
CASES: list[dict[str, Any]] = [
    {
        "name": "01_single_cube",
        "notes": "minimal baseline: one object, one normal_part, one plate, one instance",
        "parts": [
            ("cube20", (-10, -10, 0, 10, 10, 20), "normal_part", {}),
        ],
    },
    {
        "name": "02_modifier_block",
        "notes": "cube plus an inner modifier_part carrying two per-part overrides",
        "parts": [
            ("cube20", (-10, -10, 0, 10, 10, 20), "normal_part", {}),
            (
                "modifier8",
                (-4, -4, 4, 4, 4, 12),
                "modifier_part",
                {"sparse_infill_density": "35%", "wall_loops": "5"},
            ),
        ],
    },
    {
        "name": "03_negative_part",
        "notes": "cube pierced by a negative_part bar (through hole along X)",
        "parts": [
            ("cube20", (-10, -10, 0, 10, 10, 20), "normal_part", {}),
            ("negative_bar", (-12, -3, 6, 12, 3, 12), "negative_part", {}),
        ],
    },
    {
        "name": "04_support_enforcer_blocker",
        "notes": "one object carrying a support_enforcer and a support_blocker",
        "parts": [
            ("cube20", (-10, -10, 0, 10, 10, 20), "normal_part", {}),
            ("enforcer6", (-9, -9, 0, -3, -3, 12), "support_enforcer", {}),
            ("blocker6", (3, 3, 0, 9, 9, 12), "support_blocker", {}),
        ],
    },
]


# ---------------------------------------------------------------------------
# Slicer invocation
# ---------------------------------------------------------------------------

def find_slicer_dir(explicit: str | None) -> Path:
    """Resolve the binary directory: the given one, or the newest ``run-*``."""
    if explicit:
        path = Path(explicit)
        if path.is_file():
            path = path.parent
        if not (path / "orca-slicer.exe").is_file():
            raise SystemExit(f"no orca-slicer.exe under {path}")
        return path
    if not BUILDS_ROOT.is_dir():
        raise SystemExit(f"builds root not found: {BUILDS_ROOT}; pass --slicer-dir")
    candidates = [
        p for p in BUILDS_ROOT.glob("run-*") if (p / "orca-slicer.exe").is_file()
    ]
    if not candidates:
        raise SystemExit(f"no run-*/orca-slicer.exe under {BUILDS_ROOT}")
    return max(candidates, key=lambda p: p.stat().st_mtime)


def build_id(slicer_dir: Path) -> str:
    """The CI run id, parsed from a ``run-<id>`` directory name."""
    match = re.fullmatch(r"run-(\d+)", slicer_dir.name)
    return match.group(1) if match else slicer_dir.name


def _normalize_exit_code(code: int) -> int:
    """Windows reports exit codes unsigned; map them back to the signed value."""
    return code - 0x100000000 if code > 0x7FFFFFFF else code


def run_cli(exe: Path, argv: list[str], *, timeout: int = 300) -> dict[str, Any]:
    """Run the NOCTE CLI headlessly and return exit code, output and duration."""
    cmd = [str(exe), *argv]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {"cmd": cmd, "exit_code": None, "timed_out": True, "stdout": "", "stderr": ""}
    return {
        "cmd": cmd,
        "exit_code": _normalize_exit_code(proc.returncode),
        "timed_out": False,
        "stdout": proc.stdout.decode("utf-8", "replace"),
        "stderr": proc.stderr.decode("utf-8", "replace"),
    }


def profile_args(printer: str, process: str, filament: str) -> list[str]:
    """``--load-settings`` / ``--load-filaments`` pointing at the source JSONs.

    Packaged builds ship ``resources/profiles/<Vendor>.opc`` bundles which the
    CLI cannot consume; the JSON profiles live in the source tree.
    """
    machine = BBL_PROFILES / "machine" / f"{printer}.json"
    proc = BBL_PROFILES / "process" / f"{process}.json"
    fil = BBL_PROFILES / "filament" / f"{filament}.json"
    for path in (machine, proc, fil):
        if not path.is_file():
            raise SystemExit(f"profile not found: {path}")
    return [
        "--load-settings", f"{machine};{proc}",
        "--load-filaments", str(fil),
    ]


# ---------------------------------------------------------------------------
# Stage A: assemble list -> one object with N parts
# ---------------------------------------------------------------------------

def write_assemble_list(path: Path, stl_paths: list[Path], plate_name: str) -> None:
    """Write the ``--load-assemble-list`` JSON that merges every STL into one object.

    ``assemble_index`` 1 on every entry is what makes ``construct_assemble_list``
    fold them into a single ``ModelObject``; ``pos_*`` puts that object on the
    bed centre. Written UTF-8 **without** a BOM: the CLI feeds the file straight
    to nlohmann/json, which rejects a BOM.
    """
    payload = {
        "plates": [
            {
                "plate_name": plate_name,
                "need_arrange": False,
                "objects": [
                    {
                        "path": str(stl),
                        "count": 1,
                        "filaments": [1],
                        "assemble_index": [1],
                        "pos_x": [BED_CENTRE[0]],
                        "pos_y": [BED_CENTRE[1]],
                        "pos_z": [BED_CENTRE[2]],
                    }
                    for stl in stl_paths
                ],
            }
        ]
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def stage_a(
    exe: Path, work: Path, case: dict[str, Any], stl_paths: list[Path],
    printer: str, process: str, filament: str,
) -> tuple[Path, dict[str, Any]]:
    """Run the CLI over an assemble list and return the produced 3MF."""
    out_dir = work / case["name"] / "stageA"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    list_json = work / case["name"] / "assemble_list.json"
    write_assemble_list(list_json, stl_paths, plate_name=case["name"])
    run = run_cli(
        exe,
        [
            "--debug", "2",
            "--load-assemble-list", str(list_json),
            *profile_args(printer, process, filament),
            "--outputdir", str(out_dir),
            # bare file name: an absolute path is concatenated onto --outputdir
            # and the export fails with -13.
            "--export-3mf", "stageA.3mf",
        ],
    )
    run["phase"] = "stageA"
    return out_dir / "stageA.3mf", run


# ---------------------------------------------------------------------------
# Stage B: patch subtypes and per-part overrides into model_settings.config
# ---------------------------------------------------------------------------

_PART_RE = re.compile(r'(<part\b[^>]*>)(.*?)(</part>)', re.DOTALL)
_NAME_RE = re.compile(r'<metadata key="name" value="([^"]*)"/>')


def patch_model_settings(text: str, case: dict[str, Any]) -> str:
    """Set each part's ``subtype`` and inject its per-part config overrides.

    Parts are matched by their ``name`` metadata (``<stl stem>_1``, written by
    the assemble path), not by position, so a reordering by the writer cannot
    silently mislabel a part.
    """
    wanted = {f"{stem}_1": (subtype, overrides) for stem, _, subtype, overrides in case["parts"]}
    seen: list[str] = []

    def replace_part(match: re.Match[str]) -> str:
        open_tag, body, close_tag = match.group(1), match.group(2), match.group(3)
        name_match = _NAME_RE.search(body)
        name = name_match.group(1) if name_match else ""
        if name not in wanted:
            raise SystemExit(
                f"{case['name']}: stage A produced an unexpected part name {name!r}; "
                f"expected one of {sorted(wanted)}"
            )
        seen.append(name)
        subtype, overrides = wanted[name]
        open_tag = re.sub(r'subtype="[^"]*"', f'subtype="{subtype}"', open_tag)
        if overrides:
            injected = "".join(
                f'      <metadata key="{key}" value="{value}"/>\n'
                for key, value in overrides.items()
            )
            # keep <mesh_stat> last, exactly where the NOCTE writer puts it
            body = body.replace("      <mesh_stat", injected + "      <mesh_stat", 1)
        return open_tag + body + close_tag

    patched = _PART_RE.sub(replace_part, text)
    missing = sorted(set(wanted) - set(seen))
    if missing:
        raise SystemExit(f"{case['name']}: stage A did not produce parts {missing}")
    # give the object the case name instead of the generic "assemble_1"
    patched = patched.replace(
        '<metadata key="name" value="assemble_1"/>',
        f'<metadata key="name" value="{case["name"]}"/>',
        1,
    )
    return patched


def stage_b(src: Path, dst: Path, case: dict[str, Any]) -> None:
    """Copy the archive, rewriting only ``Metadata/model_settings.config``."""
    with zipfile.ZipFile(src) as zin:
        items = [(info, zin.read(info.filename)) for info in zin.infolist()]
    dst.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for info, data in items:
            if info.filename == MODEL_SETTINGS_ENTRY:
                data = patch_model_settings(data.decode("utf-8"), case).encode("utf-8")
            entry = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            entry.compress_type = info.compress_type
            entry.external_attr = info.external_attr
            zout.writestr(entry, data)


# ---------------------------------------------------------------------------
# Stage C: NOCTE reader -> NOCTE writer
# ---------------------------------------------------------------------------

def stage_c(exe: Path, work: Path, case: dict[str, Any], src: Path) -> tuple[Path, dict[str, Any]]:
    """Load the patched 3MF back into the CLI and re-export it."""
    out_dir = work / case["name"] / "stageC"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    run = run_cli(
        exe,
        [
            "--debug", "2",
            # the version gate compares SoftFever_VERSION (2.5.x) against the
            # Bambu Studio version in the Application string (2.8.x) and rejects
            # our own file with -24; see NOTES.md section 6.
            "--allow-newer-file",
            "--outputdir", str(out_dir),
            "--export-3mf", "stageC.3mf",
            str(src),
        ],
    )
    run["phase"] = "stageC"
    return out_dir / "stageC.3mf", run


# ---------------------------------------------------------------------------
# Verification + expected.json
# ---------------------------------------------------------------------------

def expected_subtypes(case: dict[str, Any]) -> dict[str, int]:
    """The ``subtype -> count`` map the case must show."""
    counts: dict[str, int] = {}
    for _, _, subtype, _ in case["parts"]:
        counts[subtype] = counts.get(subtype, 0) + 1
    return counts


def check_case(summary: dict[str, Any], case: dict[str, Any]) -> list[str]:
    """Return the list of structural expectations a summary fails to meet."""
    problems: list[str] = []
    ms = summary.get("model_settings") or {}
    if not ms.get("present"):
        return ["model_settings.config absent"]
    if ms.get("object_count") != 1:
        problems.append(f"object_count {ms.get('object_count')} != 1")
    if ms.get("part_count") != len(case["parts"]):
        problems.append(f"part_count {ms.get('part_count')} != {len(case['parts'])}")
    if ms.get("subtype_counts") != expected_subtypes(case):
        problems.append(
            f"subtypes {ms.get('subtype_counts')} != {expected_subtypes(case)}"
        )
    parts = (ms.get("objects") or [{}])[0].get("parts", [])
    by_name = {p["metadata"].get("name"): p for p in parts}
    for stem, _, subtype, overrides in case["parts"]:
        part = by_name.get(f"{stem}_1")
        if part is None:
            problems.append(f"part {stem}_1 missing (names: {sorted(by_name)})")
            continue
        if part.get("subtype") != subtype:
            problems.append(f"part {stem}_1 subtype {part.get('subtype')} != {subtype}")
        for key, value in overrides.items():
            if key not in part["metadata"]:
                problems.append(f"part {stem}_1 lost override {key}")
            elif part["metadata"][key] != value:
                problems.append(
                    f"part {stem}_1 override {key}={part['metadata'][key]!r} != {value!r}"
                )
    if not summary.get("application_prefix_ok"):
        problems.append(f"Application {summary.get('application')!r} is not native")
    return problems


def nocte_tag_of(summary: dict[str, Any]) -> str | None:
    """Value of ``<metadata name="NocteSlicer">`` in 3D/3dmodel.model, if any."""
    return summary.get("nocte_tag")


def nocte_report_in(summary: dict[str, Any]) -> bool:
    """True when the archive carries ``Metadata/nocte_report.json``."""
    return bool(summary.get("nocte_report_present"))


def build_expected(
    case: dict[str, Any],
    summary: dict[str, Any],
    *,
    written_by: str,
    slicer_build: str,
    bs_version: str,
    stl_names: list[str],
    findings: list[str],
) -> dict[str, Any]:
    """Assemble the ``<case>.expected.json`` payload from the produced file."""
    ms = summary["model_settings"]
    parts = ms["objects"][0]["parts"]
    file_name = f"{case['name']}.3mf"
    required_keys = {
        f"{stem}_1": sorted(overrides) for stem, _, _, overrides in case["parts"]
    }
    return {
        "case": case["name"],
        "notes": case["notes"],
        "written_by": written_by,
        "generator": "tools/bbl-compat/make_corpus.ps1",
        "slicer_build": slicer_build,
        "authored_with": summary.get("application"),
        "bambu_studio_oracle": bs_version,
        "sources": stl_names,
        "findings": findings,
        file_name: {
            "predicted_native": True,
            "application_prefix": "BambuStudio-",
            "object_count": 1,
            "part_count": len(case["parts"]),
            "subtypes": expected_subtypes(case),
            "part_subtypes_in_order": [p["subtype"] for p in parts],
            "part_names_in_order": [p["metadata"].get("name") for p in parts],
            "part_config_keys_required": required_keys,
            "part_config_values_required": {
                f"{stem}_1": dict(overrides) for stem, _, _, overrides in case["parts"]
            },
            "plater_ids": ms.get("plater_ids"),
            "model_instance_count": ms.get("model_instance_count"),
            "paint_attrs_present": summary.get("paint_attrs_present") or [],
            "is_sliced": summary.get("is_sliced"),
            "entries_required": [
                "3D/3dmodel.model",
                "Metadata/model_settings.config",
                "Metadata/project_settings.config",
            ],
            "nocte_tag": {
                "metadata_name": NOCTE_TAG_NAME,
                "in": "3D/3dmodel.model",
                "required_from_build": NOCTE_REQUIRED_FROM_BUILD,
                "present_in_this_file": nocte_tag_of(summary) is not None,
                "value_in_this_file": nocte_tag_of(summary),
            },
            "nocte_report": {
                "entry": NOCTE_REPORT_ENTRY,
                "required_from_build": NOCTE_REQUIRED_FROM_BUILD,
                "present_in_this_file": nocte_report_in(summary),
            },
        },
    }


# ---------------------------------------------------------------------------
# corpus_manifest.json
# ---------------------------------------------------------------------------

def update_manifest(
    corpus: Path, manifest_path: Path, *, slicer_build: str, bs_version: str
) -> int:
    """Rewrite ``corpus_manifest.json`` over the whole corpus, preserving origins."""
    old = json.loads(manifest_path.read_text(encoding="utf-8"))
    origins = {f["path"]: f.get("origin") for f in old.get("files", [])}
    files: list[dict[str, Any]] = []
    for path in sorted(corpus.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".3mf", ".stl"):
            continue
        rel = path.relative_to(corpus).as_posix()
        record: dict[str, Any] = {
            "path": rel,
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
            "origin": origins.get(rel),
        }
        if rel.startswith("nocte-cli/"):
            record["origin"] = "generated by tools/bbl-compat/make_corpus.ps1"
            record["slicer_build"] = slicer_build
        if path.suffix.lower() == ".3mf":
            summary = inspect_3mf(path)
            ms = summary["model_settings"]
            si = summary["slice_info"]
            record.update(
                application=summary["application"],
                predicted_native=summary["predicted_native"],
                is_sliced=summary["is_sliced"],
                objects=summary["object_count"],
                parts=ms.get("part_count"),
                subtypes=ms.get("subtype_counts"),
                plater_ids=ms.get("plater_ids"),
                model_instances=ms.get("model_instance_count"),
                paint_attrs=summary["paint_attrs_present"],
                ams_list_count=si.get("ams_list_count", 0),
            )
            if rel.startswith("nocte-cli/"):
                record["nocte_tag"] = nocte_tag_of(summary)
                record["nocte_report"] = nocte_report_in(summary)
        files.append(record)
    old.update(
        bambu_studio_version=bs_version,
        nocte_cli_slicer_build=slicer_build,
        generated=datetime.datetime.now().isoformat(timespec="seconds"),
        file_count=len(files),
        files=files,
    )
    manifest_path.write_text(
        json.dumps(old, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return len(files)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def generate(args: argparse.Namespace) -> int:
    """Generate every case; returns a process exit code."""
    slicer_dir = find_slicer_dir(args.slicer_dir)
    exe = slicer_dir / "orca-slicer.exe"
    run_id = build_id(slicer_dir)
    out_root = Path(args.out).resolve()
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    print(f"slicer   : {exe}")
    print(f"build id : {run_id}")
    print(f"output   : {out_root}")
    print(f"work     : {work}")

    failures = 0
    for case in CASES:
        name = case["name"]
        print(f"\n=== {name} ===")
        case_dir = out_root / name
        case_dir.mkdir(parents=True, exist_ok=True)

        stl_paths: list[Path] = []
        for stem, extents, _, _ in case["parts"]:
            stl = case_dir / f"{stem}.stl"
            write_binary_stl(stl, box_triangles(*extents))
            stl_paths.append(stl)
        print(f"  stage A: {len(stl_paths)} STL -> assemble list -> CLI")

        a_path, a_run = stage_a(
            exe, work, case, stl_paths, args.printer, args.process, args.filament
        )
        if a_run["exit_code"] != 0 or not a_path.is_file():
            print(f"  FAILED stage A: exit={a_run['exit_code']} {a_run['stderr'][-400:]}")
            failures += 1
            continue
        a_summary = inspect_3mf(a_path)
        print(
            f"           -> {a_path.name} objects={a_summary['model_settings']['object_count']} "
            f"parts={a_summary['model_settings']['part_count']} "
            f"subtypes={a_summary['model_settings']['subtype_counts']}"
        )

        b_path = work / name / "stageB.3mf"
        stage_b(a_path, b_path, case)
        b_summary = inspect_3mf(b_path)
        b_problems = check_case(b_summary, case)
        print(
            f"  stage B: patched subtypes={b_summary['model_settings']['subtype_counts']}"
            + (f"  PROBLEMS {b_problems}" if b_problems else "")
        )
        if b_problems:
            print("  FAILED stage B")
            failures += 1
            continue

        c_path, c_run = stage_c(exe, work, case, b_path)
        findings: list[str] = []
        final_src = c_path
        written_by = "nocte-cli"
        if c_run["exit_code"] != 0 or not c_path.is_file():
            findings.append(
                f"stage C (NOCTE reader->writer) failed with exit={c_run['exit_code']}; "
                "the corpus entry is the stage B file"
            )
            final_src = b_path
            written_by = "nocte-cli+patch"
            print(f"  stage C: FAILED exit={c_run['exit_code']}")
        else:
            c_summary = inspect_3mf(c_path)
            c_problems = check_case(c_summary, case)
            if c_problems:
                findings.append(
                    "stage C round trip lost: " + "; ".join(c_problems)
                    + " (the corpus entry is the stage B file)"
                )
                final_src = b_path
                written_by = "nocte-cli+patch"
                print(f"  stage C: round trip LOST {c_problems}")
            else:
                print(
                    "  stage C: round trip preserved subtypes "
                    f"{c_summary['model_settings']['subtype_counts']} and every override"
                )

        final = case_dir / f"{name}.3mf"
        shutil.copyfile(final_src, final)
        summary = inspect_3mf(final)
        expected = build_expected(
            case,
            summary,
            written_by=written_by,
            slicer_build=run_id,
            bs_version=args.bs_version,
            stl_names=[p.name for p in stl_paths],
            findings=findings,
        )
        (case_dir / f"{name}.expected.json").write_text(
            json.dumps(expected, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        tag = nocte_tag_of(summary)
        print(
            f"  wrote    {final.relative_to(out_root.parent.parent)} "
            f"({final.stat().st_size} B) written_by={written_by} "
            f"NocteSlicer tag={tag!r} nocte_report={nocte_report_in(summary)}"
        )

    if not args.no_manifest:
        count = update_manifest(
            out_root.parent,
            HERE / "corpus_manifest.json",
            slicer_build=run_id,
            bs_version=args.bs_version,
        )
        print(f"\ncorpus_manifest.json rewritten with {count} files")

    if not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)

    total = sum(
        (out_root / c["name"] / f"{c['name']}.3mf").stat().st_size
        for c in CASES
        if (out_root / c["name"] / f"{c['name']}.3mf").is_file()
    )
    print(f"corpus/nocte-cli 3MF bytes: {total}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Generate the synthetic corpus/nocte-cli 3MF corpus."
    )
    parser.add_argument(
        "--slicer-dir",
        help="directory holding orca-slicer.exe (default: newest "
        r"C:\dev\nocte-builds\run-* that contains it)",
    )
    parser.add_argument("--out", default=str(HERE / "corpus" / "nocte-cli"))
    parser.add_argument("--work", default=str(HERE / "_work" / "make_corpus"))
    parser.add_argument("--printer", default=DEFAULT_PRINTER)
    parser.add_argument("--process", default=DEFAULT_PROCESS)
    parser.add_argument("--filament", default=DEFAULT_FILAMENT)
    parser.add_argument("--bs-version", default=DEFAULT_BS_VERSION)
    parser.add_argument(
        "--no-manifest", action="store_true", help="do not rewrite corpus_manifest.json"
    )
    parser.add_argument(
        "--keep-work", action="store_true", help="keep the intermediate stage A/B/C files"
    )
    return generate(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
