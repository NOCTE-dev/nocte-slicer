// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "MigrationDialog.hpp"

#include <string>

#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#ifdef _WIN32
// Already pulled in by slic3r/pchheader.hpp (FORCEINCLUDE) with NOMINMAX and
// WIN32_LEAN_AND_MEAN, and by slic3r/win_platform.hpp when the PCH is off; the include guard
// makes this a no-op in both cases and keeps the file readable on its own.
#include <Windows.h>
#endif // _WIN32

namespace Slic3r {
namespace GUI {
namespace Nocte {

MigrationChoice ask_import_data_dir(const boost::filesystem::path &old_dir)
{
#ifdef _WIN32
    // Not translatable: wxTranslations is not loaded either, for the same reason the dialog is
    // not a wxDialog. English, and short.
    const std::wstring caption = L"NOCTE Slicer";
    const std::wstring text =
        L"An OrcaSlicer configuration was found at\n\n    " +
        boost::nowide::widen(old_dir.string()) +
        L"\n\n"
        L"Import your presets, filaments and custom shapes into NOCTE Slicer?\n\n"
        L"The OrcaSlicer folder is only read: nothing in it is moved, renamed or deleted, and "
        L"OrcaSlicer keeps working exactly as before.\n\n"
        L"Yes - import from OrcaSlicer\n"
        L"No  - start fresh\n\n"
        L"This is asked only once.";

    const int answer = ::MessageBoxW(nullptr, text.c_str(), caption.c_str(),
                                     MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 |
                                     MB_SETFOREGROUND | MB_TOPMOST);
    if (answer == IDYES)
        return MigrationChoice::Import;
    if (answer == IDNO)
        return MigrationChoice::StartFresh;

    // 0 means the message box could not be created (no window station, out of resources).
    BOOST_LOG_TRIVIAL(warning) << "nocte: could not show the data-directory import prompt, "
                                  "error " << ::GetLastError();
    return MigrationChoice::Unavailable;
#else  // _WIN32
    // NOCTE-TODO: a pre-toolkit modal for Linux and macOS - zenity/kdialog when one is on PATH,
    // CFUserNotificationDisplayAlert on macOS - or move the whole import behind a lazily loaded
    // AppConfig so it can run from OnInit() with a real wxDialog.
    (void) old_dir;
    return MigrationChoice::Unavailable;
#endif // _WIN32
}

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r
