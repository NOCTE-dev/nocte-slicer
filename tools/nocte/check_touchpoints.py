#!/usr/bin/env python3
# NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
"""CI guard for the upstream touch-point allowlist.

The NØCTE fork keeps its own code in files upstream does not have, so that syncing with
OrcaSlicer stays a merge rather than a negotiation. A small number of upstream files still have
to be edited — the build lists, the 3mf writer, a few GUI entry points. This script enforces the
two rules that keep those edits reviewable:

1. Only files on the allowlist below may be modified.
2. Every modified upstream touch point must carry at least one ``NOCTE-BEGIN`` marker, so the
   fork's additions can be found, reviewed and re-applied after a sync.

Exit codes: 0 clean, 1 violations found, 2 the check itself could not run.
"""

import argparse
import os
import subprocess
import sys

# Upstream files the fork is allowed to edit. Each must carry a NOCTE-BEGIN marker (ADR-001).
UPSTREAM_TOUCH_POINTS = (
    # ADR-001: build lists, the 3mf writer, GUI entry points, the printer-agent registry.
    "version.inc",
    "CMakeLists.txt",
    "src/libslic3r/CMakeLists.txt",
    "src/slic3r/CMakeLists.txt",
    "tests/libslic3r/CMakeLists.txt",
    "src/libslic3r/Format/bbs_3mf.cpp",
    "src/slic3r/GUI/Plater.cpp",
    "src/slic3r/GUI/MainFrame.cpp",
    "src/slic3r/GUI/GUI_ObjectList.cpp",
    "src/slic3r/GUI/AboutDialog.cpp",
    "src/OrcaSlicer.cpp",
    "src/slic3r/Utils/NetworkAgentFactory.cpp",
    "src/slic3r/Utils/NetworkAgentFactory.hpp",
    # ADR-003: offline by default, product identity, executable rename. Palette files take
    # value-only edits; the other files take small marked hunks whose logic lives in GUI/Nocte/.
    "src/slic3r/GUI/GUI_App.cpp",
    "src/libslic3r/AppConfig.cpp",
    "src/libslic3r/libslic3r.h",
    "src/slic3r/GUI/Preferences.cpp",
    "src/slic3r/GUI/Widgets/StateColor.cpp",
    "src/libslic3r/Color.hpp",
    "src/slic3r/GUI/ImGuiWrapper.cpp",
    "src/slic3r/GUI/BitmapCache.cpp",
    "src/slic3r/GUI/GLTexture.cpp",
    "src/slic3r/GUI/GLCanvas3D.cpp",
    "src/slic3r/GUI/BBLTopbar.cpp",
    "src/slic3r/GUI/WebGuideDialog.cpp",
    "src/CMakeLists.txt",
    "src/libslic3r/libslic3r_version.h.in",
    "src/dev-utils/platform/msw/OrcaSlicer.rc.in",
    "src/slic3r/Utils/Process.cpp",
    "scripts/run_gettext.bat",
    "scripts/run_gettext.sh",
    "scripts/HintsToPot.py",
    "scripts/test_build_win.ps1",
)

# Directories the fork owns outright. No marker is required inside them.
OWNED_PREFIXES = (
    "README.md",  # replaced by the NØCTE README; upstream copy lives in docs/UPSTREAM-README-OrcaSlicer.md
    "resources/",
    "localization/",
    ".github/",
    "docs/",
    "tools/",
    "tests/data/nocte/",
)

# Path fragments that mark a file as NØCTE-owned wherever it lives.
OWNED_FRAGMENTS = ("/Nocte/",)

# Basename prefixes that mark a file as NØCTE-owned wherever it lives.
OWNED_BASENAME_PREFIXES = ("test_nocte_",)

MARKER = "NOCTE-BEGIN"


def run_git(args, repo_root):
    """Runs git and returns stdout, or raises RuntimeError with git's own message."""
    proc = subprocess.run(
        ["git"] + args,
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if proc.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), proc.stderr.strip()))
    return proc.stdout


def repo_root_of(start):
    proc = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        cwd=start,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if proc.returncode != 0:
        raise RuntimeError("not inside a git repository: %s" % proc.stderr.strip())
    return proc.stdout.strip()


def changed_files(repo_root, base, include_worktree):
    """Paths changed against `base`, repo-relative and forward-slashed."""
    paths = set()

    run_git(["rev-parse", "--verify", "--quiet", base + "^{commit}"], repo_root)
    for line in run_git(["diff", "--name-only", "%s...HEAD" % base], repo_root).splitlines():
        if line.strip():
            paths.add(line.strip())

    if include_worktree:
        # Staged, unstaged and untracked, so the check is useful before committing too.
        for args in (["diff", "--name-only", "HEAD"], ["ls-files", "--others", "--exclude-standard"]):
            for line in run_git(args, repo_root).splitlines():
                if line.strip():
                    paths.add(line.strip())

    return sorted(paths)


def is_owned(path):
    if path.startswith(OWNED_PREFIXES):
        return True
    normalised = "/" + path
    if any(fragment in normalised for fragment in OWNED_FRAGMENTS):
        return True
    basename = path.rsplit("/", 1)[-1]
    return basename.startswith(OWNED_BASENAME_PREFIXES)


def has_marker(repo_root, path):
    full = os.path.join(repo_root, path.replace("/", os.sep))
    if not os.path.isfile(full):
        # Deleted in this branch; nothing to look for. Deleting an upstream file is reported
        # separately, as a forbidden touch, only if it is not a touch point.
        return True
    with open(full, "r", encoding="utf-8", errors="replace") as handle:
        return MARKER in handle.read()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--base",
        default="upstream/main",
        help="commit-ish to compare against (default: upstream/main). A tag such as v2.4.2 works too.",
    )
    parser.add_argument(
        "--committed-only",
        action="store_true",
        help="only look at commits; ignore staged, unstaged and untracked changes.",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=40,
        help="how many paths to print per section before summarising the rest (0 prints all).",
    )
    parser.add_argument(
        "--repo",
        default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        help="path inside the repository to check (default: the repository this script lives in).",
    )
    args = parser.parse_args(argv)

    try:
        repo_root = repo_root_of(args.repo)
        paths = changed_files(repo_root, args.base, not args.committed_only)
    except RuntimeError as error:
        sys.stderr.write("check_touchpoints: %s\n" % error)
        if "upstream/main" in str(error):
            sys.stderr.write("check_touchpoints: hint: `git fetch upstream`, or pass --base v2.4.2.\n")
        return 2

    owned, touched_upstream, forbidden = [], [], []
    for path in paths:
        if is_owned(path):
            owned.append(path)
        elif path in UPSTREAM_TOUCH_POINTS:
            touched_upstream.append(path)
        else:
            forbidden.append(path)

    unmarked = [path for path in touched_upstream if not has_marker(repo_root, path)]

    print("NOCTE touch-point check")
    print("  repository : %s" % repo_root)
    print("  base       : %s" % args.base)
    print("  scope      : %s" % ("commits only" if args.committed_only else "commits + working tree"))
    print("  changed    : %d file(s)" % len(paths))
    print("")

    def listing(paths_to_print, render):
        if not paths_to_print:
            print("  (none)")
            return
        shown = paths_to_print if args.limit <= 0 else paths_to_print[: args.limit]
        for path in shown:
            print(render(path))
        if len(shown) < len(paths_to_print):
            print("  ... and %d more" % (len(paths_to_print) - len(shown)))

    print("NOCTE-owned files (%d):" % len(owned))
    listing(owned, lambda path: "  ok        %s" % path)
    print("")

    print("Upstream touch points (%d):" % len(touched_upstream))
    listing(touched_upstream, lambda path: "  %-9s %s" % ("ok" if path not in unmarked else "NO MARKER", path))
    print("")

    failed = False

    if forbidden:
        failed = True
        print("FAIL: %d file(s) outside the allowlist were modified:" % len(forbidden))
        listing(forbidden, lambda path: "  %s" % path)
        print("")
        print("  Move the change into src/libslic3r/Nocte/, src/slic3r/GUI/Nocte/ or another")
        print("  NOCTE-owned location, or extend the allowlist in this script together with the ADR")
        print("  that justifies the new touch point.")
        print("")

    if unmarked:
        failed = True
        print("FAIL: %d allowlisted file(s) carry no %s marker:" % (len(unmarked), MARKER))
        listing(unmarked, lambda path: "  %s" % path)
        print("")
        print("  Wrap every NOCTE addition in `%s <topic>` / `NOCTE-END` comments so the next" % MARKER)
        print("  upstream sync can find and re-apply it.")
        print("")

    if failed:
        print("RESULT: FAIL")
        return 1

    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
