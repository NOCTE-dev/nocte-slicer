# W4 report — NØCTE panels (diagnose & repair, auto-tune), menus, Spanish strings

Branch `int/block3b`, worktree `C:\dev\nocte-slicer-b`. Nothing committed; no git state was changed.
`python tools/nocte/check_touchpoints.py --base nocte-base-2026-09-16` → **RESULT: PASS**.

---

## 1. Files

**New — NØCTE-owned (six files, for the integrator's CMake hunk)**

| File | What it is |
|---|---|
| `src/slic3r/GUI/Nocte/NocteMenu.hpp` | `PanelTarget`, `resolve_panel_target()`, `panel_target_available()`, the two menu labels |
| `src/slic3r/GUI/Nocte/NocteMenu.cpp` | `create_main_menu()`, `append_object_menu_items()` — the two frozen entry points |
| `src/slic3r/GUI/Nocte/DiagnoseDialog.hpp` | |
| `src/slic3r/GUI/Nocte/DiagnoseDialog.cpp` | the repair panel + `show_diagnose_dialog()` |
| `src/slic3r/GUI/Nocte/AutoTuneDialog.hpp` | |
| `src/slic3r/GUI/Nocte/AutoTuneDialog.cpp` | the tuning panel + `show_auto_tune_dialog()` |

**CMake for the integrator** — `src/slic3r/CMakeLists.txt`, after `GUI/Nocte/MigrationDialog.hpp`
(currently line 796). Alphabetical order is not used in that block; this matches the existing style:

```cmake
    GUI/Nocte/NocteMenu.cpp
    GUI/Nocte/NocteMenu.hpp
    GUI/Nocte/DiagnoseDialog.cpp
    GUI/Nocte/DiagnoseDialog.hpp
    GUI/Nocte/AutoTuneDialog.cpp
    GUI/Nocte/AutoTuneDialog.hpp
```

No other CMake change is needed: the engine (`libslic3r/Nocte/**`) is already listed
(`src/libslic3r/CMakeLists.txt:510-527`), and `libslic3r_cgal` is already linked.

**Upstream touch points**

| File | Hunks | What |
|---|---:|---|
| `src/slic3r/GUI/MainFrame.cpp` | 1 new (`nocte-panels`, the include) + 2 existing `nocte-identity` blocks shrunk | the inline placeholder menu deleted in both branches, replaced by one `create_main_menu(this)` call each; markers kept; net **−26/+35** lines including the .po-unrelated context |
| `src/slic3r/GUI/GUI_ObjectList.cpp` | 2 (`nocte-panels`: include + the context-menu hook) | +16 lines, no new method, no header change |

`GUI_ObjectList.hpp` is **not** allowlisted and was **not** touched — see §3.

**Localization**

* `localization/i18n/es/NocteSlicer_es.po` — 56 new entries appended at the end, CRLF preserved,
  UTF-8, one `# AI Translated` block comment.
* `localization/i18n/list.txt` — the three new `.cpp` files appended (the `.hpp` files carry no
  `_L()`).

---

## 2. Quoted signatures (every API used was opened and read)

```
Slic3r::GUI::Nocte::create_main_menu(MainFrame* frame) -> wxMenu*         Nocte/NocteUi.hpp:22
Slic3r::GUI::Nocte::append_object_menu_items(wxMenu*, int type)           Nocte/NocteUi.hpp:28
append_menu_item(wxMenu*, int id, const wxString& string, const wxString& description,
                 std::function<void(wxCommandEvent&)> cb, const std::string& icon = "",
                 wxEvtHandler* = nullptr, std::function<bool()> const cb_condition = []{return true;},
                 wxWindow* parent = nullptr, int insert_pos = wxNOT_FOUND)
                                                       wxExtensions.hpp:32-34, body .cpp:96-111
                 -- the enable predicate is bound ONLY when parent != nullptr (.cpp:87-90)
wxMenu::FindItem(const wxString&) -> int, wxNOT_FOUND when absent
                                     precedent GUI_Factories.cpp:1039, MainFrame.cpp:3673-3678
enum ItemType { itUndef=0, itPlate=1, itObject=2, itVolume=4, ... }  ObjectDataViewModel.hpp:25-36
extern void Slic3r::GUI::about()                                          GUI.hpp:80
wxString Slic3r::GUI::from_u8(const std::string&)                         GUI.hpp:72
void show_error(wxWindow*, const wxString&, bool has_code_excerpts=false) GUI.hpp:45
GUI_App::obj_list() -> ObjectList*                                        GUI_App.hpp:652
GUI_App::UpdateDlgDarkUI(wxDialog*)                                       GUI_App.hpp:434
GUI_App::UpdateDVCDarkUI(wxDataViewCtrl*, bool highlited=false)           GUI_App.hpp:437
AppConfig::get_bool(const std::string& key) const                         AppConfig.hpp:148

ObjectList::get_selection_indexes(std::vector<int>&, std::vector<int>&)   GUI_ObjectList.hpp:257
                                                                         body :548-577  (PUBLIC)
ObjectList::objects() const -> std::vector<ModelObject*>*                 GUI_ObjectList.hpp:224
ObjectList::object(const int obj_idx) const -> ModelObject*               GUI_ObjectList.hpp:226
                                    body :6951-6957 -- guards obj_idx<0 ONLY, no upper bound
ObjectList::multiple_selection() const -> bool                            GUI_ObjectList.hpp:397
ObjectList::changed_object(const int obj_idx = -1) const                  GUI_ObjectList.hpp:339
ObjectList::update_and_show_object_settings_item()                        GUI_ObjectList.hpp:430
                                              body :5875-5883 -> part_selection_changed()
ObjectList::update_item_error_icon(const int obj_idx, int vol_idx) const  GUI_ObjectList.hpp:443
ObjectList::update_info_items(size_t obj_idx, wxDataViewItemArray* = nullptr,
                              bool added_object=false, bool color_mode_changed=false)
                                                                          GUI_ObjectList.hpp:433

Plater::is_view3D_shown() const                                           Plater.hpp:462
Plater::take_snapshot(const std::string&)                                 Plater.hpp:578
Plater::changed_mesh(int obj_idx)                                         Plater.hpp:552

ModelVolume::mesh() const -> const TriangleMesh&                          Model.hpp:864
ModelVolume::set_mesh(const indexed_triangle_set&)                        Model.hpp:868
ModelVolume::save_painting() const -> std::optional<TriangleSelector::SavedPainting>
                                                                          Model.hpp:893
ModelVolume::restore_painting(const std::optional<...>&, bool keep_existing_paint=false)
                                                                          Model.hpp:896
ModelVolume::calculate_convex_hull() / invalidate_convex_hull_2d()        Model.hpp:968, :973
ModelVolume::set_new_unique_id()                                          Model.hpp:1020
ModelVolume::is_model_part() const                                        Model.hpp:914
ModelVolume::get_matrix() const -> const Transform3d&                     Model.hpp:1017
ModelInstance::get_matrix() const -> const Transform3d&                   Model.hpp:1358
ModelObject::invalidate_bounding_box() / ensure_on_bed()                  Model.hpp:454
TriangleMesh::transform(const Transform3d&, bool fix_left_handed=false)   TriangleMesh.hpp:115
TriangleMeshStats::number_of_facets (uint32_t)                            TriangleMesh.hpp:49

ModelConfig::get() const throw() -> const DynamicPrintConfig&             PrintConfig.hpp:2374
ModelConfig::set_key_value(const std::string&, ConfigOption*) -> bool     PrintConfig.hpp:2364
ConfigOptionDef::create_default_option() const -> ConfigOption*      precedent AutoTune.cpp:41-52
print_config_def.get(key) -> const ConfigOptionDef*                  precedent AutoTune.cpp:131

Nocte::diagnose(const indexed_triangle_set&, const DiagnosticsParams&)    MeshDiagnostics.hpp:186
Nocte::plan_from(const DiagnosticsResult&, const RepairOpParams&)         MeshRepair.hpp:33
Nocte::RepairSession::RepairSession(indexed_triangle_set, const DiagnosticsParams&)
                                                                          MeshRepair.hpp:42
  plan() :44   preview(size_t) :46   accept(size_t) :48   undo() :50   can_undo() :52
  current() :54   diagnostics() :56 (BY VALUE)   state_of(size_t) :59
Nocte::analyze(const indexed_triangle_set&, const AnalysisParams&, const DynamicPrintConfig&)
                                                                          GeometryAnalysis.hpp:80
Nocte::analysis_available()                                               GeometryAnalysis.hpp:84
Nocte::tune(const PartFeatures&, const DynamicPrintConfig&, const TuneProfile&)  AutoTune.hpp:74
Recommendation{rule_id,key,value,previous_value,reason,evidence,scope,volume_idx,confidence}
                                                                          AutoTune.hpp:27-44

DialogButtons(wxWindow*, std::vector<wxString> non_translated_labels,
              const wxString& primary_btn_label = "", int left_aligned_buttons_count = 0)
                                              DialogButtons.hpp:19, body DialogButtons.cpp:8-33
              -- it calls _L(label) itself, so my call sites wrap each label in L(...)
DialogButtons::GetButtonFromIndex(int) / SetPrimaryButton(wxString)   DialogButtons.hpp:27, :40
DPIAware(wxWindow*, wxWindowID, const wxString& title, const wxPoint& = wxDefaultPosition,
         const wxSize& = wxDefaultSize, long style = wxDEFAULT_FRAME_STYLE, ...)
                                              GUI_Utils.hpp:95-96; DPIDialog :283 `using ...`
virtual void on_dpi_changed(const wxRect&) = 0                            GUI_Utils.hpp:211
wxCheckListBox(parent, id, pos, size, 0, NULL, style)    precedent ConfigWizard_private.hpp:276
wxDataViewListCtrl::AppendTextColumn(label, mode, width, align, flags)
                                                         precedent PrintHostDialogs.cpp:1228
wxDataViewListCtrl::AppendItem(const wxVector<wxVariant>&)  precedent PrintHostDialogs.cpp:1300-1317
```

---

## 3. Which `ObjectList` members I used, and why no new hook was needed

Everything the panels need is **already public**, so `GUI_ObjectList.hpp` (not allowlisted) stays
untouched and no free function or lambda had to reach into privates:

* **Selection →** `get_selection_indexes(obj_idxs, vol_idxs)` (`:257`). This is the same resolver
  `fix_through_cgal()` (`:6114`) and `smooth_mesh()` (`:6267`) use, and it answers both cases in one
  call: `vol_idxs` empty means whole objects, otherwise one entry per selected part. That made
  `get_selected_item_indexes()` (`:256`, also public) and the `Selection` class unnecessary.
* **Model access →** `objects()` (`:224`) for the upper bound, `object(idx)` (`:226`) for the object.
* **Mesh write-back →** `update_item_error_icon(obj_idx, vol_idx)` (`:443`) and
  `update_info_items(obj_idx)` (`:433`), after `Plater::changed_mesh()`.
* **Config write-back →** `changed_object(obj_idx)` (`:339`) and
  `update_and_show_object_settings_item()` (`:430`).
* `multiple_selection()` (`:397`) in the context-menu hook.

**The hook itself.** `type` in `show_context_menu()` is declared *inside* the single-selection
branch (`:1641`) and is out of scope at the `if (menu) plater->PopupMenu(menu);` tail, so the marked
block re-reads it from the same source — `m_objects_model->GetItemType(GetSelection())` — which is
legal there because the block is inside `ObjectList`'s own member function. That keeps the change to
**one** hunk at the tail instead of two (a declaration before the branch plus an assignment inside
it).

---

## 4. Design decisions worth knowing

* **The dialogs own a `RepairSession`, not the model.** `accept()` mutates only the session's copy,
  so previewing and accepting inside the panel is free. Exactly one write reaches the
  `ModelVolume`, inside one `take_snapshot("NØCTE repair")`, using the replacement sequence from
  `ObjectList::smooth_mesh()` (`GUI_ObjectList.cpp:6297-6318`), painting preserved under
  `app_config->get_bool("keep_painting")` like upstream. After that write the panel goes read-only:
  undoing it is Edit → Undo, not `session.undo()`. That is the briefed "undo only before Apply",
  made explicit in the UI instead of left as a trap.
* **The session already diagnoses and plans.** `RepairSession`'s constructor runs `diagnose()` and
  `plan_from()` (MeshRepair.hpp:42, :73), so the panel reads `plan()` / `diagnostics()` rather than
  calling the two free functions a second time on the same mesh.
* **Repair reads the volume-local mesh; tuning reads the plated mesh.** A repair must write back in
  the frame it read, so `DiagnoseDialog` uses `volume->mesh().its` untransformed. `analyze()` wants
  object coordinates with Z up (`GeometryAnalysis.hpp:71`), so `AutoTuneDialog` composes
  `instance->get_matrix() * volume->get_matrix()` (the idiom at `Plater.cpp:22034`) before measuring —
  otherwise a rotated part reports the wrong overhangs and a mirrored one the wrong footprint.
* **`base_config` follows the selection**: the volume's `ModelConfig` for a part, the object's for an
  object. `tune()` reads `base` as the record of what the user already set at the target scope
  (`AutoTune.cpp:69-80`), and Orca's per-scope configs are sparse, so handing it the wrong scope
  would report every key as a user override.
* **v1 works on one model part.** The summary says which part, and says so explicitly when the
  object has more than one. Modifiers, negative volumes and connectors never resolve as a target.
* **`&` became `and` in the menu labels.** `&` is the wx mnemonic escape; `"Diagnose & repair"` would
  render as `Diagnose _repair`, and the `&&` spelling would leak into every translator's msgid.
* **All 61 msgids are ASCII.** `NØCTE` appears only in `wxString::FromUTF8("&About N\xC3\x98" "CTE
  Slicer")` (untranslated — it is the product name, per AGENTS.md's rule on brand names) and in the
  two snapshot names, which are `std::string` literals like upstream's `"smooth_mesh"`. No
  translator ever has to preserve a multi-byte escape.
* **Two strings deliberately differ from the brief's wording.** `"Count"` became `"Occurrences"` —
  the catalogue already renders `Count` as the verb *"Contar"*, and only `msgstr` may be edited.
  `"Manifold: yes/no"` became two whole sentences, because `"no"` translates to `"no"` and an entry
  whose `msgstr` equals its `msgid` counts as untranslated.
* **Known gap:** the issue explanations and step rationales come from the engine as English
  `std::string`s and are shown verbatim. Making them translatable means a message-id scheme inside
  `libslic3r/Nocte`, which is a separate change.

---

## 5. Localization — verified, not assumed

Run from the worktree with the in-tree tools:

```
tools/xgettext.exe --keyword=L --keyword=_L --keyword=_u8L ... --boost --no-wrap
                   -f <the three new .cpp> -o nocte.pot        -> 61 msgids, exit 0
tools/msgfmt.exe --check-format -o es.mo localization/i18n/es/NocteSlicer_es.po
                   -> 6132 translated, 28 untranslated, exit 0
```

A script then compared the two: **0 of the 61 extracted msgids are missing from the Spanish
catalogue**, and **0 of mine have `msgstr == msgid`**. Six msgids already existed and were *not*
re-added (`Info`, `Warning`, `Preview`, `Close`, `Apply`, and `Count`, which I stopped using).
Format strings carry `#, c-format, boost-format`, matching what xgettext emits for the existing
`"Downloading %d%%..."` entry (`NocteSlicer_es.po:6687`); placeholder order and `\n` parity are
identical on both sides of every pair.

---

## 6. Compile risks, ranked

1. **`wxDataViewListCtrl::AppendItem(fields)` and `AppendTextColumn(label, mode, width)`** — the
   issue list. Both are copied from `PrintHostDialogs.cpp:1228` and `:1300-1317`, which is the same
   class, so the overload set is proven in this tree. I deliberately did **not** use
   `AppendToggleColumn`: `FileArchiveDialog.cpp:191` calls the `wxDataViewCtrl` overload
   `(label, model_column, mode, width)`, which is a *different* signature from the
   `wxDataViewListCtrl` one `(label, mode, width, align, flags)`, and there is no in-tree precedent
   for the latter. That is why the checkbox lists are `wxCheckListBox`.
2. **`wxCheckListBox` constructed as `(this, wxID_ANY, pos, size, 0, NULL, 0)`** — exactly the ctor
   `ConfigWizard_private.hpp:276` instantiates for the same class. Only `Append`, `Check`,
   `IsChecked`, `GetCount`, `GetSelection`, `SetString` and `wxEVT_LISTBOX` are used; no
   `GetToggleValue`-style wx-3.1+ API anywhere.
3. **`DialogButtons` with custom labels.** `GetButtonFromIndex(0..3)` is the only accessor used,
   because `preview` / `apply selected` / `undo last` / `close` are not in `m_standardIDs`
   (`DialogButtons.hpp:59-96`) and therefore have no `wxID_*`. Precedent for custom labels:
   `CloneDialog.cpp:51` (`"Fill"`), `CrealityDiscoveryDialog.cpp:33` (`"Scan"`). Every label is
   wrapped in `L(...)` so xgettext sees it, since `DialogButtons` calls `_L()` on a runtime value.
4. **`namespace engine = ::Slic3r::Nocte;`** in both dialog `.cpp`s. Inside `Slic3r::GUI::Nocte`, an
   unqualified `Nocte::` finds *this* namespace, not the engine's — the alias is what makes every
   engine name below unambiguous. If it were missing, the failure would be a wall of
   "no member named 'diagnose'".
5. **`ModelVolume::set_mesh(m_session->current())`** — `current()` returns
   `const indexed_triangle_set&`, an exact match for the `Model.hpp:868` overload among six
   `set_mesh` overloads. An lvalue, so the `&&` overloads are not viable and there is no ambiguity.
6. **`-Wunused-private-field` (fatal).** Every private member of both dialog classes is read: I
   checked each one by hand. `m_committed` in particular is read in `update_buttons()` /
   `on_apply()`.
7. **`Button` is in the global namespace** (`Widgets/Button.hpp:33`), not `Slic3r::GUI`, so both
   headers forward-declare `class Button;` outside the namespace block. `-Wmismatched-tags` is
   satisfied: every forward declaration uses `class`, as the definitions do.
8. **`const engine::TuneProfile profile{};`** — value-initialised rather than
   `const TuneProfile profile;`, which some compilers reject for a class without a user-provided
   default constructor.
9. **No local compiler.** None of this was built. Everything else is sizer arithmetic.

---

## 7. Manual test script

`tests/data/nocte/` does not exist yet, so the script uses the shared corpus under `tests/data/`.

**A. The menus**

1. Start NØCTE Slicer with an empty plate. **NØCTE** appears next to **Help** — in the top-bar
   drop-down and, with "tabs as menu" on, in the menu bar.
2. Open it: *Diagnose and repair selected object…*, *Auto-tune selected part…*, a separator,
   *About NØCTE Slicer*. The first two are **greyed out** (nothing is selected). *About* opens the
   About dialog.
3. `File → Import → Import 3MF/STL/…` → `tests/data/frog_legs.obj`. Select the object in the object
   list. Both entries become **enabled**.
4. Right-click the object in the object list: the normal object menu, then a separator, then the same
   two entries. **Right-click again, and again** — there must still be exactly one separator and one
   pair of entries. (This is the idempotency guard; if it is broken you will see the pair stack up.)
5. Switch to the Preview tab: the context menu does not open at all (upstream behaviour), and the
   menu-bar entries grey out because `is_view3D_shown()` is false.
6. Select two objects: both entries grey out again.

**B. Diagnose and repair** — `tests/data/two_hollow_squares.obj` (two separate closed shells) or any
broken STL you have.

1. Object selected → *Diagnose and repair selected object…*. The panel opens after a brief busy
   cursor.
2. Summary: `Facets: N    Open edges: N    Shells: N` and one of the two manifold sentences, then
   `Inspecting part: <name>`. On a multi-part object it also says it works on one part at a time.
3. Issue list: one row per issue with the stable kind name, a translated severity, the count and the
   engine's explanation.
4. Repair plan: one ticked row per implemented step, in tier order; unimplemented steps appear
   **unticked** and marked *not available in this build*. Click a row → the right-hand pane shows the
   rationale and the issue ids it targets.
5. **Preview** on a ticked row → a two-line before/after (triangles, open edges, shells). Press it
   twice: the numbers must be identical, because preview never mutates the session.
6. **Undo last** is greyed out until something has been accepted.
7. **Apply selected** → busy cursor, the summary and the issue list refresh from the re-diagnosis,
   the status says `Applied N of M steps; the repaired mesh is now on the plate.`, and the three
   action buttons grey out. Behind the dialog the model has changed and the object list's warning
   icon is gone or reduced.
8. **Close**, then `Edit → Undo`: one step restores the original mesh, and the undo stack entry is
   named `NØCTE repair`.
9. Re-open the panel on the repaired object: the mesh reports as clean and the plan is empty, with
   *The mesh is clean. No repair is needed.* in the status line.

**C. Auto-tune** — `tests/data/frog_legs.obj` (real overhangs) or `tests/data/overhang.obj`.

1. Select the object → *Auto-tune selected part…*.
2. Header: `Inspecting part: …`, then `Volume / Solidity / Stability`, then
   `Max overhang / Steep overhang / Min wall`. The numbers must be non-zero — if they are all zero
   the transform composition in `plated_mesh_of()` is wrong.
3. Recommendation rows read `key: current -> proposed   confidence NN%   scope: object|part`.
   Rows at **confidence ≥ 50 %** open ticked; the rest open unticked. Click one → reason, rule id
   and evidence on the right. With nothing selected, the pane lists the engine's notes.
4. **Apply** → the status says `Applied N of M settings.`, the button greys out, and the object
   gains a **Settings** child in the object list carrying exactly the ticked keys.
5. `Edit → Undo` once → all of them disappear together (one snapshot, `NØCTE auto-tune`).
6. Re-open the panel: the keys you just applied now come back as **low-confidence** rows with
   *user override present* in the evidence, and unticked. That is the invariant from
   `AutoTune.hpp:6-7` visible in the UI.
7. Select a **part** of a multi-part object and repeat: the recommendations are read against, and
   written to, that part's config, not the object's.

**D. Spanish**

Preferences → language → Español, restart. Every label, column header, button and status line above
is Spanish. The engine's explanations and rationales stay English — see §4, known gap.
