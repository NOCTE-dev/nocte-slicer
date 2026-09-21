// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The one question the first-start import asks. It is deliberately NOT a wxDialog.
//
// run_data_dir_migration() is called from OrcaSlicer.cpp just before GUI_Run(), and GUI_Run()
// constructs GUI_App *before* wxEntry() (GUI_Init.cpp:44-66). GUI_App's constructor already
// reads <data_dir>/NocteSlicer.conf through init_app_config() (GUI_App.cpp:1122, :2517), and
// instance_check() takes the data directory lock immediately after. So the import has to finish
// before any wxApp exists, and no wx window, dialog or wxMessageBox can be shown at that point:
// wxApp::Initialize() (which runs gtk_init / the MSW toolkit init) has not run yet.
//
// The prompt is therefore a platform-native modal. On Windows that is MessageBoxW, the same
// thing OrcaSlicer.cpp:1345 and :7874 already use at this stage of start-up. Elsewhere there is
// no cheap pre-toolkit modal, so the prompt reports Unavailable and the caller starts fresh;
// File -> Import -> Import Configs remains the manual route (ADR-003 §3).

#ifndef slic3r_GUI_Nocte_MigrationDialog_hpp_
#define slic3r_GUI_Nocte_MigrationDialog_hpp_

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace GUI {
namespace Nocte {

enum class MigrationChoice {
    Import,      // copy the user-authored subset across
    StartFresh,  // leave the OrcaSlicer directory alone and start empty
    Unavailable, // no modal can be shown here; the caller treats this as StartFresh and logs
};

// Asks once, modally, whether to import `old_dir`. Never touches `old_dir`.
MigrationChoice ask_import_data_dir(const boost::filesystem::path &old_dir);

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_MigrationDialog_hpp_
