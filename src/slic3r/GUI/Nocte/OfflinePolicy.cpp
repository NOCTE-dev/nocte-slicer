// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "OfflinePolicy.hpp"

namespace Slic3r {
namespace GUI {
namespace Nocte {

const char* offline_reason()
{
    // English source string. The caller wraps it in _L() so it reaches the catalog from the
    // upstream file that shows it, not from here.
    return "NOCTE Slicer works offline. This feature needs an online account and is not available.";
}

} // namespace Nocte
} // namespace GUI
} // namespace Slic3r
