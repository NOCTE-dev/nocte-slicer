// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/BblCompat.hpp"

#include <cmath>

#include <boost/optional.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/libslic3r.h" // SLIC3R_VERSION, SoftFever_VERSION (via libslic3r_version.h)

namespace Slic3r {
namespace Nocte {

namespace {

// One Bambu Studio "auto" sentinel and the Orca value that means the same thing.
struct BblAutoMapping
{
    const char *key;
    double      bbl_auto_value;   // what Bambu Studio writes to mean "auto"
    bool        use_orca_default; // replace with the option's default from print_config_def
    double      orca_equivalent;  // used when use_orca_default is false
};

// Both options are scalar and non-nullable at this upstream revision:
//   * PrintConfig.cpp ~5694: add("raft_first_layer_expansion", coFloat), min = 0, default 2.0.
//     Orca has no "auto" for it, so the sentinel maps onto the Orca default.
//   * PrintConfig.cpp ~7249: add("tree_support_wall_count", coInt), min = 0, max = 2, default 0.
//     Its own tooltip says "0 means auto", so -1 maps onto 0.
const BblAutoMapping s_bbl_auto_mappings[] = {
    {"raft_first_layer_expansion", -1.0, true, 0.0},
    {"tree_support_wall_count", -1.0, false, 0.0},
};

bool is_sentinel(double value, double sentinel) { return std::fabs(value - sentinel) < 1e-9; }

} // namespace

Semver cli_reference_version(bool orca_numbered)
{
    // Semver::parse() understands the Bambu AA.BB.CC.DD form: semver_parse_version() folds the
    // fourth field into the patch as patch * 100 + build, so "02.08.01.55" becomes 2.8.155.
    // Slic3r::SEMVER (Semver.cpp:5) is built from SLIC3R_VERSION the same way.
    const boost::optional<Semver> parsed = Semver::parse(orca_numbered ? SoftFever_VERSION : SLIC3R_VERSION);
    if (parsed)
        return *parsed;
    // Both are compile-time constants that do parse; this only guards a malformed build, and it
    // errs towards accepting the file rather than rejecting every project.
    return Semver(9999, 9999, 0);
}

bool file_version_is_newer(const Semver &file_version, bool file_is_orca_numbered)
{
    const Semver cli_ver = cli_reference_version(file_is_orca_numbered);
    // Major and minor only, exactly as upstream src/OrcaSlicer.cpp does.
    return (cli_ver.maj() < file_version.maj()) ||
           ((cli_ver.maj() == file_version.maj()) && (cli_ver.min() < file_version.min()));
}

std::size_t sanitize_bbl_config(DynamicPrintConfig &config, std::vector<std::string> *notes)
{
    std::size_t changed = 0;

    for (const BblAutoMapping &mapping : s_bbl_auto_mappings) {
        ConfigOption *opt = config.option(mapping.key, false);
        if (opt == nullptr)
            continue;
        const ConfigOptionDef *def = print_config_def.get(mapping.key);
        if (def == nullptr)
            continue;

        const std::string before   = opt->serialize();
        bool              replaced = false;

        if (opt->type() == coFloat) {
            ConfigOptionFloat *fopt = static_cast<ConfigOptionFloat *>(opt);
            if (is_sentinel(fopt->value, mapping.bbl_auto_value)) {
                double replacement = mapping.orca_equivalent;
                if (mapping.use_orca_default && def->default_value && def->default_value->type() == coFloat)
                    replacement = def->get_default_value<ConfigOptionFloat>()->value;
                fopt->value = replacement;
                replaced    = true;
            }
        } else if (opt->type() == coInt) {
            ConfigOptionInt *iopt = static_cast<ConfigOptionInt *>(opt);
            if (is_sentinel(double(iopt->value), mapping.bbl_auto_value)) {
                int replacement = int(mapping.orca_equivalent);
                if (mapping.use_orca_default && def->default_value && def->default_value->type() == coInt)
                    replacement = def->get_default_value<ConfigOptionInt>()->value;
                iopt->value = replacement;
                replaced    = true;
            }
        }

        if (!replaced)
            continue;

        ++changed;
        if (notes != nullptr)
            notes->push_back(std::string(mapping.key) + ": " + before + " (Bambu Studio \"auto\") -> " +
                             opt->serialize());
    }

    return changed;
}

} // namespace Nocte
} // namespace Slic3r
