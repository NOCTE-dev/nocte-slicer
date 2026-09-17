#!/usr/bin/env python3
"""Run the Bambu Studio compatibility oracle over the whole corpus.

Walks ``corpus/`` recursively, runs :func:`oracle.run_oracle` on every ``*.3mf``
it finds, and writes a machine readable ``report/summary.json`` plus a human
readable ``report/summary.md`` with one table row per file and one column per
pass criterion (a) .. (g).

Usage::

    python roundtrip.py [--corpus corpus] [--report report] [--slice]
                        [--bs "C:\\Program Files\\Bambu Studio\\bambu-studio.exe"]
                        [--bs-version 02.08.02.61] [--log-dir DIR]
                        [--timeout 600] [--include SUBSTR] [--exclude SUBSTR]
                        [--dry-run] [--static-only] [--limit N]

Exit codes: 0 when every file passed, 1 when at least one failed, 2 on a tool
error (corpus missing, ``bambu-studio.exe`` not found).

Notes
-----
* ``--slice`` is what actually exercises criteria (e), (f) and (g); without it
  those are reported as ``skip``. Slicing the whole corpus launches
  ``bambu-studio.exe`` once per file and can take a long time.
* ``--static-only`` skips the slicer entirely and evaluates only what static
  inspection can decide (criterion (c)); it is the fast smoke mode.

Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path
from typing import Any

import oracle
from inspect3mf import inspect_3mf

CRITERIA = oracle.CRITERIA
STATE = {True: "PASS", False: "FAIL", None: "skip"}
MD_STATE = {True: "pass", False: "**FAIL**", None: "-"}


def discover(corpus: Path, include: list[str], exclude: list[str]) -> list[Path]:
    """Return every ``*.3mf`` under ``corpus``, filtered and sorted."""
    files = sorted(p for p in corpus.rglob("*.3mf") if p.is_file())
    if include:
        files = [p for p in files if any(s.lower() in str(p).lower() for s in include)]
    if exclude:
        files = [
            p for p in files if not any(s.lower() in str(p).lower() for s in exclude)
        ]
    return files


def static_only_result(path: Path, bs_version: str | None = None) -> dict[str, Any]:
    """Build an oracle-shaped report from static inspection alone."""
    src = inspect_3mf(path)
    result: dict[str, Any] = {
        "file": str(path),
        "bambu_studio": None,
        "bs_version": bs_version,
        "workdir": None,
        "slice_requested": False,
        "tool_error": None,
        "runs": [],
        "cli_result": None,
        "notes": ["static-only mode: bambu-studio.exe was not invoked"],
        "input": src,
        "predicted_native": src.get("predicted_native"),
        "log": "",
        "log_lower": "",
        "log_available": False,
        "log_line_count": 0,
        "log_head": [],
        "log_tail": [],
    }
    oracle.evaluate(result)
    return result


def _row(path: Path, corpus: Path, result: dict[str, Any]) -> dict[str, Any]:
    """Reduce one oracle report into a summary row."""
    src = result.get("input") or {}
    ms = src.get("model_settings") or {}
    runs = result.get("runs") or []
    notes: list[str] = []
    if result.get("tool_error"):
        notes.append(f"tool error: {result['tool_error']}")
    for run in runs:
        if run.get("timed_out"):
            notes.append(f"{run['phase']} timed out after {run['duration_s']}s")
    for key in CRITERIA:
        verdict = (result.get("criteria") or {}).get(key)
        if verdict and verdict.get("ok") is False:
            notes.append(f"({key}) {verdict['note']}")
    notes.extend(result.get("notes", []))

    try:
        rel = path.relative_to(corpus).as_posix()
    except ValueError:
        rel = path.as_posix()

    cli = result.get("cli_result") or {}
    return {
        "file": rel,
        "abs_path": str(path),
        "size": src.get("size"),
        "sha256": src.get("sha256"),
        "application": src.get("application"),
        "predicted_native": src.get("predicted_native"),
        "is_sliced": src.get("is_sliced"),
        "objects": ms.get("object_count"),
        "parts": ms.get("part_count"),
        "subtypes": ms.get("subtype_counts"),
        "plater_ids": ms.get("plater_ids"),
        "instances": ms.get("model_instance_count"),
        "paint": src.get("paint_attrs_present"),
        "exit_codes": {r["phase"]: r["exit_code"] for r in runs},
        "cli_return_code": cli.get("return_code"),
        "cli_error_string": cli.get("error_string"),
        "log_available": result.get("log_available"),
        "criteria": {
            key: (result.get("criteria") or {}).get(key, {}).get("ok")
            for key in CRITERIA
        },
        "criteria_notes": {
            key: (result.get("criteria") or {}).get(key, {}).get("note")
            for key in CRITERIA
        },
        "passed": result.get("passed"),
        "failed_criteria": result.get("failed_criteria"),
        "notes": notes,
        "tool_error": result.get("tool_error"),
    }


def _relative_to_tool(path: Path, here: Path) -> str:
    """Render a path relative to the tool directory so reports stay portable."""
    try:
        return path.relative_to(here).as_posix()
    except ValueError:
        return path.as_posix()


def _md_escape(text: str) -> str:
    """Escape the characters that would break a Markdown table cell."""
    return str(text).replace("|", "\\|").replace("\n", " ")


def render_markdown(summary: dict[str, Any]) -> str:
    """Render the corpus summary as Markdown."""
    lines: list[str] = []
    add = lines.append
    add("# Bambu Studio 3MF compatibility round-trip report")
    add("")
    add(f"- generated: `{summary['generated']}`")
    add(f"- corpus: `{summary['corpus']}`")
    add(f"- bambu-studio: `{summary['bambu_studio']}`")
    add(f"- mode: `{summary['mode']}`")
    add(f"- files: {summary['total']} (pass {summary['passed']}, fail {summary['failed']})")
    add("")
    add("Criteria: **a** exit code 0 - **b** no 'not from Bambu Lab' - "
        "**c** `Application` starts with `BambuStudio-` - **d** no 'newer version' "
        "warning - **e** re-export keeps objects/parts/subtypes/config keys/plates/"
        "instances - **f** paint strings kept - **g** `<ams_list>` kept. "
        "`-` means the criterion could not be evaluated in this run.")
    add("")
    add(
        "`rc` is `return_code` from the `result.json` the CLI writes into "
        "`--outputdir`; it is the only machine readable verdict Bambu Studio gives, "
        "because the binary writes no stdout and encrypts its studio log."
    )
    add("")
    header = "| file | native | exit | rc | " + " | ".join(CRITERIA) + " | notes |"
    add(header)
    add("|" + "---|" * (5 + len(CRITERIA)))
    for row in summary["rows"]:
        exits = ", ".join(f"{k}={v}" for k, v in row["exit_codes"].items()) or "-"
        rc = row.get("cli_return_code")
        cells = " | ".join(MD_STATE[row["criteria"][k]] for k in CRITERIA)
        note = "; ".join(row["notes"])[:220] or ""
        add(
            f"| `{_md_escape(row['file'])}` | {row['predicted_native']} | "
            f"{_md_escape(exits)} | {'-' if rc is None else rc} | {cells} | "
            f"{_md_escape(note)} |"
        )
    add("")
    add("## Per-file detail")
    add("")
    for row in summary["rows"]:
        add(f"### `{row['file']}`")
        add("")
        add(f"- size: {row['size']} bytes, sha256 `{row['sha256']}`")
        add(f"- `Application`: `{row['application']}`")
        add(f"- sliced project: {row['is_sliced']}")
        add(
            f"- objects: {row['objects']}, parts: {row['parts']}, "
            f"subtypes: `{row['subtypes']}`"
        )
        add(f"- plater_ids: `{row['plater_ids']}`, model_instances: {row['instances']}")
        add(f"- paint attributes: `{row['paint'] or '-'}`")
        add(f"- exit codes: `{row['exit_codes'] or '-'}`")
        add(
            f"- `result.json`: return_code=`{row.get('cli_return_code')}` "
            f"error_string=`{row.get('cli_error_string')}`"
        )
        add(f"- plaintext log available: {row.get('log_available')}")
        add(f"- verdict: **{'PASS' if row['passed'] else 'FAIL'}**")
        for key in CRITERIA:
            add(
                f"  - ({key}) {STATE[row['criteria'][key]]}: "
                f"{row['criteria_notes'][key]}"
            )
        add("")
    return "\n".join(lines)


def run(
    corpus: Path,
    report_dir: Path,
    *,
    bs: str,
    do_slice: bool,
    timeout: int,
    include: list[str],
    exclude: list[str],
    static_only: bool,
    limit: int | None,
    dry_run: bool,
    bs_version: str | None = None,
    log_dir: str | None = None,
) -> tuple[int, dict[str, Any]]:
    """Run the corpus sweep; returns ``(exit_code, summary)``."""
    files = discover(corpus, include, exclude)
    if limit:
        files = files[:limit]

    here = Path(__file__).resolve().parent
    summary: dict[str, Any] = {
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "corpus": _relative_to_tool(corpus, here),
        "report_dir": _relative_to_tool(report_dir, here),
        "bambu_studio": None if static_only else bs,
        "bs_version": bs_version,
        "mode": "static-only" if static_only else ("slice" if do_slice else "info-only"),
        "timeout_s": timeout,
        "rows": [],
    }

    if dry_run:
        for path in files:
            print(path)
        summary["total"] = len(files)
        summary["passed"] = 0
        summary["failed"] = 0
        return 0, summary

    if not files:
        print(f"no *.3mf found under {corpus}", file=sys.stderr)

    tool_error = False
    for index, path in enumerate(files, 1):
        print(f"[{index}/{len(files)}] {path}", flush=True)
        if static_only:
            result = static_only_result(path, bs_version=bs_version)
        else:
            workdir = report_dir / "work" / f"{index:03d}_{path.stem}"
            result = oracle.run_oracle(
                path,
                bs=bs,
                workdir=workdir,
                do_slice=do_slice,
                timeout=timeout,
                bs_version=bs_version,
                log_dir=log_dir,
            )
        if result.get("tool_error"):
            tool_error = True
            print(f"    TOOL ERROR: {result['tool_error']}", flush=True)
        row = _row(path, corpus, result)
        summary["rows"].append(row)
        state = "PASS" if row["passed"] else "FAIL"
        print(
            f"    {state} "
            + " ".join(f"{k}={STATE[row['criteria'][k]]}" for k in CRITERIA),
            flush=True,
        )

    summary["total"] = len(summary["rows"])
    summary["passed"] = sum(1 for r in summary["rows"] if r["passed"])
    summary["failed"] = summary["total"] - summary["passed"]

    report_dir.mkdir(parents=True, exist_ok=True)
    (report_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    (report_dir / "summary.md").write_text(render_markdown(summary), encoding="utf-8")
    print()
    print(
        f"wrote {report_dir / 'summary.json'} and {report_dir / 'summary.md'} "
        f"({summary['passed']}/{summary['total']} passed)"
    )

    if tool_error:
        return 2, summary
    return (0 if summary["failed"] == 0 else 1), summary


def main(argv: list[str] | None = None) -> int:
    """CLI entry point."""
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Run the compatibility oracle over every 3MF in the corpus."
    )
    parser.add_argument(
        "--corpus", default=str(here / "corpus"), help="corpus directory to walk"
    )
    parser.add_argument(
        "--report", default=str(here / "report"), help="directory for the report files"
    )
    parser.add_argument(
        "--bs", default=oracle.DEFAULT_BS, help="path to bambu-studio.exe"
    )
    parser.add_argument(
        "--slice",
        dest="do_slice",
        action="store_true",
        help="slice and re-export each file so criteria (e)(f)(g) are evaluated",
    )
    parser.add_argument(
        "--static-only",
        action="store_true",
        help="never launch bambu-studio.exe; evaluate static criteria only",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=oracle.DEFAULT_TIMEOUT,
        help=f"per-invocation timeout in seconds (default {oracle.DEFAULT_TIMEOUT})",
    )
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        metavar="SUBSTR",
        help="only test paths containing this substring (repeatable)",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        metavar="SUBSTR",
        help="skip paths containing this substring (repeatable)",
    )
    parser.add_argument(
        "--bs-version",
        metavar="VER",
        help="dotted version of the build under test, e.g. 02.08.02.61; enables the "
        "static 'newer version' check when no log is available",
    )
    parser.add_argument(
        "--log-dir",
        metavar="DIR",
        help="directory scanned for plaintext logs written during each run",
    )
    parser.add_argument("--limit", type=int, help="test at most N files")
    parser.add_argument(
        "--dry-run", action="store_true", help="list the files that would be tested"
    )
    args = parser.parse_args(argv)

    corpus = Path(args.corpus).resolve()
    if not corpus.is_dir():
        print(f"corpus directory not found: {corpus}", file=sys.stderr)
        return 2

    code, _ = run(
        corpus,
        Path(args.report).resolve(),
        bs=args.bs,
        do_slice=args.do_slice,
        timeout=args.timeout,
        include=args.include,
        exclude=args.exclude,
        static_only=args.static_only,
        limit=args.limit,
        dry_run=args.dry_run,
        bs_version=args.bs_version,
        log_dir=args.log_dir,
    )
    return code


if __name__ == "__main__":
    sys.exit(main())
