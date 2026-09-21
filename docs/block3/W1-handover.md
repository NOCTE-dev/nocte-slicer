# W1 → W2 handover: offline-by-default hunks for `MainFrame.cpp`, `Plater.cpp`, `GUI_App.cpp`

W1 owns `AppConfig.cpp`, `Preferences.cpp`, `WebGuideDialog.cpp`, `resources/web/guide/**`,
`resources/profiles/*.json` and `src/slic3r/GUI/Nocte/OfflinePolicy.{hpp,cpp}`. The three files
below are W2's. Every block here is complete and ready to paste; the anchor lines are quoted from
the tree at `int/block3a` as of this handover, so search for the anchor rather than trusting the
line number after your own edits have shifted the file.

All hunks are `// NOCTE-BEGIN nocte-offline` … `// NOCTE-END`, per ADR-003 §4.

`OfflinePolicy.hpp` gives you `Slic3r::GUI::Nocte::is_offline_build()` (constexpr, always true) and
`Nocte::offline_reason()`. Most hunks below do not need it — an unconditional early return is
shorter and does not add an include — so it is used only where the guard should read as policy.

---

## 0. What W1 already changed that W2 must know

| Change | File | Consequence for W2 |
|---|---|---|
| `VERSION_CHECK_URL` / `PROFILE_UPDATE_URL` → `""` | `AppConfig.cpp:43-49` | `PresetUpdater::priv::set_download_prefs` (`PresetUpdater.cpp:290-299`) now leaves `enabled_config_update` false, so `sync_vendor_config()` is dead. **`GUI_App::check_new_version_sf()` is not guarded** — it would call `Http::get("")`. Hunk 3.2 is mandatory. |
| `stealth_mode` default → `true` | `AppConfig.cpp:402-412` | The startup sync block at `GUI_App.cpp:990-1002` is already inert, except for the unguarded `this->check_new_version_sf();` on line 999. No hunk needed there once 3.2 lands. |
| `hide_login_side_panel` default → `true` | `AppConfig.cpp:405-412` | Home page ships without the login panel. |
| `sync_system_preset` default → `false` | `AppConfig.cpp:557-562` | — |
| `dark_color_mode` seeded `"1"` for **all** platforms (moved out of `#ifdef _WIN32`) | `AppConfig.cpp:353-362` | **The seed alone is not enough on non-Windows.** `GUI_App::on_init_inner` overwrites it from the system appearance at `GUI_App.cpp:3066-3070` (`#ifdef _MSW_DARK_MODE` / `#ifndef __WINDOWS__`: `set("dark_color_mode", app.IsDark() ? "1" : "0")` followed by `save()`), and `update_dark_config()` (`GUI_Utils.cpp:315-320`) does the same on every system colour change. W2 owns the visual side and `GUI_App.cpp`; it needs a hunk there if dark-by-default is to hold on macOS/Linux. Also: the Preferences "Enable dark Mode" toggle is still inside `#ifdef _WIN32` (`Preferences.cpp:1615-1618`), so those users would have no way back — W1 did not touch it. |
| `cloud_providers` fresh-install default → `""` | `AppConfig.cpp:929-936` | `AppConfig::get_cloud_providers()` (`AppConfig.cpp:1733-1753`) still returns `{"orca"}` for an empty value — it hard-inserts `"orca"`. That is harmless because the cloud agent is null, but do not read `has_cloud_provider("orca") == false` as "offline". |
| Preferences **Online** tab removed in full; "Multi device management" removed | `Preferences.cpp` | `NetworkTestDialog` has no caller left in `Preferences.cpp`. After your hunk 1.1 it has none in `MainFrame.cpp` either — see the orphan list in W1's report. |
| Wizard flow is now `1 → 21 → 22 → finish` | `resources/web/guide/**` | Pages `11` (login region), `4orca` (stealth mode), `5` (network plug-in), `6` (plug-in download), `3`, `31` are unreachable. W1 could not delete them (tool permission); the integrator should. |

---

## 1. `src/slic3r/GUI/MainFrame.cpp`

### 1.1 Help menu — drop "Open Network Test" and "Check for Updates"

Anchor: `static wxMenu* generate_help_menu()` at **`MainFrame.cpp:2669`**.

Two edits in one function. First, at `:2688-2695`, the anchor is:

```cpp
    // Troubleshoot center
    append_menu_item(helpMenu, wxID_ANY, _L("Troubleshoot Center"), "",
        [](wxCommandEvent&) { wxGetApp().troubleshoot(); });
```

Replace the four lines that follow it (`:2691-2694`) with:

```cpp
    // NOCTE-BEGIN nocte-offline
    // ADR-003: NØCTE Slicer opens no outbound socket, so there is nothing for a network test to
    // test and no release feed to check. "Open Network Test" and "Check for Updates" are removed;
    // "Troubleshoot Center" and "Show Tip of the Day" stay.
    // NOCTE-END
```

Second, at `:2708-2714`, delete the whole "Check New Version" entry:

```cpp
    // Check New Version
    append_menu_item(helpMenu, wxID_ANY, _L("Check for Updates"), _L("Check for Updates"),
        [](wxCommandEvent&) {
            wxGetApp().check_new_version_sf(true, 1);
        }, "", nullptr, []() {
            return true;
        });
```

→ nothing (the `// NOCTE-END` of the first block covers the topic; if you prefer a second marked
block, use the same header text).

**Reference check:** `NetworkTestDialog` is then referenced nowhere in `MainFrame.cpp` except the
`#include "NetworkTestDialog.hpp"` at `:60`. Leave the include — `TroubleshootDialog.cpp:48`
includes the same header. `GUI_App::check_new_version_sf` keeps its declaration
(`GUI_App.hpp:528`) and its definition; hunk 3.2 empties it.

### 1.2 "Sync Presets" ×2

Anchors: `MainFrame.cpp:3324-3340` (top menu) and `MainFrame.cpp:3461-3477` (File menu). Both
blocks are byte-identical apart from the menu variable (`top_menu` / `fileMenu`). Anchor for the
first:

```cpp
    append_menu_item(
        top_menu, wxID_ANY, _L("Sync Presets"), _L("Pull and apply the latest presets from OrcaCloud"),
```

Replace each whole `append_menu_item(...)` call (up to and including the `}, this);` on `:3340`
/ `:3477`) with:

```cpp
    // NOCTE-BEGIN nocte-offline
    // ADR-003: there is no cloud account and no preset synchronisation, so "Sync Presets" is
    // removed. Its enable predicate was `is_user_login() && !get_stealth_mode()`, which is
    // permanently false now anyway — the entry is removed rather than left greyed out.
    // NOCTE-END
```

Note the `top_menu->AppendSeparator();` on `:3342` and `fileMenu->AppendSeparator();` on `:3479`
belong to the following "Plugins" entry — keep them.

### 1.3 Hidden tabs: Device, Multi-device, device calibration

Anchor: `MainFrame.cpp:1315-1342`, inside the tab-panel construction.

Read `show_device()` first (`MainFrame.cpp:1360-1530`): it is the *only* other place that adds
these pages, and it already treats every panel as possibly-null (`if (!m_monitor) m_monitor = new
MonitorPanel(...)` at `:1383-1386`, `:1465-1468`; same shape for `m_multi_machine` at `:1409-1412`
and `m_calibration` at `:1422-1425`). So the safe variant is: **keep constructing the panels,
never `AddPage` them.** Every other consumer is already null-safe or name-based:

- `MainFrame.cpp:2575-2580`, `:2642-2645` — `if (m_monitor) …`, `if (m_calibration) …`
- `MainFrame.cpp:3972-3976` — `if (!m_monitor) return;` then `SelectPageByName(TAB_ID_MONITOR)`;
  `SelectPageByName` on a page that is not in the panel is a no-op, not a crash.
- `MainFrame.cpp:3982-3985` — `if (!m_multi_machine) return;`
- `MainFrame.cpp:4332` — `is_printer_view()` compares the *selected page name*; false forever.
- `MainFrame.cpp:4387` — `if (m_multi_machine) { m_multi_machine->clear_page(); }`
- `MainFrame.cpp:1280` — `else if (panel == m_monitor)`; never reached.

Replace `:1315-1318`:

```cpp
        //BBS add pages
    m_monitor = new MonitorPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_monitor->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_MONITOR, m_monitor, _L("Device"), "tab_monitor_active");
```

with:

```cpp
        //BBS add pages
    // NOCTE-BEGIN nocte-offline
    // ADR-003: the Device tab stays hidden until the NØCTE LAN agent (ADR-002) can show live
    // printer state. The panel is still constructed — show_device() and every m_monitor-> user
    // (msw_rescale, on_sys_color_changed, jump_to_monitor) assume a non-null pointer — it is
    // simply never added to the tab panel.
    m_monitor = new MonitorPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_monitor->SetBackgroundColour(*wxWHITE);
    // NOCTE-END
```

Replace `:1329-1334`:

```cpp
    if (wxGetApp().is_enable_multi_machine()) {
        m_multi_machine = new MultiMachinePage(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_multi_machine->SetBackgroundColour(*wxWHITE);
        // TODO: change the bitmap
        m_tabpanel->AddPage(TAB_ID_MULTI_DEVICE, m_multi_machine, _L("Multi-device"), "tab_multi_active");
    }
```

with:

```cpp
    // NOCTE-BEGIN nocte-offline
    // ADR-003: no Multi-device tab. `enable_multi_machine` is false by default and its Preferences
    // toggle was removed (Preferences.cpp), so m_multi_machine simply stays null; every user of it
    // is null-guarded (MainFrame.cpp:2577, :3982, :4387).
    // NOCTE-END
```

Replace `:1340-1342` (the *device* calibration **tab**; the Calibration **menu** stays — it is a
slicer feature, built in `MainFrame::init_menubar_as_editor()`):

```cpp
    m_calibration = new CalibrationPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_calibration->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_CALIBRATION, m_calibration, _L("Calibration"), "tab_calibration_active");
```

with:

```cpp
    // NOCTE-BEGIN nocte-offline
    // ADR-003: the device-calibration tab needs a connected printer, so it stays hidden with the
    // Device tab. The Calibration *menu* (temperature tower, flow rate, pressure advance) is a
    // slicer feature and is untouched. m_calibration stays null; MainFrame.cpp:2579, :2644 and
    // :4034 all null-check it.
    // NOCTE-END
```

Checked: `MainFrame::get_calibration_curr_tab()` (`MainFrame.cpp:4033-4037`) already reads
`if (m_calibration) return m_calibration->get_tabpanel()->GetSelection(); return -1;` — a null
`m_calibration` is a defined outcome, not a fall-off-the-end.

### 1.4 The sync dialog

Anchors: `MainFrame.cpp:497`, `:4368-4372`, `:4390-4423`.

The simplest correct variant is to drop the event binding; `show_sync_dialog()` and
`on_select_default_preset()` then become dead but still compile, and the only caller of
`show_sync_dialog()` (`GUI_App.cpp:5665`) posts an event nobody handles.

At `MainFrame.cpp:497` the anchor is:

```cpp
    Bind(EVT_SYNC_CLOUD_PRESET, &MainFrame::on_select_default_preset, this);
```

Replace with:

```cpp
    // NOCTE-BEGIN nocte-offline
    // ADR-003: nothing synchronises presets from a cloud, so the "Do you want to synchronize your
    // personal data from Orca Cloud?" dialog is never raised. on_select_default_preset() and
    // show_sync_dialog() stay defined (MainFrame.hpp:301,303 declare them) but are unbound.
    // NOCTE-END
```

`EVT_SYNC_CLOUD_PRESET` keeps its `wxDEFINE_EVENT` at `:279` — `show_sync_dialog()` still names it.

### 1.5 `add_common_publish_menu_items`

Anchor: `MainFrame.cpp:2727`. **No hunk needed.** Its only call site,
`MainFrame.cpp:3153-3157`, is already inside a `/* … */` comment upstream, and the function body
is itself inside `#ifndef __WINDOWS__`. It is an unused `static` function; `-Wunused-function` is
disabled in the NØCTE warning set, so it compiles clean as-is. If you want it gone anyway, delete
the whole function — but then also delete the `#include` it needs, and note that it is the only
thing in `MainFrame.cpp` naming `open_publish_page_dialog` / `open_mall_page_dialog` besides
hunks 3.7/3.8.

---

## 2. `src/slic3r/GUI/Plater.cpp`

### 2.1 Stop posting `EVT_INSTALL_PLUGIN_HINT`

Anchor: `Plater.cpp:12969-12979`, in the tab-changing handler.

```cpp
    if (use_native_device_tab && new_name == TAB_ID_MONITOR) {
        // BBL network module is only required for BBL-vendor printers.
        // Non-BBL Python plugins (e.g. moonraker) drive the Device tab without it.
        if (!use_printer_agents && wxGetApp().preset_bundle->is_bbl_vendor() && !Slic3r::NetworkAgent::is_network_module_loaded()) {
            e.Veto();
            BOOST_LOG_TRIVIAL(info) << boost::format("skipped tab switch from %1% to %2%, lack of network plugins") % old_sel % new_sel;
            if (q) {
                wxCommandEvent* evt = new wxCommandEvent(EVT_INSTALL_PLUGIN_HINT);
                wxQueueEvent(q, evt);
            }
        }
    } else {
```

Replace the inner `if (q) { … }` (three statements, `:12975-12978`) with:

```cpp
                // NOCTE-BEGIN nocte-offline
                // ADR-003: NØCTE never prompts for the Bambu network plug-in, so the veto is
                // silent. This path is unreachable anyway while the Device tab is not added
                // (MainFrame hunk 1.3), but the veto must stay in case it is re-added.
                // NOCTE-END
```

Keep `e.Veto()` and the log line — `old_sel` and `new_sel` stay used, so nothing becomes an
unused variable.

### 2.2 `show_install_plugin_hint()` → no-op

Anchor: `Plater.cpp:13209-13212`.

```cpp
void Plater::priv::show_install_plugin_hint(wxCommandEvent &event)
{
    notification_manager->bbl_show_plugin_install_notification(into_u8(_L("The network plug-in was not detected. Network related features are unavailable.")));
}
```

→

```cpp
void Plater::priv::show_install_plugin_hint(wxCommandEvent &event)
{
    // NOCTE-BEGIN nocte-offline
    // ADR-003: no network plug-in is ever expected, so there is nothing to notify about. The
    // handler and its EVT_INSTALL_PLUGIN_HINT binding stay so the event type keeps a consumer.
    (void) event;
    // NOCTE-END
}
```

### 2.3 `PublishDialog` wiring

Anchors: `Plater.cpp:119` (include), `:6705` (`PublishDialog *m_publish_dlg = nullptr;`),
`:14087-14104` (`show_publish_dlg`), `:19794-19799` (`publish_job_finished`), `:22027`.

⚠ **Do not remove `m_publish_dlg`.** It is a private field of `Plater::priv`, a class *defined in
this .cpp*, so `-Wunused-private-field` applies and is fatal. Keep the field and keep its uses at
`:12687`, `:12749-12750`, `:12776-12779`, `:12886`. Change only the entry point:

```cpp
bool Plater::priv::show_publish_dlg(bool show)
{
    if (q != nullptr) { BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":recevied publish event\n"; }

    if (!m_publish_dlg) m_publish_dlg = new PublishDialog(q);
```

→ insert right after the opening brace:

```cpp
bool Plater::priv::show_publish_dlg(bool show)
{
    // NOCTE-BEGIN nocte-offline
    // ADR-003: publishing a model to a cloud gallery is not a NØCTE feature. PublishSettingsDialog
    // is a *local* export dialog and is untouched; only this uploader entry point is closed.
    (void) show;
    return false;
    // NOCTE-END
```

…and leave the rest of the body in place (unreachable, but it keeps `PublishDialog` and
`m_publish_dlg` referenced so neither the field nor the class becomes unused).

Then null-guard `Plater::publish_job_finished` at `:19794`, which currently dereferences the
pointer unconditionally:

```cpp
void Plater::publish_job_finished(wxCommandEvent &evt)
{
    p->m_publish_dlg->EndModal(wxID_OK);
```

→

```cpp
void Plater::publish_job_finished(wxCommandEvent &evt)
{
    // NOCTE-BEGIN nocte-offline
    // ADR-003: show_publish_dlg() returns early, so m_publish_dlg is never constructed.
    if (p->m_publish_dlg)
        p->m_publish_dlg->EndModal(wxID_OK);
    // NOCTE-END
```

### 2.4 The "Find on OrcaCloud" notifications

Anchor: `Plater.cpp:21381-21409`, in the missing-plugin notification updater. Two of the four
`update(...)` calls offer `_u8L("Find on OrcaCloud")` and one offers `_u8L("Install Plugins")`,
all of which reach `open_missing_plugins_on_cloud()` / `install_missing_cloud_plugins()`.

Keep the notifications (a user still needs to know a preset needs a plugin) and remove only the
action button. The local `update` lambda is declared at `Plater.cpp:21352-21356` as

```cpp
    const auto update = [&](NotificationType type, const std::vector<MissingPlugin>& missing,
                            std::string* shown_sig, const std::string& header,
                            const std::string& resolve_label,
                            std::function<bool(wxEvtHandler*)> resolve_action) {
```

so there is no text-only overload: pass an empty `resolve_label` and a capture-less callback that
returns `false`.

All **three** action buttons go, not just the two "Find on OrcaCloud" ones:
`open_missing_plugins_on_cloud()` (`src/slic3r/plugin/PluginResolver.cpp:475`, declared
`PluginResolver.hpp:71`) opens a browser, and `install_missing_cloud_plugins()`
(`Plater.cpp:21428`) really does download and install from a plugin registry on a worker thread —
it is not a local "activate what you already have". `_u8L("Activate Now")`
(`NotificationType::OrcaPluginInactiveError`, `:21400-21404`) is purely local and **stays**.

```cpp
    // NOCTE-BEGIN nocte-offline
    // ADR-003: the notification stays (the preset really does need a plugin) but the "Find on
    // OrcaCloud" action is removed — NØCTE opens no browser to a plugin marketplace.
    update(NotificationType::OrcaLocalPluginMissingError, missing_local,
           &p->m_local_missing_shown_sig,
           _u8L("Local plugins required by the current preset are missing:"),
           "",
           [](wxEvtHandler*) { return false; });
    // NOCTE-END
```

Do the same for `NotificationType::OrcaPluginCapabilityUnavailableError` (`:21405-21409`) and for
`NotificationType::OrcaCloudPluginMissingError` / `_u8L("Install Plugins")` (`:21381-21385`).

⚠ After this, `missing_cloud_refs`, `missing_local_refs` and `broken_refs` (`:21378-21379`,
`:21398`) become unused *locals*
(`-Wunused-variable` is disabled — fine) but they are also *lambda captures*; make sure you remove
them from the capture lists, because `-Wunused-lambda-capture` **is** fatal. The replacements
above capture nothing, so this is handled — just do not leave `[this, missing_cloud_refs]`,
`[missing_local_refs]` or `[broken_refs]` behind. `install_missing_cloud_plugins()` then has no
caller; it is a public member of `Plater` declared in `Plater.hpp`, so it stays linkable and does
not trigger any warning — leave the definition.

---

## 3. `src/slic3r/GUI/GUI_App.cpp`

Eight hunks, none over 20 lines. ADR-003 budgets twelve for this file in total, so W2 has four
left for its own work.

### 3.1 `ShowUserLogin()` → early return

Anchor: `GUI_App.cpp:4715` (declared `GUI_App.hpp:491`:
`void ShowUserLogin(bool show = true, const std::string& provider = ORCA_CLOUD_PROVIDER);`).

```cpp
void GUI_App::ShowUserLogin(bool show, const std::string& provider)
{
    // Show user Login Dialog for specified cloud
    if (show) {
```

→ insert after the opening brace:

```cpp
void GUI_App::ShowUserLogin(bool show, const std::string& provider)
{
    // NOCTE-BEGIN nocte-offline
    // ADR-003: no account, no login dialog. This is the single funnel: request_login()
    // (GUI_App.cpp:4950) calls straight into here, check_login() (:4994) asks the agent, whose
    // cloud methods are null-guarded no-ops and answer "not logged in", and request_user_login()
    // (:5015) posts EVT_USER_LOGIN whose handler goes through the same agent. None of them needs
    // its own hunk.
    (void) show; (void) provider;
    return;
    // NOCTE-END
```

Leave the rest of the body — it keeps `ZUserLogin` (`WebUserLoginDialog.cpp`) referenced and
`login_dlg` (`GUI_App.hpp:320`) used.

### 3.2 `check_new_version_sf()` → early return  **(mandatory)**

Anchor: `GUI_App.cpp:6045` (declared `GUI_App.hpp:528`:
`void check_new_version_sf(bool show_tips = false, int by_user = 0);`).

This is the one consumer that does **not** cope with an empty `version_check_url`: it appends a
query string and calls `Http::get(version_check_url)` at `:6069` regardless.

```cpp
void GUI_App::check_new_version_sf(bool show_tips, int by_user)
{
    AppConfig* app_config = wxGetApp().app_config;
```

→

```cpp
void GUI_App::check_new_version_sf(bool show_tips, int by_user)
{
    // NOCTE-BEGIN nocte-offline
    // ADR-003: NØCTE never checks for updates. AppConfig::version_check_url() is "" now, but this
    // function would still build "?iid=…" onto it and call Http::get(), so the early return is
    // what actually keeps the socket closed. Callers: the Help menu entry (removed, hunk 1.1) and
    // the unguarded call in GUI_App::post_init (GUI_App.cpp:999).
    (void) show_tips; (void) by_user;
    return;
    // NOCTE-END
```

### 3.3 `show_network_plugin_download_dialog()` → early return

Anchor: `GUI_App.cpp:1952` (declared `GUI_App.hpp:770`:
`void show_network_plugin_download_dialog(bool is_update = false);`).

```cpp
void GUI_App::show_network_plugin_download_dialog(bool is_update)
{
    auto load_error = Slic3r::NetworkAgent::get_load_error();
```

→

```cpp
void GUI_App::show_network_plugin_download_dialog(bool is_update)
{
    // NOCTE-BEGIN nocte-offline
    // ADR-002/ADR-003: NØCTE never ships, downloads or prompts for BambuNetworkLibrary.
    (void) is_update;
    return;
    // NOCTE-END
```

### 3.4 `ShowDownNetPluginDlg()` → early return

Anchor: `GUI_App.cpp:4701` (declared `GUI_App.hpp:490`: `void ShowDownNetPluginDlg();`).

```cpp
void GUI_App::ShowDownNetPluginDlg() {
    try {
```

→

```cpp
void GUI_App::ShowDownNetPluginDlg() {
    // NOCTE-BEGIN nocte-offline
    // ADR-002/ADR-003: no plug-in download. W1 already removed the wizard's only call site
    // (WebGuideDialog.cpp, "user_guide_finish").
    return;
    // NOCTE-END
    try {
```

Leaving the body keeps `DownloadProgressDialog` referenced — which does not matter, because
`MediaPlayCtrl.cpp:497-500` uses it for the virtual-camera tools and it must stay in the build.

### 3.5 The "Quit Stealth Mode" escape

Anchor: `GUI_App.cpp:5112-5134`, in the webview command handler.

```cpp
            if (app_config->get_stealth_mode() && stealth_blocked_login_commands.count(command_str)) {
                CallAfter([this, command_str] {
                    MessageDialog dlg(mainframe,
                        _L("You are currently in Stealth Mode. To log into the Cloud, you need to disable Stealth Mode first."),
```

Replace the whole `if (…) { … return ""; }` (`:5112-5134`) with:

```cpp
            // NOCTE-BEGIN nocte-offline
            // ADR-003: stealth mode is the permanent state of NØCTE Slicer — it defaults to true
            // (AppConfig.cpp) and has no Preferences toggle. The dialog that offered to turn it
            // off ("Quit Stealth Mode") is removed; a login command from the web page is simply
            // swallowed, exactly like the info commands handled just above.
            if (app_config->get_stealth_mode() && stealth_blocked_login_commands.count(command_str))
                return "";
            // NOCTE-END
```

`stealth_blocked_login_commands` (`:5100-5104`) stays used. Check `request_login` has no other
caller you care about afterwards — `ShowUserLogin` is already a no-op from hunk 3.1 either way.

### 3.6 The 2.4.0 first-run notice

Anchor: `GUI_App.cpp:1007-1032`.

```cpp
    // Orca: notify users upgrading from a pre-2.4.0 version that profile syncing
    // moved from Bambu Cloud to Orca Cloud.
    if (is_editor() && m_last_config_version && m_last_config_version->valid()
        && *m_last_config_version < Semver(2, 4, 0)) {
```

Replace the whole `if (…) { CallAfter([] { … }); }` (`:1007-1032`) with:

```cpp
    // NOCTE-BEGIN nocte-offline
    // ADR-003: the "profile syncing moved from Bambu Cloud to Orca Cloud" notice describes a
    // migration between two clouds NØCTE Slicer has never used, and its "Learn more" button
    // opened orcaslicer.com. There is no first-run network notice.
    // NOCTE-END
```

⚠ `m_last_config_version` is a member of `GUI_App` declared in `GUI_App.hpp` (not a class defined
in this .cpp), so `-Wunused-private-field` does **not** fire; but grep for other readers before
assuming it can go — it is also used by the config migration.

### 3.7 `open_mall_page_dialog()` → no-op

Anchor: `GUI_App.cpp:9047` (declared `GUI_App.hpp:680`: `void open_mall_page_dialog();`).

```cpp
void GUI_App::open_mall_page_dialog()
{
    std::string host_url;
```

→

```cpp
void GUI_App::open_mall_page_dialog()
{
    // NOCTE-BEGIN nocte-offline
    // ADR-003: no model marketplace. Live callers: GUI_App.cpp:5194 (a webview command) and
    // MainFrame.cpp:2752 (inside add_common_publish_menu_items, whose call site is commented out).
    return;
    // NOCTE-END
```

### 3.8 `open_publish_page_dialog()` → no-op

Anchor: `GUI_App.cpp:9089` (declared `GUI_App.hpp:681`: `void open_publish_page_dialog();`).

```cpp
void GUI_App::open_publish_page_dialog()
{
    std::string host_url;
```

→

```cpp
void GUI_App::open_publish_page_dialog()
{
    // NOCTE-BEGIN nocte-offline
    // ADR-003: no model marketplace. Live caller: BBLTopbar.cpp:491.
    return;
    // NOCTE-END
```

### Not to touch

- `get_http_url()` / `get_model_http_url()` (`GUI_App.cpp:1164-1206`) — string builders with no
  request of their own; ADR-003 leaves them. After 3.7/3.8 their only remaining readers are the
  dead bodies below the early returns, which is fine.
- `GUI_App.cpp:990-1002`, the startup sync block — already gated on `!get_stealth_mode()`, which
  is now permanently false. Only line 999 (`this->check_new_version_sf();`) sits outside the
  guard, and hunk 3.2 handles it. No hunk here.

---

## 4. Strings W2 owns in `resources/web/data/text.js`

W1 changed the inline English fallback in the wizard HTML, but `TranslatePage()` overwrites every
`class="trans"` element from `LangText`, so these `tid`s must change in `text.js` (W2's file) or
the wizard still says "Orca Slicer". Only the English block is load-bearing; the other locales
fall back to it when a key is missing, but they currently carry their own translated "Orca Slicer".

| tid | current English (`text.js:3-4`) | should read |
|---|---|---|
| `t1` | `Welcome to Orca Slicer` | `Welcome to NØCTE Slicer` |
| `t2` | `Orca Slicer will be setup in several steps. Let's start!` | `NØCTE Slicer will be setup in several steps. Let's start!` |

The same two keys exist per locale (`ca`: `:123-124`, `es`: `:238-239`, and so on) — each locale's
`t1`/`t2` names the product too.

Keys that belong to pages W1 unlinked and that the integrator will delete with them — safe to drop
from `text.js` in the same pass, but harmless if left: `t47`–`t49`, `t60`–`t63` (login region),
`t54`–`t56` (the privacy text with the bambulab.com link), `t64`–`t70`, `t71`–`t86` (network
plug-in download and its failure steps), `orca3`–`orca5`, `orca12` (the stealth-mode page).
`orca6`, `orca7`, `orca9` ("Bambu Cloud", "Orca Cloud Account", "Bambu Cloud Account") belong to
the **home page**, not the wizard — they are W2's call under the home-page work, not W1's.
