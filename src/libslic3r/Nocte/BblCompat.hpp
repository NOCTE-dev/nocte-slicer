// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// Bambu Studio project compatibility for the headless CLI.
//
// A .3mf written by Bambu Studio is a valid OrcaSlicer project, but two of its conventions make the
// strict CLI path reject it (see tools/bbl-compat/NOTES.md §6):
//
//   * the file version is read from the `Application` metadata (`BambuStudio-02.08.02.61`) and is in
//     *Bambu* numbering (2.8.x), while the CLI compares it against `SoftFever_VERSION`, which is in
//     *Orca* numbering (2.5.x). Every Bambu Studio >= 2.6 project therefore looks "newer" and the CLI
//     exits with -24 CLI_FILE_VERSION_NOT_SUPPORTED;
//   * Bambu Studio 02.08.02 writes `-1` for `raft_first_layer_expansion` and `tree_support_wall_count`
//     to mean "auto". Orca defines both with `min = 0`, so `DynamicPrintConfig::validate(true)` flags
//     them and the CLI exits with -18 CLI_INVALID_VALUES_IN_3MF.
//
// Both fixes live here rather than in the upstream files so that an upstream sync stays a merge.
// Engine code: no dependency on src/slic3r/ or wx.

#ifndef slic3r_Nocte_BblCompat_hpp_
#define slic3r_Nocte_BblCompat_hpp_

#include <cstddef>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"

namespace Slic3r {
namespace Nocte {

// The version the CLI compares a project against. `orca_numbered` selects `SoftFever_VERSION`
// (Orca numbering, e.g. 2.5.0-dev); otherwise `SLIC3R_VERSION` (the Bambu-compatible version,
// e.g. 02.08.01.55, which Semver parses as 2.8.155 — see Semver::to_string()'s AA.BB.CC.DD form).
Semver cli_reference_version(bool orca_numbered);

// True when `file_version` is ahead of this build, comparing major and minor only, exactly as
// upstream src/OrcaSlicer.cpp does. `file_is_orca_numbered` says which numbering `file_version`
// uses: a 3mf carrying the `OrcaSlicer` metadata tag is Orca-numbered, a 3mf whose version comes
// from a `BambuStudio-`/`OrcaSlicer-` `Application` string is Bambu-numbered.
bool file_version_is_newer(const Semver &file_version, bool file_is_orca_numbered);

// Replaces the Bambu Studio "auto" sentinels with the values Orca uses for the same meaning, so a
// Bambu Studio project survives DynamicPrintConfig::validate(true). Only keys present in `config`
// and holding exactly the sentinel are touched, which makes the call idempotent and leaves legal
// values alone. Returns the number of values changed; `notes`, when given, receives one
// human-readable line per change.
std::size_t sanitize_bbl_config(DynamicPrintConfig &config, std::vector<std::string> *notes = nullptr);

} // namespace Nocte
} // namespace Slic3r

#endif // slic3r_Nocte_BblCompat_hpp_
