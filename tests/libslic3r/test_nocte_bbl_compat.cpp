// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include <catch2/catch_all.hpp>

#include <map>
#include <string>
#include <vector>

#include "libslic3r/Nocte/BblCompat.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/libslic3r.h" // SLIC3R_VERSION, SoftFever_VERSION (via libslic3r_version.h)

using namespace Slic3r;

namespace {

// A full config whose two Bambu Studio "auto" keys carry the -1 sentinel that Bambu Studio 02.08.02
// writes. raft_first_layer_expansion is coFloat, tree_support_wall_count is coInt.
DynamicPrintConfig bambu_studio_style_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set("raft_first_layer_expansion", -1.0);
    config.set("tree_support_wall_count", -1);
    return config;
}

double orca_default_raft_first_layer_expansion()
{
    return print_config_def.get("raft_first_layer_expansion")->get_default_value<ConfigOptionFloat>()->value;
}

} // namespace

TEST_CASE("The Bambu Studio auto sentinels are what makes a project fail CLI validation", "[NocteBblCompat]")
{
    DynamicPrintConfig config = bambu_studio_style_config();

    const std::map<std::string, std::string> before = config.validate(true);
    // Only our two keys are asserted on; the rest of the full config is not this test's subject.
    CHECK(before.count("raft_first_layer_expansion") == 1);
    CHECK(before.count("tree_support_wall_count") == 1);
}

TEST_CASE("Sanitizing a Bambu Studio config clears both auto sentinels", "[NocteBblCompat]")
{
    DynamicPrintConfig       config = bambu_studio_style_config();
    std::vector<std::string> notes;

    REQUIRE(Nocte::sanitize_bbl_config(config, &notes) == 2);
    CHECK(notes.size() == 2);

    // tree_support_wall_count already spells "auto" as 0 (its tooltip says so).
    CHECK(config.option<ConfigOptionInt>("tree_support_wall_count")->value == 0);
    // raft_first_layer_expansion has no auto, so the sentinel becomes the Orca default.
    CHECK_THAT(config.option<ConfigOptionFloat>("raft_first_layer_expansion")->value,
               Catch::Matchers::WithinAbs(orca_default_raft_first_layer_expansion(), 1e-9));

    const std::map<std::string, std::string> after = config.validate(true);
    CHECK(after.count("raft_first_layer_expansion") == 0);
    CHECK(after.count("tree_support_wall_count") == 0);
}

TEST_CASE("Sanitizing is idempotent", "[NocteBblCompat]")
{
    DynamicPrintConfig config = bambu_studio_style_config();

    REQUIRE(Nocte::sanitize_bbl_config(config) == 2);
    const double raft_after_first = config.option<ConfigOptionFloat>("raft_first_layer_expansion")->value;
    const int    walls_after_first = config.option<ConfigOptionInt>("tree_support_wall_count")->value;

    std::vector<std::string> notes;
    CHECK(Nocte::sanitize_bbl_config(config, &notes) == 0);
    CHECK(notes.empty());
    CHECK_THAT(config.option<ConfigOptionFloat>("raft_first_layer_expansion")->value,
               Catch::Matchers::WithinAbs(raft_after_first, 1e-9));
    CHECK(config.option<ConfigOptionInt>("tree_support_wall_count")->value == walls_after_first);
}

TEST_CASE("Sanitizing leaves legal values untouched", "[NocteBblCompat]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set("raft_first_layer_expansion", 1.5);
    config.set("tree_support_wall_count", 2);

    std::vector<std::string> notes;
    CHECK(Nocte::sanitize_bbl_config(config, &notes) == 0);
    CHECK(notes.empty());
    CHECK_THAT(config.option<ConfigOptionFloat>("raft_first_layer_expansion")->value,
               Catch::Matchers::WithinAbs(1.5, 1e-9));
    CHECK(config.option<ConfigOptionInt>("tree_support_wall_count")->value == 2);
}

TEST_CASE("A Bambu-numbered project at the compatible version is not newer than this build", "[NocteBblCompat]")
{
    const Semver compatible = Nocte::cli_reference_version(/*orca_numbered*/ false);
    REQUIRE(compatible.maj() == Semver::parse(SLIC3R_VERSION)->maj());
    REQUIRE(compatible.min() == Semver::parse(SLIC3R_VERSION)->min());

    // Same major.minor, a later patch: not newer, because only major and minor are compared.
    const Semver same_minor(compatible.maj(), compatible.min(), compatible.patch() + 10);
    CHECK_FALSE(Nocte::file_version_is_newer(same_minor, /*file_is_orca_numbered*/ false));
}

TEST_CASE("A Bambu-numbered project one minor ahead is newer than this build", "[NocteBblCompat]")
{
    const Semver compatible = Nocte::cli_reference_version(/*orca_numbered*/ false);
    const Semver next_minor(compatible.maj(), compatible.min() + 1, 0);

    CHECK(Nocte::file_version_is_newer(next_minor, /*file_is_orca_numbered*/ false));
}

TEST_CASE("An Orca-numbered project at this build's own version is not newer", "[NocteBblCompat]")
{
    const Semver orca = Nocte::cli_reference_version(/*orca_numbered*/ true);
    REQUIRE(orca.maj() == Semver::parse(SoftFever_VERSION)->maj());
    REQUIRE(orca.min() == Semver::parse(SoftFever_VERSION)->min());

    CHECK_FALSE(Nocte::file_version_is_newer(orca, /*file_is_orca_numbered*/ true));
}

TEST_CASE("An Orca-numbered project one minor ahead is newer", "[NocteBblCompat]")
{
    const Semver orca = Nocte::cli_reference_version(/*orca_numbered*/ true);
    const Semver next_minor(orca.maj(), orca.min() + 1, 0);

    CHECK(Nocte::file_version_is_newer(next_minor, /*file_is_orca_numbered*/ true));
}

TEST_CASE("The Bambu-compatible version is at least as permissive as the Orca version", "[NocteBblCompat]")
{
    // The whole point of the -24 fix: a file the Bambu-numbered gate accepts must never be rejected
    // just because SoftFever_VERSION is behind SLIC3R_VERSION.
    const Semver orca = Nocte::cli_reference_version(/*orca_numbered*/ true);
    const Semver bbl  = Nocte::cli_reference_version(/*orca_numbered*/ false);

    CHECK(bbl.maj() >= orca.maj());
}
