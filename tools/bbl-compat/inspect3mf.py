#!/usr/bin/env python3
"""Static inspection of 3MF project files for the NOCTE Slicer / Bambu Studio
compatibility harness.

The module opens a ``.3mf`` archive without any slicer involved and builds a
JSON-serialisable summary of everything the harness needs in order to reason
about round-trip fidelity:

* the archive entry list (name, uncompressed size, CRC),
* ``3D/3dmodel.model`` metadata (notably the ``Application`` string, which is
  what decides whether Bambu Studio treats the file as *native*),
* objects / components / build items with their transforms, including objects
  stored in split ``3D/Objects/object_N.model`` files reached through the
  production extension ``p:path`` attribute,
* per-object parts taken from ``Metadata/model_settings.config`` with their
  ``subtype``, ``uuid`` presence and per-part ``<metadata key=...>`` names and
  values,
* plates (``plater_id``) and their ``<model_instance>`` entries, which is how we
  detect that Bambu Studio split a multi-instance object,
* the ``Metadata/project_settings.config`` key inventory,
* a ``Metadata/slice_info.config`` summary with filaments, ``tray_info_idx`` and
  ``ams_list``,
* MD5 verification of ``Metadata/plate_N.gcode`` against
  ``Metadata/plate_N.gcode.md5``,
* triangle paint attributes (``paint_color``, ``paint_supports``, ``paint_seam``,
  ``paint_fuzzy_skin``, ``face_property``) reduced to order-independent digests.

Usage as a CLI::

    python inspect3mf.py file.3mf [--json] [--out summary.json]

Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CORE_NS = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
PRODUCTION_NS = "http://schemas.bambulab.com/package/2021"
PRODUCTION_NS_ALT = "http://schemas.microsoft.com/3dmanufacturing/production/2015/06"
SLIC3RPE_NS = "http://schemas.slic3r.org/3mf/2017/06"

MODEL_ENTRY = "3D/3dmodel.model"
MODEL_SETTINGS_ENTRY = "Metadata/model_settings.config"
PROJECT_SETTINGS_ENTRY = "Metadata/project_settings.config"
SLICE_INFO_ENTRY = "Metadata/slice_info.config"
CUT_INFORMATION_ENTRY = "Metadata/cut_information.xml"
CUSTOM_GCODE_ENTRY = "Metadata/custom_gcode_per_layer.xml"
LAYER_RANGES_ENTRY = "Metadata/layer_config_ranges.xml"
FILAMENT_SEQUENCE_ENTRY = "Metadata/filament_sequence.json"

NATIVE_APPLICATION_PREFIX = "BambuStudio-"

#: Triangle attributes that carry painting / per-face state.
PAINT_ATTRS = (
    "paint_color",
    "paint_supports",
    "paint_seam",
    "paint_fuzzy_skin",
    "face_property",
)

#: Known part subtypes used by Bambu Studio and OrcaSlicer.
KNOWN_SUBTYPES = (
    "normal_part",
    "negative_part",
    "modifier_part",
    "support_enforcer",
    "support_blocker",
)

#: ``project_settings.config`` keys whose value legitimately changes on every
#: save and which therefore must never be diffed.
VOLATILE_PROJECT_KEYS = ("version", "name", "from")

_WS_RE = re.compile(r"\s+")


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def localname(tag: str) -> str:
    """Return the local name of a possibly namespaced ElementTree tag."""
    if tag.startswith("{"):
        return tag.split("}", 1)[1]
    return tag


def attr(elem: ET.Element, name: str) -> str | None:
    """Fetch an attribute by local name, ignoring any namespace prefix."""
    value = elem.get(name)
    if value is not None:
        return value
    for key, val in elem.attrib.items():
        if localname(key) == name:
            return val
    return None


def norm_ws(text: str | None) -> str:
    """Collapse all whitespace runs to single spaces and strip the result."""
    if not text:
        return ""
    return _WS_RE.sub(" ", text).strip()


def digest_values(values: list[str]) -> str:
    """Order-independent digest of a list of strings (sorted, then sha256)."""
    h = hashlib.sha256()
    for value in sorted(values):
        h.update(value.encode("utf-8", "replace"))
        h.update(b"\x00")
    return h.hexdigest()


def sha256_file(path: Path) -> str:
    """Return the hex sha256 of a file read in 1 MiB chunks."""
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _parse_xml(data: bytes) -> ET.Element | None:
    """Parse XML bytes, returning None on malformed content."""
    try:
        return ET.fromstring(data)
    except ET.ParseError:
        return None


def _collect_namespaces(data: bytes) -> dict[str, str]:
    """Extract xmlns declarations from raw XML bytes (prefix -> uri)."""
    namespaces: dict[str, str] = {}
    text = data[:8192].decode("utf-8", "replace")
    for match in re.finditer(r'xmlns(?::([A-Za-z0-9_.-]+))?\s*=\s*"([^"]+)"', text):
        prefix = match.group(1) or ""
        namespaces[prefix] = match.group(2)
    return namespaces


# ---------------------------------------------------------------------------
# Paint attribute scanning
# ---------------------------------------------------------------------------

def scan_paint(root: ET.Element) -> dict[str, dict[str, Any]]:
    """Collect triangle paint attributes from a parsed ``.model`` tree.

    Returns a mapping ``attr -> {count, unique, digest, samples}``. The digest is
    order independent, so a re-export that reshuffles triangles still compares
    equal as long as the painted data itself survived.
    """
    buckets: dict[str, list[str]] = {name: [] for name in PAINT_ATTRS}
    for elem in root.iter():
        if localname(elem.tag) != "triangle":
            continue
        for name in PAINT_ATTRS:
            value = attr(elem, name)
            if value:
                buckets[name].append(value)
    result: dict[str, dict[str, Any]] = {}
    for name, values in buckets.items():
        if not values:
            continue
        result[name] = {
            "count": len(values),
            "unique": len(set(values)),
            "digest": digest_values(values),
            "samples": sorted(set(values))[:5],
        }
    return result


def _merge_paint(
    target: dict[str, dict[str, Any]], extra: dict[str, dict[str, Any]]
) -> None:
    """Merge a per-file paint summary into an archive-wide accumulator."""
    for name, info in extra.items():
        slot = target.setdefault(
            name,
            {"count": 0, "unique": 0, "digest": "", "samples": [], "_parts": []},
        )
        slot["count"] += info["count"]
        slot["_parts"].append(info["digest"])
        slot["samples"] = sorted(set(slot["samples"]) | set(info["samples"]))[:5]
        slot["unique"] = max(slot["unique"], info["unique"])


def _finalise_paint(target: dict[str, dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Fold the per-file digests of a merged paint summary into one digest."""
    for info in target.values():
        parts = info.pop("_parts", [])
        info["digest"] = digest_values(parts)
    return target


# ---------------------------------------------------------------------------
# 3dmodel.model parsing
# ---------------------------------------------------------------------------

def _parse_object(elem: ET.Element, source: str) -> dict[str, Any]:
    """Summarise one ``<object>`` element of a ``.model`` part."""
    obj: dict[str, Any] = {
        "source": source,
        "id": attr(elem, "id"),
        "type": attr(elem, "type"),
        "name": attr(elem, "name"),
        "p_uuid_present": attr(elem, "UUID") is not None,
        "has_mesh": False,
        "vertex_count": 0,
        "triangle_count": 0,
        "components": [],
    }
    for child in elem:
        local = localname(child.tag)
        if local == "mesh":
            obj["has_mesh"] = True
            for sub in child:
                sub_local = localname(sub.tag)
                if sub_local == "vertices":
                    obj["vertex_count"] = sum(
                        1 for v in sub if localname(v.tag) == "vertex"
                    )
                elif sub_local == "triangles":
                    obj["triangle_count"] = sum(
                        1 for t in sub if localname(t.tag) == "triangle"
                    )
        elif local == "components":
            for sub in child:
                if localname(sub.tag) != "component":
                    continue
                obj["components"].append(
                    {
                        "objectid": attr(sub, "objectid"),
                        "transform": norm_ws(attr(sub, "transform")),
                        "p_path": attr(sub, "path"),
                        "p_uuid_present": attr(sub, "UUID") is not None,
                    }
                )
    return obj


def parse_model(data: bytes, source: str) -> dict[str, Any]:
    """Parse a ``.model`` XML payload into a summary dict."""
    root = _parse_xml(data)
    if root is None:
        return {"source": source, "parse_error": True}
    namespaces = _collect_namespaces(data)
    summary: dict[str, Any] = {
        "source": source,
        "parse_error": False,
        "unit": attr(root, "unit"),
        "namespaces": namespaces,
        "has_production_ext": any(
            uri in (PRODUCTION_NS, PRODUCTION_NS_ALT) for uri in namespaces.values()
        ),
        "metadata": {},
        "objects": [],
        "items": [],
    }
    for elem in root:
        local = localname(elem.tag)
        if local == "metadata":
            summary["metadata"][attr(elem, "name") or ""] = norm_ws(elem.text)
        elif local == "resources":
            for res in elem:
                if localname(res.tag) == "object":
                    summary["objects"].append(_parse_object(res, source))
        elif local == "build":
            for item in elem:
                if localname(item.tag) != "item":
                    continue
                summary["items"].append(
                    {
                        "objectid": attr(item, "objectid"),
                        "transform": norm_ws(attr(item, "transform")),
                        "printable": attr(item, "printable"),
                        "p_path": attr(item, "path"),
                        "p_uuid_present": attr(item, "UUID") is not None,
                    }
                )
    summary["paint"] = scan_paint(root)
    return summary


# ---------------------------------------------------------------------------
# model_settings.config parsing
# ---------------------------------------------------------------------------

def parse_model_settings(data: bytes) -> dict[str, Any]:
    """Parse ``Metadata/model_settings.config`` into a summary dict."""
    root = _parse_xml(data)
    if root is None:
        return {"present": True, "parse_error": True}
    result: dict[str, Any] = {
        "present": True,
        "parse_error": False,
        "objects": [],
        "plates": [],
        "assemble_items": [],
    }
    for elem in root:
        local = localname(elem.tag)
        if local == "object":
            obj: dict[str, Any] = {"id": attr(elem, "id"), "metadata": {}, "parts": []}
            for child in elem:
                child_local = localname(child.tag)
                if child_local == "metadata":
                    if attr(child, "key") is None:
                        obj.setdefault("stats", {}).update({k: v for k, v in child.attrib.items()})
                        continue
                    obj["metadata"][attr(child, "key")] = attr(child, "value")
                elif child_local == "part":
                    part: dict[str, Any] = {
                        "id": attr(child, "id"),
                        "subtype": attr(child, "subtype"),
                        "uuid_present": attr(child, "uuid") is not None,
                        "metadata": {},
                        "mesh_stat": {},
                    }
                    for sub in child:
                        sub_local = localname(sub.tag)
                        if sub_local == "metadata":
                            if attr(sub, "key") is None:
                                part.setdefault("stats", {}).update({k: v for k, v in sub.attrib.items()})
                                continue
                            part["metadata"][attr(sub, "key")] = attr(sub, "value")
                        elif sub_local == "mesh_stat":
                            part["mesh_stat"] = dict(sub.attrib)
                    obj["parts"].append(part)
            result["objects"].append(obj)
        elif local == "plate":
            plate: dict[str, Any] = {
                "metadata": {},
                "model_instances": [],
                "filament_maps": [],
            }
            for child in elem:
                child_local = localname(child.tag)
                if child_local == "metadata":
                    if attr(child, "key") is None:
                        continue
                    plate["metadata"][attr(child, "key")] = attr(child, "value")
                elif child_local == "model_instance":
                    inst: dict[str, Any] = {}
                    for sub in child:
                        if localname(sub.tag) == "metadata":
                            if attr(sub, "key") is None:
                                continue
                            inst[attr(sub, "key")] = attr(sub, "value")
                    plate["model_instances"].append(inst)
                elif child_local == "filament_map":
                    plate["filament_maps"].append(dict(child.attrib))
            plate["plater_id"] = plate["metadata"].get("plater_id")
            result["plates"].append(plate)
        elif local == "assemble":
            for child in elem:
                result["assemble_items"].append(dict(child.attrib))

    subtype_counts: dict[str, int] = {}
    part_count = 0
    for obj in result["objects"]:
        for part in obj["parts"]:
            part_count += 1
            subtype = part["subtype"] or "<none>"
            subtype_counts[subtype] = subtype_counts.get(subtype, 0) + 1
    result["object_count"] = len(result["objects"])
    result["part_count"] = part_count
    result["subtype_counts"] = subtype_counts
    result["unknown_subtypes"] = sorted(
        s for s in subtype_counts if s not in KNOWN_SUBTYPES and s != "<none>"
    )
    result["plater_ids"] = [p.get("plater_id") for p in result["plates"]]
    result["model_instance_count"] = sum(
        len(p["model_instances"]) for p in result["plates"]
    )
    return result


# ---------------------------------------------------------------------------
# slice_info.config parsing
# ---------------------------------------------------------------------------

def parse_slice_info(data: bytes) -> dict[str, Any]:
    """Parse ``Metadata/slice_info.config`` into a summary dict."""
    root = _parse_xml(data)
    if root is None:
        return {"present": True, "parse_error": True, "has_ams_list": False}
    result: dict[str, Any] = {
        "present": True,
        "parse_error": False,
        "header": {},
        "plates": [],
    }
    for elem in root:
        local = localname(elem.tag)
        if local == "header":
            for child in elem:
                key = attr(child, "key") or localname(child.tag)
                result["header"][key] = attr(child, "value")
        elif local == "plate":
            plate: dict[str, Any] = {
                "metadata": {},
                "filaments": [],
                "objects": [],
                "ams_list": [],
                "warnings": [],
            }
            for child in elem:
                child_local = localname(child.tag)
                if child_local == "metadata":
                    if attr(child, "key") is None:
                        continue
                    plate["metadata"][attr(child, "key")] = attr(child, "value")
                elif child_local == "filament":
                    plate["filaments"].append(dict(child.attrib))
                elif child_local == "object":
                    plate["objects"].append(dict(child.attrib))
                elif child_local == "ams_list":
                    entry: dict[str, Any] = {
                        "attrib": dict(child.attrib),
                        "children": [],
                    }
                    for sub in child:
                        entry["children"].append(
                            {"tag": localname(sub.tag), "attrib": dict(sub.attrib)}
                        )
                    plate["ams_list"].append(entry)
                elif child_local == "warning":
                    plate["warnings"].append(dict(child.attrib))
            plate["index"] = plate["metadata"].get("index")
            plate["tray_info_idx"] = [f.get("tray_info_idx") for f in plate["filaments"]]
            result["plates"].append(plate)
    result["plate_count"] = len(result["plates"])
    result["has_ams_list"] = any(p["ams_list"] for p in result["plates"])
    result["ams_list_count"] = sum(len(p["ams_list"]) for p in result["plates"])
    result["filament_count"] = sum(len(p["filaments"]) for p in result["plates"])
    result["tray_info_idx_all"] = sorted(
        {idx for p in result["plates"] for idx in p["tray_info_idx"] if idx}
    )
    return result


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def inspect_3mf(path: str | Path) -> dict[str, Any]:
    """Inspect a ``.3mf`` file and return a JSON-serialisable summary dict.

    The function never raises for malformed content: problems are reported via
    the ``errors`` / ``warnings`` lists and per-section ``parse_error`` flags.
    """
    path = Path(path)
    summary: dict[str, Any] = {
        "path": str(path),
        "name": path.name,
        "size": path.stat().st_size if path.exists() else None,
        "errors": [],
        "warnings": [],
    }
    if not path.exists():
        summary["errors"].append("file does not exist")
        return summary
    summary["sha256"] = sha256_file(path)

    try:
        zf = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as exc:
        summary["errors"].append(f"not a readable zip archive: {exc}")
        return summary

    with zf:
        entries = []
        for info in zf.infolist():
            entries.append(
                {
                    "name": info.filename,
                    "size": info.file_size,
                    "compress_size": info.compress_size,
                    "crc": f"{info.CRC:08X}",
                    "is_dir": info.is_dir(),
                }
            )
        summary["entries"] = entries
        names = [e["name"] for e in entries]
        summary["entry_names"] = names
        name_set = set(names)

        def read(entry: str) -> bytes | None:
            """Read one archive entry, returning None when it is absent."""
            try:
                return zf.read(entry)
            except KeyError:
                return None

        # -- required OPC parts -------------------------------------------
        summary["has_content_types"] = "[Content_Types].xml" in name_set
        summary["has_rels"] = "_rels/.rels" in name_set
        if not summary["has_content_types"]:
            summary["warnings"].append("missing [Content_Types].xml")
        if not summary["has_rels"]:
            summary["warnings"].append("missing _rels/.rels")

        # -- 3D/3dmodel.model ---------------------------------------------
        model_bytes = read(MODEL_ENTRY)
        if model_bytes is None:
            summary["errors"].append(f"missing {MODEL_ENTRY}")
            summary["model"] = {"present": False}
        else:
            model = parse_model(model_bytes, MODEL_ENTRY)
            model["present"] = True
            summary["model"] = model

        # -- split object models ------------------------------------------
        sub_model_names = sorted(
            n for n in names if n.lower().endswith(".model") and n != MODEL_ENTRY
        )
        sub_models = []
        for entry in sub_model_names:
            data = read(entry)
            if data is None:
                continue
            sub_models.append(parse_model(data, entry))
        summary["sub_models"] = sub_models
        summary["sub_model_names"] = sub_model_names

        # -- aggregate geometry across every .model part -------------------
        all_models = []
        if summary.get("model", {}).get("present"):
            all_models.append(summary["model"])
        all_models.extend(sub_models)

        all_objects: list[dict[str, Any]] = []
        for model in all_models:
            all_objects.extend(model.get("objects", []))
        summary["objects"] = all_objects
        summary["object_count"] = len(all_objects)
        summary["mesh_object_count"] = sum(1 for o in all_objects if o["has_mesh"])
        summary["component_count"] = sum(len(o["components"]) for o in all_objects)
        summary["triangle_total"] = sum(o["triangle_count"] for o in all_objects)
        summary["vertex_total"] = sum(o["vertex_count"] for o in all_objects)
        root_items = summary.get("model", {}).get("items", []) or []
        summary["item_count"] = len(root_items)
        summary["item_transforms"] = [i["transform"] for i in root_items]

        merged_paint: dict[str, dict[str, Any]] = {}
        for model in all_models:
            _merge_paint(merged_paint, model.get("paint", {}))
        summary["paint"] = _finalise_paint(merged_paint)
        summary["paint_attrs_present"] = sorted(summary["paint"])
        summary["has_paint"] = bool(summary["paint"])

        # -- Application string / nativeness prediction --------------------
        metadata = summary.get("model", {}).get("metadata", {}) or {}
        application = metadata.get("Application")
        summary["application"] = application
        summary["application_prefix_ok"] = bool(
            application and application.startswith(NATIVE_APPLICATION_PREFIX)
        )
        # Bambu Studio only takes the native code path when Application starts
        # with "BambuStudio-"; anything else loads geometry data only.
        summary["predicted_native"] = summary["application_prefix_ok"]
        summary["bambustudio_version"] = (
            application[len(NATIVE_APPLICATION_PREFIX) :]
            if summary["application_prefix_ok"]
            else None
        )
        summary["model_metadata"] = metadata

        # -- model_settings.config ----------------------------------------
        ms_bytes = read(MODEL_SETTINGS_ENTRY)
        if ms_bytes is None:
            summary["model_settings"] = {"present": False}
        else:
            summary["model_settings"] = parse_model_settings(ms_bytes)

        # -- project_settings.config --------------------------------------
        ps_bytes = read(PROJECT_SETTINGS_ENTRY)
        if ps_bytes is None:
            summary["project_settings"] = {"present": False}
        else:
            try:
                data = json.loads(ps_bytes.decode("utf-8-sig"))
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                summary["project_settings"] = {
                    "present": True,
                    "parse_error": True,
                    "message": str(exc),
                }
            else:
                flat = data if isinstance(data, dict) else {}
                keys = sorted(flat)
                summary["project_settings"] = {
                    "present": True,
                    "parse_error": False,
                    "key_count": len(keys),
                    "keys": keys,
                    "printer_model": flat.get("printer_model"),
                    "printer_settings_id": flat.get("printer_settings_id"),
                    "print_settings_id": flat.get("print_settings_id"),
                    "filament_settings_id": flat.get("filament_settings_id"),
                    "nozzle_diameter": flat.get("nozzle_diameter"),
                    "version": flat.get("version"),
                    "from": flat.get("from"),
                }

        # -- slice_info.config --------------------------------------------
        si_bytes = read(SLICE_INFO_ENTRY)
        if si_bytes is None:
            summary["slice_info"] = {"present": False, "has_ams_list": False}
        else:
            summary["slice_info"] = parse_slice_info(si_bytes)

        # -- gcode + md5 ---------------------------------------------------
        gcode_entries = sorted(
            n for n in names if n.endswith(".gcode") and n.startswith("Metadata/")
        )
        gcode_report = []
        for entry in gcode_entries:
            data = read(entry)
            actual = hashlib.md5(data).hexdigest().upper() if data is not None else None
            recorded_raw = read(entry + ".md5")
            recorded = (
                recorded_raw.decode("ascii", "replace").strip().upper()
                if recorded_raw is not None
                else None
            )
            gcode_report.append(
                {
                    "name": entry,
                    "size": len(data) if data is not None else None,
                    "md5_file_present": recorded is not None,
                    "md5_recorded": recorded,
                    "md5_actual": actual,
                    "md5_ok": recorded is not None and recorded == actual,
                }
            )
            if recorded is not None and recorded != actual:
                summary["warnings"].append(f"md5 mismatch for {entry}")
        summary["gcode"] = gcode_report
        summary["gcode_count"] = len(gcode_report)
        summary["is_sliced"] = bool(gcode_report)

        # -- thumbnails / extras -------------------------------------------
        summary["thumbnails"] = sorted(n for n in names if n.lower().endswith(".png"))
        summary["plate_json"] = sorted(
            n for n in names if re.fullmatch(r"Metadata/plate_\d+\.json", n)
        )
        summary["extras"] = {
            "cut_information": CUT_INFORMATION_ENTRY in name_set,
            "custom_gcode_per_layer": CUSTOM_GCODE_ENTRY in name_set,
            "layer_config_ranges": LAYER_RANGES_ENTRY in name_set,
            "filament_sequence": FILAMENT_SEQUENCE_ENTRY in name_set,
            "auxiliaries": sorted(
                n for n in names if n.startswith("Auxiliaries/") and not n.endswith("/")
            ),
        }

    return summary


# ---------------------------------------------------------------------------
# Human readable rendering
# ---------------------------------------------------------------------------

def format_summary(summary: dict[str, Any]) -> str:
    """Render an inspection summary as a compact human readable report."""
    lines: list[str] = []
    add = lines.append
    add(f"File            : {summary.get('path')}")
    add(f"Size            : {summary.get('size')} bytes")
    add(f"sha256          : {summary.get('sha256')}")
    if summary.get("errors"):
        for err in summary["errors"]:
            add(f"ERROR           : {err}")
        return "\n".join(lines)
    add(f"Entries         : {len(summary.get('entry_names', []))}")
    add(f"Application     : {summary.get('application')!r}")
    prefix_state = "ok" if summary.get("application_prefix_ok") else "MISSING"
    add(
        f"Predicted native: {summary.get('predicted_native')} "
        f"(prefix 'BambuStudio-' {prefix_state})"
    )
    model = summary.get("model", {})
    add(f"Production ext  : {model.get('has_production_ext')}")
    add(
        f"Geometry        : objects={summary.get('object_count')} "
        f"mesh={summary.get('mesh_object_count')} "
        f"components={summary.get('component_count')} "
        f"items={summary.get('item_count')} "
        f"tris={summary.get('triangle_total')}"
    )
    add(f"Sub-models      : {summary.get('sub_model_names') or '-'}")

    ms = summary.get("model_settings", {})
    if ms.get("present"):
        add(
            f"model_settings  : objects={ms.get('object_count')} "
            f"parts={ms.get('part_count')} subtypes={ms.get('subtype_counts')}"
        )
        add(
            f"Plates          : plater_ids={ms.get('plater_ids')} "
            f"model_instances={ms.get('model_instance_count')}"
        )
        for obj in ms.get("objects", []):
            add(f"  object {obj['id']}: cfg={sorted(obj['metadata'])}")
            for part in obj["parts"]:
                add(
                    f"    part {part['id']} subtype={part['subtype']} "
                    f"cfg={sorted(part['metadata'])}"
                )
    else:
        add("model_settings  : ABSENT")

    ps = summary.get("project_settings", {})
    if ps.get("present"):
        add(
            f"project_settings: keys={ps.get('key_count')} "
            f"printer={ps.get('printer_settings_id')!r} "
            f"print={ps.get('print_settings_id')!r}"
        )
    else:
        add("project_settings: ABSENT")

    si = summary.get("slice_info", {})
    if si.get("present"):
        add(
            f"slice_info      : plates={si.get('plate_count')} "
            f"filaments={si.get('filament_count')} "
            f"ams_list={si.get('ams_list_count')} "
            f"tray_info_idx={si.get('tray_info_idx_all')}"
        )
    else:
        add("slice_info      : ABSENT")

    paint = summary.get("paint", {})
    if paint:
        for name, info in sorted(paint.items()):
            add(
                f"paint {name:<16}: count={info['count']} unique={info['unique']} "
                f"digest={info['digest'][:12]} samples={info['samples']}"
            )
    else:
        add("paint           : none")

    if summary.get("gcode"):
        for g in summary["gcode"]:
            if g["md5_ok"]:
                status = "OK"
            elif not g["md5_file_present"]:
                status = "NO .md5"
            else:
                status = "MISMATCH"
            add(f"gcode           : {g['name']} {g['size']}B md5 {status}")
    else:
        add("gcode           : none (project, not sliced)")

    extras = summary.get("extras", {})
    present_extras = [k for k, v in extras.items() if v and k != "auxiliaries"]
    add(f"extras          : {present_extras or '-'}")
    add(f"thumbnails      : {len(summary.get('thumbnails', []))}")
    for warn in summary.get("warnings", []):
        add(f"WARNING         : {warn}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Inspect a 3MF project file and summarise its structure."
    )
    parser.add_argument("file", nargs="+", help="3MF file(s) to inspect")
    parser.add_argument(
        "--json", action="store_true", help="print the summary as JSON instead of text"
    )
    parser.add_argument(
        "--out", metavar="PATH", help="also write the JSON summary to this path"
    )
    args = parser.parse_args(argv)

    summaries = [inspect_3mf(f) for f in args.file]
    payload = summaries[0] if len(summaries) == 1 else summaries

    if args.json:
        print(json.dumps(payload, indent=2, ensure_ascii=False))
    else:
        for index, summary in enumerate(summaries):
            if index:
                print()
            print(format_summary(summary))

    if args.out:
        Path(args.out).write_text(
            json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8"
        )

    return 0 if all(not s.get("errors") for s in summaries) else 1


if __name__ == "__main__":
    sys.exit(main())
