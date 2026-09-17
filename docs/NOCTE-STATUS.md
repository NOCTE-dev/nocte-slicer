# NØCTE Slicer — implementation status

Living document (the HLSD describes the design as it stands and carries no status markers, per `AGENTS.md`).

Base: OrcaSlicer upstream `main` @ `6b0e190e64` (tag `nocte-base-2026-09-16`). Updated: 2026-09-17.

## CI

- First full `Build all` run on the fork (workflow_dispatch, 2026-09-17): green on Windows x64, Linux x86_64/aarch64, macOS arm64/x86_64 and Flatpak. Windows deps built in 55 min, slicer in 1 h 24 min, unit tests 5 min. Artifact `nocte-slicer-windows-x64` produced. `NOCTE checks` green.
- Known: the initial push to the empty repository did not fire `push` workflows; manual dispatch works and later pushes trigger normally.

## M0 scaffold

The engine skeleton is in the tree and unit-tested; everything that needs CGAL, libigl, OpenVDB or
the Manifold dependency is declared, planned and reported, but not executed yet.

**Implemented**

- `NocteVersion.hpp` — version, app names, 3mf metadata tag, report schema id.
- `MeshDiagnostics.{hpp,cpp}` — `diagnose()` with plain C++17 topology, no libigl: degenerate
  facets (repeated indices and zero-area slivers), duplicate vertices, open boundary loops chained
  from the edge table, non-manifold edges, inconsistent facet orientation, disconnected shells and
  zero-volume closed shells. Every issue carries a stable `issue_id` and a sentence of explanation;
  `its_mesh_edges()` is the shared edge table the repair side reuses.
- `RepairOps.hpp` — the operation vocabulary, parameters, metrics and step results.
- `MeshRepair.{hpp,cpp}` — `compute_metrics()`, `plan_from()` (tier-ordered, rationale naming the
  issue ids) and `RepairSession` with preview / accept / reject / undo over a snapshot stack capped
  at 8. Tier-1 operations that run today: `RemoveDegenerateFaces`, `MergeVertices`,
  `OrientConsistently` (patch-wise BFS, then flip a patch that encloses a negative volume) and
  `RemoveTinyShells`.
- `Report.{hpp,cpp}` — `NocteReport::to_json()` (nlohmann) and `to_markdown()`, covering the
  diagnosis, the repair log and the placeholder geometry/recommendation sections.
- CMake wiring in `src/libslic3r/CMakeLists.txt` and `tests/libslic3r/CMakeLists.txt`, both inside
  `NOCTE-BEGIN` / `NOCTE-END` markers.
- `tools/nocte/check_touchpoints.py` — the allowlist and marker guard.
- Catch2 coverage: `tests/libslic3r/test_nocte_mesh_diagnostics.cpp` and
  `tests/libslic3r/test_nocte_mesh_repair.cpp`.

**Stubbed or declared only**

- `SelfIntersection` and `InvertedShell` diagnostics — listed in `DiagnosticsResult::skipped_checks`;
  `check_self_intersections()` and `check_inverted_shell()` return false with a reason.
  `NonManifoldVertex`, `TinyFeature` and `ThinWall` are in `IssueKind` but not yet detected.
- Repair operations `StitchBorders`, `DuplicateNonManifoldVertices`, `FillHolesCgal`, `FlipShell`,
  `NormalizeManifold` and `VoxelRemesh` — planned with a rationale where the diagnosis calls for
  them, but `accept()` fails with "not implemented in M0".
- `GeometryAnalysis.{hpp,cpp}` and `AutoTune.{hpp,cpp}` — final struct shapes, `analyze()` and
  `tune()` return empty results (`TODO(M1)`).
- `Rules/RuleSet.hpp` — rule and sink types, with an empty `rule_set_v1()`; `Rules/rules_v1.cpp`
  arrives in M1.
- `NocteCgal.cpp`, the GUI layer under `src/slic3r/GUI/Nocte/` and the LAN agent under
  `src/slic3r/Utils/Nocte/` do not exist yet.
