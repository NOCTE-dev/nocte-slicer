# ADR-003 — Own product: offline by default, own identity, renamed executable and data directory

- **Status:** Accepted
- **Date:** 2026-09-18
- **Deciders:** NØCTE Engineering
- **Related:** ADR-001 (fork of OrcaSlicer, upstream touch points), ADR-002 (no Bambu network plugin, LAN Developer Mode)

## Context

Block 2 produced a binary that compiles and passes its tests on every platform, but what a user sees is OrcaSlicer with a NØCTE icon: the window is titled OrcaSlicer, the home page offers an Orca Cloud account and a Bambu cloud login, presets synchronise with Orca Cloud, the Help menu checks for updates, Preferences has an Online tab, and the Device tab asks for Bambu's network plugin.

The product direction is the one Bambu Studio took with PrusaSlicer: a complete fork, engine and interface, that becomes its own product through a redesign of the interface it inherited, not a rewrite. The NØCTE engine (`src/libslic3r/Nocte/`) was written without any dependency on the interface for exactly this reason.

The upstream base (`6b0e190e64`, a post-2.4 development tree) carries more online surface than the OrcaSlicer releases: a first-party Orca Cloud with OAuth login, keychain token storage, preset synchronisation and a plugin marketplace, a multi-provider cloud abstraction, and a Python plugin system. Most of it is already off by default (`installed_networking`, `enable_ota`, `use_printer_agents`, `enable_multi_machine` are false; telemetry is hard-disabled), but the cloud agent is always created, and the visible entry points remain.

## Decision

### 1. Offline by default

NØCTE Slicer creates no cloud agent, offers no account, never checks for updates online, never synchronises presets or profiles, and never downloads a network plugin. Concretely:

- `NetworkAgentFactory::create_agent_from_config()` returns a `NetworkAgent` without a cloud agent, and `create_cloud_agent()` returns null for every provider. `NetworkAgent`'s cloud methods are null-guarded no-ops, so every login, sync and token path degrades to "not logged in" without touching its callers. The Bambu printer agent (a thunk over the proprietary plugin, ADR-002) is no longer registered.
- The version-check and profile-update URLs are empty; `sync_system_preset` is off; the Preferences Online tab, the Help entries "Check for Updates" and "Open Network Test", the "Sync Presets" entries, the network-plugin prompts and dialogs, the login dialogs and the "Quit Stealth Mode" escape are removed; the home page and the setup wizard carry no account or plugin pages.
- The Device, Multi-device and device-calibration tabs stay hidden until the NØCTE LAN agent (ADR-002) can show live printer state. The Calibration menu (temperature towers, flow, pressure advance) is a slicer feature and stays.
- Printing is LAN Developer Mode through `NocteLanPrinterAgent`, registered under the id `nocte-lan`.

Verification is behavioural: the application must open no outbound socket at start-up, on the home page, in the wizard, in Preferences or when opening a project.

### 2. Own identity

- Monochrome palette: black, white and greys; no teal (`#009688` and derivatives) and no green (`#00AE42`). The palette is changed at its central points (`StateColor::gDarkColors`, `ColorRGBA::ORCA()`, the ImGui palette, the SVG replace map in `BitmapCache`, the toolbar tints in `GLTexture`, the 3D background, `resources/web/include/global.css`) rather than at every call site; the remaining direct colour literals are edited where they are visible.
- Dark theme by default on Windows, Linux and macOS.
- The user-visible name is "NØCTE Slicer" everywhere a user reads it: window title, dialogs, wizard, home page, hints. Translatable strings keep English as the source language; Spanish translations are added for NØCTE strings. Vendor identifiers such as `OrcaFilamentLibrary` are preset keys, not user-facing names, and are not renamed.
- The setup wizard lists Bambu Lab printers only. The other vendor profiles stay in `resources/profiles`, so existing presets that inherit from them keep resolving; the filter lives in the wizard's own web page.
- The NØCTE features are reachable from a NØCTE menu next to Help and from the object context menu, as dialogs (`DPIDialog`), because the gizmo registry (`GLGizmosManager`) is not a touch point.

### 3. Renamed executable and data directory, with a copying migration

- The runtime key becomes `NocteSlicer`: the data directory is `%APPDATA%\NocteSlicer` (`~/.config/NocteSlicer`, `~/Library/Application Support/NocteSlicer`), the configuration file `NocteSlicer.conf`, the translation catalog `NocteSlicer.mo`. It is a separate CMake variable, `SLIC3R_RUNTIME_APP_KEY`, fed into the `SLIC3R_APP_KEY` macro of `libslic3r_version.h`; the CMake variable `SLIC3R_APP_KEY` keeps `OrcaSlicer` because the Linux AppImage script and the macOS `Info.plist` derive asset and bundle names from it and those packaging identities are deferred. The Windows launcher is `nocte-slicer.exe` (also in `Utils/Process.cpp`, which spawns a new instance by file name). The library `OrcaSlicer.dll`, its `orcaslicer_main` export, the CMake project and target names, the unix binary `orca-slicer` and the `build/OrcaSlicer` install tree are not renamed: the launcher loads the library by a name compiled into a file that is not a touch point, and the CI gate and build scripts pin the others.
- The key, the catalog file names (`localization/i18n/**`), the CMake gettext targets, `scripts/run_gettext.bat` and `.github/workflows/check_locale.yml` change in one commit: the catalog domain is the key, so any partial rename silently loses every translation.
- On the first start with an empty NØCTE data directory, if an OrcaSlicer data directory exists, the application offers to import it. Import **copies** the user-authored subset (`user/**`, `shapes/`, `SVG/`, the configuration file, renamed and stripped of the keys that describe the other application's file associations, desktop integration and network plugin) and never writes to, renames or locks the source. Runtime directories (`cache/`, `log/`, `plugins/`, `system/`, `ota/`, the machine id, `user_backup-*`) are not copied. The migration is skipped under `--datadir` and in portable mode. The manual fallback stays: File → Import → Import Configs.
- The installer never touches an OrcaSlicer installation: its own package name (and therefore install directory and Start Menu folder), its own registry key, and no "uninstall the previous version first" step.
- Linux and macOS packaging identity (`.desktop`, AppImage, Flatpak id, `Info.plist`) is deferred; those builds must stay green, so the files that carry it are not edited in this block.

### 4. Touch-point allowlist extension

The following upstream files join `tools/nocte/check_touchpoints.py`, each with the discipline stated:

| File | Why | Discipline |
|---|---|---|
| `src/slic3r/GUI/GUI_App.cpp` | app display name, dark mode on macOS, stealth escape, login funnel, update check, plugin dialogs, printer-agent resolution, name strings | at most twelve marked hunks of at most twenty lines; bodies live in `src/slic3r/GUI/Nocte/` |
| `src/libslic3r/AppConfig.cpp` | empty version and profile URLs; offline and dark defaults | one defaults block and two constants |
| `src/libslic3r/libslic3r.h` | `SLIC3R_APP_FULL_NAME` and the gcode-viewer names | one hunk, in the rename commit |
| `src/slic3r/GUI/Preferences.cpp` | removal of the Online tab | deletions only |
| `src/slic3r/GUI/Widgets/StateColor.cpp`, `src/libslic3r/Color.hpp`, `src/slic3r/GUI/ImGuiWrapper.cpp`, `src/slic3r/GUI/GLTexture.cpp`, `src/slic3r/GUI/GLCanvas3D.cpp`, `src/slic3r/GUI/BBLTopbar.cpp` | the palette | colour values only, no logic |
| `src/slic3r/GUI/BitmapCache.cpp` | SVG recolouring at load | two map entries |
| `src/slic3r/GUI/WebGuideDialog.cpp` | removal of the plugin page of the wizard | deletions only |
| `src/CMakeLists.txt`, `src/libslic3r/libslic3r_version.h.in`, `src/dev-utils/platform/msw/OrcaSlicer.rc.in`, `src/slic3r/Utils/Process.cpp`, `scripts/run_gettext.bat`, `scripts/run_gettext.sh`, `scripts/HintsToPot.py`, `scripts/test_build_win.ps1` | launcher name, runtime key, version resource, new-instance file name, catalog names, CI gate | integrator only |

Still not touch points: `GUI_Factories.cpp`, `GLGizmosManager.*`, `ConfigWizard.cpp`, `PresetBundle.cpp`, `OrcaSlicer_app_msvc.cpp`, `build_win.bat`, everything under `deps/`, and the Linux and macOS packaging files.

## Consequences

**Positive:** the product is what the user expects it to be: no account, no cloud, no third-party plugin, its own look and name, its own data directory, safe to install next to OrcaSlicer. The engine and the panels are untouched by any of this and remain reusable under a different interface later.

**Negative:** the monthly upstream merge now spans more files, mostly palette blocks and small marked hunks; `GUI_App.cpp` in particular churns upstream and will need care at every sync. Translations of upstream strings that mention OrcaSlicer are orphaned wherever the English source changed, and fall back to English until retranslated. Users of non-Bambu printers must wait for the multi-vendor wizard. Deferred: 3D highlighting of repairs, Linux and macOS packaging identity, the Device tab, and sending a print, which still requires an explicit decision.
