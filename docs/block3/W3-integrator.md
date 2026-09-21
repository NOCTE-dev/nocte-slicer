# W3 — identity key rename: integrator list

Everything in this file is outside W3's ownership (`CMakeLists.txt`, `src/CMakeLists.txt`,
`scripts/**`, `.github/**`, `*.rc.in`, and the `localization/i18n/**` file *names*). W3 edited
`version.inc`, `src/libslic3r/libslic3r.h`, `src/OrcaSlicer.cpp` and added
`src/slic3r/GUI/Nocte/{DataDirMigration,DataDirMigrationRules,MigrationDialog}.{hpp,cpp}` plus
`tools/nocte/migration_dry_run.py`. All of it lands in **one commit** — a partial rename loses
every translation and breaks the Linux job.

## 0. Blockers — do these or the rename is not shippable

### 0a. `SLIC3R_APP_KEY` also drives Linux and macOS packaging

`version.inc` now sets `SLIC3R_APP_KEY "NocteSlicer"`. Three templates read `@SLIC3R_APP_KEY@`
as a *packaging* identity, and the assets they name do not exist under the new name:

- `src/dev-utils/platform/unix/build_appimage.sh.in:7,19,21,22,24,26,35,38-41,52` —
  `resources/images/@SLIC3R_APP_KEY@_192px.png` (only `OrcaSlicer_192px.png` exists),
  `scripts/flatpak/com.orcaslicer.@SLIC3R_APP_KEY@.metainfo.xml` (only
  `com.orcaslicer.OrcaSlicer.metainfo.xml` exists), and
  `APP_IMAGE="@SLIC3R_APP_KEY@_Linux_V@SoftFever_VERSION@.AppImage"`.
- `.github/workflows/build_orca.yml:632` then runs
  `mv -n ./build/OrcaSlicer_Linux_V${{ env.ver_pure }}.AppImage "$appimage"` (and the `AppImage`
  the script emits is named from `@SLIC3R_APP_KEY@`) — with the key
  renamed the file is `NocteSlicer_Linux_V…`, `mv` fails, and the step (`bash -eo pipefail`)
  **fails the Linux job**.
- `src/dev-utils/platform/osx/Info.plist.in:6,12` — `CFBundleExecutable`/`CFBundleName` would
  become `NocteSlicer` while the binary inside `OrcaSlicer.app/Contents/MacOS` is still
  `OrcaSlicer`: the bundle will not launch. macOS CI does not launch it, so the job stays
  green and ships a broken `.app`.

ADR-003 §3 defers Linux/macOS packaging identity and requires those builds to stay green, so
**decouple** (recommended, one line, no packaging touched):

```
# src/libslic3r/libslic3r_version.h.in   (add to check_touchpoints.py's allowlist)
-#define SLIC3R_APP_KEY "@SLIC3R_APP_KEY@"
+#define SLIC3R_APP_KEY "@SLIC3R_RUNTIME_APP_KEY@"
```

and in `version.inc` split the two, keeping `set(SLIC3R_APP_KEY "OrcaSlicer")` for packaging and
`set(SLIC3R_RUNTIME_APP_KEY "NocteSlicer")` for the data directory, conf and catalog. W3's C++
is unaffected either way — it only ever reads the `SLIC3R_APP_KEY` macro.
The alternative is to add `resources/images/NocteSlicer_192px.png`,
`scripts/flatpak/com.orcaslicer.NocteSlicer.metainfo.xml` and fix `build_orca.yml:640` —
more moving parts, same result.

### 0b. `src/slic3r/Utils/Process.cpp:43` pins the executable name

```cpp
path += (instance_type == NewSlicerInstanceType::Slicer) ? "orca-slicer.exe" : "bambu-gcodeviewer.exe";
```

Not on the allowlist. After the launcher rename, "open in a new instance" on Windows launches a
file that does not exist. Either allowlist the file and change the literal to `nocte-slicer.exe`,
or leave the launcher named `orca-slicer.exe` this block. Do not do the rename without one.

## 1. Catalog rename — `localization/i18n/**` (names only; contents need no change)

The domain is the file name: `wxLocale::AddCatalog(SLIC3R_APP_KEY)` (`GUI_App.cpp:8084`),
`GetBestTranslation(SLIC3R_APP_KEY)` (`:7915`) and
`GetAvailableTranslations(SLIC3R_APP_KEY)` (`Preferences.cpp:373`) all follow the macro
automatically — **confirmed, no source change needed**. Nothing inside a `.po`/`.pot` matters:
`Project-Id-Version` is unused at runtime, and `localization/i18n/list.txt` holds source paths
only (no product name). The `.mo` files are generated, not tracked (`resources/i18n/` holds only
`placeholder.txt`).

```
localization/i18n/OrcaSlicer.pot -> localization/i18n/NocteSlicer.pot
```
and for each of the 23 languages `ca cs de en es eu fr hu it ja ko lt nl pl pt_BR ru sv th tr uk
vi zh_CN zh_TW`:
```
localization/i18n/<lang>/OrcaSlicer_<lang>.po -> localization/i18n/<lang>/NocteSlicer_<lang>.po
```

References to update in the same commit:

| File:line | now | becomes |
|---|---|---|
| `CMakeLists.txt:903` | `-o "${BBL_L18N_DIR}/OrcaSlicer.pot"` | `…/NocteSlicer.pot` |
| `CMakeLists.txt:912` | `file(GLOB BBL_L10N_PO_FILES "${BBL_L18N_DIR}/*/OrcaSlicer*.po")` | `…/*/NocteSlicer*.po` |
| `CMakeLists.txt:915` | `SET(po_new_file "${po_dir}/OrcaSlicer_.po")` | `…/NocteSlicer_.po` (dead variable, rename for consistency) |
| `CMakeLists.txt:918` | `COMMAND msgmerge -N -o ${po_file} ${po_file} "${BBL_L18N_DIR}/OrcaSlicer.pot"` | `…/NocteSlicer.pot` |
| `CMakeLists.txt:925` | `file(GLOB L10N_PO_FILES "${BBL_L18N_DIR}/*/OrcaSlicer*.po")` | `…/*/NocteSlicer*.po` |
| `CMakeLists.txt:929` | `SET(mo_file "${L10N_DIR}/${po_dir}/OrcaSlicer.mo")` | `…/NocteSlicer.mo` |
| `scripts/run_gettext.bat:13` | `set "pot_file=./localization/i18n/OrcaSlicer.pot"` | `…/NocteSlicer.pot` |
| `scripts/run_gettext.bat:18` | `set "generated_pot=%generated_i18n%\OrcaSlicer.pot"` | `…\NocteSlicer.pot` |
| `scripts/run_gettext.bat:122` | `set "lang=%name:OrcaSlicer_=%"` | `%name:NocteSlicer_=%` |
| `scripts/run_gettext.bat:135` | `…-o "./resources/i18n/!lang!/OrcaSlicer.mo" "%file%"` | `…/NocteSlicer.mo` |
| `scripts/run_gettext.sh:8,85,108,111,117,120,124` | same four names in the POSIX twin | same |
| `.github/workflows/check_locale.yml:28,34,36` | `pot_file="./localization/i18n/OrcaSlicer.pot"`, `if [ -f "$dir/OrcaSlicer_${lang}.po" ]`, `msgfmt --check-format -o ./resources/i18n/${lang}/OrcaSlicer.mo $dir/OrcaSlicer_${lang}.po` | `NocteSlicer` in all three |
| `scripts/HintsToPot.py:23` | `path_to_pot = Path(sys.argv[2]).parent / "i18n" / "OrcaSlicer.pot"` | `NocteSlicer.pot` |
| `AGENTS.md:71,97` | documents the catalog paths | `NocteSlicer` |

**Caveat:** `deps_src/hints/HintsToPot.cpp:63` hard-codes `"OrcaSlicer.pot"` and `deps/**` is
off limits. It is used only by the CMake `gettext_make_pot` target (`CMakeLists.txt:904, 1253`);
`run_gettext.bat`/`.sh` call `scripts/HintsToPot.py` instead. So the CMake pot target will append
hints to a stale `OrcaSlicer.pot` until `deps_src` is patched — harmless at runtime, worth a
follow-up. Also note `localization/i18n/list.txt` lists no `src/slic3r/GUI/Nocte/**` file, so
NØCTE strings are not extracted yet.

## 2. Windows launcher and installer

| File:line | now | becomes |
|---|---|---|
| `src/CMakeLists.txt:222` | `        OUTPUT_NAME "orca-slicer"` (target `OrcaSlicer_app_gui`) | `        OUTPUT_NAME "nocte-slicer"` |
| `src/dev-utils/platform/msw/OrcaSlicer.rc.in:9` | `   VALUE "CompanyName", "SoftFever"` | `   VALUE "CompanyName", "NOCTE Engineering"` |
| `src/dev-utils/platform/msw/OrcaSlicer.rc.in:16` | `   VALUE "OriginalFilename", "orca-slicer.exe"` | `   VALUE "OriginalFilename", "nocte-slicer.exe"` |
| `CMakeLists.txt:1336` | `set (CPACK_PACKAGE_FILE_NAME "OrcaSlicer_Windows_Installer_V${SoftFever_VERSION}")` | `"NocteSlicer_Windows_Installer_V${SoftFever_VERSION}"` |
| `CMakeLists.txt:1353` | `set (CPACK_NSIS_INSTALLED_ICON_NAME  "$INSTDIR\\\\orca-slicer.exe")` | `…\\\\nocte-slicer.exe` |
| `CMakeLists.txt:1364` | `set(CPACK_PACKAGE_EXECUTABLES "orca-slicer;OrcaSlicer")` | `"nocte-slicer;NOCTE Slicer"` |
| `CMakeLists.txt:1365` | `set(CPACK_CREATE_DESKTOP_LINKS "orca-slicer")` | `"nocte-slicer"` |
| `.github/workflows/build_orca.yml:540` | `          path: ${{ github.workspace }}/${{ env.BUILD_DIR }}/OrcaSlicer*.exe` | `…/NocteSlicer*.exe` |
| `scripts/msix/build_msix.ps1:34-35` | `Test-Path (Join-Path $InstallDir 'orca-slicer.exe')` and its `throw` | `nocte-slicer.exe` |

**Do NOT change** `src/CMakeLists.txt:166-167`:
```
    set_target_properties(OrcaSlicer PROPERTIES OUTPUT_NAME "orca-slicer")
    set(SLIC3R_APP_CMD "orca-slicer")
```
That is the unix binary name. `build_orca.yml:630` runs
`./scripts/check_appimage_libs.sh ./build/package ./build/package/bin/orca-slicer`,
`build_appimage.sh.in:16` does `mv @SLIC3R_APP_CMD@ AppRun`, the external regression tests at
`build_orca.yml:684` use the same path, and `tests/cli` runs the `orca-slicer` binary. Changing
it breaks Linux packaging for no Windows benefit.

`build_orca.yml:576` (`asset_path: …/OrcaSlicer_Windows_Installer_…exe`) is inside
`github.repository == 'OrcaSlicer/OrcaSlicer'` and never runs in the fork; update it for
tidiness only. Same for the MSIX steps at `:592-610`.

**Keep `CMakeLists.txt:317`** (`set_property(CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE
"${CMAKE_BINARY_DIR}/OrcaSlicer")`). The install tree name is consumed by `build_win.bat:798`,
`build_orca.yml:475, 506, 530, 601`, `scripts/msix/build_msix.ps1:11` and
`scripts/test_build_win.ps1:761`. Renaming it is five more edits for a directory no user sees.

### `build_win.bat` / `test_build_win.ps1` — leave both alone

`build_win.bat:797-798` only *prints* the path (`echo Run it !slicer_exe!` at :830); it never
tests for the file, so a stale name breaks nothing. But `build_win.bat` is explicitly **not** a
touch point (ADR-003 §4) while `scripts/test_build_win.ps1` is, and
`test_build_win.ps1:759, 761, 775, 778` assert on the **string build_win.bat prints**. Changing
only the test makes it fail. Recommendation: leave all six lines as `orca-slicer.exe` this
block and file a follow-up to allowlist `build_win.bat` and change them together.

`tests/libslic3r/test_config.cpp:890` and friends pass `argv[0] = "orca-slicer"` to the CLI
parser — cosmetic, never resolved to a file. Leave it.

## 3. New files to add to `src/slic3r/CMakeLists.txt` (`SLIC3R_GUI_SOURCES`)

```
    GUI/Nocte/DataDirMigration.cpp
    GUI/Nocte/DataDirMigration.hpp
    GUI/Nocte/DataDirMigrationRules.cpp
    GUI/Nocte/DataDirMigrationRules.hpp
    GUI/Nocte/MigrationDialog.cpp
    GUI/Nocte/MigrationDialog.hpp
```

`src/OrcaSlicer.cpp` now includes `slic3r/GUI/Nocte/NocteUi.hpp` and calls
`Slic3r::GUI::Nocte::run_data_dir_migration()` inside the existing `#ifdef SLIC3R_GUI` block, so
a `SLIC3R_GUI=OFF` build never sees either. No `GUI_App.cpp` hunk is needed — see
`docs/block3/W3-handover.md`.

## 4. Consumers that follow the macro on their own (checked, no action)

`AppConfig::config_path()` (`AppConfig.cpp:1820`), `PluginAuditManager.cpp:286-288`,
`GUI_App.cpp:2520 SetAppName`, `:7944 GetBestTranslation`, `:8113 AddCatalog`,
`Preferences.cpp:373 GetAvailableTranslations`, `BackgroundSlicingProcess.cpp:945` (temp upload
name) and `OrcaSlicer.cpp:8029` (the `--help` banner) all read `SLIC3R_APP_KEY` and need no edit.

Two side effects worth knowing about:

- `src/libslic3r/Format/3mf.cpp:2564` writes the generic-3MF `Application` metadata as
  `SLIC3R_APP_KEY-SLIC3R_VERSION`, so plain `.3mf` exports now say `NocteSlicer-…`. The BBS
  writer/reader in `bbs_3mf.cpp` compares the literals `"BambuStudio-"` / `"OrcaSlicer-"`
  (`:3995, :3998`), not the macro, so project files are unaffected.
- `src/slic3r/GUI/wxExtensions.cpp:376` does `tooltip.Replace("Slic3r", SLIC3R_APP_KEY, true)`,
  so those tooltips now read "NocteSlicer" rather than "NØCTE Slicer". Not a touch point;
  cosmetic, for a later block.

## 5. Manual verification (the source directory must be untouched)

```powershell
$src = "$env:APPDATA\OrcaSlicer"
$before = Get-ChildItem $src -Recurse -File |
    ForEach-Object { [pscustomobject]@{ P = $_.FullName.Substring($src.Length)
                                        H = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
                                        W = $_.LastWriteTimeUtc } } | Sort-Object P
$before | Export-Csv "$env:TEMP\orca-before.csv" -NoTypeInformation

python tools\nocte\migration_dry_run.py          # read-only preview, compare with what follows
Remove-Item "$env:APPDATA\NocteSlicer" -Recurse -Force -ErrorAction SilentlyContinue
& "<build>\OrcaSlicer\nocte-slicer.exe"          # answer "Yes", then quit

$after = Get-ChildItem $src -Recurse -File |
    ForEach-Object { [pscustomobject]@{ P = $_.FullName.Substring($src.Length)
                                        H = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
                                        W = $_.LastWriteTimeUtc } } | Sort-Object P
Compare-Object $before $after -Property P,H,W    # MUST print nothing
Get-ChildItem "$env:APPDATA\NocteSlicer"         # user/ shapes/ NocteSlicer.conf, no cache/log/system
```

Then restart: the prompt must not appear a second time. Run once more with
`--datadir "$env:TEMP\dd"` and once with a `data_dir` folder next to the exe: no prompt either
time.
