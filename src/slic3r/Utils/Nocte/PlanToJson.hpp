#pragma once

// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
//
// The --nocte-plan output. Sibling of MeshInspect.hpp, and written the same way: one function, one
// ostream, one JSON document, no state.
//
// This file is how the orientation engine is VALIDATED. There is no panel yet, so the only way to
// see whether a plan is right is to read the numbers it produced, on a real part, and compare them
// with the print. The JSON is therefore the product and not a debug dump, and it carries two rules
// that a printf could not:
//
//  1. A number whose measurement flag is false is written as JSON `null`, never as 0. Zero is the
//     BEST value on every term the combination inverts (Scores.hpp, `combine()`), so writing an
//     unmeasured quantity as 0 lets a reader — or a harness — mistake a failed measurement for a
//     perfect result. Every emitted quantity is paired with the flag that governs it.
//  2. The support volume is never presented as a mass. It is uncalibrated
//     (docs/HLSD/nocte-planning-engine.md §8: "tier 0 and tier 1 volumes rank candidates but are
//     not quoted as mass"), so mm^3 is the only unit it appears in, and the tier that produced it
//     is emitted beside it.
//
// Every quantity names its unit in its key — `support_mm3`, `cusp_p95_mm`, `time_s` — so that a
// value can never be read in the wrong one.

#include <iosfwd>
#include <string>
#include <vector>

#include "libslic3r/Nocte/Plan/OrientEngine.hpp"
#include "libslic3r/Nocte/Plan/PlanIntent.hpp"

namespace Slic3r {

class Model;

namespace Nocte {

// Writes the --nocte-plan JSON for `model` to `out`: for every object, the candidate orientations
// the engine ranked under `intent`, each with its rotation, its five physical score terms, the
// measurement flags that say which of those numbers are real, and its place in the ranking; then
// the rejected candidates with the constraint that removed each one.
//
// `source_paths` lists every input file, as MeshInspect's does — the CLI merges them into one model
// — and each object additionally reports its own `input_file` when it has one.
//
// Never throws on user geometry: an object the engine cannot plan is emitted with `ok: false` and
// the run continues to the next one, so one bad mesh in a batch does not cost the whole report.
void plan_to_json(const Model &model, const std::vector<std::string> &source_paths,
                  const PlanIntent &intent, const OrientParams &params, std::ostream &out);

} // namespace Nocte
} // namespace Slic3r
