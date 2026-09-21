// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "DataDirMigrationRules.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace GUI {
namespace Nocte {

namespace {

bool starts_with(const std::string &value, const char *prefix)
{
    const std::string p(prefix);
    return value.size() >= p.size() && value.compare(0, p.size(), p) == 0;
}

bool contains_lowercased(const std::string &value, const char *needle)
{
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find(needle) != std::string::npos;
}

} // namespace

bool is_copied_top_level_dir(const std::string &name)
{
    // user/    presets, filament and machine settings, hints.cereal - everything the user made.
    // shapes/  the custom shape gallery (Slic3r::custom_shapes_dir()).
    // SVG/     arrange diagnostics (ArrangeJob.cpp:502); kept because ADR-003 §3 names it.
    return name == "user" || name == "shapes" || name == "SVG";
}

bool is_never_copied_top_level_entry(const std::string &name)
{
    static const char *const runtime_entries[] = {
        "cache",                   // downloaded resources and preset caches, rebuilt on demand
        "log",                     // the other application's logs
        "plugins",                 // Bambu network plugin binaries (ADR-002: never loaded)
        "orca_plugins",            // Python plugins, downloaded code
        "python",                  // the plugin interpreter's virtual environment
        "system",                  // vendor profiles, shipped in resources/
        "ota",                     // update payloads
        "models",                  // downloaded model cache
        "printers",                // per-model device resources, re-fetched
        ".orcaslicer_machine_id",  // the other application's installation identity
    };
    for (const char *entry : runtime_entries) {
        if (name == entry)
            return true;
    }
    // user_backup-v2.5.0-dev and friends: a snapshot of a presets tree, not the live one.
    return starts_with(name, "user_backup-");
}

bool is_dropped_app_config_key(const std::string &key)
{
    static const char *const dropped[] = {
        // File associations: they point the shell at the other application's executable, and
        // AppConfig re-reads the registry for a fresh install (GUI_App.cpp:2614-2625).
        "associate_3mf", "associate_stl", "associate_step", "associate_gcode", "associate_drc",
        // Cloud and network plugin (ADR-002, ADR-003 §1). The preset-sync switches come across
        // enabled from an OrcaSlicer that was logged in; the fork's own defaults apply instead.
        "cloud_providers", "installed_networking", "use_printer_agents", "enable_ota",
        "sync_system_preset", "sync_user_preset", "stealth_mode", "hide_login_side_panel",
        // Session identity. The tokens themselves live in the keychain, not here, but a stale
        // user id would make the app look logged in.
        "user_id",
        // Dropped so the NOCTE default (dark) applies rather than the OrcaSlicer preference.
        "dark_color_mode",
    };
    for (const char *candidate : dropped) {
        if (key == candidate)
            return true;
    }
    // desktop_integration_linux, desktop_integration_app_path, ...
    if (starts_with(key, "desktop_integration"))
        return true;
    // network_plugin_version, network_plugin_compability, ... (AppConfig.cpp:157)
    if (starts_with(key, "network_plugin"))
        return true;
    // Anything that reads like a credential, whatever a future release calls it.
    return contains_lowercased(key, "token") || contains_lowercased(key, "secret") ||
           contains_lowercased(key, "password");
}

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r
