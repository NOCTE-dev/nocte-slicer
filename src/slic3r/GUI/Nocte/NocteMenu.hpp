// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The NØCTE menu, the two object/part entries, and the one thing both panels need before they can
// run: which single object or part the user picked.
//
// ADR-003 §2 puts the NØCTE features behind a menu next to Help and behind the object context
// menu, as DPIDialogs, because the gizmo registry (GLGizmosManager) is not an upstream touch
// point. The two builders are declared in NocteUi.hpp, which is what the upstream files include;
// everything here is the private vocabulary the two dialogs share with them.

#ifndef slic3r_GUI_Nocte_NocteMenu_hpp_
#define slic3r_GUI_Nocte_NocteMenu_hpp_

#include <wx/string.h>

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace GUI {
namespace Nocte {

// The one object or part the panels operate on.
//
// v1 works on a single ModelVolume. When the user selected a part, that is the part; when the
// user selected the whole object, it is the object's first model part, and the panel says so in
// its summary. Modifiers, negative volumes and support blockers are never the target: they carry
// no printable surface, so diagnosing or tuning them would be meaningless.
struct PanelTarget
{
    int          obj_idx = -1;
    // Index into ModelObject::volumes of the volume the panels read and write.
    int          vol_idx = -1;
    ModelObject *object  = nullptr;
    ModelVolume *volume  = nullptr;
    // The user selected a part rather than the whole object. Decides the default config scope of
    // the auto-tuner and which ModelConfig it reads as `base`.
    bool         part_selected = false;
    // How many model parts the object has. Above one, the panels say which part they are on.
    int          model_part_count = 0;

    bool valid() const { return this->object != nullptr && this->volume != nullptr && this->obj_idx >= 0; }
};

// Resolves the object list selection into a PanelTarget. The result is invalid unless exactly one
// object, or exactly one model part, is selected.
PanelTarget resolve_panel_target();

// The enable predicate behind both entries: the 3D view is up and the selection resolves.
bool panel_target_available();

// The menu labels, in one place: the builder appends them and the idempotency guard in
// append_object_menu_items() looks them up again with wxMenu::FindItem().
wxString menu_label_diagnose();
wxString menu_label_auto_tune();

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_NocteMenu_hpp_
