// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// What the first-start import copies out of an OrcaSlicer data directory, and what it drops
// out of the configuration file it brings across (ADR-003 §3).
//
// These are pure predicates: no wx, no I/O, no state. They are mirrored line for line by
// tools/nocte/migration_dry_run.py, which is the only way the rules can be exercised against a
// real data directory without building the GUI. Change one, change the other.

#ifndef slic3r_GUI_Nocte_DataDirMigrationRules_hpp_
#define slic3r_GUI_Nocte_DataDirMigrationRules_hpp_

#include <string>

namespace Slic3r {
namespace GUI {
namespace Nocte {

// Top-level entries of the source data directory that are copied, whole. An allowlist: anything
// a future OrcaSlicer release adds is skipped until someone decides it is user-authored.
bool is_copied_top_level_dir(const std::string &name);

// Top-level entries that are runtime state, caches, downloaded code or another application's
// identity, and are never copied. Everything outside is_copied_top_level_dir() is skipped
// anyway; this is the documented half of the rule, and the assertion the dry run prints.
bool is_never_copied_top_level_entry(const std::string &name);

// Keys erased from the "app" object of the imported configuration file. They describe the other
// application's file associations, desktop integration, network plugin and cloud session, or
// they hold a credential. `preset_folder` is deliberately kept: it names the user/<id> subfolder
// that is copied with user/** (GUI_App.cpp:3315, :5599, :8815).
bool is_dropped_app_config_key(const std::string &key);

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_DataDirMigrationRules_hpp_
