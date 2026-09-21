// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "NocteMenu.hpp"

#include "AutoTuneDialog.hpp"
#include "DiagnoseDialog.hpp"
#include "NocteUi.hpp"

#include <vector>

#include <wx/menu.h>

#include "libslic3r/Model.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

namespace Slic3r {
namespace GUI {
namespace Nocte {

// The labels carry "..." rather than a mnemonic. An "&" in a wxMenu label is the accelerator
// escape, so the briefed "Diagnose & repair" would either render as "Diagnose _repair" or have to
// be spelled "&&" — which then leaks into the msgid every translator sees. "and" costs nothing.
wxString menu_label_diagnose()
{
    return _L("Diagnose and repair selected object...");
}

wxString menu_label_auto_tune()
{
    return _L("Auto-tune selected part...");
}

PanelTarget resolve_panel_target()
{
    PanelTarget target;

    ObjectList *list = wxGetApp().obj_list();
    if (list == nullptr)
        return target;

    // get_selection_indexes() is the same resolver fix_through_cgal() and smooth_mesh() use
    // (GUI_ObjectList.cpp:6114, :6267). obj_idxs is sorted and de-duplicated; vol_idxs is empty
    // when whole objects are selected and holds one entry per selected part otherwise.
    std::vector<int> obj_idxs, vol_idxs;
    list->get_selection_indexes(obj_idxs, vol_idxs);
    if (obj_idxs.size() != 1 || vol_idxs.size() > 1)
        return target;

    // ObjectList::object() only guards the lower bound (GUI_ObjectList.cpp:6951-6957), so the
    // upper one is checked here rather than trusted.
    const std::vector<ModelObject *> *objects = list->objects();
    if (objects == nullptr || obj_idxs.front() < 0 || obj_idxs.front() >= int(objects->size()))
        return target;

    ModelObject *object = list->object(obj_idxs.front());
    if (object == nullptr || object->volumes.empty())
        return target;

    int part_count = 0;
    for (const ModelVolume *volume : object->volumes)
        if (volume != nullptr && volume->is_model_part())
            ++ part_count;
    if (part_count == 0)
        return target;

    int vol_idx = -1;
    bool part_selected = false;
    if (vol_idxs.empty()) {
        // The whole object: v1 operates on its first model part and says so in the summary.
        for (size_t i = 0; i < object->volumes.size(); ++ i)
            if (object->volumes[i] != nullptr && object->volumes[i]->is_model_part()) {
                vol_idx = int(i);
                break;
            }
    } else {
        vol_idx = vol_idxs.front();
        if (vol_idx < 0 || vol_idx >= int(object->volumes.size()))
            return target;
        // A modifier, a negative volume or a support blocker carries no printable surface.
        if (object->volumes[vol_idx] == nullptr || ! object->volumes[vol_idx]->is_model_part())
            return target;
        part_selected = true;
    }
    if (vol_idx < 0)
        return target;

    target.obj_idx          = obj_idxs.front();
    target.vol_idx          = vol_idx;
    target.object           = object;
    target.volume           = object->volumes[vol_idx];
    target.part_selected    = part_selected;
    target.model_part_count = part_count;
    return target;
}

bool panel_target_available()
{
    const Plater *plater = wxGetApp().plater();
    if (plater == nullptr || ! plater->is_view3D_shown())
        return false;
    return resolve_panel_target().valid();
}

namespace {

// Both entries, appended to `menu` in order. `parent` is the window the enable predicate is bound
// to through wxEVT_UPDATE_UI; append_menu_item() ignores the predicate entirely when it is null
// (wxExtensions.cpp:87-90), so it always has to be a live window.
void append_nocte_entries(wxMenu *menu, wxWindow *parent)
{
    append_menu_item(menu, wxID_ANY, menu_label_diagnose(),
        _L("Check the selected object for mesh errors and repair them step by step"),
        [](wxCommandEvent &) { show_diagnose_dialog(); }, "", nullptr,
        []() { return panel_target_available(); }, parent);

    append_menu_item(menu, wxID_ANY, menu_label_auto_tune(),
        _L("Measure the selected part and propose print settings for it"),
        [](wxCommandEvent &) { show_auto_tune_dialog(); }, "", nullptr,
        []() { return panel_target_available(); }, parent);
}

} // namespace

wxMenu* create_main_menu(MainFrame *frame)
{
    wxMenu *menu = new wxMenu();

    append_nocte_entries(menu, frame);

    menu->AppendSeparator();

    // Not translated, and deliberately so: it is the product name. The split escape keeps \x from
    // swallowing the following "C" as a fourth hex digit; the precedent is AboutDialog.cpp:25.
    const wxString about = wxString::FromUTF8("&About N\xC3\x98" "CTE Slicer");
    append_menu_item(menu, wxID_ANY, about, about,
        [](wxCommandEvent &) { Slic3r::GUI::about(); }, "", nullptr,
        []() { return true; }, frame);

    return menu;
}

void append_object_menu_items(wxMenu *menu, int type)
{
    if (menu == nullptr)
        return;
    // Object and part menus only. Plates, instances, layer ranges and settings items have no
    // single mesh behind them.
    if ((type & (itObject | itVolume)) == 0)
        return;

    // The plater's context menus are long-lived singletons, rebuilt only when the menu factory is
    // rebuilt, so show_context_menu() reaches the same wxMenu again and again. wxMenu::FindItem()
    // compares against the mnemonic-stripped item label and returns wxNOT_FOUND when there is no
    // match (precedent: GUI_Factories.cpp:1039, MainFrame.cpp:3673). Our labels carry no "&", so
    // the comparison is exact, and asking for the label we are about to append is the cheapest
    // idempotency guard there is — one that also survives the menu being destroyed and rebuilt,
    // which a cache of wxMenu pointers would not.
    const wxString diagnose = menu_label_diagnose();
    const wxString tune     = menu_label_auto_tune();
    if (menu->FindItem(diagnose) != wxNOT_FOUND || menu->FindItem(tune) != wxNOT_FOUND)
        return;

    menu->AppendSeparator();
    append_nocte_entries(menu, wxGetApp().plater());
}

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r
