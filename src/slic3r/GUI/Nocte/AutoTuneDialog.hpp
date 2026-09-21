// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The "Auto-tune" panel: what the geometry of the selected part measures, what the rule table
// proposes because of it, and an opt-in application of those proposals into the per-object or
// per-part config.
//
// Nothing is written until "Apply", and then everything is written inside one Plater snapshot, so
// a run of the tuner is a single entry in the undo stack. The write itself is the sequence
// ObjectList::add_category_to_settings_from_selection() uses (GUI_ObjectList.cpp:2072-2097):
// take_snapshot, ModelConfig::set_key_value() with a cloned option, changed_object(),
// update_and_show_object_settings_item().
//
// Legality of a key at a scope is the engine's job, not the panel's: tune() validates every
// recommendation against print_config_def before it emits it (AutoTune.cpp:131-144). The panel
// re-derives the option from print_config_def anyway, because it has to build a ConfigOption to
// hand to set_key_value() and a value that fails to deserialize must not become a silent no-op.

#ifndef slic3r_GUI_Nocte_AutoTuneDialog_hpp_
#define slic3r_GUI_Nocte_AutoTuneDialog_hpp_

#include "libslic3r/Nocte/AutoTune.hpp"
#include "libslic3r/Nocte/GeometryAnalysis.hpp"

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Nocte/NocteMenu.hpp"

class Button;
class wxCheckListBox;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {
namespace Nocte {

class AutoTuneDialog : public DPIDialog
{
public:
    AutoTuneDialog(wxWindow *parent, const PanelTarget &target);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void fill_features();
    void fill_recommendations();
    void update_details();
    void set_status(const wxString &text);
    void on_apply();

    PanelTarget                   m_target;
    Slic3r::Nocte::PartFeatures   m_features;
    Slic3r::Nocte::TuneResult     m_result;

    wxStaticText   *m_features_text = nullptr;
    wxCheckListBox *m_recs          = nullptr;
    wxTextCtrl     *m_details       = nullptr;
    wxStaticText   *m_status        = nullptr;

    Button *m_btn_apply = nullptr;

    // Set once the settings have been written. The panel then stops offering to write them again,
    // so a second Apply cannot stack a second snapshot of the same proposals.
    bool m_committed = false;
};

// Opens the panel for the current selection, or explains why it cannot. Called from both menus.
void show_auto_tune_dialog();

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_AutoTuneDialog_hpp_
