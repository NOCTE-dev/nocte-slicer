# NØCTE planning engine — the physics

The orientation engine ranks candidate orientations of a part, and the profile calculator derives
process settings from the same measurements. This document is the derivation behind both. It is the
reference an implementer checks a formula against, and the reason a number shown to the user can be
defended.

The governing rule: **every term is an estimate of a quantity that can be measured on a real print,
and it is reported in that unit** — cubic millimetres of support, millimetres of stair-step,
seconds, newtons. Only the final combination is dimensionless, and there the weights are preferences
the user can see and edit.

This is the opposite of `src/libslic3r/Orient.cpp`, which we deliberately do not reuse: it carries 28
undocumented tuned constants (`Orient.hpp:51-97`), and its objective function divides by
`area_projected`, a field that is never assigned (`Orient.cpp:479,483`). Its answers cannot be
explained, and explaining them is what we sell.

## Conventions

`its` is an `indexed_triangle_set` in object coordinates, Z up, millimetres. A candidate is a
rotation `R`; `ẑ` is the build direction. Facet `i` has area `a_i` and unit outward normal `n_i`,
and under the candidate `n'_i = R n_i`.

The **overhang angle** is measured from the vertical, matching `GeometryAnalysis.cpp:45-59`:

    φ_i = asin(|n'_i · ẑ|)        0° = vertical wall,  90° = horizontal ceiling or floor

Orca's `support_threshold_angle` uses the complementary slope convention, so
`support_threshold_angle = 90° − φ`. Every formula below uses `φ`.

## Measured inputs

Resolved by following the inheritance chain of the shipped vendor profiles in
`resources/profiles/BBL` for the A1 with a 0.4 nozzle and Bambu PLA Basic. These are the values the
machine actually runs, not figures from a marketing page.

| Symbol | Key | Value |
|---|---|---|
| `Q_max` | `filament_max_volumetric_speed` | 21 mm³/s |
| `v_machine` | `machine_max_speed_x/y` | 500 mm/s (200 in silent mode) |
| `a_machine` | `machine_max_acceleration_x/y` | 12 000 mm/s² |
| `t_min` | `slow_down_layer_time` | 6 s |
| `ρ` | `filament_density` | 1.26 g/cm³ |

## 1. Surface roughness — cusp height

A planar face inclined `θ` from the bed is approximated by stair treads of horizontal run
`h/tan θ`. The perpendicular deviation from the ideal plane is `(h/tan θ)·sin θ = h·cos θ`, and
since `|n·ẑ| = cos θ` for a face at `θ` from the bed,

    δ_i = h · |n'_i · ẑ| = h · sin φ_i        [mm]

The literature writes this as `h·cos α` with `α` the angle between the normal and the build
direction. Since `α = 90° − φ`, it is the same formula. It is stated here once so nobody re-derives
it with the wrong convention.

**Near-horizontal faces are excluded.** As `φ → 90°` the formula returns `δ → h`, but a flat top has
no staircase at all — there is exactly one tread. Facets within 5° of horizontal are therefore
dropped from the roughness measure and routed to the top-surface constraint instead, where quality
is governed by `top_shell_layers` and ironing. Without this exclusion the engine would rank a
perfect flat top as the worst possible surface.

We report the area-weighted mean and the area-weighted 95th percentile over the **visible** facets.
Visibility today is a **self-occlusion ray only**: a facet is visible when a ray leaving its centroid
along its own normal escapes the mesh. Reading `eExteriorAppearance` per facet (`FaceDetector`, which
persists in the 3mf) is the cheaper source and is **not yet wired**; it is an optimisation, not a
correctness gap.

One consequence is a failure mode rather than an inaccuracy, so it belongs here: on an **inverted
mesh** every ray starts inside the surface and reports the whole part invisible, so no facet survives
the filter and every cusp statistic comes out zero — the *best* value on that term. That is why the
result carries `cusp_measured`, and why a zero without the flag must never be read as a perfect
surface. An ordinary broken STL reaches this.

For a target cusp `δ*`, the admissible layer height is `h ≤ δ* / δ_p95` over the visible sloped faces,
using the **95th percentile** rather than the maximum. The maximum is set by whichever single sliver
happens to be steepest, and letting one facet dictate the layer height of the whole part trades a
great deal of print time for a defect nobody sees. The p95 is the worst area a user actually notices.

A note on the mean, because it is easy to over-read. `δ̄` is a **conditional** mean: the excluded flat
facets leave the denominator as well as the numerator, so it answers "how rough is the rough part",
not "how much roughness is there". Two orientations can tie on `δ̄` while one has a ninth of the
staircase of the other. The extensive form `Σ a_i·δ_i` (mm³ of surface error) separates them. Both are
computed and reported; which one belongs in the objective function is an open question, and the batch
harness against real prints is what settles it.

It also explains, without any tuning, why a sign prints flat: the face that matters ends up
horizontal, its cusp is zero, and quality passes entirely to the top shell.

## 2. Support volume

Support is a **column** under an overhang, running from the overhang down to whatever it lands on.
Its volume is therefore an area times a height, and the only way to get that out of slices is to
carry the support region downwards:

    R = {}                          R is the support region, carried from the layer above
    for k = top … 0:                top-down
        R ← R ∪ B_{k+1}             an overhang above needs a column through this layer
        R ← R \ S_k                 the part itself is in the way here
        V_s += ρ_s · area(R) · h

`S_k` is the solid slice at layer `k` and `B_k` the downward-facing slab projection at layer `k` —
the material with nothing directly beneath it. The `\ S_k` step is what makes the estimate aware of
the part supporting itself, and it is also what stops the column where the part is in the way, which
is the correct behaviour when support is allowed to rest on the model.

The sweep must run **top-down**. Bottom-up cannot work: at layer `k` you do not yet know which
overhangs above will need a column through it.

`ρ_s` is the support density from the process config — real support is printed sparse, and without
that factor the solid column over-predicts by 3 to 8 times. For tree supports `ρ_s` has no meaning
and tier 2 is required.

**The error this paragraph exists to prevent**, because the first draft of this document contained
it: summing `area(B_k \ ⋃_{j<k} S_j)·h` bottom-up sums *interface* areas one layer thick. A flat
overhang of area `A` at height `H` then scores `ρ_s·A·h` instead of `ρ_s·A·H`. That is not a constant
factor — it is wrong by the height of the part, and the height is exactly what the orientation
changes, so it can invert the ranking on the one term every other term is ranked against.

**The overhang threshold applies at every tier.** `slice_mesh_slabs` returns *every* downward-facing
facet, not only those past `overhang_threshold_deg`, so tier 1 must slice a sub-mesh filtered to the
steep facets — otherwise the threshold silently does nothing on the default path and every draft
face, chamfer and fillet is charged a full support column.

The case that shows it is a **sphere resting on the bed**. Every facet of the lower hemisphere faces
downward, and because the sphere's cross-section shrinks going down, the annuli carried from above
always fall outside `S_k` and are never subtracted. An unfiltered carry therefore charges a support
ring from the equator to the bed. With `|n·ẑ| = cos t` at polar angle `t` from the bottom pole, the
overhang angle is `90° − t`, so a 45° threshold supports only the cap below `z = R(1 − cos 45°)`:

    filtered   ∫₀^0.293R π(0.5R² − 2Rz + z²) dz = 0.217 R³
    unfiltered ∫₀^R     π(R − z)²          dz = 1.047 R³

A factor of 4.8. A flat ceiling cannot expose this — it is at 90° and passes any threshold — which is
why the sphere is the fixture the test suite uses.

With the filter in place, tiers 0 and 1 apply the same threshold and differ in exactly one thing:
whether they see the part supporting itself. That is what the tier field claims, and now it is true.

Restricting support to columns that actually reach the bed is a **different algorithm**, not a flag:
it requires tracking each connected component of `R` down the stack and discarding those that land
on the model. It belongs in tier 2, where a real `PrintObject` already knows the answer. There is
deliberately no such flag at tier 1, because a flag that provably does nothing is the defect this
engine exists to avoid.

Three tiers, cheapest first:

- **Tier 0**, facet sweep, milliseconds. `V_s ≈ ρ_s · Σ a_i·sin φ_i · z̄_i` over downward facets past
  the threshold. This is already an area-times-height column volume, so tiers 0 and 1 measure the
  same physical quantity and differ only in whether they see the part supporting itself. Blind to
  self-support. **Never reported in grams.**
- **Tier 1**, the column carry above, seconds, the default. `slice_mesh_slabs()` plus Clipper.
- **Tier 2**, the truth, only on the two or three finalists. A `PrintObject` at `posSlice` driven
  through `TreeSupport::detect_overhangs(true)` exactly as `PrintObject::is_support_necessary()`
  does (`PrintObject.cpp:4686-4700`), which additionally classifies sharp tails and cantilevers.

Assumptions: constant layer height, and no credit for bridging — tiers 0 and 1 over-count a long
bridge. A hollow part with internal overhangs receives support that cannot be removed; that is
flagged, not scored.

**Support contact on visible surface**, `A_c` [mm²], comes from the same tier-1 computation
intersected with the visible facet set. For signage it is a hard constraint (`A_c = 0` on the
showcase face), not a weighted term.

## 3. Print time

Three bounds are binding, and the smallest wins.

**Flow.** With layer height `h`, line width `w` and speed `v`, the volumetric rate is `Q = h·w·v`:

    v ≤ Q_max / (h·w)

At `h = 0.2` with `w = 0.42` this is `21/(0.2·0.42)` = **250 mm/s**. Writing 400 into a profile does
not print faster; it misreports. At `h = 0.28` with the `w = 0.45` that Orca pairs with that height,
`21/(0.28·0.45)` = **167 mm/s**. The line width must be carried through with the layer height — at a
fixed `w = 0.42` the same 0.28 layer would give 179 mm/s, and quoting one number without its width
is how this kind of table goes quietly wrong.

**Acceleration and feature size.** A move of length `L` from rest cannot exceed

    v_eff ≤ √(2·a·L)

At `a = 12 000 mm/s²`: 490 mm/s over 10 mm, 219 mm/s over 2 mm, 110 mm/s over 0.5 mm. On small
parts acceleration governs, not the speed setting.

**Cooling.** With extruded volume per layer `V_layer`, requiring `t_layer ≥ t_min` gives

    v ≤ V_layer / (h·w·t_min)

This closes the gap block 2 left open: rule R004 detects small cross-sections but can only leave a
note, because the remedy is a filament-scope setting. A NØCTE process preset can set it, and this is
the formula that says to what.

**Total.** `T ≈ V_e/Q_eff + N·t_layer` with `N = height(R)/h`. The part's own extruded volume does
not change with orientation, so what actually ranks candidates is `N·t_layer` plus the support.
`t_layer` is the **one calibrated constant in the model**: measured as `Δt/ΔN` between two already
sliced orientations of the same part, it is a property of the machine and belongs in the printer
profile.

**On acceleration, deliberately narrow.** We do not derive a global acceleration from an invented
corner-deviation tolerance. Ringing amplitude depends on the machine's measured resonance, and the
A1 already runs input shaping — a value computed here would fight it. What is defensible is lowering
`top_surface_acceleration` alone for the visual intent. Any claim about global acceleration requires
measuring `f_res` with a ringing tower first.

## 4. Strength

FDM parts fail at the interface between layers. The load-bearing area is therefore the minimum
section **on planes normal to the load direction** `d̂` — not the minimum horizontal section, which
is what `PartFeatures::min_cross_section_area` measures and which is correct only when the load runs
along `ẑ`. In the part frame that minimum is orientation-invariant, so it costs one slice sweep per
(part, load direction) for the whole plan rather than one per candidate.

Layer-interface anisotropy, transversely isotropic about `ẑ`:

    σ_allow(d̂) = σ_z + (σ_xy − σ_z)·(1 − (d̂·ẑ)²)        [MPa]
    F_fail     = A_min(d̂) · σ_allow(d̂)                   [N]      → maximise

`k = σ_z/σ_xy` is a **measured material property**. The literature gives 0.4–0.75 for PLA and less
for PETG. It ships per filament with a source column and a "measured on our A1" column; the default
is 0.5 and the panel says so.

**Caveat, and it is printed in the panel:** this is a net-section tensile proxy. In bending the
governing quantity is the section modulus `I/c`, not area, and for a cantilevered sign bracket the
two disagree. It is not finite-element analysis and is never called that.

## 5. Stability and adhesion

A lateral force — nozzle drag — tips the part when its moment exceeds the restoring moment of the
weight over the footprint. The part rotates about the convex-hull edge of the footprint nearest the
ground projection of the centre of mass. With `d_min` that distance and `z_com` the centre-of-mass
height,

    S = d_min / z_com        (dimensionless)

The part tips when lateral acceleration exceeds `S·g`. We require `S ≥ 0.35`, which tolerates about
3.4 m/s² at the toolhead — optimistically, because nozzle drag acts at the layer being printed, not
at the centre of mass. The true moment arm is understated by up to `z_top/z_com`, a factor of two for
a uniform prism. The margin is a ranking quantity and a coarse gate, not a guarantee.

**The footprint is bed contact, from the first-layer slice — not the part's silhouette.** For anything
wider above than at its base the two are different objects, and the silhouette flatters both
constraints that read it: a 20 × 20 mm cap on a 4 × 4 mm stem has a 400 mm² shadow and a 16 mm²
footprint, which is the difference between `S = 0.88` (passes) and `S = 0.18` (fails). A sphere
resting on the bed has almost none — about 1.8 mm² on a coarse tessellation, which is what its first
layer actually prints, against the 314 mm² its silhouette claims.

`GeometryAnalysis::footprint_area` measures the same *quantity* by a different construction — it sums
downward facets whose vertices all sit within a tolerance of the lowest Z. The two agree on a flat
bottom and disagree on a curved or coarsely tessellated one, where that construction returns zero.
Same intent, not the same definition; neither is a drop-in for the other.

Tipping measures against the convex hull of that contact — the support polygon — while adhesion reads
the raw contact area, because a ring's hull would overstate its adhesion badly. Support material
enlarges the real support polygon and is deliberately not modelled: the number is the part's own
footing.

Block 2's `stability_ratio = √(A_footprint)/z_com` is isotropic and is wrong for an L-shaped or
elongated footprint: it averages a safe direction with an unsafe one. `S` replaces it.

Adhesion is a **constraint, not a term**: `A_footprint ≥ A_min`, and the remedy when it fails is
`brim_type`/`brim_width`.

## 6. Combination

The terms carry different units and are not summed raw. Each is min–max normalised against the best
candidate, with a **physical indifference floor**:

    ŝ_k = (s_k − s_min) / max(s_max − s_min, ε_k) ∈ [0,1]
    Score = Σ_k w_k · ŝ_k ,   Σ w_k = 1,  lower is better

`ε_k` is not a numerical epsilon. It is the difference below which we do not care: 500 mm³ of
support, 0.02 mm of cusp, 60 s, 5 % of the best force, 0.1 of stability. It is what stops the engine
presenting a 0.3 % support difference as a decision. A z-score would be worse here — the candidate
set is small, arbitrary and often bimodal.

The floor is applied as a statement of equality, not only as a denominator: when
`s_max − s_min < ε_k`, every candidate's `ŝ_k` is 0. Dividing by `max(range, ε_k)` alone would only
shrink a sub-floor ordering — 1000 against 1200 mm³ of support would still come out 0 and 0.4 and
still decide — which is the opposite of "we do not care". A term value that is not a finite number is
not a measurement and takes `ŝ_k = 1`, the worst value, never the best.

Hard constraints are applied as a **filter before normalisation**, never as a large penalty: a
penalty distorts the min–max range of every other term.

Degenerate cases: if the filter leaves **zero** candidates, the current orientation is returned
**with the name of the constraint that emptied the set**, and flagged as degenerate.

One surviving candidate is not a degenerate case and must not be treated as one. A plate imported
face-down with a designated showcase face leaves exactly one survivor — the flip — and discarding it
in favour of the current orientation would hand back the orientation that shows the wrong face. An
earlier draft of this section said "zero or one" and was wrong.

Two responsibilities belong to the **caller**, not to the combination step, and are named here
because attributing them to the wrong layer is how they end up implemented nowhere:

- **Tie-breaking**, in a deterministic chain — less support, then less height, then less rotation
  from the current orientation, then lower index. The combination returns raw scores and applies no
  ordering. Each key is rounded to a whole number of a quantum before it is compared (score 1e-9,
  support `ε_support`, height 1 µm, rotation 1e-4 rad), because a chain compared on raw doubles is
  decided by round-off before it reaches the rule it states, and a tolerance compare is not a valid
  sort order.
- **Dropping candidates whose measurement flags are false.** A failed measurement leaves its term at
  zero, which is the best value on every term the combination inverts, so an unmeasured candidate
  would otherwise outrank every candidate measured honestly. The combination cannot filter this
  itself without making its output no longer line up with its input.

Every number shown to the user stays in its physical unit. The normalised score appears only as a
rank.

## 7. Intent

The user declares what the part is for, and that selects the weights and the hard constraints.
Weights are preferences, editable and visible; the physics lives in the terms.

| Intent | support | cusp | time | strength | stability | Hard constraints |
|---|---|---|---|---|---|---|
| Ornament | 0.30 | 0.45 | 0.15 | — | 0.10 | `S ≥ 0.35` |
| Functional, strength | 0.20 | 0.05 | 0.10 | 0.50 | 0.15 | `S ≥ 0.35`, `A_min(d̂) > 0` |
| Functional, visual (signage) | 0.25 | 0.40 | 0.10 | 0.10 | 0.15 | showcase normal within 5° of `+ẑ`; `A_c = 0` on it; `S ≥ 0.35` |
| Draft | 0.30 | — | 0.60 | — | 0.10 | `S ≥ 0.35` |
| Unspecified | 0.25 | 0.25 | 0.20 | 0.15 | 0.15 | `S ≥ 0.35` |

## 8. What is not yet calibrated

Stated plainly, because the value of this engine is that its claims are checkable:

- **The support density factor `ρ_s`** against real support grams. Until the batch harness regression
  runs, tier 0 and tier 1 volumes rank candidates but are not quoted as mass.
- **`t_layer`**, the per-layer overhead. Until measured on the machine, the time term ranks but does
  not predict.
- **`k = σ_z/σ_xy`**, from literature rather than from our own tensile comparison.
- **The ironing flow model** `ironing_flow% ≈ 100·(1−π/4)·(s/w)`, which is a gap-volume argument and
  is decade-correct but unproven.
- **The target cusp `δ*` that makes signage legible**, which is a judgement about our own product and
  only a printed set can settle.

## 9. What is derived here but not yet implemented

Separate from §8, which is about numbers we cannot yet defend. These are numbers we do not yet
compute at all, listed so that nobody reads this document as a description of the code:

- **`eExteriorAppearance` as a visibility source.** Only the self-occlusion ray exists. An
  optimisation, not a correctness gap.
- **The width-carrying time bounds**, `v ≤ Q_max/(h·w)` and `v ≤ V_layer/(h·w·t_min)`. Neither the
  machine speed nor the minimum layer time reaches the scoring entry point, so the time term is the
  flow ceiling alone. They land with the profile calculator, where both are in scope.
- **The per-face signage constraint.** `support_contact_mm2` is a whole-part total over every visible
  overhang. "No support on *the* showcase face" needs the face selection the panel carries, and until
  then the scalar is a necessary condition and nothing more.
- **Tier 2 support** (`TreeSupport::detect_overhangs`), and with it build-plate-only support.
- **The adhesion constraint `A_footprint ≥ A_min`.** §5 states it as live; it is not. `PlanConstraints`
  carries `min_footprint_mm2` and no intent ever sets it, so nothing is rejected for adhesion. The
  reason it is not merely cosmetic: on a cap-on-a-stem part the engine accepts candidates balanced on
  two 0.3 mm slivers totalling about 7 mm² of bed contact. They lose on score today, but nothing would
  stop them winning on a part where they scored better. What blocks the fix is that `A_min` is a
  calibration question — what actually sticks — and guessing it would put a fabricated number in a
  hard constraint, which is the one place this engine must not have one.
- **The cusp term is whole-part, not per-face.** §7's weight for the signage intent governs the
  roughness of the entire visible surface; there is no face selection in `measure_cusp`. The showcase
  face drives the hard constraints, not the cusp weight.

Everything else in this document is provable in CI.
