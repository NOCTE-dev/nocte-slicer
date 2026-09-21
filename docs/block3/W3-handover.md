# W3 — handover: no `GUI_App.cpp` hunk needed (ticket `nocte-rename`)

**For W2, who owns `src/slic3r/GUI/GUI_App.cpp` in this PR: W3 needs nothing from you.**

The plan allowed for a ≤6-line marked block in `GUI_App::init_app_config()` calling
`Slic3r::GUI::Nocte::run_data_dir_migration()` after `SetAppName(SLIC3R_APP_KEY)`
(`GUI_App.cpp:2520`). It is not needed, and it would not have worked. Reasons, so nobody
re-adds it at the next sync:

1. `init_app_config()` is called from the **`GUI_App` constructor** (`GUI_App.cpp:1122`), not
   from `OnInit()`.
2. `GUI_App` is constructed **before `wxEntry()`**: `GUI_Init.cpp:44` does
   `GUI::GUI_App* gui = new GUI::GUI_App();` and only reaches `wxEntry(...)` at `:63/:65`.
   So at `:2520` `wxApp::Initialize()` has not run — no `gtk_init`, no MSW toolkit init — and
   no wx window, dialog or `wxMessageBox` can be shown. A prompt there is not a style choice,
   it is a crash on GTK.
3. Worse, `GUI_Init.cpp:47` reads `gui->app_config->get("app", "single_instance")` immediately
   after construction and then `Slic3r::instance_check()` takes the data-directory lock. The
   import therefore has to be **finished** before `new GUI_App()`, not merely before
   `AppConfig::exists()` at `GUI_App.cpp:2602`.

So the call lives in `src/OrcaSlicer.cpp`, in the existing `#ifdef SLIC3R_GUI` branch, on the
line before `return Slic3r::GUI::GUI_Run(params);` — after `CLI::setup()` has run
`set_data_dir(m_config.opt_string("datadir"))` (`OrcaSlicer.cpp:7946`), which is how
`run_data_dir_migration()` detects `--datadir` with no arguments of its own.

The prompt is a native `MessageBoxW`, the same thing `OrcaSlicer.cpp:1345` and `:7874` already
use at this point in start-up. See `src/slic3r/GUI/Nocte/MigrationDialog.hpp` for the full
reasoning and the Linux/macOS follow-up.

## The one thing W3 does ask of W2

`src/libslic3r/libslic3r.h` now defines `SLIC3R_APP_FULL_NAME "NOCTE Slicer"` — **ASCII**, no
`Ø`. Its consumers build a `wxString` straight from the narrow literal with no UTF-8
conversion (`wxString(SLIC3R_APP_FULL_NAME)` at `CreatePresetsDialog.cpp:1054, 1063, 1070, …`,
plain `std::string` at `OrcaSlicer.cpp:1339` and `Config.cpp:1411`), so a UTF-8 `Ø` would be
decoded in the current 8-bit locale and render as mojibake. Anywhere the real `NØCTE Slicer`
should appear must go through `from_u8()` / `_L()` / `_u8L()` at the call site, not through this
macro.
