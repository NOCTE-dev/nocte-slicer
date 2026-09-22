# ADR-004 — Intent-driven planning engine, and one new upstream touch point for its CLI action

- **Status:** Accepted
- **Date:** 2026-09-22
- **Deciders:** NØCTE Engineering
- **Related:** ADR-001 (fork of OrcaSlicer, upstream touch points), ADR-002 (LAN Developer Mode), ADR-003 (own product)

## Context

After block 3 the product is ours from the outside — no cloud, own identity, own name, repair and
auto-tune panels — but the functional advantage over Bambu Studio is still narrow: explainable mesh
repair, five auto-tune rules, and no network. Everything else is OrcaSlicer, which already matches or
beats Bambu Studio.

Block 4 is the block that creates the advantage. The user declares what a part is **for** — ornament,
functional by strength, functional by appearance (our actual business is signage), or a draft — and
the engine plans the print from that: it ranks orientations, derives a process profile, and offers
modifiers defined relative to the part. Every number it shows is an estimate of something measurable
on a real print, in that quantity's own unit, with the derivation written down.

The derivation is `docs/HLSD/nocte-planning-engine.md`. It is a normative document: an implementer
checks a formula against it, and a number that cannot be traced to it does not get shown to a user.

Three findings from the design work shape the decisions below, and each was verified in the tree:

1. A custom **per-object config key breaks the 3mf load outright**. The importer loop at
   `Format/bbs_3mf.cpp:2154-2162` calls `set_deserialize()` with no `try`/`catch`, and
   `ConfigBase::set_deserialize_raw` throws `UnknownOptionException` for an unknown key
   (`Config.cpp:607-630`). The whole project fails to open — in our build and in Bambu Studio, which
   shares the code.
2. `TuneResult` has **no channel for print-scope keys**. `initial_layer_print_height`
   (`PrintConfig.hpp:1808`) and `slow_down_layer_time` (`:1843`) are `PrintConfig`, not
   `PrintObjectConfig`, so they cannot be written as per-object overrides at all. This is why rule
   R004 (`Rules/rules_v1.cpp:230-244`) can only leave a note.
3. Orca **already ships** every primitive and modifier type — cube, cylinder, sphere, cone, disc,
   torus, slab, text, SVG, as part or modifier (`GUI_Factories.cpp:557-572`). There is nothing to add
   there.

## Decision

### 1. We write our own orientation engine rather than reusing `Orient.cpp`

`src/libslic3r/Orient.cpp` works, but it is the opposite of what this fork sells. It carries 28
undocumented tuned constants (`Orient.hpp:51-97`); its objective function divides by `area_projected`,
a field that is **never assigned** (`Orient.cpp:479,483`); `volume`, `area_total` and `radius` are
computed per candidate and never used; the cancellation predicate `stopcond_` is accepted and never
consulted (`Orient.cpp:84`); it copies the whole mesh twice per candidate (`:329,348`) and writes to
`std::cout` from a worker thread; and it has no tests.

None of that can be explained to a user, and explaining it is the product. `Orient.cpp` is left
exactly where it is — the existing menu entry and CLI action keep using it — and the NØCTE engine is
written beside it under `src/libslic3r/Nocte/Plan/`. We reuse what is genuinely reusable and already
tested: `lay_on_face_planes()` and `lay_on_face()` (`LayOnFace.hpp:29,46`), `slice_mesh_slabs()` and
`project_mesh()` (`TriangleMeshSlicer.hpp:103,122`), and `TreeSupport::detect_overhangs()` for the
highest-fidelity support tier.

### 2. The engine proposes; the user accepts

The engine never rotates a part on its own. It presents the best candidates with their numbers in
physical units and the reason each won, and the user accepts one, inside a single undo snapshot.
This matches the repair and auto-tune panels from block 3, and it is the only honest presentation of
a model whose support and time terms are not yet calibrated against real prints.

The showcase face for the visual intent, and the load direction for the strength intent, are chosen
from a list of the part's own candidate faces (`lay_on_face_planes()` already returns each with its
normal, centre, area and outline). The largest detailed face is preselected, which is correct for a
sign nearly always, and one click changes it. Picking a face by clicking in the 3D view is deferred:
the gizmo registry (`GLGizmosManager`) is not a touch point, as recorded in ADR-003.

### 3. Intent and modifier recipes are persisted in `Metadata/nocte_report.json`, never as config keys

Finding 1 above makes a custom per-object config key unusable: it would make our own projects
unopenable in Bambu Studio, which is the compatibility ADR-001 exists to protect. The report file is
already written additively and non-fatally (`bbs_3mf.cpp:6575-6589`) and Bambu Studio ignores it.

Two consequences:

- The report needs a **stable per-object key**. Today its `objects` array is positional and carries
  only `name`, which is not stable. Block 4 adds `oid`, a 64-bit FNV-1a hash over each volume's facet
  and vertex counts, quantised volume and quantised bounding box, combined across volumes so the
  order does not matter. It survives renaming, re-export and the known per-part matrix-compounding
  bug (which does not affect geometry).
- The report needs a **reader**, which does not exist. It is added with a load hook wrapped in a
  `catch (...)` that can never fail the project load. A NØCTE project that loses its report degrades
  to a plain project; it never fails to open.

A parametric modifier is written to the 3mf as an ordinary `ModelVolumeType::PARAMETER_MODIFIER` mesh,
so Bambu Studio sees a normal modifier and only the recipe is lost. Degradation is automatic and
cannot corrupt anything.

### 4. Computed process settings go into real presets, not into `TuneResult`

Finding 2 makes this structural rather than stylistic. Two homes, both in territory the fork already
owns (`resources/` is in `OWNED_PREFIXES`):

- Shipped system profiles under `resources/profiles/NOCTE/`, inheriting from the Bambu Lab A1 system
  process presets. The signage profiles live here; they are a business asset.
- A user preset written by `PresetCollection::save_current_preset()` (`Preset.hpp:638`) when the user
  asks to keep a computed plan.

`TuneResult` gains an additive `preset_suggestions` field; no existing field changes. Every key the
profile calculator emits is validated against `print_config_def` **at its declared scope in a unit
test**, so a print-scope key can never ship as a per-object override.

### 5. Touch-point allowlist extension

The planning engine adds one upstream file to the allowlist of ADR-001:

- `src/libslic3r/PrintConfig.cpp` — to declare the `--nocte-plan` CLI action in
  `CLIActionsConfigDef`, beside the upstream `inspect_mesh` (`:11987`) and `ground_largest_face`
  (`:12115`) actions. The file is today **unmodified** from the upstream base, so this is the fork's
  first edit to it. `src/OrcaSlicer.cpp`, where the action is handled next to `inspect_mesh`
  (`:6159-6167`), is already a touch point.

The LAN printing work in section 6 may need one or two more — the "Successfully sent" misreport lives
in `src/slic3r/GUI/Jobs/PrintJob.cpp` and the SD filename in `src/slic3r/GUI/PartPlate.cpp`, neither
of which is a touch point today. They are not granted here in advance: each is added when the change
that needs it is written, with its marker and its justification, or the fix is found somewhere the
fork already owns.

The rule is unchanged: the edit carries a `NOCTE-BEGIN nocte-plan` marker, and the logic lives in
`src/libslic3r/Nocte/Plan/`, not in the touch point.

The headless CLI action is not a convenience. It is what makes the engine testable before any user
interface exists, and it is what the batch calibration harness drives.

### 6. LAN printing is enabled, printing itself stays a deliberate act

Validation requires that a project sliced here can **either** be opened in Bambu Studio **or** be sent
over LAN to the printer. The second path exists in the tree since block 3 but is unreachable:
`GUI_App::resolve_printer_agent_id()` returns the Bambu agent id for a Bambu vendor
(`GUI_App.cpp:3976-3981`), and `NocteLanPrinterAgent::set_allow_print()` has no caller.

Block 4 makes the agent reachable and puts `allow_print` behind an explicit opt-in in Preferences.
Consistent with ADR-002, that opt-in is the authorisation to send a file; **actually starting a print
on a real machine remains a separate, explicit act by the operator**, and no automated path may
initiate one. The refusal path is also corrected: it currently emits `PrintingStageFinished`, which
the UI renders as "Successfully sent" (`PrintJob.cpp:443`).

## Consequences

- The engine is validated headlessly in CI before it has a panel, and calibrated against real prints
  afterwards. Until that calibration runs, the support volume ranks candidates but is **not quoted in
  grams**, and the tier that produced it is carried in the result so it cannot be misread.
- `t_layer` and `k = σ_z/σ_xy` are named as uncalibrated in the HLSD and in the panel. We would rather
  show a number with its provenance than a number that looks authoritative.
- The rules gain an intent-aware context, which changes `Rule::match`/`emit` signatures across five
  rules and two call sites. Mechanical churn, one owner, no parallelism.
- Nothing in this block changes the 3mf identity gate: `Application` keeps its `BambuStudio-` prefix
  (ADR-001), which is what makes the dual path possible at all.
