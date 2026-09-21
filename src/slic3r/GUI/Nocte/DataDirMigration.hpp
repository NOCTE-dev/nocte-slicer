// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// First-start import of an OrcaSlicer data directory into the NOCTE one (ADR-003 §3).
//
// The entry point is Slic3r::GUI::Nocte::run_data_dir_migration(), declared in NocteUi.hpp and
// called from OrcaSlicer.cpp just before GUI_Run(). The pieces below are exposed so the
// detection can be read, reasoned about and reused without running the copy.
//
// The source directory is read-only, always: nothing under it is written, renamed, deleted or
// locked, and no lock file is created there. An OrcaSlicer installation keeps working unchanged.

#ifndef slic3r_GUI_Nocte_DataDirMigration_hpp_
#define slic3r_GUI_Nocte_DataDirMigration_hpp_

#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace GUI {
namespace Nocte {

// The application key of the fork this build can import from.
extern const char *const IMPORT_SOURCE_APP_KEY; // "OrcaSlicer"

// Where wxStandardPaths::GetUserDataDir() would put <app_key>, computed without wx because the
// import runs before any wxApp exists. Windows: %APPDATA%\<key>. macOS: ~/Library/Application
// Support/<key>. Linux: $XDG_CONFIG_HOME/<key>, else ~/.config/<key> (GUI_App.cpp:2554-2565).
// Empty when the environment does not say where the user's data lives.
boost::filesystem::path user_data_dir_for(const std::string &app_key);

// True when <exe dir>/data_dir exists, which makes the build portable and pins the data
// directory next to the binary (GUI_App.cpp:2530-2545). The import is skipped in that case.
bool portable_data_dir_present();

// Copies the user-authored subset of `old_dir` into `new_dir` and writes the rewritten
// configuration file. Throws nothing; reports failure by returning false after logging.
bool import_data_dir(const boost::filesystem::path &old_dir, const boost::filesystem::path &new_dir);

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_DataDirMigration_hpp_
