#!/usr/bin/env python3
# NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
"""Dry run of the first-start data-directory import (ADR-003 §3).

Mirrors the rules in ``src/slic3r/GUI/Nocte/DataDirMigrationRules.cpp`` so they can be exercised
against a real OrcaSlicer data directory before the C++ ever runs. The three predicates below are
the same three functions, in the same order, with the same lists; if one changes, change both.

This script **never writes anything**. It opens the source directory read-only, lists what would
be copied and what would be skipped, and prints the configuration keys that would be dropped.
Values are never printed: a configuration file can hold a session identifier.

    python tools/nocte/migration_dry_run.py                      # %APPDATA%\\OrcaSlicer
    python tools/nocte/migration_dry_run.py <dir> --per-file     # another directory, in detail

Exit codes: 0 the source looks importable, 1 there is nothing to import, 2 the run itself failed.
"""

import argparse
import json
import os
import sys

SOURCE_APP_KEY = "OrcaSlicer"
TARGET_APP_KEY = "NocteSlicer"

# --- the rules, mirrored from DataDirMigrationRules.cpp ---------------------------------------

COPIED_TOP_LEVEL_DIRS = ("user", "shapes", "SVG")

NEVER_COPIED_TOP_LEVEL_ENTRIES = (
    "cache",
    "log",
    "plugins",
    "orca_plugins",
    "python",
    "system",
    "ota",
    "models",
    "printers",
    ".orcaslicer_machine_id",
)

DROPPED_APP_CONFIG_KEYS = (
    "associate_3mf",
    "associate_stl",
    "associate_step",
    "associate_gcode",
    "associate_drc",
    "cloud_providers",
    "installed_networking",
    "use_printer_agents",
    "enable_ota",
    "sync_system_preset",
    "sync_user_preset",
    "stealth_mode",
    "hide_login_side_panel",
    "user_id",
    "dark_color_mode",
)

DROPPED_APP_CONFIG_KEY_PREFIXES = ("desktop_integration", "network_plugin")

DROPPED_APP_CONFIG_KEY_FRAGMENTS = ("token", "secret", "password")


def is_copied_top_level_dir(name):
    return name in COPIED_TOP_LEVEL_DIRS


def is_never_copied_top_level_entry(name):
    return name in NEVER_COPIED_TOP_LEVEL_ENTRIES or name.startswith("user_backup-")


def is_dropped_app_config_key(key):
    if key in DROPPED_APP_CONFIG_KEYS:
        return True
    if key.startswith(DROPPED_APP_CONFIG_KEY_PREFIXES):
        return True
    lowered = key.lower()
    return any(fragment in lowered for fragment in DROPPED_APP_CONFIG_KEY_FRAGMENTS)


# --- the walk ----------------------------------------------------------------------------------


def default_source_dir():
    """Where the C++ would look for the source directory on this platform."""
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA", "")
        return os.path.join(appdata, SOURCE_APP_KEY) if appdata else ""
    if sys.platform == "darwin":
        home = os.environ.get("HOME", "")
        return os.path.join(home, "Library", "Application Support", SOURCE_APP_KEY) if home else ""
    xdg = os.environ.get("XDG_CONFIG_HOME", "")
    if xdg:
        return os.path.join(xdg, SOURCE_APP_KEY)
    home = os.environ.get("HOME", "")
    return os.path.join(home, ".config", SOURCE_APP_KEY) if home else ""


def tree_size(root):
    """(file count, total bytes) under `root`, following the same recursion as the C++ copy."""
    count, total = 0, 0
    for current, _dirs, files in os.walk(root):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(current, name))
                count += 1
            except OSError:
                count += 1
    return count, total


def human(size):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return "%.1f %s" % (size, unit) if unit != "B" else "%d B" % size
        size /= 1024.0
    return "%d B" % size


def config_keys_to_drop(conf_path):
    """Key names only, never values. The file is JSON with an optional MD5 comment line after
    the closing brace (AppConfig.cpp:961-1112), which is what the C++ strips too."""
    with open(conf_path, "r", encoding="utf-8-sig", errors="replace") as handle:
        whole = handle.read()
    last_brace = whole.rfind("}")
    if last_brace < 0:
        raise ValueError("not a JSON object")
    parsed = json.loads(whole[: last_brace + 1])
    app = parsed.get("app")
    if not isinstance(app, dict):
        return [], []
    dropped = sorted(key for key in app if is_dropped_app_config_key(key))
    kept = sorted(key for key in app if not is_dropped_app_config_key(key))
    return dropped, kept


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("source", nargs="?", default=None, help="the data directory to read")
    parser.add_argument(
        "--per-file", action="store_true", help="list every file that would be copied, with sizes"
    )
    args = parser.parse_args(argv)

    source = args.source or default_source_dir()
    if not source:
        sys.stderr.write("migration_dry_run: cannot work out the default data directory\n")
        return 2
    if not os.path.isdir(source):
        print("No %s data directory at %s - nothing would be imported." % (SOURCE_APP_KEY, source))
        return 1

    conf_name = SOURCE_APP_KEY + ".conf"
    conf_path = os.path.join(source, conf_name)

    print("NOCTE data-directory import, dry run (nothing is written)")
    print("  source : %s" % source)
    print("  target : %s.conf in the %s data directory" % (TARGET_APP_KEY, TARGET_APP_KEY))
    print("")

    if not os.path.isfile(conf_path):
        print("No %s - the import would stop here (step 4)." % conf_name)
        return 1

    copied, skipped = [], []
    try:
        entries = sorted(os.listdir(source))
    except OSError as error:
        sys.stderr.write("migration_dry_run: %s\n" % error)
        return 2

    for name in entries:
        full = os.path.join(source, name)
        if os.path.isdir(full) and is_copied_top_level_dir(name):
            copied.append((name, full))
        else:
            skipped.append(name)

    print("Would copy (%d top-level entr%s):" % (len(copied), "y" if len(copied) == 1 else "ies"))
    total_files, total_bytes = 0, 0
    for name, full in copied:
        count, size = tree_size(full)
        total_files += count
        total_bytes += size
        print("  %-12s %5d file(s)  %s" % (name + "/", count, human(size)))
        if args.per_file:
            for current, _dirs, files in os.walk(full):
                for file_name in sorted(files):
                    path = os.path.join(current, file_name)
                    try:
                        print("      %10d  %s" % (os.path.getsize(path), os.path.relpath(path, source)))
                    except OSError:
                        print("      %10s  %s" % ("?", os.path.relpath(path, source)))
    print("  %-12s %5d file(s)  %s" % ("(total)", total_files, human(total_bytes)))
    print("")

    print("Would skip (%d top-level entries):" % len(skipped))
    for name in skipped:
        reason = "runtime state" if is_never_copied_top_level_entry(name) else "not on the allowlist"
        if name == conf_name:
            reason = "rewritten into %s.conf" % TARGET_APP_KEY
        print("  %-26s %s" % (name, reason))
    print("")

    try:
        dropped, kept = config_keys_to_drop(conf_path)
    except (ValueError, json.JSONDecodeError) as error:
        sys.stderr.write("migration_dry_run: cannot read %s: %s\n" % (conf_name, error))
        return 2

    print("Configuration: %d key(s) in \"app\", %d dropped, %d kept." % (
        len(dropped) + len(kept), len(dropped), len(kept)))
    for key in dropped:
        print("  drop  %s" % key)
    print("")
    print("RESULT: the source is importable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
