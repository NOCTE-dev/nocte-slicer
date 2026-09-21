// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "AutoTuneDialog.hpp"

#include "NocteMenu.hpp"

#include <memory>
#include <string>
#include <vector>

#include <wx/checklst.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/DialogButtons.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

namespace Slic3r {
namespace GUI {
namespace Nocte {

// See the note in DiagnoseDialog.cpp: `Nocte::` alone would find this namespace, not the engine's.
namespace engine = ::Slic3r::Nocte;

namespace {

wxString scope_label(engine::ConfigScope scope)
{
    switch (scope) {
    case engine::ConfigScope::Object:     return _L("object");
    case engine::ConfigScope::Volume:     return _L("part");
    case engine::ConfigScope::LayerRange: return _L("layer range");
    }
    return wxEmptyString;
}

// The config at the scope the recommendations target, which tune() reads two ways: as the context
// a rule matches on, and as the record of what the user already set there. Orca's per-object and
// per-volume configs are sparse, so "the key is present" is exactly "the user set it here"
// (AutoTune.cpp:69-80). A part selection therefore has to hand over the part's own config.
const DynamicPrintConfig& base_config_of(const PanelTarget &target)
{
    return target.part_selected ? target.volume->config.get() : target.object->config.get();
}

// The mesh as it sits on the plate: the volume's own transform, then the first instance's, so the
// overhang, footprint and stability measures are taken against the Z the printer will use.
// analyze() wants object coordinates with Z up in millimetres (GeometryAnalysis.hpp:71).
indexed_triangle_set plated_mesh_of(const PanelTarget &target)
{
    TriangleMesh mesh = target.volume->mesh();

    // Same composition as Plater.cpp:22034, instance first.
    Transform3d trafo = target.volume->get_matrix();
    if (! target.object->instances.empty() && target.object->instances.front() != nullptr)
        trafo = target.object->instances.front()->get_matrix() * trafo;
    mesh.transform(trafo, true);

    return mesh.its;
}

} // namespace

AutoTuneDialog::AutoTuneDialog(wxWindow *parent, const PanelTarget &target)
    : DPIDialog(parent, wxID_ANY, _L("Auto-tune"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_target(target)
{
    {
        wxBusyCursor cursor;
        const DynamicPrintConfig &base = base_config_of(m_target);
        m_features = engine::analyze(plated_mesh_of(m_target), engine::AnalysisParams(), base);
        m_result   = engine::tune(m_features, base, engine::TuneProfile());
    }

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    m_features_text = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_features_text->SetFont(Label::Body_14);
    main_sizer->Add(m_features_text, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    wxStaticText *recs_title = new wxStaticText(this, wxID_ANY, _L("Recommendations"));
    recs_title->SetFont(Label::Head_14);
    main_sizer->Add(recs_title, 0, wxBOTTOM, FromDIP(5));

    wxBoxSizer *recs_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_recs = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(400), FromDIP(190)), 0, NULL, 0);
    recs_sizer->Add(m_recs, 1, wxEXPAND | wxRIGHT, FromDIP(10));

    m_details = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(280), FromDIP(190)),
                               wxTE_MULTILINE | wxTE_READONLY);
    recs_sizer->Add(m_details, 1, wxEXPAND, 0);

    main_sizer->Add(recs_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status->SetFont(Label::Body_13);
    m_status->SetMinSize(wxSize(FromDIP(690), FromDIP(40)));
    main_sizer->Add(m_status, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    DialogButtons *dlg_btns = new DialogButtons(this, {L("Apply"), L("Close")});
    m_btn_apply = dlg_btns->GetButtonFromIndex(0);
    Button *btn_close = dlg_btns->GetButtonFromIndex(1);

    if (m_btn_apply != nullptr)
        m_btn_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->on_apply(); });
    if (btn_close != nullptr)
        btn_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (this->IsModal())
                this->EndModal(wxID_CLOSE);
            else
                this->Close();
        });
    dlg_btns->SetPrimaryButton(_L("Apply"));

    main_sizer->Add(dlg_btns, 0, wxEXPAND);

    m_recs->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { this->update_details(); });

    fill_features();
    fill_recommendations();
    update_details();

    if (m_btn_apply != nullptr)
        m_btn_apply->Enable(m_recs->GetCount() > 0);

    wxBoxSizer *outer_sizer = new wxBoxSizer(wxVERTICAL);
    outer_sizer->Add(main_sizer, 1, wxEXPAND | wxALL, FromDIP(20));
    SetSizerAndFit(outer_sizer);
    Layout();

    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

void AutoTuneDialog::on_dpi_changed(const wxRect &)
{
    this->Layout();
    this->Refresh();
}

void AutoTuneDialog::fill_features()
{
    wxString text = wxString::Format(_L("Inspecting part: %s"), from_u8(m_target.volume->name));
    if (m_target.model_part_count > 1)
        text += "  " + wxString::Format(_L("This object has %d model parts; this panel works on one part at a time."),
                                        m_target.model_part_count);

    // Units are spelled "mm3" / "mm2" on purpose: every string this panel adds to the catalogue is
    // plain ASCII, so no translator ever has to preserve a superscript byte sequence.
    text += "\n" + wxString::Format(_L("Volume: %.2f mm3    Solidity: %.2f    Stability: %.2f"),
                                    m_features.volume, m_features.solidity, m_features.stability_ratio);
    text += "\n" + wxString::Format(_L("Max overhang: %.1f deg    Steep overhang: %.1f %%    Min wall: %.2f mm"),
                                    m_features.max_overhang_angle, m_features.overhang_area_fraction * 100.,
                                    m_features.min_wall_thickness);

    if (! engine::analysis_available())
        text += "\n" + _L("The geometry measurements are not available in this build.");

    m_features_text->SetLabel(text);
}

void AutoTuneDialog::fill_recommendations()
{
    // TuneProfile's default threshold is what tune() was run with, so it is also what decides
    // which rows open ticked (AutoTune.hpp:42, :51).
    const engine::TuneProfile profile{};

    for (const engine::Recommendation &rec : m_result.recommendations) {
        wxString label = from_u8(rec.key) + ": " + from_u8(rec.previous_value) + " -> " + from_u8(rec.value);
        label += "  " + wxString::Format(_L("confidence %d%%"), int(rec.confidence * 100. + 0.5));
        label += "  " + wxString::Format(_L("scope: %s"), scope_label(rec.scope));

        const int row = m_recs->Append(label);
        m_recs->Check(unsigned(row), rec.confidence >= profile.min_confidence);
    }

    if (m_result.recommendations.empty())
        set_status(_L("The rules propose no change for this part."));
    else if (! m_result.notes.empty())
        set_status(wxString::Format(_L("%d notes; select a recommendation to read them."), int(m_result.notes.size())));
}

void AutoTuneDialog::update_details()
{
    const int selection = m_recs->GetSelection();
    if (selection == wxNOT_FOUND || selection < 0 || size_t(selection) >= m_result.recommendations.size()) {
        wxString text = _L("Select a recommendation to see the evidence behind it.");
        for (const std::string &note : m_result.notes)
            text += "\n\n" + from_u8(note);
        m_details->SetValue(text);
        return;
    }

    const engine::Recommendation &rec = m_result.recommendations[size_t(selection)];
    wxString text = from_u8(rec.reason);
    text += "\n\n" + wxString::Format(_L("Rule: %s"), from_u8(rec.rule_id));
    for (const std::string &item : rec.evidence)
        text += "\n" + from_u8(item);
    m_details->SetValue(text);
}

void AutoTuneDialog::set_status(const wxString &text)
{
    m_status->SetLabel(text);
    this->Layout();
}

void AutoTuneDialog::on_apply()
{
    if (m_committed)
        return;

    std::vector<size_t> chosen;
    for (unsigned int i = 0; i < m_recs->GetCount(); ++ i)
        if (m_recs->IsChecked(i))
            chosen.push_back(size_t(i));

    if (chosen.empty()) {
        set_status(_L("Nothing is ticked to apply."));
        return;
    }

    Plater     *plater = wxGetApp().plater();
    ObjectList *list   = wxGetApp().obj_list();
    ModelObject *object = m_target.object;
    if (plater == nullptr || list == nullptr || object == nullptr)
        return;

    wxBusyCursor cursor;

    // One snapshot for the whole run: a tuning pass is one decision, so it is one undo step. The
    // write itself is ObjectList::add_category_to_settings_from_selection()'s sequence
    // (GUI_ObjectList.cpp:2072-2097): take_snapshot, set_key_value with a cloned option, then
    // changed_object() and the settings-item refresh. The snapshot is taken right before the
    // first write, so a run that skips every recommendation leaves no empty undo step.
    int      written = 0;
    wxString skipped;
    for (size_t index : chosen) {
        const engine::Recommendation &rec = m_result.recommendations[index];

        ModelConfig *config = nullptr;
        if (rec.scope == engine::ConfigScope::Object) {
            config = &object->config;
        } else if (rec.scope == engine::ConfigScope::Volume) {
            const int volume_idx = rec.volume_idx >= 0 ? rec.volume_idx : m_target.vol_idx;
            if (volume_idx >= 0 && volume_idx < int(object->volumes.size()) && object->volumes[volume_idx] != nullptr)
                config = &object->volumes[volume_idx]->config;
        }

        if (config == nullptr) {
            if (skipped.IsEmpty())
                skipped = wxString::Format(_L("Skipped %s: there is no config at that scope."), from_u8(rec.key));
            continue;
        }

        // tune() already validated the key and the value against print_config_def. Building the
        // option again here is not redundant: set_key_value() needs a ConfigOption to own, and a
        // value that fails to deserialize must be reported rather than written as a no-op.
        std::unique_ptr<ConfigOption> option;
        const ConfigOptionDef *definition = print_config_def.get(rec.key);
        if (definition != nullptr) {
            try {
                option.reset(definition->create_default_option());
                if (! option || ! option->deserialize(rec.value))
                    option.reset();
            } catch (...) {
                option.reset();
            }
        }

        if (! option) {
            if (skipped.IsEmpty())
                skipped = wxString::Format(_L("Skipped %s: the proposed value is not legal for that setting."),
                                           from_u8(rec.key));
            continue;
        }

        if (written == 0)
            plater->take_snapshot("N\xC3\x98" "CTE auto-tune");
        config->set_key_value(rec.key, option.release());
        ++ written;
    }

    if (written > 0) {
        list->changed_object(m_target.obj_idx);
        list->update_and_show_object_settings_item();
    }

    m_committed = written > 0;
    if (m_btn_apply != nullptr)
        m_btn_apply->Enable(! m_committed);

    wxString status = wxString::Format(_L("Applied %d of %d settings."), written, int(chosen.size()));
    if (! skipped.IsEmpty())
        status += "\n" + skipped;
    set_status(status);
}

void show_auto_tune_dialog()
{
    const PanelTarget target = resolve_panel_target();
    if (! target.valid()) {
        show_error(nullptr, _L("Select exactly one object, or one model part of it, first."));
        return;
    }

    AutoTuneDialog dialog(static_cast<wxWindow *>(wxGetApp().mainframe), target);
    dialog.ShowModal();
}

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r
