<div align="center">

# NØCTE Slicer

**Building the Impossible.**

A 3D-printing slicer by NØCTE Engineering. Explainable mesh repair, per-part parameter auto-tuning,
and `.3mf` projects that Bambu Studio opens as native.

[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-000000?style=flat)](LICENSE.txt)
[![Base: OrcaSlicer main 2026-09-16](https://img.shields.io/badge/base-OrcaSlicer%20main%20%40%206b0e190-000000?style=flat)](https://github.com/OrcaSlicer/OrcaSlicer)
[![Windows build](https://github.com/NOCTE-dev/nocte-slicer/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/NOCTE-dev/nocte-slicer/actions)

</div>

---

## What NØCTE Slicer adds

| Feature | What it means |
|---|---|
| **Explainable mesh repair** | Missing faces, open edges, non-manifold geometry, inverted shells and degenerate triangles are *diagnosed*, then repaired step by step. Every step states what it changes and why, shows before/after metrics, and can be accepted, rejected or undone. No black-box auto-fix. |
| **Per-part auto-tuning** | Print parameters are derived from the real geometry of each part (overhangs, wall thickness, bridges, stability) and written as per-object / per-part overrides with a reason for each recommendation. Bambu Studio shows them natively. |
| **Native Bambu Studio `.3mf`** | Modifiers, negative parts, support enforcers/blockers, painted multi-material, AMS mapping, plates and all proprietary metadata survive the round trip. Verified continuously against the Bambu Studio CLI by [`tools/bbl-compat`](tools/bbl-compat/). |
| **LAN printing in Developer Mode** | Send sliced projects to Bambu Lab printers over the local network (FTPS + MQTT) with no cloud and no proprietary plugin. |

Everything above is built on top of the OrcaSlicer engine and keeps its full feature set and printer support.

## Status

Early development (milestone M0: foundation). Nothing is released yet. Follow the design in
[`docs/HLSD/nocte-overview.md`](docs/HLSD/nocte-overview.md) and the decisions in
[`docs/ADR/`](docs/ADR/).

## Building

NØCTE Slicer builds exactly like OrcaSlicer. See the upstream instructions in
[`docs/UPSTREAM-README-OrcaSlicer.md`](docs/UPSTREAM-README-OrcaSlicer.md) and the
[OrcaSlicer build wiki](https://www.orcaslicer.com/wiki/how_to_build). Windows x64 binaries are
also produced by GitHub Actions on every push to `main` (see [`.github/NOCTE-CI.md`](.github/NOCTE-CI.md)).

## Repository layout (NØCTE additions)

```
src/libslic3r/Nocte/       engine: MeshDiagnostics, MeshRepair, GeometryAnalysis, AutoTune, Report
src/slic3r/GUI/Nocte/      UI: repair report, auto-tune panel, branding
src/slic3r/Utils/Nocte/    LAN Developer Mode printer agent
tests/libslic3r/test_nocte_*.cpp
tools/bbl-compat/          Bambu Studio compatibility oracle, structural differ, golden corpus
tools/nocte/               repo hygiene (upstream touch-point check)
docs/ADR/, docs/HLSD/      architecture decisions and subsystem design
```

All NØCTE code lives in the directories above. Edits inside upstream files are wrapped in
`NOCTE-BEGIN` / `NOCTE-END` markers and limited to an allowlist, so that syncing with upstream
OrcaSlicer stays cheap.

## Heritage and attribution

NØCTE Slicer is a fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer), which descends
from [Bambu Studio](https://github.com/bambulab/BambuStudio), [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)
and [Slic3r](https://github.com/Slic3r/Slic3r). We are grateful to every contributor of those projects;
the original OrcaSlicer README is preserved in [`docs/UPSTREAM-README-OrcaSlicer.md`](docs/UPSTREAM-README-OrcaSlicer.md).

OrcaSlicer, Bambu Studio, Bambu Lab, PrusaSlicer and Prusa are trademarks of their respective owners.
NØCTE Slicer is not affiliated with or endorsed by any of them.

## License

NØCTE Slicer is licensed under the **GNU Affero General Public License, version 3** (see
[`LICENSE.txt`](LICENSE.txt)). The full source code of every distributed build is published in this
repository. NØCTE Slicer does **not** bundle or redistribute Bambu Lab's proprietary networking plugin.

The NØCTE name, the Ø wordmark and the tagline "Building the Impossible" are trademarks of
NØCTE Engineering and are not covered by the software license.
