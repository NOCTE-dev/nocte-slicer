// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The one place that answers "is this an offline build?". ADR-003 makes NØCTE Slicer offline by
// default: no cloud agent, no account, no update check, no preset synchronisation, no network
// plugin. The marked hunks in the upstream touch points (GUI_App.cpp, MainFrame.cpp, Plater.cpp)
// call into here instead of repeating the policy, so each of those hunks stays a one-liner and the
// next upstream sync has one decision to re-apply rather than a dozen.
//
// is_offline_build() is constexpr so a hunk written as `if (Nocte::is_offline_build()) return;`
// costs nothing at runtime and the compiler still type-checks the code it guards.

#ifndef slic3r_GUI_Nocte_OfflinePolicy_hpp_
#define slic3r_GUI_Nocte_OfflinePolicy_hpp_

#include <string>

namespace Slic3r {
namespace GUI {
namespace Nocte {

// True for every NØCTE build. There is no "online build" of NØCTE Slicer; the function exists so
// the upstream hunks read as policy rather than as unexplained early returns, and so a future
// build flavour has a single place to change.
constexpr bool is_offline_build() { return true; }

// A short, translatable-at-the-call-site reason to show the user where an entry point had to be
// declined rather than simply hidden (for example a project file that asks to open a cloud page).
// Returns a stable English source string; callers wrap it in _L() when it reaches the UI.
// Implemented in OfflinePolicy.cpp.
const char* offline_reason();

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Nocte_OfflinePolicy_hpp_
