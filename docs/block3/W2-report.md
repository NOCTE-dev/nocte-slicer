# W2 report — visual identity, names, home page

Branch `int/block3a`. Everything below is in the working tree; nothing was committed.

---

## 1. Files changed

**New**

| File | What it is |
|---|---|
| `src/slic3r/GUI/Nocte/NocteTheme.hpp` | The NØCTE greys in one place: hex strings, 0..1 float triples and byte triples. Header only, no `.cpp` — **no CMake change needed**. |
| `resources/web/image/nocte_wordmark_light.svg` | The black horizontal wordmark, copied from `resources/images/OrcaSlicer_horizontal_light.svg` (already NØCTE artwork). |
| `resources/web/image/nocte_wordmark_dark.svg` | The white variant, from `OrcaSlicer_horizontal_dark.svg`. |
| `docs/block3/W2-report.md` | This file. |

I did **not** add `resources/web/image/nocte_wordmark.png`. The brand mark already exists as two
paired SVGs that are pure geometry (`circle`, `rect`, `polygon`, `ellipse`, `line`, `path`), 1.5 KB
each, DPI-independent, and the web view is Chromium/WebKit, which renders them natively. A Pillow
re-draw would have been a strictly worse, larger, fixed-resolution copy of an asset we already own.
Say the word if a raster is wanted anyway.

**Upstream touch points — hunk counts**

| File | Hunks | Kind |
|---|---:|---|
| `src/slic3r/GUI/GUI_App.cpp` | **4** | display name, dark-mode ×2, label colours |
| `src/slic3r/GUI/Plater.cpp` | 11 | 4 palette, 2 window title, 5 product name |
| `src/slic3r/GUI/MainFrame.cpp` | 3 | dock tooltip, NØCTE menu ×2 |
| `src/slic3r/GUI/AboutDialog.cpp` | 1 new (8 total, 7 pre-existing) | licence line |
| `src/slic3r/GUI/Widgets/StateColor.cpp` | 2 | include + `gDarkColors` |
| `src/slic3r/GUI/BitmapCache.cpp` | 1 | the SVG replace map |
| `src/slic3r/GUI/ImGuiWrapper.cpp` | 3 | palette, confirm button, radio |
| `src/slic3r/GUI/GLCanvas3D.cpp` | 2 | 3D background, slicing progress bar |
| `src/slic3r/GUI/GLTexture.cpp` | 1 | toolbar sprite tints |
| `src/slic3r/GUI/BBLTopbar.cpp` | 1 | the 7 accent literals |
| `src/libslic3r/Color.hpp` | 2 | `ColorRGB::ORCA()`, `ColorRGBA::ORCA()` |

`tools/nocte/check_touchpoints.py --base nocte-base-2026-09-16` → **RESULT: PASS**.

**GUI_App.cpp budget.** ADR-003 allows twelve hunks; W1's handover uses eight, leaving four. I took
exactly four, and had to drop two I had already written — see §6.

**NØCTE-owned**

`resources/web/homepage/**` (rewritten; 2.7 MB → 44 KB), `resources/web/include/global.css`,
`resources/web/data/text.js`, `resources/data/hints.ini`, six SVGs under `resources/images/`.

---

## 2. Quoted signatures (every API used, opened and read)

```
wxAppConsoleBase::SetAppDisplayName(const wxString& name)          wx/app.h (wxWidgets)
wxString wxString::FromUTF8(const char* utf8)                      wx/string.h
StateColor::darkModeColorFor(wxColour const& color)                Widgets/StateColor.hpp:36
StateColor::lightModeColorFor(wxColour const& color)               Widgets/StateColor.hpp:37   (reverse of gDarkColors)
BBLTopbar::AddDropDownSubMenu(wxMenu* sub_menu, const wxString& title)
                                                                   BBLTopbar.hpp:62  -> m_top_menu.AppendSubMenu(...)  BBLTopbar.cpp:499-502
append_menu_item(wxMenu* menu, int id, const wxString& string, const wxString& description,
                 std::function<void(wxCommandEvent&)> cb, const std::string& icon = "",
                 wxEvtHandler* event_handler = nullptr,
                 std::function<bool()> const cb_condition = []{return true;},
                 wxWindow* parent = nullptr, int insert_pos = wxNOT_FOUND)
                                                                   wxExtensions.hpp:32-34, body wxExtensions.cpp:96-111
                 (with event_handler == nullptr it binds on the menu itself, wxExtensions.cpp:81-86 — same as MainFrame.cpp:3344)
extern void Slic3r::GUI::about()                                   GUI.hpp:81   (precedent MainFrame.cpp:2718-2720)
Slic3r::GUI::Nocte::create_main_menu(MainFrame* frame)             Nocte/NocteUi.hpp:22        (W4; left as NOCTE-TODO)
BitmapCache::nsvgParseFromFileWithReplace(const char* filename, const char* units, float dpi,
                 const std::map<std::string,std::string>& replaces) BitmapCache.cpp:273-299
                 -> boost::replace_all(str, val.first, val.second) per entry, in std::map key order (:296-297)
WebViewPanel::SendRecentList(int images)                            WebViewDialog.cpp:435-446  (window.postMessage(...))
WebViewPanel::SetLoginPanelVisibility(bool bshow)                   WebViewDialog.cpp:430-434  (calls SetLoginPanelVisibility() by name)
MainFrame::get_recent_projects(wptree&, int images)                 MainFrame.cpp:4254-4272    (keys: project_name, path, published, time, image)
GUI_App::handle_web_request  homepage_* command names               GUI_App.cpp:5193-5246
Slic3r::GUI::I18N::translate(const char* s) -> wxString(s, wxConvUTF8)  GUI/I18N.hpp:44   (so UTF-8 byte escapes in _L() are correct)
```

---

## 3. Palette

The tokens are in `NocteTheme.hpp`. Two of them need explaining:

* **`NOCTE_ACCENT` is white on dark, `NOCTE_ACCENT_ON_LIGHT` near-black on light** — as briefed.
* **`NOCTE_ACCENT_SURFACE` `#55555D` / `_HOVER` `#6A6A73`** is a third token I had to add. Upstream
  paints the confirm button with `#009688` **and its label with `#FEFEFE`** (`Widgets/Button.cpp:183`,
  `btn_confirm[10]`). Mapping `#009688` to white would have made every confirm button white-on-white.
  So wherever the accent is a *filled surface carrying white text*, it is the surface grey; the pure
  white accent is used where it is a *mark* on a dark surface (radio dot, progress bar, icons).

**`gDarkColors`** — keys untouched (they are what light-mode widgets emit and `darkModeColorFor()`
looks up); the dark column rewritten. Values are pairwise distinct apart from the three collisions
upstream already had, so `lightModeColorFor()`'s reverted map is no worse than before. One key
added: **`#4A4A50` → `#5A5A62`**, the NØCTE accent key, because light mode cannot be reached through
this map at all — `darkModeColorFor()` returns its argument unchanged when `gDarkMode` is false
(`StateColor.cpp:190-197`). Call sites I own that painted teal directly now emit that key instead
(`BBLTopbar.cpp` ×7, `Plater.cpp` ×6), so they are grey in **both** themes.

**`ORCA()`** → `#8C8C90` (`NOCTE_NEUTRAL`). It reaches gizmo highlights, the measure tool and
`ImGuiWrapper::COL_ORCA`, all of which sit over both the 0.91 light and the 0.10 dark 3D background;
`#8C8C90` is the one grey with usable contrast against both (≈3.2:1 either way).

**`BitmapCache::load_svg` — the important finding.** Upstream's replace map only carried the
*quoted* key `"\"#009688\""`, which matches `fill="#009688"`. Of the 2061 teal literals under
`resources/images`, **1415 are inside a `style="…"` attribute and only 70 in `fill=`** — so upstream's
dark-mode recolour was reaching about 5 % of the icon set. Every brand colour now has **both** the
quoted and the bare form, in **both** themes (`#FFFFFF` on dark, `#1A1A1C` on light), plus
`#00AE42`, `#52c7b8`, `#00675B`, `#26A69A`, `#008172`, `#BFE1DE`, `#E5F0EE`, `#EBF9F0` and the two
lower-case spellings that actually occur. Ordering is safe and deliberate: `std::map` iterates by
key, `"` (0x22) sorts before `#` (0x23), so every quoted rule runs before every bare one and a bare
rule can never undo a quoted one — the quoted rule has already consumed the text it would match. I
verified no replacement *output* is also a replacement *key* in either branch. The
`if (!new_color.empty())` override still wins and now sets the bare form too, so a caller-chosen
colour also reaches `style=` attributes.

Also changed: `GLTexture.cpp` sprite tints (`normal_color` was `#2B3436`, teal-tinted);
`GLCanvas3D.cpp` 3D background (`#E8E8E9` / `#1A1A1C`) and the all-plates slicing progress bar
(`IM_COL32(0,150,136)` → `242,242,242`); the ImGui palette (`COL_GREEN_LIGHT`, `COL_WINDOW_BG_DARK`,
`COL_TOOLBAR_BG_DARK`, `COL_ORCA_DARK/_HOVER/_HOVER_DARK`, the confirm button, the radio button).

**Six SVGs recoloured on disk** — they are loaded by `IMTexture::load_from_svg_file`
(`Gizmos/GLGizmosManager.cpp:254-299`), which bypasses `BitmapCache` entirely, so the replace map
can never reach them. All six carried the colour in a `style=` attribute:

| File | was | now |
|---|---|---|
| `toolbar_tooltip.svg` | `fill:#009688` | `#8c8c90` |
| `toolbar_tooltip_hover.svg` | `fill:#33aba1` | `#b4b4b8` |
| `canvas_menu.svg` | `stroke:#2b3436` ×3 | `#2a2a2e` |
| `canvas_menu_hover.svg` | `stroke:#2b3436` ×3, `fill:#e5f0ee` | `#2a2a2e`, `#e8e8e9` |
| `canvas_menu_dark.svg` | `fill:#393c42` | `#3a3a3e` |
| `canvas_menu_dark_hover.svg` | `fill:#283232` | `#2a2a2e` |

`toolbar_reset*.svg` keep `#ff6f00`: orange is upstream's secondary/status colour, not on ADR-003's
ban list. The bed texture needs nothing — no SVG under `resources/profiles` contains any banned hex.

---

## 4. Dark by default

W1 seeds `dark_color_mode = "1"` in `AppConfig.cpp:360`. Two things were eating that seed; I fixed
both, and one remains:

1. **`GUI_App::on_init_inner`, `GUI_App.cpp:3064-3070`** — inside `#ifndef __WINDOWS__` it
   overwrote the key from `wxSystemSettings::GetAppearance()` **and saved**, on every start. Removed.
   (`wxSystemAppearance app` was its only use; nothing below reads it.)
2. **`GUI_App::dark_mode()`, `#if __APPLE__` branch** — asked the OS unconditionally, so the
   Preferences toggle did nothing on macOS. It now honours an explicit `"1"`/`"0"` first, exactly
   like the Windows/Linux branch, and falls back to the OS only when the key was never written.
3. **Not fixed — integrator action.** `GUI_Utils.cpp:315-320` (`update_dark_config()`) still writes
   `dark_color_mode` from the system appearance on every system colour-scheme change. That file is
   **not on the touch-point allowlist**, so I could not edit it. Until it is, changing the desktop
   theme while NØCTE is running still flips the app. Also W1's note stands: the Preferences
   "Enable dark Mode" toggle is inside `#ifdef _WIN32` (`Preferences.cpp:1615-1618`), so macOS and
   Linux users have no way back to light mode.

`init_label_colours()` now takes its values from `NocteTheme.hpp`.

---

## 5. Names and the NØCTE menu

Non-ASCII is written as `"N\xC3\x98" "CTE Slicer"` everywhere, split so `\x`'s greedy hex parse
cannot swallow the following `C` — the precedent is `AboutDialog.cpp:25`. `_L()` decodes its
argument with `wxConvUTF8` (`GUI/I18N.hpp:44`), so the escapes survive translation lookup.

* `GUI_App.cpp:2523` — `SetAppDisplayName(wxString::FromUTF8("N\xC3\x98" "CTE Slicer"))`, uncommented
  and given the display string rather than `SLIC3R_APP_NAME` (which is ASCII `NOCTE Slicer`).
  `SetAppName(SLIC3R_APP_KEY)` above is untouched — that one keys the data directory.
* `Plater.cpp` window title, both paths (`set_project_name`, `update_title_dirty_status`).
* `MainFrame.cpp` macOS dock tooltip. Icon **file names** are unchanged, as briefed.
* `Plater.cpp` ×5 `_L()` strings, `AboutDialog.cpp` ×1 ("… is licensed under"). The AGPL credit
  paragraphs that name OrcaSlicer/PrusaSlicer/BambuStudio/Slic3r **deliberately stay**.
* `_L("Untitled")` ×11 in `Plater.cpp` — left alone, as briefed.
* URLs, `.orca_printer`/`.orca_bundle`/`.orca_filament` filters, config keys, IPC names and
  `libslic3r.h` — not touched.

**NØCTE menu** — added in both branches of `init_menubar_as_editor()`: the top-bar drop-down (after
`AddDropDownSubMenu(helpMenu, _L("Help"))`) and the plain menu bar. It is **appended after Help**, not
before it, so the index in the `_MSW_DARK_MODE` `m_menubar->EnableTop(6, false)` call below does not
move. One entry, "&About NØCTE Slicer" → `Slic3r::GUI::about()`. The W4 call is present as
`// NOCTE-TODO(W4): wxMenu* nocte_menu = Slic3r::GUI::Nocte::create_main_menu(this);` at the exact
spot; W4 deletes the inline block and uncomments that line.

---

## 6. Residue — what I did NOT change

**`GUI_App.cpp` — two hunks written then reverted for the twelve-hunk budget.** Both are ready to
paste if the integrator finds room (e.g. if W1 merges any of its eight):

| Anchor | Strings |
|---|---|
| `GUI_App.cpp:746, :755, :763` (one hunk, 20 lines) | "OrcaSlicer will terminate because of running out of memory…", "…because of a localization error…", "OrcaSlicer got an unhandled exception: %1%" |
| `GUI_App.cpp:8062, :8069` (one hunk, 8 lines) | "Switching Orca Slicer to language %s failed.", "Orca Slicer - Switching language failed" |

**`GUI_App.cpp` — other `_L()` strings I left**, with the reason:

| Line | String | Why |
|---|---|---|
| `:2489` | "Orca Slicer requires the Microsoft WebView2 Runtime…" | budget; Windows-only, first run without WebView2 |
| `:3174`, `:3225`, `:5463` | update-check dialogs | W1 empties `check_new_version_sf()`; dead |
| `:1014`, `:5536`, `:5540`, `:6749` | Orca Cloud / OrcaCloud preset sync | W1 removes the cloud |
| `:6213`, `:6223`, `:6233`, `:6253` | printer/firmware errors naming OrcaSlicer | budget |
| `:6268` | "To use OrcaSlicer with Bambu Lab printers, enable LAN mode and Developer mode" | budget **and** it sits inside the "Network Plug-in Restriction" dialog W1 may delete. Worth revisiting for ADR-002 — the text is about to become NØCTE's own LAN story. |

**`MainFrame.cpp`** — `:3325`, `:3462` ("…from OrcaCloud") and `:4393` ("…from Orca Cloud") belong to
entries W1 removes. `:2704` ("Report a bug of OrcaSlicer") is already commented out upstream.
`:3788` is a file-extension filter — must not change.
`MainFrame.cpp:2466-2470` has a dead local `StateColor m_btn_bg_enable` holding teal `(0,150,136)`
and green `(48,221,112)`; every use of it is commented out (`:2472-2476`), so it draws nothing.

**`Plater.cpp`** — `:21406`, `:21415`, `:21431` (`_u8L("OrcaCloud plugins …")`, `"Find on OrcaCloud"`
×2) are W1's hunk 2.4. `:933` is a commented-out teal literal.

**Teal/green still in `src/`: 116 literals** (excluding the intentional map keys in `BitmapCache.cpp`,
`StateColor.cpp` and `NocteTheme.hpp`). They are all `wxColour("#009688")`-style call sites, and in
**dark mode — the default — every one of them is already grey**, because `StateColor` routes them
through `gDarkColors`. Only light mode still shows teal there. Biggest groups:

* **Out of scope per the brief (Bambu device pages):** `SelectMachine.cpp` 5, `AmsMappingPopup.cpp` 5,
  `SyncAmsInfoDialog.cpp` 2, plus `CalibrationWizardPresetPage.cpp` 3, `MediaFilePanel.cpp` 2,
  `AMSMaterialsSetting.cpp`, `UpgradePanel.cpp`, `FanControl.cpp`, `SideTools.cpp`, `DeviceTab/*`.
  (`MultiTaskManagerPage.cpp`, `StatusPanel.cpp`, `MultiMachineManagerPage.cpp` and
  `SendMultiMachinePage.cpp` turned out to carry none.)
* **Shared widgets, not mine:** `Widgets/SwitchButton.cpp` 10, `Widgets/Button.cpp` 10 (the
  `btn_confirm`/`btn_regular`/`btn_alert` tables at `:182-185`, including `#22bfb0` and `#00FFD4`),
  `Widgets/TempInput.cpp` 4, `Widgets/StepCtrl.cpp` 4, `Widgets/MultiNozzleSync.cpp` 4,
  `Widgets/SideButton.cpp` 2, `Widgets/ProgressDialog.cpp` 2, and one each in `DropDown.cpp`,
  `ComboBox.cpp`, `TextInput.cpp`, `SpinInput.cpp`, `RadioGroup.cpp`, `TabCtrl.cpp`, `CheckList.cpp`,
  `Label.cpp`, `AxisCtrlButton.cpp`, `ImageSwitchButton.cpp`, `ProgressBar.*`, `HyperLink.cpp`.
* **Other GUI:** `TextureImportDialog.cpp` 9, `Notebook.cpp` 4, `GCodeViewer.cpp` 3, `DailyTips.cpp` 3,
  `TabButton.cpp` 2, `Search.cpp` 2, `OG_CustomCtrl.cpp` 2, `IMSlider.cpp` 2, `KBShortcutsDialog.cpp` 2,
  `NotificationManager.cpp` 2, `GUI_ObjectList.cpp` 2, `FilamentMapPanel.cpp` 2, `CapsuleButton.cpp` 2,
  `PartSkipDialog.cpp` 2, `Project.hpp` 2, `Auxiliary.hpp` 2, `PublishSettingsDialog.cpp` 2,
  `libslic3r/PresetBundle.cpp` 2, and ~20 single hits. `GUI_App.cpp:342` is the splash progress bar
  (grey in dark mode, teal in light).

**Web residue outside my files** (I did not touch them because they are not on my list; all are
NØCTE-owned and a one-line fix each):

| File | Hits | Note |
|---|---:|---|
| `src/slic3r/GUI/Widgets/WebViewHostDialog.cpp:37` | 1 | **The single-point fix for every `resources/web/dialog/**` page.** `theme.css:25-27` sets `--main-color` from `--orca-accent`, which that line injects as `StateColor::darkModeColorFor(wxColour("#009688"))` — already grey in dark mode, still teal in light. Changing the literal to `"#4A4A50"` (the NØCTE accent key) fixes both. The `bg`/`fg`/`muted`/`border` it injects on lines 33-36 already pick up my `init_label_colours()` values. Not on my file list. |
| `resources/web/dialog/PluginsDialog/styles.css` | 6 | W1 removes the plugin marketplace |
| `resources/web/dialog/css/common.css` | 4 | |
| `resources/web/dialog/SpeedDial/style.css` | 3 | inline `#009688` fallbacks in `var(--main-color, …)` |
| `resources/web/model/model.css`, `model/css/dark.css` | 5 | model mall, removed by W1 |
| `resources/web/flush/NozzleListTable.html` | 3 | |
| `resources/web/guide/**` | 8 | **W1's files** |

`resources/data/hints.ini`: 8 product names replaced; the five `orcaslicer.com` documentation URLs
stay (URLs are out of scope).

`resources/web/data/text.js`: **127 replacements** across all 15 locales (104 "Orca Slicer",
22 "OrcaSlicer", 1 "Orca slicer"). 17 "Orca Cloud" strings remain — they belong to `orca7`/`orca9`
and to wizard pages W1 unlinked; they are unreferenced, not wrong. Added `nocte1`
("No recent projects yet", plus an `es_ES` translation) for the home page's empty state.
File stayed UTF-8 without BOM, CRLF.

---

## 7. Home page

`resources/web/homepage/` rewritten: **2.7 MB → 44 KB** (25 Orca marketing images deleted; `img/d.png`
kept, it is already a neutral `#8E8E8E` glyph and is the `onerror` fallback in `ShowRecentFileList`).

Dark monochrome. Left rail: NØCTE wordmark, the tagline "Building the Impossible", and a single
"Recent" item. No accounts, no OrcaCloud shortcut, no Bambu cloud section, no plugin banner. Right
pane: New Project / Open Project cards, then the recent-file grid with "Clear all" and the
right-click menu (Remove / Open Containing Folder). All icons are inline monochrome SVG using
`currentColor`, so there are no more raster icons to theme.

**Contract preserved and verified:**

* path `web/homepage/index.html` (`WebViewDialog.cpp:39,42`), `?lang=` query read by `TranslatePage()`
* `window.wx.postMessage({sequence_id, command, data})` via `SendWXMessage` in `../include/globalapi.js`
* all seven commands: `homepage_newproject`, `homepage_openproject`, `get_recent_projects`,
  `homepage_open_recentfile`, `homepage_delete_recentfile`, `homepage_delete_all_recentfile`,
  `homepage_explore_recentfile` (plus `get_web_shortcut`)
* `window.postMessage = HandleStudio` at the bottom of `home.js`; `ShowRecentFileList(pList)` reads
  `path` / `project_name` / `time` / `image` exactly as `MainFrame::get_recent_projects` writes them
* `SetLoginPanelVisibility()` kept as an explicit no-op — `WebViewDialog.cpp:431` calls it **by name**,
  and removing it would raise a JS error on every login-status tick
* `HandleStudio` ignores unknown commands rather than throwing, so a cloud message from an older
  build is harmless
* dark/light driven by the `dark` token in the UA string: `../include/globalapi.js:401-427` adds and
  removes `./css/dark.css`, so that exact href is kept in `index.html` and `css/dark.css` exists
* `text.js` `tid` mechanism kept — the page uses `t12 t28 t31 t32 t33 t35 t88 t89 nocte1`, all
  verified present in the `en` block

`node --check` passes on `home.js` and `text.js`. Every `src`/`href` in `index.html` resolves.

`resources/web/homepage/js/{globalapi,json2,jquery-3.6.0.min}.js` are **kept unchanged** even though
the new page does not use them: `resources/web/orca/missing_connection.html:10-13` loads them (and
`home.js`) by relative path. That page already fails at `OnInit()` upstream, because it loads
`home.js` but not `text.js`, so `TranslatePage()` is undefined there — a pre-existing bug, unchanged
by this work.

`resources/web/include/global.css` tokens rewritten for both blocks; `--main-color` is the accent
*surface*, for the same white-text reason as in the C++ palette.

---

## 8. Compile risks, ranked

1. **`GUI_App.cpp:3064-3070`, removing `wxSystemAppearance app`.** I checked that nothing below the
   `#endif` reads `app`, but this is inside `#ifndef __WINDOWS__ / #ifdef _MSW_DARK_MODE`, i.e. only
   the macOS and Linux jobs compile it and only they can prove it. If anything did use it,
   `-Wunused-variable` is disabled, so the failure would be a hard error, not a warning — visible
   immediately.
2. **`StateColor.cpp` including `../Nocte/NocteTheme.hpp`.** The first time a `Widgets/` file reaches
   into `GUI/Nocte/`. The path is relative and the header has no includes of its own, so it should be
   fine, but if the include path surprises us this is where it shows.
3. **`inline constexpr` in `NocteTheme.hpp`.** C++17 inline variables. External linkage, one
   definition, so no `-Wunused-const-variable` (which `-Wno-unused-variable` covers anyway) and no
   ODR trouble. Would break only on a pre-C++17 compiler, which this tree is not.
4. **The `append_menu_item` overload in the NØCTE menu.** `""` as the 6th argument resolves to
   `const std::string&`, not `const wxBitmap&`. It is the same call shape as `MainFrame.cpp:3344`,
   so if it were ambiguous the file would already not compile. Both lambdas are capture-less, so
   `-Wunused-lambda-capture` (fatal) cannot fire.
5. **UTF-8 byte escapes.** `"N\xC3\x98" "CTE Slicer"` — every literal is split before the `C`. I
   grepped the result in each file; no unsplit `\x98C` anywhere. `/utf-8` is on, but these do not
   depend on it.
6. **`replaces["#009688"] = new_color;`** — `new_color` is `const std::string&`, assigned to a
   `std::string` mapped value. Trivial.
7. **No local compiler**, so none of the above was built. Everything else is a numeric literal swap.

---

## 9. What the user should see in the screenshots

- [ ] **Window title**: `<project> - NØCTE Slicer` (Windows/Linux). macOS shows just the project name, as upstream.
- [ ] **Dark on first start**, on a light-themed Windows, macOS *and* Linux desktop.
- [ ] **3D viewport** near-black `#1A1A1C`, not the old blue-grey `#54545A`.
- [ ] **Top bar** `#0F0F10`; hovering a top-bar tool gives a grey `#5A5A62` fill, never teal.
- [ ] **A "NØCTE" menu next to Help** (in the top-bar drop-down, and in the menu bar when
      "tabs as menu" is on), with one entry: "About NØCTE Slicer", which opens the About dialog.
- [ ] **About dialog** says "NØCTE Slicer is licensed under [AGPL v3]" and still credits OrcaSlicer,
      PrusaSlicer, BambuStudio and Slic3r below it.
- [ ] **Toolbar and sidebar icons monochrome** — this is the biggest single change. Worth a
      side-by-side: switch to light mode too, where upstream's replace map used to do almost nothing.
- [ ] **Home page**: wordmark top-left, "BUILDING THE IMPOSSIBLE" under it, only "Recent" in the rail,
      New/Open cards, recent grid. No account row, no OrcaCloud, no Bambu section, no plugin banner.
- [ ] **Home page in light mode** (Preferences → dark mode off, Windows): the wordmark flips to black,
      the page to white. If the wordmark disappears, the `.BrandLight`/`.BrandDark` swap is wrong.
- [ ] **Right-click a recent file** → "Remove" / "Open Containing Folder"; both act, and the list
      shows "No recent projects yet" once emptied.
- [ ] **Confirm buttons and the Slice button** are grey `#55555D` with white text — legible, not
      white-on-white. This is the one place I deviated from "accent = white".
- [ ] **Gizmo tooltips and the measure tool** in grey `#8C8C90` over both backgrounds.
- [ ] **Tip of the Day** hints name NØCTE Slicer.
- [ ] **Nothing green or teal anywhere**, except the orange `#FF6F00` "reset"/"modified" markers and
      the red error background, which are status colours and stay.
