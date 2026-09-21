// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The "Diagnose and repair" panel: what is wrong with the selected part, the plan that would fix
// it, and a per-step accept/preview/undo loop over Nocte::RepairSession.
//
// The panel owns the session, not the model: accepting a step changes only the session's private
// copy of the mesh. Nothing reaches the ModelVolume until "Apply selected", which takes one
// Plater snapshot and then performs the mesh replacement sequence ObjectList::smooth_mesh() uses
// (GUI_ObjectList.cpp:6297-6318). That is what makes every experiment inside the panel free, and
// the single write to the model undoable from Edit > Undo like any other edit.
//
// Threading: the engine calls run synchronously under a wxBusyCursor. Meshes of the sizes this
// panel is meant for diagnose in well under a second, and a background thread would buy a
// progress bar at the cost of a mesh lifetime problem — the ModelVolume can be deleted from under
// a worker. Revisit when the 10M-triangle case from MeshRepair.hpp's [VERIFY] note lands.

#ifndef slic3r_GUI_Nocte_DiagnoseDialog_hpp_
#define slic3r_GUI_Nocte_DiagnoseDialog_hpp_

#include <memory>

#include "libslic3r/Nocte/MeshRepair.hpp"

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Nocte/NocteMenu.hpp"

class Button;
class wxCheckListBox;
class wxDataViewListCtrl;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {
namespace Nocte {

class DiagnoseDialog : public DPIDialog
{
public:
    DiagnoseDialog(wxWindow *parent, const PanelTarget &target);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void fill_summary();
    void fill_issues();
    void fill_plan();
    void update_details();
    void update_buttons();
    void set_status(const wxString &text);

    void on_preview();
    void on_apply();
    void on_undo();

    PanelTarget                                   m_target;
    std::unique_ptr<Slic3r::Nocte::RepairSession> m_session;

    wxStaticText       *m_summary = nullptr;
    wxDataViewListCtrl *m_issues  = nullptr;
    wxCheckListBox     *m_steps   = nullptr;
    wxTextCtrl         *m_details = nullptr;
    wxStaticText       *m_status  = nullptr;

    Button *m_btn_preview = nullptr;
    Button *m_btn_apply   = nullptr;
    Button *m_btn_undo    = nullptr;

    // Set once the repaired mesh has been written to the ModelVolume. From then on the panel is
    // read-only: undoing the write is the application's undo stack, not the session's.
    bool m_committed = false;
};

// Opens the panel for the current selection, or explains why it cannot. Called from both menus.
void show_diagnose_dialog();

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_DiagnoseDialog_hpp_
