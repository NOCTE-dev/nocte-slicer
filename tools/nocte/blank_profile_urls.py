#!/usr/bin/env python3
# NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
"""Blanks the top-level ``"url"`` of every vendor profile in ``resources/profiles``.

ADR-003: NØCTE Slicer never synchronises profiles. ``PresetUpdater::sync_vendor_config()`` and
``PresetUpdater::check_new_vendors()`` build their requests from ``AppConfig::profile_update_url()``
(now empty), but each vendor JSON also carries a ``"url"`` pointing at the vendor's own update
server. Those are blanked here so nothing in the tree still names an update endpoint.

Only the top-level key of ``resources/profiles/<Vendor>.json`` is touched — one tab of indentation,
the third line of every file that has it. ``version``, ``name`` and everything under
``resources/profiles/<Vendor>/`` are left exactly as they are, and the edit is textual so the
files keep their tabs, key order and line endings.

Run from anywhere::

    python tools/nocte/blank_profile_urls.py           # rewrite in place
    python tools/nocte/blank_profile_urls.py --check   # report only, exit 1 if any url is set

Exit codes: 0 nothing left to do (or rewritten), 1 in ``--check`` with a non-empty url, 2 error.
"""

import argparse
import io
import os
import re
import sys

# A top-level key: exactly one tab of indentation. Nested "url" keys (if a vendor ever grows one)
# are indented deeper and are not matched.
TOP_LEVEL_URL = re.compile(r'^(\t"url"\s*:\s*")([^"]*)(")', re.MULTILINE)


def profiles_dir(repo_root):
    return os.path.join(repo_root, "resources", "profiles")


def vendor_files(directory):
    """Top-level vendor JSONs only — never the per-vendor subfolders."""
    for name in sorted(os.listdir(directory)):
        if name.lower().endswith(".json") and os.path.isfile(os.path.join(directory, name)):
            yield os.path.join(directory, name)


def blank_one(path, dry_run):
    with io.open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()

    found = TOP_LEVEL_URL.search(text)
    if not found:
        return "no-url", None
    if not found.group(2):
        return "already-empty", None

    old_url = found.group(2)
    if not dry_run:
        with io.open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(TOP_LEVEL_URL.sub(r"\1\3", text, count=1))
    return "blanked", old_url


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report only; do not write.")
    parser.add_argument(
        "--repo",
        default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        help="repository root (default: the repository this script lives in).",
    )
    args = parser.parse_args(argv)

    directory = profiles_dir(args.repo)
    if not os.path.isdir(directory):
        sys.stderr.write("blank_profile_urls: no such directory: %s\n" % directory)
        return 2

    counts = {"blanked": 0, "already-empty": 0, "no-url": 0}
    for path in vendor_files(directory):
        state, old_url = blank_one(path, args.check)
        counts[state] += 1
        if state == "blanked":
            print("  %-9s %s  (was %s)" % ("would be" if args.check else "blanked", os.path.basename(path), old_url))

    print("")
    print("vendor profiles scanned : %d" % sum(counts.values()))
    print("  url blanked           : %d" % counts["blanked"])
    print("  url already empty     : %d" % counts["already-empty"])
    print("  no top-level url key  : %d" % counts["no-url"])

    return 1 if (args.check and counts["blanked"]) else 0


if __name__ == "__main__":
    sys.exit(main())
