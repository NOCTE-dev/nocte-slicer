#!/usr/bin/env python3
r"""Bambu Studio compatibility oracle for a single 3MF file.

There is no ``--validate`` flag in the Bambu Studio CLI, so the oracle is built
out of the signals that actually exist:

1. the **exit code** of ``bambu-studio.exe``,
2. the ``result.json`` the CLI drops into ``--outputdir`` (``return_code``,
   ``error_string``, ``upward_compatible_machine``, ...),
3. the **log text**, when any is obtainable (see the caveat below),
4. the **structure of the file it re-exports**, compared against the input.

Caveat verified on Bambu Studio 02.08.02.61 (Windows)
-----------------------------------------------------
``bambu-studio.exe`` is linked as a *GUI subsystem* binary, so a CLI run writes
**nothing** to stdout or stderr even at ``--debug 5``, and its studio log
(``%APPDATA%\BambuStudio\log\studio_*_enc.log.N``) has a plaintext
``BEGIN_HEADER`` block followed by an **encrypted** body (``enc_version
1.0.0.0``). There is therefore no greppable log by default. The consequences:

* ``result.json`` in ``--outputdir`` is the primary machine readable verdict;
* the log-text criteria (b) and (d) degrade to ``skip`` unless a plaintext log is
  supplied with ``--log-file`` / ``--log-dir`` (which is what a debug build of
  NOCTE Slicer, or an Orca-derived binary with a console subsystem, provides);
* when no log is available, criterion (b) still *fails* if the static
  ``Application`` prediction says the non-native import path is unavoidable, and
  criterion (d) still fails if ``--bs-version`` shows the file was written by a
  build newer than the one under test.

Pipeline
--------
1. Static inspection of the input (``inspect3mf``): ``Application`` string,
   is-native prediction, archive entries, object/part/subtype counts, plate ids,
   paint attribute presence.
2. ``bambu-studio.exe --info --debug 5 --outputdir <workdir> <file>``.
3. Optional (``--slice``): ``bambu-studio.exe --slice 0
   --export-3mf <workdir>/reexport.3mf --outputdir <workdir> --debug 2 <file>``.
4. If ``reexport.3mf`` exists: inspect it and compare with the input
   (``structdiff``).
5. Evaluate the pass criteria (a) .. (g) and print a PASS/FAIL table.

Pass criteria
-------------
a. every Bambu Studio invocation returned exit code 0 (and ``result.json``
   reported ``return_code`` 0);
b. the log contains neither "not from Bambu Lab" nor "load geometry data only";
c. ``<metadata name="Application">`` starts with ``BambuStudio-``;
d. the log carries no "newer version" warning;
e. the re-export preserves the object count, the part count, every part
   ``subtype``, the per-part ``<metadata key>`` names, the ``plater_id`` set and
   the ``<model_instance>`` count (i.e. instances were not split);
f. paint attribute strings are preserved;
g. ``slice_info.config`` ``<ams_list>`` is preserved when the input had one.

A criterion is reported as ``skip`` when the evidence needed to decide it was not
available (no re-export, no log, nothing to preserve).

Usage::

    python oracle.py <file.3mf> [--bs "C:\Program Files\Bambu Studio\bambu-studio.exe"]
                     [--workdir DIR] [--slice] [--json out.json] [--timeout 600]
                     [--log-file PATH] [--log-dir DIR] [--bs-version 02.08.02.61]

Exit codes: 0 all criteria pass, 1 at least one criterion failed, 2 tool error
(input unreadable, ``bambu-studio.exe`` not found, workdir not usable).

Standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import structdiff
from inspect3mf import NATIVE_APPLICATION_PREFIX, inspect_3mf

DEFAULT_BS = r"C:\Program Files\Bambu Studio\bambu-studio.exe"
DEFAULT_TIMEOUT = 600

#: File the CLI writes into ``--outputdir`` with its machine readable verdict.
CLI_RESULT_NAME = "result.json"

#: Where Bambu Studio keeps its (encrypted) studio logs on Windows.
DEFAULT_BS_LOG_DIR = (
    Path(os.environ.get("APPDATA", "")) / "BambuStudio" / "log"
    if os.environ.get("APPDATA")
    else None
)

#: Return codes documented from NOCTE's own experience with the Bambu CLI.
KNOWN_RETURN_CODES = {
    0: "success",
    -13: "Failed exporting 3mf files",
    -18: "Invalid parameter value(s) included in the 3mf file",
    -100: "unrepairable mesh",
}

#: Log substrings (lowercase) that prove the non-native import path was taken.
NON_NATIVE_MARKERS = (
    "not from bambu lab",
    "load geometry data only",
)

#: Log substrings (lowercase) that mean the file was written by a newer build.
NEWER_VERSION_MARKERS = (
    "newer version",
    "newer 3mf",
    "higher version",
)

#: Marker of an encrypted Bambu Studio log (plaintext header, ciphertext body).
ENCRYPTED_LOG_MARKER = "enc_version"

#: Criteria identifiers, in report order.
CRITERIA = ("a", "b", "c", "d", "e", "f", "g")

CRITERIA_TITLES = {
    "a": "exit code 0 from every Bambu Studio call",
    "b": "log free of 'not from Bambu Lab' / 'load geometry data only'",
    "c": "Application starts with 'BambuStudio-'",
    "d": "log free of 'newer version' warning",
    "e": "re-export preserves objects/parts/subtypes/config keys/plates/instances",
    "f": "paint attribute strings preserved",
    "g": "slice_info <ams_list> preserved",
}


# ---------------------------------------------------------------------------
# Version helpers
# ---------------------------------------------------------------------------

def parse_version(text: str | None) -> tuple[int, ...] | None:
    """Parse a dotted version such as ``02.08.02.61`` into a tuple of ints."""
    if not text:
        return None
    parts = []
    for chunk in str(text).split("."):
        digits = "".join(c for c in chunk if c.isdigit())
        if not digits:
            return None
        parts.append(int(digits))
    return tuple(parts) or None


def version_gt(a: str | None, b: str | None) -> bool:
    """True when dotted version ``a`` is strictly greater than ``b``."""
    va, vb = parse_version(a), parse_version(b)
    if va is None or vb is None:
        return False
    return va > vb


# ---------------------------------------------------------------------------
# Subprocess helpers
# ---------------------------------------------------------------------------

def _normalize_exit_code(code: int | None) -> int | None:
    """Map a Windows unsigned exit status onto the signed value the CLI meant.

    Windows reports process exit codes as unsigned 32-bit values, so the CLI's
    documented ``-13`` arrives as ``4294967283``.
    """
    if code is not None and code > 0x7FFFFFFF:
        return code - 0x100000000
    return code


def _creationflags() -> int:
    """Return CREATE_NO_WINDOW on Windows, 0 elsewhere."""
    return getattr(subprocess, "CREATE_NO_WINDOW", 0)


def run_bambu(
    bs: Path, argv: list[str], *, timeout: int, cwd: Path | None = None
) -> dict[str, Any]:
    """Run ``bambu-studio.exe`` with ``argv`` and capture everything.

    Returns a dict with ``cmd``, ``exit_code`` (normalised), ``timed_out``,
    ``duration_s``, ``stdout``, ``stderr`` and ``error``. Never raises for a
    non-zero exit; only an OS-level launch failure is reported via ``error``.
    """
    cmd = [str(bs), *argv]
    record: dict[str, Any] = {
        "cmd": cmd,
        "exit_code": None,
        "exit_meaning": None,
        "timed_out": False,
        "duration_s": None,
        "stdout": "",
        "stderr": "",
        "error": None,
    }
    started = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            timeout=timeout,
            cwd=str(cwd) if cwd else None,
            creationflags=_creationflags(),
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        record["timed_out"] = True
        record["duration_s"] = round(time.monotonic() - started, 2)
        record["stdout"] = (exc.stdout or b"").decode("utf-8", "replace")
        record["stderr"] = (exc.stderr or b"").decode("utf-8", "replace")
        record["error"] = f"timed out after {timeout}s (process was killed)"
        return record
    except OSError as exc:
        record["duration_s"] = round(time.monotonic() - started, 2)
        record["error"] = f"failed to launch: {exc}"
        return record
    record["duration_s"] = round(time.monotonic() - started, 2)
    record["exit_code"] = _normalize_exit_code(proc.returncode)
    record["stdout"] = proc.stdout.decode("utf-8", "replace")
    record["stderr"] = proc.stderr.decode("utf-8", "replace")
    record["exit_meaning"] = KNOWN_RETURN_CODES.get(
        record["exit_code"], "undocumented return code"
    )
    return record


def read_cli_result(workdir: Path) -> dict[str, Any] | None:
    """Read the ``result.json`` the CLI leaves in ``--outputdir``.

    Returns None when the file is absent, or ``{"parse_error": ...}`` when it is
    unreadable. ``run_oracle`` removes any stale copy before the first phase, so
    a missing file here means this phase produced no verdict of its own.
    """
    path = workdir / CLI_RESULT_NAME
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (json.JSONDecodeError, OSError) as exc:
        return {"parse_error": str(exc)}


def read_plaintext_log(path: Path) -> tuple[str, str | None]:
    """Read a log file, refusing an encrypted Bambu Studio studio log.

    Returns ``(text, skip_reason)``; ``text`` is empty when the file was skipped.
    """
    try:
        data = path.read_bytes()
    except OSError as exc:
        return "", f"unreadable: {exc}"
    head = data[:512].decode("utf-8", "replace")
    if ENCRYPTED_LOG_MARKER in head:
        return "", "encrypted studio log (plaintext header only)"
    text = data.decode("utf-8", "replace")
    if text.count("\ufffd") > max(64, len(text) // 200):
        return "", "not plaintext (binary content)"
    return text, None


def collect_log_files(
    dirs: list[Path], since: float, *, extra: list[Path] | None = None
) -> tuple[dict[str, str], dict[str, str]]:
    """Gather plaintext log text produced during the run.

    ``dirs`` are scanned for ``*.log`` / ``*.txt`` touched at or after ``since``;
    ``extra`` files are always read. Returns ``(texts, skipped)`` keyed by path.
    """
    texts: dict[str, str] = {}
    skipped: dict[str, str] = {}
    candidates: list[Path] = []
    for directory in dirs:
        if not directory or not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix.lower() not in (".log", ".txt") and ".log." not in path.name:
                continue
            try:
                if path.stat().st_mtime < since - 2:
                    continue
            except OSError:
                continue
            candidates.append(path)
    candidates.extend(p for p in (extra or []) if p.is_file())
    for path in candidates:
        text, reason = read_plaintext_log(path)
        if reason:
            skipped[str(path)] = reason
        elif text.strip():
            texts[str(path)] = text
    return texts, skipped


# ---------------------------------------------------------------------------
# Criteria evaluation
# ---------------------------------------------------------------------------

def _verdict(ok: bool | None, note: str) -> dict[str, Any]:
    """Build one criterion result: ok True/False, or None for 'skip'."""
    return {"ok": ok, "note": note}


def _eval_reexport_structure(
    src: dict[str, Any], dst: dict[str, Any], diff: dict[str, Any]
) -> dict[str, Any]:
    """Criterion (e): structural preservation across the re-export."""
    problems: list[str] = []

    src_ms = src.get("model_settings") or {}
    dst_ms = dst.get("model_settings") or {}
    if not src_ms.get("present"):
        return _verdict(None, "input has no model_settings.config to preserve")
    if not dst_ms.get("present"):
        return _verdict(False, "re-export dropped Metadata/model_settings.config")

    if src_ms.get("object_count") != dst_ms.get("object_count"):
        problems.append(
            f"object count {src_ms.get('object_count')} -> {dst_ms.get('object_count')}"
        )
    if src_ms.get("part_count") != dst_ms.get("part_count"):
        problems.append(
            f"part count {src_ms.get('part_count')} -> {dst_ms.get('part_count')}"
        )
    if src_ms.get("subtype_counts") != dst_ms.get("subtype_counts"):
        problems.append(
            f"subtypes {src_ms.get('subtype_counts')} -> {dst_ms.get('subtype_counts')}"
        )
    if sorted(filter(None, src_ms.get("plater_ids") or [])) != sorted(
        filter(None, dst_ms.get("plater_ids") or [])
    ):
        problems.append(
            f"plater_id set {src_ms.get('plater_ids')} -> {dst_ms.get('plater_ids')}"
        )
    if src_ms.get("model_instance_count") != dst_ms.get("model_instance_count"):
        problems.append(
            f"model_instance count {src_ms.get('model_instance_count')} -> "
            f"{dst_ms.get('model_instance_count')} (instances split?)"
        )

    # Per-part <metadata key> names, object by object, part by part.
    for index, (obj_a, obj_b) in enumerate(
        zip(src_ms.get("objects", []), dst_ms.get("objects", []))
    ):
        for part_index, (part_a, part_b) in enumerate(
            zip(obj_a.get("parts", []), obj_b.get("parts", []))
        ):
            lost = sorted(set(part_a["metadata"]) - set(part_b["metadata"]))
            if lost:
                problems.append(
                    f"object #{index} part #{part_index} lost config keys {lost}"
                )

    # Anything structdiff flagged as unexpected in those same sections.
    for rec in diff.get("differences", []):
        if rec["severity"] != "unexpected":
            continue
        if rec["category"].startswith("model_settings"):
            problems.append(f"structdiff: {rec['message']}")

    if problems:
        return _verdict(False, "; ".join(dict.fromkeys(problems)))
    return _verdict(
        True, "objects, parts, subtypes, config keys, plates and instances kept"
    )


def _eval_paint(
    src: dict[str, Any], dst: dict[str, Any], diff: dict[str, Any]
) -> dict[str, Any]:
    """Criterion (f): paint attribute strings preserved."""
    src_paint = src.get("paint") or {}
    if not src_paint:
        return _verdict(None, "input carries no paint attributes")
    dst_paint = dst.get("paint") or {}
    problems = []
    for name, info in sorted(src_paint.items()):
        other = dst_paint.get(name)
        if other is None:
            problems.append(f"{name} lost ({info['count']} values)")
        elif other["digest"] != info["digest"]:
            problems.append(
                f"{name} changed ({info['count']} -> {other['count']} values)"
            )
    for rec in diff.get("differences", []):
        if rec["severity"] == "unexpected" and rec["category"] == "paint":
            problems.append(f"structdiff: {rec['message']}")
    if problems:
        return _verdict(False, "; ".join(dict.fromkeys(problems)))
    return _verdict(True, f"preserved: {sorted(src_paint)}")


def _eval_ams(src: dict[str, Any], dst: dict[str, Any]) -> dict[str, Any]:
    """Criterion (g): ``<ams_list>`` preserved when the input had one."""
    src_si = src.get("slice_info") or {}
    if not src_si.get("has_ams_list"):
        return _verdict(None, "input slice_info has no <ams_list>")
    dst_si = dst.get("slice_info") or {}
    if not dst_si.get("has_ams_list"):
        return _verdict(False, "<ams_list> lost in the re-export")
    if src_si.get("ams_list_count") != dst_si.get("ams_list_count"):
        return _verdict(
            False,
            f"<ams_list> count {src_si.get('ams_list_count')} -> "
            f"{dst_si.get('ams_list_count')}",
        )
    return _verdict(True, f"{src_si.get('ams_list_count')} <ams_list> entr(ies) kept")


def evaluate(result: dict[str, Any]) -> dict[str, Any]:
    """Fill ``result['criteria']`` from the collected evidence."""
    src = result.get("input") or {}
    runs = result.get("runs") or []
    log = result.get("log_lower") or ""
    log_available = bool(result.get("log_available"))
    criteria: dict[str, dict[str, Any]] = {}

    # (a) exit codes + result.json return_code ----------------------------
    bad: list[str] = []
    for run in runs:
        if run["exit_code"] != 0:
            note = f"{run['phase']}: exit={run['exit_code']}"
            if run["timed_out"]:
                note += " (timeout)"
            if run.get("exit_meaning"):
                note += f" [{run['exit_meaning']}]"
            bad.append(note)
        cli = run.get("cli_result") or {}
        rc = cli.get("return_code")
        if rc is not None and rc != 0:
            bad.append(
                f"{run['phase']}: result.json return_code={rc} "
                f"[{KNOWN_RETURN_CODES.get(rc, cli.get('error_string', '?'))}]"
            )
    if not runs:
        criteria["a"] = _verdict(None, "no Bambu Studio invocation was made")
    elif bad:
        criteria["a"] = _verdict(False, "; ".join(bad))
    else:
        ok_note = ", ".join(f"{r['phase']}=0" for r in runs)
        cli = result.get("cli_result") or {}
        if cli.get("error_string"):
            ok_note += f"; result.json error_string={cli['error_string']!r}"
        criteria["a"] = _verdict(True, ok_note)

    # (b) non-native import markers ---------------------------------------
    hits = [m for m in NON_NATIVE_MARKERS if m in log]
    if hits:
        criteria["b"] = _verdict(False, f"log contains {hits}")
    elif log_available:
        criteria["b"] = _verdict(True, "no non-native import marker in log")
    elif src.get("application_prefix_ok") is False:
        criteria["b"] = _verdict(
            False,
            "no log available, but Application lacks the 'BambuStudio-' prefix, so "
            "the non-native 'load geometry data only' path is unavoidable",
        )
    else:
        criteria["b"] = _verdict(
            None,
            "no plaintext log available (bambu-studio.exe writes no stdout and its "
            "studio log is encrypted); pass --log-file/--log-dir to decide this",
        )

    # (c) Application string ----------------------------------------------
    app = src.get("application")
    criteria["c"] = (
        _verdict(True, f"Application={app!r}")
        if src.get("application_prefix_ok")
        else _verdict(
            False,
            f"Application={app!r} does not start with {NATIVE_APPLICATION_PREFIX!r}",
        )
    )

    # (d) newer version warning -------------------------------------------
    version_hits = [m for m in NEWER_VERSION_MARKERS if m in log]
    file_version = src.get("bambustudio_version")
    tested_version = result.get("bs_version")
    if version_hits:
        criteria["d"] = _verdict(False, f"log contains {version_hits}")
    elif tested_version and version_gt(file_version, tested_version):
        criteria["d"] = _verdict(
            False,
            f"file was written by BambuStudio-{file_version} which is newer than the "
            f"build under test ({tested_version}): a 'newer version' warning is expected",
        )
    elif log_available:
        criteria["d"] = _verdict(True, "no 'newer version' warning in log")
    elif tested_version:
        criteria["d"] = _verdict(
            True,
            f"no log available, but file version {file_version} <= tested build "
            f"{tested_version}, so no 'newer version' warning is expected",
        )
    else:
        criteria["d"] = _verdict(
            None,
            "no plaintext log available and no --bs-version given to compare "
            f"against the file's version ({file_version})",
        )

    # (e)(f)(g) need a re-export -------------------------------------------
    dst = result.get("reexport")
    diff = result.get("diff") or {}
    if not dst:
        reason = result.get("reexport_note") or "no re-export produced (use --slice)"
        criteria["e"] = _verdict(None, reason)
        criteria["f"] = _verdict(None, reason)
        criteria["g"] = _verdict(None, reason)
    else:
        criteria["e"] = _eval_reexport_structure(src, dst, diff)
        criteria["f"] = _eval_paint(src, dst, diff)
        criteria["g"] = _eval_ams(src, dst)

    result["criteria"] = criteria
    result["failed_criteria"] = [k for k in CRITERIA if criteria[k]["ok"] is False]
    result["skipped_criteria"] = [k for k in CRITERIA if criteria[k]["ok"] is None]
    result["passed"] = not result["failed_criteria"]
    return result


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------

def run_oracle(
    file: str | Path,
    *,
    bs: str | Path = DEFAULT_BS,
    workdir: str | Path | None = None,
    do_slice: bool = False,
    timeout: int = DEFAULT_TIMEOUT,
    ignore_keys: list[str] | None = None,
    log_files: list[str | Path] | None = None,
    log_dir: str | Path | None = None,
    bs_version: str | None = None,
) -> dict[str, Any]:
    """Run the full oracle pipeline over one 3MF file and return the report."""
    file = Path(file).resolve()
    bs_path = Path(bs)
    result: dict[str, Any] = {
        "file": str(file),
        "bambu_studio": str(bs_path),
        "bs_version": bs_version,
        "slice_requested": do_slice,
        "timeout_s": timeout,
        "tool_error": None,
        "runs": [],
        "notes": [],
    }

    if not file.is_file():
        result["tool_error"] = f"input file not found: {file}"
        return result

    # -- step 1: static inspection ---------------------------------------
    src = inspect_3mf(file)
    result["input"] = src
    if src.get("errors"):
        result["notes"].extend(f"input: {e}" for e in src["errors"])
    result["predicted_native"] = src.get("predicted_native")

    if workdir is None:
        workdir_path = Path(tempfile.mkdtemp(prefix="bblcompat_"))
        result["notes"].append(f"created temporary workdir {workdir_path}")
    else:
        workdir_path = Path(workdir).resolve()
        try:
            workdir_path.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            result["tool_error"] = f"cannot create workdir {workdir_path}: {exc}"
            return result
    result["workdir"] = str(workdir_path)

    if not bs_path.is_file():
        result["tool_error"] = f"bambu-studio executable not found: {bs_path}"
        return result

    started = time.time()
    # A stale result.json from an earlier run must not be mistaken for ours.
    stale = workdir_path / CLI_RESULT_NAME
    if stale.exists():
        try:
            stale.unlink()
        except OSError:
            pass

    # -- step 2: --info --------------------------------------------------
    info_run = run_bambu(
        bs_path,
        ["--info", "--debug", "5", "--outputdir", str(workdir_path), str(file)],
        timeout=timeout,
        cwd=workdir_path,
    )
    info_run["phase"] = "info"
    info_run["cli_result"] = read_cli_result(workdir_path)
    result["runs"].append(info_run)
    if info_run["error"] and info_run["exit_code"] is None and not info_run["timed_out"]:
        result["tool_error"] = f"--info: {info_run['error']}"
        return result

    # -- step 3: optional slice + re-export -------------------------------
    reexport_path = workdir_path / "reexport.3mf"
    if do_slice:
        if reexport_path.exists():
            try:
                reexport_path.unlink()
            except OSError:
                pass
        slice_run = run_bambu(
            bs_path,
            [
                "--slice",
                "0",
                "--export-3mf",
                str(reexport_path),
                "--outputdir",
                str(workdir_path),
                "--debug",
                "2",
                str(file),
            ],
            timeout=timeout,
            cwd=workdir_path,
        )
        slice_run["phase"] = "slice"
        slice_run["cli_result"] = read_cli_result(workdir_path)
        result["runs"].append(slice_run)

    result["cli_result"] = next(
        (r["cli_result"] for r in reversed(result["runs"]) if r.get("cli_result")), None
    )

    # -- logs -------------------------------------------------------------
    scan_dirs = [workdir_path]
    if log_dir:
        scan_dirs.append(Path(log_dir))
    log_texts, log_skipped = collect_log_files(
        scan_dirs, started, extra=[Path(p) for p in (log_files or [])]
    )
    result["log_files"] = sorted(log_texts)
    result["log_files_skipped"] = log_skipped
    parts: list[str] = []
    for run in result["runs"]:
        parts.append(run["stdout"])
        parts.append(run["stderr"])
    parts.extend(log_texts.values())
    log_text = "\n".join(p for p in parts if p)
    result["log"] = log_text
    result["log_lower"] = log_text.lower()
    result["log_available"] = bool(log_text.strip())
    lines = [ln.rstrip() for ln in log_text.splitlines() if ln.strip()]
    result["log_line_count"] = len(lines)
    result["log_head"] = lines[:15]
    result["log_tail"] = lines[-15:]
    if not result["log_available"]:
        result["notes"].append(
            "no log text captured: bambu-studio.exe is a GUI-subsystem binary "
            "(empty stdout/stderr) and its studio log is encrypted"
        )
    for path, reason in log_skipped.items():
        result["notes"].append(f"log skipped ({reason}): {path}")

    # -- step 4: inspect + diff the re-export ------------------------------
    if do_slice:
        if reexport_path.is_file():
            result["reexport_path"] = str(reexport_path)
            result["reexport"] = inspect_3mf(reexport_path)
            result["diff"] = structdiff.compare_3mf(
                file, reexport_path, ignore_keys=ignore_keys
            )
        else:
            result["reexport_note"] = "re-export file was not produced"
            result["notes"].append("slice ran but reexport.3mf does not exist")

    # -- step 5: criteria --------------------------------------------------
    evaluate(result)
    return result


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def format_report(result: dict[str, Any], *, show_log: bool = True) -> str:
    """Render the oracle report as a readable PASS/FAIL table."""
    lines: list[str] = []
    add = lines.append
    add("=" * 78)
    add(f"Bambu Studio compatibility oracle - {Path(result['file']).name}")
    add("=" * 78)
    add(f"file        : {result['file']}")
    add(f"bambu-studio: {result['bambu_studio']}")
    add(f"workdir     : {result.get('workdir')}")

    if result.get("tool_error"):
        add(f"TOOL ERROR  : {result['tool_error']}")
        return "\n".join(lines)

    src = result.get("input", {})
    add(f"Application : {src.get('application')!r}")
    add(f"native pred.: {src.get('predicted_native')}")
    ms = src.get("model_settings", {})
    add(
        f"structure   : objects={ms.get('object_count')} parts={ms.get('part_count')} "
        f"subtypes={ms.get('subtype_counts')} plater_ids={ms.get('plater_ids')} "
        f"instances={ms.get('model_instance_count')}"
    )
    add(f"paint       : {src.get('paint_attrs_present') or '-'}")
    si = src.get("slice_info", {})
    add(
        f"slice_info  : present={si.get('present')} "
        f"ams_list={si.get('ams_list_count', 0)}"
    )
    add("")
    for run in result.get("runs", []):
        extra = " TIMED OUT" if run["timed_out"] else ""
        add(
            f"run {run['phase']:<6}: exit={run['exit_code']} "
            f"({run.get('exit_meaning', 'n/a')}) {run['duration_s']}s{extra}"
        )
    cli = result.get("cli_result")
    if cli:
        add(
            f"result.json : return_code={cli.get('return_code')} "
            f"error_string={cli.get('error_string')!r} "
            f"prepare_time={cli.get('prepare_time')} "
            f"export_time={cli.get('export_time')}"
        )
        upward = cli.get("upward_compatible_machine")
        if upward:
            add(f"              upward_compatible_machine: {len(upward)} machine(s)")
    add(f"log         : {'available' if result.get('log_available') else 'NONE'}")
    if result.get("reexport_path"):
        add(f"re-export   : {result['reexport_path']}")
    elif result.get("reexport_note"):
        add(f"re-export   : {result['reexport_note']}")
    add("")

    add(f"{'':<3}{'crit':<5}{'result':<8}criterion / note")
    add("-" * 78)
    for key in CRITERIA:
        verdict = result.get("criteria", {}).get(key, {"ok": None, "note": "n/a"})
        state = {True: "PASS", False: "FAIL", None: "skip"}[verdict["ok"]]
        add(f"   ({key})  {state:<8}{CRITERIA_TITLES[key]}")
        add(f"{'':<16}-> {verdict['note']}")
    add("-" * 78)
    add(
        f"VERDICT: {'PASS' if result.get('passed') else 'FAIL'}   "
        f"failed={result.get('failed_criteria')} skipped={result.get('skipped_criteria')}"
    )

    diff = result.get("diff")
    if diff:
        add("")
        add("structdiff summary: " + json.dumps(diff.get("counts", {}), sort_keys=True))
        for rec in diff.get("differences", []):
            if rec["severity"] == "unexpected":
                add(f"  !! [{rec['category']}] {rec['message']}")

    if show_log and result.get("log_line_count"):
        add("")
        add(f"log ({result['log_line_count']} non-empty lines) - head:")
        for line in result.get("log_head", []):
            add(f"  | {line}")
        if result["log_line_count"] > 30:
            add("  | ...")
            add("log - tail:")
            for line in result.get("log_tail", []):
                add(f"  | {line}")
    for note in result.get("notes", []):
        add(f"note: {note}")
    return "\n".join(lines)


def _json_safe(result: dict[str, Any]) -> dict[str, Any]:
    """Drop the bulky lowercase log copy before serialising."""
    payload = dict(result)
    payload.pop("log_lower", None)
    return payload


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Run the Bambu Studio compatibility oracle over one 3MF file."
    )
    parser.add_argument("file", help="the 3MF file to test")
    parser.add_argument(
        "--bs",
        default=os.environ.get("BAMBU_STUDIO_EXE", DEFAULT_BS),
        help=f"path to bambu-studio.exe (default: {DEFAULT_BS})",
    )
    parser.add_argument(
        "--workdir",
        help="directory for CLI output and the re-export (default: a temp directory)",
    )
    parser.add_argument(
        "--slice",
        dest="do_slice",
        action="store_true",
        help="also slice all plates and re-export a 3MF, then structurally diff it",
    )
    parser.add_argument("--json", metavar="PATH", help="write the full report as JSON")
    parser.add_argument(
        "--timeout",
        type=int,
        default=DEFAULT_TIMEOUT,
        help=f"per-invocation timeout in seconds (default {DEFAULT_TIMEOUT})",
    )
    parser.add_argument(
        "--ignore-key",
        action="append",
        default=[],
        metavar="KEY",
        help="extra project_settings key to ignore in the diff (repeatable)",
    )
    parser.add_argument(
        "--log-file",
        action="append",
        default=[],
        metavar="PATH",
        help="a plaintext log to search for import warnings (repeatable); needed "
        "because bambu-studio.exe writes no stdout and encrypts its studio log",
    )
    parser.add_argument(
        "--log-dir",
        metavar="DIR",
        help="directory scanned for plaintext logs written during the run "
        f"(Bambu Studio's own is {DEFAULT_BS_LOG_DIR}, but it is encrypted)",
    )
    parser.add_argument(
        "--bs-version",
        metavar="VER",
        help="dotted version of the build under test, e.g. 02.08.02.61; enables the "
        "static 'newer version' check when no log is available",
    )
    parser.add_argument(
        "--no-log", action="store_true", help="do not print log head/tail"
    )
    args = parser.parse_args(argv)

    result = run_oracle(
        args.file,
        bs=args.bs,
        workdir=args.workdir,
        do_slice=args.do_slice,
        timeout=args.timeout,
        ignore_keys=args.ignore_key,
        log_files=args.log_file,
        log_dir=args.log_dir,
        bs_version=args.bs_version,
    )
    print(format_report(result, show_log=not args.no_log))

    if args.json:
        out = Path(args.json)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(_json_safe(result), indent=2, ensure_ascii=False),
            encoding="utf-8",
        )

    if result.get("tool_error"):
        return 2
    return 0 if result.get("passed") else 1


if __name__ == "__main__":
    sys.exit(main())
