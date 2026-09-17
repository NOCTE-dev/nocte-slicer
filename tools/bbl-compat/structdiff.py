#!/usr/bin/env python3
"""Structural diff between two 3MF project files.

The comparison is deliberately *semantic* rather than byte oriented, because a
slicer re-export legitimately changes byte order, timestamps, UUID values and a
handful of bookkeeping keys. What it must never change is the structure that the
NOCTE Slicer / Bambu Studio compatibility harness cares about.

What is compared:

``3D/3dmodel.model`` (and every ``3D/Objects/*.model``)
    Canonicalised: elements in document order, attributes sorted by local name,
    all whitespace normalised. Production-extension ``UUID`` attribute *values*
    are replaced by a ``<present>`` placeholder so that a regenerated UUID is not
    a difference, while a *disappearing* UUID still is. Mesh payloads are by
    default reduced to counts plus order-independent digests (``--mesh-detail
    full`` expands every vertex and triangle instead).

``Metadata/model_settings.config``
    Per-object part lists with their ``subtype``, the set of per-part
    ``<metadata key>`` names and their values, plus plates (``plater_id``) and
    their ``<model_instance>`` entries.

``Metadata/project_settings.config``
    Deep JSON diff with a default ignore list of ``version``, ``name`` and
    ``from`` (extendable through ``--ignore-key``).

Paint attributes
    ``paint_color`` / ``paint_supports`` / ``paint_seam`` / ``paint_fuzzy_skin``
    / ``face_property`` compared as order-independent digests and counts.

``Metadata/slice_info.config``
    Plate count, filament count and ``ams_list`` presence.

Usage as a CLI::

    python structdiff.py a.3mf b.3mf [--ignore-key KEY]... [--mesh-detail full]
                         [--json] [--context N] [--max-lines N]

Exit code is 0 when there is no unexpected difference, 1 when there is, and 2 on
a tool error (unreadable file).

Standard library only.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from typing import Any

from inspect3mf import (
    MODEL_ENTRY,
    MODEL_SETTINGS_ENTRY,
    PAINT_ATTRS,
    PROJECT_SETTINGS_ENTRY,
    SLICE_INFO_ENTRY,
    VOLATILE_PROJECT_KEYS,
    attr,
    digest_values,
    localname,
    norm_ws,
    parse_model_settings,
    parse_slice_info,
)

#: Attributes whose value is regenerated on every save: presence is compared,
#: value is not.
UUID_ATTRS = ("UUID",)

#: Default cap on the number of diff lines printed per section.
DEFAULT_MAX_LINES = 200

#: Cap on the printed length of one setting value. Custom G-code values run to
#: several kilobytes and would otherwise bury the report.
VALUE_PRINT_LIMIT = 160


def short(value: object, limit: int = VALUE_PRINT_LIMIT) -> str:
    """Return ``repr(value)`` truncated to ``limit`` characters."""
    text = repr(value)
    if len(text) <= limit:
        return text
    return f"{text[:limit]}...<{len(text)} chars>"


# ---------------------------------------------------------------------------
# Canonicalisation of .model XML
# ---------------------------------------------------------------------------

def _canon_attrs(elem: ET.Element) -> str:
    """Render an element's attributes sorted by local name, UUID values masked."""
    parts = []
    for key in sorted(elem.attrib, key=localname):
        local = localname(key)
        value = norm_ws(elem.attrib[key])
        if local in UUID_ATTRS:
            value = "<present>"
        parts.append(f"{local}={value!r}")
    return " ".join(parts)


def _mesh_digest(elem: ET.Element) -> str:
    """Order-preserving digest of a ``<vertices>`` or ``<triangles>`` payload."""
    h = hashlib.sha256()
    for child in elem:
        h.update(localname(child.tag).encode())
        for key in sorted(child.attrib, key=localname):
            h.update(localname(key).encode())
            h.update(b"=")
            h.update(norm_ws(child.attrib[key]).encode())
        h.update(b"\x00")
    return h.hexdigest()[:16]


def canonical_model_lines(
    data: bytes, *, mesh_detail: str = "summary", source: str = ""
) -> list[str]:
    """Canonicalise a ``.model`` XML payload into a list of comparable lines.

    ``mesh_detail='summary'`` collapses ``<vertices>``/``<triangles>`` bodies into
    a single line holding the child count, a content digest and the paint
    attribute digests. ``mesh_detail='full'`` emits every child element.
    """
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        return [f"<<parse error in {source}: {exc}>>"]

    lines: list[str] = []

    def walk(elem: ET.Element, depth: int) -> None:
        indent = "  " * depth
        local = localname(elem.tag)
        lines.append(f"{indent}<{local} {_canon_attrs(elem)}>".rstrip())
        text = norm_ws(elem.text)
        if text:
            lines.append(f"{indent}  #text {text!r}")
        if local in ("vertices", "triangles") and mesh_detail != "full":
            children = list(elem)
            summary = f"{indent}  ...{len(children)} children digest={_mesh_digest(elem)}"
            if local == "triangles":
                paint = scan_paint_of_triangles(children)
                if paint:
                    summary += " paint=" + json.dumps(paint, sort_keys=True)
            lines.append(summary)
            return
        for child in elem:
            walk(child, depth + 1)

    walk(root, 0)
    return lines


def scan_paint_of_triangles(triangles: list[ET.Element]) -> dict[str, Any]:
    """Return ``{attr: {count, digest}}`` for a list of ``<triangle>`` elements."""
    result: dict[str, Any] = {}
    for name in PAINT_ATTRS:
        values = [v for v in (attr(t, name) for t in triangles) if v]
        if values:
            result[name] = {"count": len(values), "digest": digest_values(values)[:16]}
    return result


# ---------------------------------------------------------------------------
# Archive access helpers
# ---------------------------------------------------------------------------

def _read_entries(path: Path) -> tuple[dict[str, bytes], list[str]]:
    """Read the entries a structural diff needs from a 3MF archive.

    Returns ``(payloads, all_names)``; ``payloads`` maps entry name to bytes for
    every ``.model`` part plus the three config files that are diffed.
    """
    wanted_exact = {
        MODEL_SETTINGS_ENTRY,
        PROJECT_SETTINGS_ENTRY,
        SLICE_INFO_ENTRY,
    }
    payloads: dict[str, bytes] = {}
    with zipfile.ZipFile(path) as zf:
        names = [i.filename for i in zf.infolist()]
        for name in names:
            if name in wanted_exact or name.lower().endswith(".model"):
                payloads[name] = zf.read(name)
    return payloads, names


# ---------------------------------------------------------------------------
# Difference records
# ---------------------------------------------------------------------------

def _diff(
    records: list[dict[str, Any]],
    category: str,
    message: str,
    *,
    severity: str = "unexpected",
    detail: list[str] | None = None,
) -> None:
    """Append a difference record."""
    records.append(
        {
            "category": category,
            "severity": severity,
            "message": message,
            "detail": detail or [],
        }
    )


# ---------------------------------------------------------------------------
# Section comparisons
# ---------------------------------------------------------------------------

def _compare_models(
    a_payloads: dict[str, bytes],
    b_payloads: dict[str, bytes],
    records: list[dict[str, Any]],
    *,
    mesh_detail: str,
    context: int,
    max_lines: int,
) -> None:
    """Compare every ``.model`` part of both archives."""
    a_models = sorted(n for n in a_payloads if n.lower().endswith(".model"))
    b_models = sorted(n for n in b_payloads if n.lower().endswith(".model"))

    if set(a_models) != set(b_models):
        only_a = sorted(set(a_models) - set(b_models))
        only_b = sorted(set(b_models) - set(a_models))
        # A re-export is free to merge or split object model parts; the root
        # 3dmodel.model disappearing however is fatal.
        severity = (
            "unexpected"
            if (MODEL_ENTRY in only_a or MODEL_ENTRY in only_b)
            else "layout"
        )
        _diff(
            records,
            "model/parts",
            f".model part set differs (only in A: {only_a}, only in B: {only_b})",
            severity=severity,
        )

    for name in sorted(set(a_models) & set(b_models)):
        a_lines = canonical_model_lines(
            a_payloads[name], mesh_detail=mesh_detail, source=f"A:{name}"
        )
        b_lines = canonical_model_lines(
            b_payloads[name], mesh_detail=mesh_detail, source=f"B:{name}"
        )
        if a_lines == b_lines:
            continue
        diff = list(
            difflib.unified_diff(
                a_lines,
                b_lines,
                fromfile=f"A/{name}",
                tofile=f"B/{name}",
                n=context,
                lineterm="",
            )
        )
        truncated = len(diff) > max_lines
        _diff(
            records,
            "model/xml",
            f"canonicalised {name} differs ({len(diff)} diff lines)",
            detail=diff[:max_lines] + (["... (truncated)"] if truncated else []),
        )


def _compare_paint(
    a_payloads: dict[str, bytes],
    b_payloads: dict[str, bytes],
    records: list[dict[str, Any]],
) -> None:
    """Compare paint attribute strings across all ``.model`` parts."""

    def collect(payloads: dict[str, bytes]) -> dict[str, dict[str, Any]]:
        buckets: dict[str, list[str]] = {name: [] for name in PAINT_ATTRS}
        for name, data in payloads.items():
            if not name.lower().endswith(".model"):
                continue
            try:
                root = ET.fromstring(data)
            except ET.ParseError:
                continue
            for elem in root.iter():
                if localname(elem.tag) != "triangle":
                    continue
                for key in PAINT_ATTRS:
                    value = attr(elem, key)
                    if value:
                        buckets[key].append(value)
        return {
            key: {"count": len(values), "digest": digest_values(values)}
            for key, values in buckets.items()
            if values
        }

    a_paint = collect(a_payloads)
    b_paint = collect(b_payloads)
    for key in sorted(set(a_paint) | set(b_paint)):
        a_info = a_paint.get(key)
        b_info = b_paint.get(key)
        if a_info is None:
            _diff(records, "paint", f"{key} appeared in B ({b_info['count']} values)")
        elif b_info is None:
            _diff(
                records,
                "paint",
                f"{key} LOST in B (A had {a_info['count']} values)",
            )
        elif a_info != b_info:
            _diff(
                records,
                "paint",
                f"{key} differs: A count={a_info['count']} digest={a_info['digest'][:12]} "
                f"vs B count={b_info['count']} digest={b_info['digest'][:12]}",
            )


def _compare_model_settings(
    a_payloads: dict[str, bytes],
    b_payloads: dict[str, bytes],
    records: list[dict[str, Any]],
) -> None:
    """Compare ``Metadata/model_settings.config`` structurally."""
    a_raw = a_payloads.get(MODEL_SETTINGS_ENTRY)
    b_raw = b_payloads.get(MODEL_SETTINGS_ENTRY)
    if a_raw is None and b_raw is None:
        return
    if a_raw is None or b_raw is None:
        _diff(
            records,
            "model_settings",
            f"{MODEL_SETTINGS_ENTRY} present in "
            f"{'B only' if a_raw is None else 'A only'}",
        )
        return
    a = parse_model_settings(a_raw)
    b = parse_model_settings(b_raw)
    if a.get("parse_error") or b.get("parse_error"):
        _diff(records, "model_settings", "model_settings.config failed to parse")
        return

    if a["object_count"] != b["object_count"]:
        _diff(
            records,
            "model_settings/objects",
            f"object count {a['object_count']} -> {b['object_count']}",
        )
    if a["part_count"] != b["part_count"]:
        _diff(
            records,
            "model_settings/parts",
            f"part count {a['part_count']} -> {b['part_count']}",
        )
    if a["subtype_counts"] != b["subtype_counts"]:
        _diff(
            records,
            "model_settings/subtypes",
            f"part subtype histogram {a['subtype_counts']} -> {b['subtype_counts']}",
        )

    # Pair objects up by their position in the file: ids are renumbered freely.
    for index, (obj_a, obj_b) in enumerate(zip(a["objects"], b["objects"])):
        if sorted(obj_a["metadata"]) != sorted(obj_b["metadata"]):
            only_a = sorted(set(obj_a["metadata"]) - set(obj_b["metadata"]))
            only_b = sorted(set(obj_b["metadata"]) - set(obj_a["metadata"]))
            _diff(
                records,
                "model_settings/object_keys",
                f"object #{index} config keys differ (lost: {only_a}, gained: {only_b})",
            )
        for key in sorted(set(obj_a["metadata"]) & set(obj_b["metadata"])):
            if obj_a["metadata"][key] != obj_b["metadata"][key]:
                _diff(
                    records,
                    "model_settings/object_values",
                    f"object #{index} key {key!r}: "
                    f"{short(obj_a['metadata'][key])} -> "
                    f"{short(obj_b['metadata'][key])}",
                )
        if len(obj_a["parts"]) != len(obj_b["parts"]):
            _diff(
                records,
                "model_settings/parts",
                f"object #{index} part count "
                f"{len(obj_a['parts'])} -> {len(obj_b['parts'])}",
            )
        for part_index, (part_a, part_b) in enumerate(
            zip(obj_a["parts"], obj_b["parts"])
        ):
            label = f"object #{index} part #{part_index}"
            if part_a["subtype"] != part_b["subtype"]:
                _diff(
                    records,
                    "model_settings/subtypes",
                    f"{label} subtype {part_a['subtype']!r} -> {part_b['subtype']!r}",
                )
            if sorted(part_a["metadata"]) != sorted(part_b["metadata"]):
                only_a = sorted(set(part_a["metadata"]) - set(part_b["metadata"]))
                only_b = sorted(set(part_b["metadata"]) - set(part_a["metadata"]))
                _diff(
                    records,
                    "model_settings/part_keys",
                    f"{label} config keys differ (lost: {only_a}, gained: {only_b})",
                )
            for key in sorted(set(part_a["metadata"]) & set(part_b["metadata"])):
                if part_a["metadata"][key] != part_b["metadata"][key]:
                    _diff(
                        records,
                        "model_settings/part_values",
                        f"{label} key {key!r}: "
                        f"{short(part_a['metadata'][key])} -> "
                        f"{short(part_b['metadata'][key])}",
                    )

    if a["plater_ids"] != b["plater_ids"]:
        _diff(
            records,
            "model_settings/plates",
            f"plater_id list {a['plater_ids']} -> {b['plater_ids']}",
        )
    if a["model_instance_count"] != b["model_instance_count"]:
        _diff(
            records,
            "model_settings/instances",
            f"model_instance count {a['model_instance_count']} -> "
            f"{b['model_instance_count']} (instances split or merged)",
        )
    for index, (plate_a, plate_b) in enumerate(zip(a["plates"], b["plates"])):
        if len(plate_a["model_instances"]) != len(plate_b["model_instances"]):
            _diff(
                records,
                "model_settings/instances",
                f"plate #{index} model_instance count "
                f"{len(plate_a['model_instances'])} -> {len(plate_b['model_instances'])}",
            )


def _compare_project_settings(
    a_payloads: dict[str, bytes],
    b_payloads: dict[str, bytes],
    records: list[dict[str, Any]],
    ignore_keys: set[str],
) -> None:
    """Deep-diff ``Metadata/project_settings.config`` JSON with an ignore list."""
    a_raw = a_payloads.get(PROJECT_SETTINGS_ENTRY)
    b_raw = b_payloads.get(PROJECT_SETTINGS_ENTRY)
    if a_raw is None and b_raw is None:
        return
    if a_raw is None or b_raw is None:
        _diff(
            records,
            "project_settings",
            f"{PROJECT_SETTINGS_ENTRY} present in "
            f"{'B only' if a_raw is None else 'A only'}",
        )
        return
    try:
        a = json.loads(a_raw.decode("utf-8-sig"))
        b = json.loads(b_raw.decode("utf-8-sig"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        _diff(records, "project_settings", f"failed to parse JSON: {exc}")
        return
    if not isinstance(a, dict) or not isinstance(b, dict):
        _diff(records, "project_settings", "project_settings.config is not an object")
        return

    only_a = sorted(set(a) - set(b) - ignore_keys)
    only_b = sorted(set(b) - set(a) - ignore_keys)
    if only_a:
        _diff(
            records,
            "project_settings/keys",
            f"{len(only_a)} key(s) lost in B",
            detail=[f"- {k} = {short(a[k])}" for k in only_a],
        )
    if only_b:
        _diff(
            records,
            "project_settings/keys",
            f"{len(only_b)} key(s) gained in B",
            detail=[f"+ {k} = {short(b[k])}" for k in only_b],
        )

    changed: list[str] = []
    ignored: list[str] = []
    for key in sorted(set(a) & set(b)):
        if a[key] == b[key]:
            continue
        line = f"  {key}: {short(a[key])} -> {short(b[key])}"
        if key in ignore_keys:
            ignored.append(line)
        else:
            changed.append(line)
    if changed:
        _diff(
            records,
            "project_settings/values",
            f"{len(changed)} value(s) changed",
            detail=changed,
        )
    if ignored:
        _diff(
            records,
            "project_settings/ignored",
            f"{len(ignored)} ignored value(s) changed ({sorted(ignore_keys)})",
            severity="ignored",
            detail=ignored,
        )


def _compare_slice_info(
    a_payloads: dict[str, bytes],
    b_payloads: dict[str, bytes],
    records: list[dict[str, Any]],
) -> None:
    """Compare ``Metadata/slice_info.config`` plate/filament/ams_list structure."""
    a_raw = a_payloads.get(SLICE_INFO_ENTRY)
    b_raw = b_payloads.get(SLICE_INFO_ENTRY)
    if a_raw is None and b_raw is None:
        return
    if a_raw is None:
        _diff(
            records,
            "slice_info",
            f"{SLICE_INFO_ENTRY} appeared in B (B was sliced)",
            severity="layout",
        )
        return
    if b_raw is None:
        _diff(records, "slice_info", f"{SLICE_INFO_ENTRY} LOST in B")
        return
    a = parse_slice_info(a_raw)
    b = parse_slice_info(b_raw)
    if a.get("plate_count") != b.get("plate_count"):
        _diff(
            records,
            "slice_info/plates",
            f"plate count {a.get('plate_count')} -> {b.get('plate_count')}",
        )
    if a.get("filament_count") != b.get("filament_count"):
        _diff(
            records,
            "slice_info/filaments",
            f"filament count {a.get('filament_count')} -> {b.get('filament_count')}",
        )
    if a.get("tray_info_idx_all") != b.get("tray_info_idx_all"):
        _diff(
            records,
            "slice_info/tray_info_idx",
            f"tray_info_idx set {a.get('tray_info_idx_all')} -> "
            f"{b.get('tray_info_idx_all')}",
        )
    if a.get("has_ams_list") and not b.get("has_ams_list"):
        _diff(records, "slice_info/ams_list", "<ams_list> LOST in B")
    elif a.get("ams_list_count") != b.get("ams_list_count"):
        _diff(
            records,
            "slice_info/ams_list",
            f"<ams_list> count {a.get('ams_list_count')} -> {b.get('ams_list_count')}",
        )


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def compare_3mf(
    path_a: str | Path,
    path_b: str | Path,
    *,
    ignore_keys: list[str] | None = None,
    mesh_detail: str = "summary",
    context: int = 2,
    max_lines: int = DEFAULT_MAX_LINES,
) -> dict[str, Any]:
    """Compare two 3MF archives structurally.

    Returns a dict with ``a``, ``b``, ``differences`` (a list of records with
    ``category`` / ``severity`` / ``message`` / ``detail``), the per-severity
    ``counts``, and ``ok`` which is True when nothing unexpected changed.
    Severities: ``unexpected`` (a real regression), ``layout`` (a benign
    repackaging such as splitting object model parts or adding slice data), and
    ``ignored`` (a volatile key from the ignore list).
    """
    path_a, path_b = Path(path_a), Path(path_b)
    ignores = set(VOLATILE_PROJECT_KEYS) | set(ignore_keys or ())
    result: dict[str, Any] = {
        "a": str(path_a),
        "b": str(path_b),
        "ignore_keys": sorted(ignores),
        "mesh_detail": mesh_detail,
        "errors": [],
        "differences": [],
    }
    try:
        a_payloads, a_names = _read_entries(path_a)
        b_payloads, b_names = _read_entries(path_b)
    except (zipfile.BadZipFile, OSError, FileNotFoundError) as exc:
        result["errors"].append(str(exc))
        result["ok"] = False
        result["counts"] = {}
        return result

    records: list[dict[str, Any]] = result["differences"]

    only_a = sorted(set(a_names) - set(b_names))
    only_b = sorted(set(b_names) - set(a_names))
    if only_a or only_b:
        _diff(
            records,
            "archive/entries",
            f"entry list differs ({len(only_a)} only in A, {len(only_b)} only in B)",
            severity="layout",
            detail=[f"- {n}" for n in only_a] + [f"+ {n}" for n in only_b],
        )

    _compare_models(
        a_payloads,
        b_payloads,
        records,
        mesh_detail=mesh_detail,
        context=context,
        max_lines=max_lines,
    )
    _compare_paint(a_payloads, b_payloads, records)
    _compare_model_settings(a_payloads, b_payloads, records)
    _compare_project_settings(a_payloads, b_payloads, records, ignores)
    _compare_slice_info(a_payloads, b_payloads, records)

    counts: dict[str, int] = {}
    for rec in records:
        counts[rec["severity"]] = counts.get(rec["severity"], 0) + 1
    result["counts"] = counts
    result["ok"] = counts.get("unexpected", 0) == 0
    return result


def format_report(result: dict[str, Any], *, show_detail: bool = True) -> str:
    """Render a comparison result as a unified-ish text report."""
    lines: list[str] = []
    add = lines.append
    add(f"--- A {result['a']}")
    add(f"+++ B {result['b']}")
    add(f"ignored keys: {', '.join(result['ignore_keys'])}")
    add(f"mesh detail : {result['mesh_detail']}")
    for err in result.get("errors", []):
        add(f"ERROR: {err}")
    if not result["differences"]:
        add("")
        add("no structural differences")
        return "\n".join(lines)
    add("")
    order = {"unexpected": 0, "layout": 1, "ignored": 2}
    for rec in sorted(
        result["differences"], key=lambda r: (order.get(r["severity"], 3), r["category"])
    ):
        marker = {"unexpected": "!!", "layout": "~~", "ignored": ".."}.get(
            rec["severity"], "??"
        )
        add(f"{marker} [{rec['category']}] {rec['message']}")
        if show_detail:
            for detail in rec["detail"]:
                add(f"      {detail}")
    add("")
    add(
        "summary: "
        + ", ".join(f"{k}={v}" for k, v in sorted(result["counts"].items()))
        + f"  ->  {'OK' if result['ok'] else 'DIFFERENT'}"
    )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Compare two 3MF files structurally (model, model_settings, "
        "project_settings, paint attributes, slice_info)."
    )
    parser.add_argument("a", help="reference 3MF file")
    parser.add_argument("b", help="3MF file to compare against the reference")
    parser.add_argument(
        "--ignore-key",
        action="append",
        default=[],
        metavar="KEY",
        help="extra project_settings key to ignore (repeatable); "
        f"{', '.join(VOLATILE_PROJECT_KEYS)} are always ignored",
    )
    parser.add_argument(
        "--mesh-detail",
        choices=("summary", "full"),
        default="summary",
        help="'summary' digests vertex/triangle payloads (default), "
        "'full' expands every element",
    )
    parser.add_argument(
        "--context", type=int, default=2, help="unified diff context lines (default 2)"
    )
    parser.add_argument(
        "--max-lines",
        type=int,
        default=DEFAULT_MAX_LINES,
        help=f"max diff lines per section (default {DEFAULT_MAX_LINES})",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON instead of text")
    parser.add_argument(
        "--quiet", action="store_true", help="omit per-difference detail lines"
    )
    args = parser.parse_args(argv)

    result = compare_3mf(
        args.a,
        args.b,
        ignore_keys=args.ignore_key,
        mesh_detail=args.mesh_detail,
        context=args.context,
        max_lines=args.max_lines,
    )
    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
    else:
        print(format_report(result, show_detail=not args.quiet))

    if result.get("errors"):
        return 2
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
