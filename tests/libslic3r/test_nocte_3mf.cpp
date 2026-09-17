// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The 3mf identity contract of ADR-001: a NOCTE-written project is recognisable as NOCTE without
// ever losing native Bambu Studio mode. That means the Application tag stays "BambuStudio-<ver>",
// the NocteSlicer tag rides alongside it exactly once however many times the project is re-saved,
// the project report is an additive archive entry, and a minimal published 3MF carries neither.

#include <catch2/catch_all.hpp>

#include <memory>
#include <string>

#include <boost/algorithm/string/predicate.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Nocte/NocteVersion.hpp"
#include "libslic3r/Nocte/ProjectReport.hpp"

#include "test_utils.hpp"

using namespace Slic3r;

namespace {

// Reads one entry of a .3mf by name. Returns false when the entry is not in the archive, so the
// same helper answers both "is it there" and "what does it say".
bool read_archive_entry(const std::string &archive_path, const std::string &entry, std::string &out)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, archive_path))
        return false;

    bool      found = false;
    const int index = mz_zip_reader_locate_file(&archive, entry.c_str(), nullptr, 0);
    if (index >= 0) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&archive, index, &stat)) {
            out.assign(static_cast<size_t>(stat.m_uncomp_size), '\0');
            found = out.empty() || mz_zip_reader_extract_to_mem(&archive, index, &out[0], out.size(), 0);
        }
    }

    close_zip_reader(&archive);
    return found;
}

size_t count_occurrences(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// A one-object model on the plate, ready to be stored.
void load_cube_model(Model &model)
{
    const std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
    REQUIRE(load_stl(src_file.c_str(), &model));
    model.add_default_instances();
}

void store(Model &model, DynamicPrintConfig &config, const std::string &path, SaveStrategy strategy)
{
    StoreParams store_params;
    store_params.path     = path.c_str();
    store_params.model    = &model;
    store_params.config   = &config;
    store_params.strategy = strategy;
    REQUIRE(store_bbs_3mf(store_params));
}

} // namespace

TEST_CASE("A stored project carries the NocteSlicer tag beside an unchanged BambuStudio Application tag", "[Nocte3mf]")
{
    Model model;
    load_cube_model(model);

    // store_bbs_3mf stages project_settings.config through the model's backup path; point it at a
    // writable temp dir (the default lives under a read-only root in CI).
    ScopedTemporaryDir backup_dir("nocte_identity");
    model.set_backup_path(backup_dir.string());

    ScopedTemporaryFile temp(".3mf");
    DynamicPrintConfig  config = DynamicPrintConfig::full_print_config();
    store(model, config, temp.string(), SaveStrategy::Zip64 | SaveStrategy::Silence);

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             dst_plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(temp.string().c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                         &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));

    REQUIRE(dst_model.model_info != nullptr);
    REQUIRE(dst_model.model_info->metadata_items.count(NOCTE_3MF_METADATA_TAG) == 1);
    REQUIRE(dst_model.model_info->metadata_items[NOCTE_3MF_METADATA_TAG] == std::string(NOCTE_VERSION));

    // The identity that decides native mode is untouched: Bambu Studio only sets m_is_bbl_3mf when
    // Application starts with "BambuStudio-" (ADR-001).
    REQUIRE(boost::starts_with(dst_model.model_info->metadata_items["Application"], "BambuStudio-"));
    REQUIRE(is_bbl_3mf);

    release_PlateData_list(dst_plates);
}

TEST_CASE("A stored project carries an additive project report describing its objects", "[Nocte3mf]")
{
    Model model;
    load_cube_model(model);

    ScopedTemporaryDir backup_dir("nocte_report");
    model.set_backup_path(backup_dir.string());

    ScopedTemporaryFile temp(".3mf");
    DynamicPrintConfig  config = DynamicPrintConfig::full_print_config();
    store(model, config, temp.string(), SaveStrategy::Zip64 | SaveStrategy::Silence);

    std::string payload;
    REQUIRE(read_archive_entry(temp.string(), NOCTE_PROJECT_REPORT_FILE, payload));
    REQUIRE_FALSE(payload.empty());

    // at() rather than operator[]: a missing key must fail the test, not read past a const json.
    const nlohmann::json report = nlohmann::json::parse(payload);
    REQUIRE(report.at("schema").get<std::string>() == std::string(NOCTE_PROJECT_REPORT_SCHEMA));
    REQUIRE(report.at("nocte_version").get<std::string>() == std::string(NOCTE_VERSION));
    REQUIRE(report.at("generator").get<std::string>() == std::string(NOCTE_APP_NAME));

    REQUIRE(report.at("objects").is_array());
    REQUIRE(report.at("objects").size() == model.objects.size());
    REQUIRE(report.at("objects").at(0).at("volumes").get<size_t>() == model.objects.front()->volumes.size());
    REQUIRE(report.at("objects").at(0).at("facets").get<size_t>() == model.objects.front()->volumes.front()->mesh().its.indices.size());

    // The per-mesh repair reports are not attached yet, but the key is part of schema 1.
    REQUIRE(report.at("reports").is_array());
    REQUIRE(report.at("reports").empty());
}

TEST_CASE("Re-saving a loaded project writes the NocteSlicer tag exactly once", "[Nocte3mf]")
{
    Model model;
    load_cube_model(model);

    ScopedTemporaryDir backup_dir("nocte_resave_src");
    model.set_backup_path(backup_dir.string());

    ScopedTemporaryFile first(".3mf");
    DynamicPrintConfig  config = DynamicPrintConfig::full_print_config();
    store(model, config, first.string(), SaveStrategy::Zip64 | SaveStrategy::Silence);

    // The loader stores every metadata name it does not recognise into model_info->metadata_items,
    // and the exporter seeds its metadata map from there - so this is the round trip that would
    // duplicate the tag if it were emitted rather than assigned into the map.
    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             dst_plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(first.string().c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                         &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(dst_model.model_info != nullptr);
    REQUIRE(dst_model.model_info->metadata_items.count(NOCTE_3MF_METADATA_TAG) == 1);

    ScopedTemporaryDir reload_backup_dir("nocte_resave_dst");
    dst_model.set_backup_path(reload_backup_dir.string());

    ScopedTemporaryFile second(".3mf");
    store(dst_model, config, second.string(), SaveStrategy::Zip64 | SaveStrategy::Silence);

    std::string model_xml;
    REQUIRE(read_archive_entry(second.string(), "3D/3dmodel.model", model_xml));
    REQUIRE(count_occurrences(model_xml, std::string("name=\"") + NOCTE_3MF_METADATA_TAG + "\"") == 1);
    REQUIRE(count_occurrences(model_xml, "name=\"Application\"") == 1);

    release_PlateData_list(dst_plates);
}

TEST_CASE("A minimal published 3MF carries neither the NocteSlicer tag nor the project report", "[Nocte3mf]")
{
    Model model;
    load_cube_model(model);
    // The tag the exporter would otherwise carry through from the source project: publishing must
    // strip it like the other slicer-identifying tags.
    model.model_info = std::make_shared<ModelInfo>();
    model.model_info->metadata_items[NOCTE_3MF_METADATA_TAG] = NOCTE_VERSION;

    ScopedTemporaryDir backup_dir("nocte_min_pub");
    model.set_backup_path(backup_dir.string());

    ScopedTemporaryFile temp(".3mf");
    DynamicPrintConfig  config = DynamicPrintConfig::full_print_config();
    store(model, config, temp.string(), SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::MinimalPublished);

    std::string model_xml;
    REQUIRE(read_archive_entry(temp.string(), "3D/3dmodel.model", model_xml));
    REQUIRE(count_occurrences(model_xml, std::string("name=\"") + NOCTE_3MF_METADATA_TAG + "\"") == 0);

    std::string payload;
    REQUIRE_FALSE(read_archive_entry(temp.string(), NOCTE_PROJECT_REPORT_FILE, payload));

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             dst_plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(temp.string().c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                         &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(dst_model.model_info != nullptr);
    REQUIRE(dst_model.model_info->metadata_items.count(NOCTE_3MF_METADATA_TAG) == 0);

    release_PlateData_list(dst_plates);
}
