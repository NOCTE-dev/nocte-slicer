// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "DiagnoseDialog.hpp"

#include "NocteMenu.hpp"

#include <optional>
#include <string>
#include <vector>

#include <wx/checklst.h>
#include <wx/dataview.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

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

// The engine lives in Slic3r::Nocte; we are in Slic3r::GUI::Nocte, where an unqualified `Nocte::`
// would find this namespace instead. One alias, and every engine name below is unambiguous.
namespace engine = ::Slic3r::Nocte;

namespace {

wxString severity_label(engine::Severity severity)
{
    switch (severity) {
    case engine::Severity::Info:     return _L("Info");
    case engine::Severity::Warning:  return _L("Warning");
    case engine::Severity::Blocking: return _L("Blocking");
    }
    return wxEmptyString;
}

wxString join_u8(const std::vector<std::string> &items, const wxString &separator)
{
    wxString out;
    for (const std::string &item : items) {
        if (! out.IsEmpty())
            out += separator;
        out += from_u8(item);
    }
    return out;
}

} // namespace

DiagnoseDialog::DiagnoseDialog(wxWindow *parent, const PanelTarget &target)
    : DPIDialog(parent, wxID_ANY, _L("Diagnose and repair"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_target(target)
{
    // The mesh is read in the volume's own coordinate system, which is where the repaired mesh is
    // written back: a repair must not silently bake the object or instance transform into the
    // geometry. ObjectList::smooth_mesh() replaces meshes on exactly these terms.
    {
        wxBusyCursor cursor;
        m_session = std::make_unique<engine::RepairSession>(m_target.volume->mesh().its);
    }

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_summary->SetFont(Label::Body_14);
    main_sizer->Add(m_summary, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    wxStaticText *issues_title = new wxStaticText(this, wxID_ANY, _L("Issues found"));
    issues_title->SetFont(Label::Head_14);
    main_sizer->Add(issues_title, 0, wxBOTTOM, FromDIP(5));

    m_issues = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(660), FromDIP(160)));
    m_issues->AppendTextColumn(_L("Issue"),       wxDATAVIEW_CELL_INERT, FromDIP(150));
    m_issues->AppendTextColumn(_L("Severity"),    wxDATAVIEW_CELL_INERT, FromDIP(90));
    // "Occurrences" rather than "Count": the catalogue already renders "Count" as a verb in
    // Spanish ("Contar"), which is wrong for a column header, and only the msgstr may be edited.
    m_issues->AppendTextColumn(_L("Occurrences"), wxDATAVIEW_CELL_INERT, FromDIP(90));
    m_issues->AppendTextColumn(_L("Explanation"), wxDATAVIEW_CELL_INERT, FromDIP(330));
    main_sizer->Add(m_issues, 1, wxEXPAND | wxBOTTOM, FromDIP(10));

    wxStaticText *plan_title = new wxStaticText(this, wxID_ANY, _L("Repair plan"));
    plan_title->SetFont(Label::Head_14);
    main_sizer->Add(plan_title, 0, wxBOTTOM, FromDIP(5));

    wxBoxSizer *plan_sizer = new wxBoxSizer(wxHORIZONTAL);

    // A plain wxCheckListBox rather than a toggle column: the tree already builds one
    // (ConfigWizard_private.hpp:276 constructs wxCheckListBox through exactly this ctor), and it
    // needs no model, no renderer and no value round-trip to tell us what the user ticked.
    m_steps = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(390), FromDIP(150)), 0, NULL, 0);
    plan_sizer->Add(m_steps, 1, wxEXPAND | wxRIGHT, FromDIP(10));

    m_details = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(260), FromDIP(150)),
                               wxTE_MULTILINE | wxTE_READONLY);
    plan_sizer->Add(m_details, 1, wxEXPAND, 0);

    main_sizer->Add(plan_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status->SetFont(Label::Body_13);
    m_status->SetMinSize(wxSize(FromDIP(660), FromDIP(40)));
    main_sizer->Add(m_status, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    DialogButtons *dlg_btns = new DialogButtons(this, {L("Preview"), L("Apply selected"), L("Undo last"), L("Close")});
    m_btn_preview = dlg_btns->GetButtonFromIndex(0);
    m_btn_apply   = dlg_btns->GetButtonFromIndex(1);
    m_btn_undo    = dlg_btns->GetButtonFromIndex(2);
    Button *btn_close = dlg_btns->GetButtonFromIndex(3);

    if (m_btn_preview != nullptr)
        m_btn_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->on_preview(); });
    if (m_btn_apply != nullptr)
        m_btn_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->on_apply(); });
    if (m_btn_undo != nullptr)
        m_btn_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { this->on_undo(); });
    if (btn_close != nullptr)
        btn_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (this->IsModal())
                this->EndModal(wxID_CLOSE);
            else
                this->Close();
        });
    dlg_btns->SetPrimaryButton(_L("Apply selected"));

    main_sizer->Add(dlg_btns, 0, wxEXPAND);

    m_steps->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { this->update_details(); });

    fill_summary();
    fill_issues();
    fill_plan();
    update_details();
    update_buttons();

    wxBoxSizer *outer_sizer = new wxBoxSizer(wxVERTICAL);
    outer_sizer->Add(main_sizer, 1, wxEXPAND | wxALL, FromDIP(20));
    SetSizerAndFit(outer_sizer);
    Layout();

    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(m_issues);
}

void DiagnoseDialog::on_dpi_changed(const wxRect &)
{
    this->Layout();
    this->Refresh();
}

void DiagnoseDialog::fill_summary()
{
    const engine::DiagnosticsResult diagnostics = m_session->diagnostics();

    // Spelled as two whole sentences rather than "Manifold: yes/no": "no" translates to "no" in
    // Spanish, and an entry whose msgstr equals its msgid reads as untranslated to every catalogue
    // check there is.
    wxString text = wxString::Format(_L("Facets: %d    Open edges: %d    Shells: %d"),
                                     int(diagnostics.stats.number_of_facets), diagnostics.open_edges,
                                     diagnostics.shells);
    text += "    " + (diagnostics.manifold ? _L("The mesh is manifold.") : _L("The mesh is not manifold."));

    text += "\n" + wxString::Format(_L("Inspecting part: %s"), from_u8(m_target.volume->name));
    if (m_target.model_part_count > 1)
        text += "  " + wxString::Format(_L("This object has %d model parts; this panel works on one part at a time."),
                                        m_target.model_part_count);

    if (! diagnostics.skipped_checks.empty())
        text += "\n" + wxString::Format(_L("Checks skipped: %s"), join_u8(diagnostics.skipped_checks, "; "));

    m_summary->SetLabel(text);
}

void DiagnoseDialog::fill_issues()
{
    m_issues->DeleteAllItems();

    const engine::DiagnosticsResult diagnostics = m_session->diagnostics();
    for (const engine::MeshIssue &issue : diagnostics.issues) {
        wxVector<wxVariant> fields;
        fields.push_back(wxVariant(from_u8(engine::to_string(issue.kind))));
        fields.push_back(wxVariant(severity_label(issue.severity)));
        fields.push_back(wxVariant(wxString::Format("%d", int(issue.count))));
        fields.push_back(wxVariant(from_u8(issue.explanation)));
        m_issues->AppendItem(fields);
    }
}

void DiagnoseDialog::fill_plan()
{
    const engine::RepairPlan &plan = m_session->plan();

    // The plan is built once, at construction, and its indices stay valid for the session's life
    // (MeshRepair.hpp:71-73). So the rows are appended once and only their labels are refreshed.
    const bool first_fill = m_steps->GetCount() == 0;

    for (size_t i = 0; i < plan.ops.size(); ++ i) {
        const engine::RepairOp &op = plan.ops[i];

        wxString label = from_u8(op.title);
        label += "  " + wxString::Format(_L("tier %d"), op.tier);
        if (op.lossy)
            label += "  " + _L("lossy");
        if (! op.implemented)
            label += "  " + _L("not available in this build");

        switch (m_session->state_of(i)) {
        case engine::RepairStepState::Applied:  label += "  " + _L("applied");  break;
        case engine::RepairStepState::Rejected: label += "  " + _L("rejected"); break;
        case engine::RepairStepState::Failed:   label += "  " + _L("failed");   break;
        case engine::RepairStepState::Pending:  break;
        }

        if (first_fill) {
            m_steps->Append(label);
            // Steps this build cannot run are shown, so the escalation path is visible, but they
            // are never ticked: accepting one would fail with a message and nothing else.
            m_steps->Check(unsigned(i), op.implemented);
        } else {
            m_steps->SetString(unsigned(i), label);
        }
    }

    if (first_fill && plan.ops.empty())
        set_status(_L("The mesh is clean. No repair is needed."));
}

void DiagnoseDialog::update_details()
{
    const int selection = m_steps->GetSelection();
    if (selection == wxNOT_FOUND) {
        m_details->SetValue(_L("Select a step to see why it is proposed."));
        return;
    }

    const engine::RepairPlan &plan = m_session->plan();
    if (selection < 0 || size_t(selection) >= plan.ops.size())
        return;

    const engine::RepairOp &op = plan.ops[size_t(selection)];
    wxString text = from_u8(op.title) + "\n\n" + from_u8(op.rationale);
    if (! op.targets_issue_ids.empty())
        text += "\n\n" + join_u8(op.targets_issue_ids, ", ");
    m_details->SetValue(text);
}

void DiagnoseDialog::update_buttons()
{
    const bool has_steps = m_steps->GetCount() > 0;
    if (m_btn_preview != nullptr)
        m_btn_preview->Enable(! m_committed && has_steps);
    if (m_btn_apply != nullptr)
        m_btn_apply->Enable(! m_committed && has_steps);
    if (m_btn_undo != nullptr)
        m_btn_undo->Enable(! m_committed && m_session->can_undo());
}

void DiagnoseDialog::set_status(const wxString &text)
{
    m_status->SetLabel(text);
    this->Layout();
}

void DiagnoseDialog::on_preview()
{
    const int selection = m_steps->GetSelection();
    if (selection == wxNOT_FOUND) {
        set_status(_L("Select a step first."));
        return;
    }

    const engine::RepairPlan &plan = m_session->plan();
    if (selection < 0 || size_t(selection) >= plan.ops.size())
        return;
    if (! plan.ops[size_t(selection)].implemented) {
        set_status(_L("This step cannot be run in this build."));
        return;
    }

    engine::RepairStepResult result;
    {
        wxBusyCursor cursor;
        result = m_session->preview(size_t(selection));
    }

    if (! result.succeeded) {
        set_status(wxString::Format(_L("The step failed: %s"), from_u8(result.message)));
        return;
    }

    set_status(wxString::Format(_L("Before: %d triangles, %d open edges, %d shells."),
                                result.before.tris, result.before.open_edges, result.before.shells) +
               "\n" +
               wxString::Format(_L("After: %d triangles, %d open edges, %d shells."),
                                result.after.tris, result.after.open_edges, result.after.shells));
}

void DiagnoseDialog::on_undo()
{
    if (! m_session->can_undo()) {
        set_status(_L("There is nothing to undo."));
        return;
    }

    {
        wxBusyCursor cursor;
        m_session->undo();
    }

    fill_summary();
    fill_issues();
    fill_plan();
    update_buttons();
    set_status(_L("The last accepted step was undone."));
}

void DiagnoseDialog::on_apply()
{
    if (m_committed)
        return;

    std::vector<size_t> chosen;
    for (unsigned int i = 0; i < m_steps->GetCount(); ++ i)
        if (m_steps->IsChecked(i))
            chosen.push_back(size_t(i));

    if (chosen.empty()) {
        set_status(_L("Nothing is ticked to apply."));
        return;
    }

    Plater     *plater = wxGetApp().plater();
    ObjectList *list   = wxGetApp().obj_list();
    if (plater == nullptr || list == nullptr)
        return;

    int      applied = 0;
    wxString failure;
    {
        wxBusyCursor cursor;
        // In plan order, which is tier order: a later step relies on what an earlier one cleaned up.
        for (size_t step : chosen) {
            const engine::RepairStepResult result = m_session->accept(step);
            if (result.succeeded)
                ++ applied;
            else if (failure.IsEmpty())
                failure = wxString::Format(_L("The step failed: %s"), from_u8(result.message));
        }
    }

    fill_summary();
    fill_issues();
    fill_plan();

    if (applied == 0) {
        set_status(failure.IsEmpty() ? _L("No step changed the mesh.") : failure);
        update_buttons();
        return;
    }

    // One snapshot for the whole apply, taken before the model is touched, so Edit > Undo puts the
    // original mesh back in a single step. The replacement sequence below is
    // ObjectList::smooth_mesh(), GUI_ObjectList.cpp:6297-6318.
    plater->take_snapshot("N\xC3\x98" "CTE repair");

    ModelObject *object = m_target.object;
    ModelVolume *volume = m_target.volume;

    const bool keep_painting = wxGetApp().app_config->get_bool("keep_painting");
    const std::optional<TriangleSelector::SavedPainting> saved_painting =
        keep_painting ? volume->save_painting() : std::optional<TriangleSelector::SavedPainting>{};

    volume->set_mesh(m_session->current());
    volume->restore_painting(saved_painting);
    volume->calculate_convex_hull();
    volume->invalidate_convex_hull_2d();
    volume->set_new_unique_id();

    object->invalidate_bounding_box();
    object->ensure_on_bed();
    plater->changed_mesh(m_target.obj_idx);

    list->update_item_error_icon(m_target.obj_idx, m_target.vol_idx);
    list->update_info_items(size_t(m_target.obj_idx));

    m_committed = true;
    update_buttons();

    wxString status = wxString::Format(_L("Applied %d of %d steps; the repaired mesh is now on the plate."),
                                       applied, int(chosen.size()));
    if (! failure.IsEmpty())
        status += "\n" + failure;
    set_status(status);
}

void show_diagnose_dialog()
{
    const PanelTarget target = resolve_panel_target();
    if (! target.valid()) {
        show_error(nullptr, _L("Select exactly one object, or one model part of it, first."));
        return;
    }

    DiagnoseDialog dialog(static_cast<wxWindow *>(wxGetApp().mainframe), target);
    dialog.ShowModal();
}

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r
