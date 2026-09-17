// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/GeometryAnalysis.hpp"

namespace Slic3r {
namespace Nocte {

// TODO(M1): measure the features. See the header for which libslic3r facility each field comes from.
PartFeatures analyze(const indexed_triangle_set & /* its */, const AnalysisParams & /* params */, const DynamicPrintConfig & /* ctx */)
{
    return PartFeatures();
}

bool analysis_available()
{
    return false;
}

} // namespace Nocte
} // namespace Slic3r
