// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "DataDirMigration.hpp"

#include "DataDirMigrationRules.hpp"
#include "MigrationDialog.hpp"
#include "NocteUi.hpp"

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {
namespace Nocte {

const char *const IMPORT_SOURCE_APP_KEY = "OrcaSlicer";

namespace {

namespace fs = boost::filesystem;

// Written into the new data directory when the user declines, so the question is asked once
// even if the application is closed before AppConfig first saves <key>.conf.
const char *const DECLINED_MARKER = ".nocte-import-declined";
const char *const FAILURE_REPORT  = "MIGRATION_INCOMPLETE.txt";

std::string config_file_name(const std::string &app_key) { return app_key + ".conf"; }

// The environment variable, read as UTF-8 on Windows the way the rest of the tree does
// (boost::nowide::getenv, OrcaSlicer.cpp:1474). Empty string when unset.
std::string env_value(const char *name)
{
    const char *value = boost::nowide::getenv(name);
    return (value == nullptr) ? std::string() : std::string(value);
}

// Drops the keys ADR-003 §3 lists out of the "app" object and writes the result as <new>.conf.
// The file AppConfig reads is JSON with an optional "# MD5 checksum ..." line after the closing
// brace (AppConfig.cpp:961-1112). We write no checksum line: the loader only logs a mismatch
// (AppConfig.cpp:1113-1116). The trailing newline matters - the loader takes
// substr(last_brace + 2), which throws if the file ends exactly at the brace.
bool rewrite_config_file(const fs::path &old_conf, const fs::path &new_conf)
{
    std::string whole;
    {
        boost::nowide::ifstream ifs;
        ifs.open(old_conf.string());
        if (!ifs.good()) {
            BOOST_LOG_TRIVIAL(error) << "nocte: cannot read " << old_conf.string();
            return false;
        }
        std::stringstream buffer;
        buffer << ifs.rdbuf();
        whole = buffer.str();
    }

    const size_t last_brace = whole.find_last_of('}');
    if (last_brace == std::string::npos) {
        BOOST_LOG_TRIVIAL(error) << "nocte: " << old_conf.string() << " is not a JSON object";
        return false;
    }

    nlohmann::json j = nlohmann::json::parse(whole.substr(0, last_brace + 1));

    const auto app_it = j.find("app");
    if (app_it != j.end() && app_it->is_object()) {
        std::vector<std::string> dropped;
        for (auto it = app_it->begin(); it != app_it->end(); ++it) {
            if (is_dropped_app_config_key(it.key()))
                dropped.push_back(it.key());
        }
        for (const std::string &key : dropped)
            app_it->erase(key);
        BOOST_LOG_TRIVIAL(info) << "nocte: dropped " << dropped.size()
                                << " key(s) from the imported configuration";
    }

    boost::nowide::ofstream ofs;
    ofs.open(new_conf.string(), std::ios::out | std::ios::trunc);
    if (!ofs.good()) {
        BOOST_LOG_TRIVIAL(error) << "nocte: cannot write " << new_conf.string();
        return false;
    }
    ofs << j.dump(1, '\t') << std::endl;
    ofs.close();
    return !ofs.fail();
}

void report_failure(const fs::path &new_dir, const std::string &message)
{
    BOOST_LOG_TRIVIAL(error) << "nocte: data directory import failed: " << message;
    try {
        fs::create_directories(new_dir);
        boost::nowide::ofstream ofs;
        ofs.open((new_dir / FAILURE_REPORT).string(), std::ios::out | std::ios::trunc);
        if (ofs.good()) {
            ofs << "Importing the OrcaSlicer configuration did not finish.\n\n"
                << message << "\n\n"
                << "NOCTE Slicer started with its own empty configuration instead. Nothing in "
                   "the OrcaSlicer folder was changed; you can import it by hand from\n"
                   "File > Import > Import Configs.\n";
        }
        // A failed import must not prompt again on every start: the report above tells the
        // user what happened, and the manual import path stays available.
        boost::nowide::ofstream marker;
        marker.open((new_dir / DECLINED_MARKER).string(), std::ios::out | std::ios::trunc);
        if (marker.good())
            marker << "The import from an OrcaSlicer data directory failed; see " << FAILURE_REPORT << ".\n";
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "nocte: could not write " << FAILURE_REPORT << ": " << ex.what();
    }
}

} // namespace

fs::path user_data_dir_for(const std::string &app_key)
{
    if (app_key.empty())
        return {};
#ifdef _WIN32
    const std::string appdata = env_value("APPDATA");
    if (appdata.empty())
        return {};
    return fs::path(appdata) / app_key;
#elif defined(__APPLE__)
    const std::string home = env_value("HOME");
    if (home.empty())
        return {};
    return fs::path(home) / "Library" / "Application Support" / app_key;
#else
    // Since 2.3 the Linux data directory follows XDG (GUI_App.cpp:2554-2561). Inside a Flatpak
    // sandbox XDG_CONFIG_HOME points into ~/.var/app/<id>/config, where no OrcaSlicer directory
    // is reachable, so the import finds nothing and returns - which is the intended behaviour.
    const std::string xdg = env_value("XDG_CONFIG_HOME");
    if (!xdg.empty())
        return fs::path(xdg) / app_key;
    const std::string home = env_value("HOME");
    if (home.empty())
        return {};
    return fs::path(home) / ".config" / app_key;
#endif
}

bool portable_data_dir_present()
{
    try {
        fs::path app_folder = boost::dll::program_location().parent_path();
#ifdef __APPLE__
        // The executable sits in <name>.app/Contents/MacOS (GUI_App.cpp:2538-2541).
        app_folder = app_folder.parent_path().parent_path().parent_path();
#endif
        return fs::exists(app_folder / "data_dir");
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "nocte: could not locate the executable: " << ex.what();
        return false;
    }
}

bool import_data_dir(const fs::path &old_dir, const fs::path &new_dir)
{
    try {
        fs::create_directories(new_dir);

        // One pass over the source, copying only what the rules allow. merge_mode is on so
        // copy_directory_recursively() never calls remove_all() on anything (utils.cpp:1744).
        for (const auto &entry : fs::directory_iterator(old_dir)) {
            const std::string name = entry.path().filename().string();
            if (!fs::is_directory(entry.path()) || !is_copied_top_level_dir(name)) {
                BOOST_LOG_TRIVIAL(debug) << "nocte: import skips " << name;
                continue;
            }
            BOOST_LOG_TRIVIAL(info) << "nocte: importing " << name;
            copy_directory_recursively(entry.path(), new_dir / name, nullptr, true);
        }

        const fs::path old_conf = old_dir / config_file_name(IMPORT_SOURCE_APP_KEY);
        const fs::path new_conf = new_dir / config_file_name(SLIC3R_APP_KEY);
        if (!rewrite_config_file(old_conf, new_conf)) {
            report_failure(new_dir, "The configuration file could not be converted.");
            return false;
        }
    } catch (const std::exception &ex) {
        report_failure(new_dir, ex.what());
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "nocte: imported the data directory from " << old_dir.string();
    return true;
}

void run_data_dir_migration()
{
    try {
        // 1. --datadir was given: the CLI called set_data_dir() in CLI::setup()
        //    (OrcaSlicer.cpp:7946), so data_dir() is non-empty only in that case.
        if (!Slic3r::data_dir().empty()) {
            BOOST_LOG_TRIVIAL(info) << "nocte: --datadir given, no data directory import";
            return;
        }
        if (portable_data_dir_present()) {
            BOOST_LOG_TRIVIAL(info) << "nocte: portable mode, no data directory import";
            return;
        }

        // 2. Where the data directory is about to be.
        const fs::path new_dir = user_data_dir_for(SLIC3R_APP_KEY);
        if (new_dir.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "nocte: cannot locate the user data directory";
            return;
        }

        // 3. Not a first run, or the question was already answered.
        if (fs::exists(new_dir / config_file_name(SLIC3R_APP_KEY)) ||
            fs::exists(new_dir / DECLINED_MARKER))
            return;

        // 4. Nothing to import from.
        const fs::path old_dir = user_data_dir_for(IMPORT_SOURCE_APP_KEY);
        if (old_dir.empty() || old_dir == new_dir ||
            !fs::exists(old_dir / config_file_name(IMPORT_SOURCE_APP_KEY)))
            return;

        // 5. Ask, once.
        const MigrationChoice choice = ask_import_data_dir(old_dir);
        if (choice == MigrationChoice::Import) {
            import_data_dir(old_dir, new_dir);
            return;
        }

        if (choice == MigrationChoice::Unavailable) {
            // No modal is possible here yet (see MigrationDialog.hpp). Leave no marker, so the
            // platform gets its prompt as soon as one exists.
            BOOST_LOG_TRIVIAL(info) << "nocte: an OrcaSlicer data directory exists at "
                                    << old_dir.string()
                                    << " but no import prompt can be shown on this platform";
            return;
        }

        BOOST_LOG_TRIVIAL(info) << "nocte: the user chose to start fresh";
        fs::create_directories(new_dir);
        boost::nowide::ofstream marker;
        marker.open((new_dir / DECLINED_MARKER).string(), std::ios::out | std::ios::trunc);
        if (marker.good())
            marker << "The import from an OrcaSlicer data directory was declined.\n";
    } catch (const std::exception &ex) {
        // Never stop the application from starting over this.
        BOOST_LOG_TRIVIAL(error) << "nocte: data directory import aborted: " << ex.what();
    }
}

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r
