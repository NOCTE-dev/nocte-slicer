// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The interface between the NØCTE GUI additions and the upstream GUI files they hook into.
// Every function here is implemented under src/slic3r/GUI/Nocte/ and called from a marked hunk
// in an upstream touch point (ADR-003), so the two sides can be written independently.

#ifndef slic3r_GUI_Nocte_NocteUi_hpp_
#define slic3r_GUI_Nocte_NocteUi_hpp_

class wxMenu;

namespace Slic3r {

namespace GUI {

class MainFrame;

namespace Nocte {

// The "NØCTE" menu shown next to Help. Owned by the caller's menu bar or top bar once appended.
// Implemented in NocteMenu.cpp; called from MainFrame::init_menubar_as_editor().
wxMenu* create_main_menu(MainFrame* frame);

// Adds the NØCTE entries ("Diagnose & repair…", "Auto-tune this part…") to an object or part
// context menu. The menus are long-lived singletons, so the call is idempotent: entries already
// present are not added again. `type` is the ObjectList item type bit mask of the selection.
// Implemented in NocteMenu.cpp; called from ObjectList::show_context_menu().
void append_object_menu_items(wxMenu* menu, int type);

// First-start migration from an OrcaSlicer data directory into the NØCTE one: copies the
// user-authored subset, never writes to the source, skips under --datadir and in portable mode,
// asks the user once. Implemented in DataDirMigration.cpp; called from the CLI entry point before
// the GUI application is constructed.
void run_data_dir_migration();

} // namespace Nocte
} // namespace GUI

namespace Nocte {

// Id of the NØCTE LAN Developer Mode printer agent in NetworkAgentFactory and in the
// `printer_agent` option of a printer preset.
constexpr const char* LAN_AGENT_ID = "nocte-lan";

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_NocteUi_hpp_
