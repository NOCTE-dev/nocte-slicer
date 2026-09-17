# ADR-002 — Networking under AGPL-3.0: LAN Developer Mode instead of the Bambu network plugin

- **Status:** Accepted
- **Date:** 2026-09-16
- **Deciders:** NØCTE Engineering
- **Related:** ADR-001 (fork of OrcaSlicer, native 3mf compatibility)

## Context

### License obligations

NØCTE Slicer is a distributed fork of OrcaSlicer, so the entire binary is AGPL-3.0:

- **§6 (corresponding source):** every distributed binary — installer, portable zip, CI artifact — must be accompanied by the complete corresponding source of that exact build, including build scripts.
- **§13 (network use):** if we ever offer NØCTE functionality to users over a network, those users must be offered the corresponding source of the running version.
- **No proprietary in-process modules.** Anything linked into, or loaded into, the NØCTE process and distributed by us must be AGPL-compatible.
- **No dual licensing.** The upstream code has hundreds of authors and no CLA; relicensing is not available to us.
- **Trademark rights are retained** under §7(e): the "NØCTE" name, wordmark and logo are ours and are not licensed by the AGPL. The code is free; the brand is not.

### The Bambu network stack

- Bambu's `BambuNetworkLibrary.dll` is **proprietary and loaded at runtime** from `data_dir()/plugins`. It is not part of the source tree; slicers download it on demand.
- In **May 2026** the Software Freedom Conservancy publicly alleged that shipping this arrangement constitutes an AGPL violation. The dispute is unresolved.
- Bambu's **Authorization Control** firmware (January 2025) restricts unauthenticated control of the printer.
- **Bambu Connect** is closed software and is not integrated by OrcaSlicer.

A fork that redistributes the Bambu plugin under its own brand would take on both the license risk and the compliance burden of that dispute, with no upside for NØCTE.

## Decision

### 1. NØCTE never bundles or redistributes the Bambu network plugin

No NØCTE installer, portable build, CI artifact or first-run flow ships, downloads, or offers `BambuNetworkLibrary.dll` (or any equivalent proprietary Bambu networking component). No NØCTE cloud service.

### 2. The supported print path is LAN Developer Mode, through a NØCTE-owned agent

`NocteLanPrinterAgent` (`src/slic3r/Utils/Nocte/`) implements Orca's existing `IPrinterAgent` interface and is registered through `NetworkAgentFactory`. It is 100% NØCTE code under AGPL-3.0, with no proprietary dependency.

Protocol, as ported from FDM-HUB (see §6 of the kickoff plan):

| Concern | Decision / detail |
|---|---|
| Upload | **FTPS, implicit TLS, port 990** (explicit TLS is the number-one cause of "does not connect"), user `bblp`, password = printer access code, self-signed certificate accepted, `PROT P`, `STOR <name>` at the SD-card root with no paths, `.3mf` only, 32 KB blocks, 60 s timeout, ASCII filenames |
| Upload success check | The server frequently omits the `226` reply; success is decided by re-listing the SD card (`NLST`), never by the response code |
| Control | **MQTT over TLS, port 8883**, MQTT 3.1.1, user `bblp`, password = access code, self-signed certificate, keepalive 60, CONNACK waited up to 15 s; rc 4/5 = wrong access code |
| Topics | Subscribe **exactly** `device/<serial>/report` (wildcards cause DISCONNECT rc=128); publish to `device/<serial>/request`. The serial is mandatory; it is discovered via SSDP M-SEARCH (UDP 1990, reply 2021), which conflicts with a running Bambu Studio |
| State | `{"pushing":{"sequence_id":"1","command":"pushall"}}` on connect and every 300 s, because reports are partial deltas |
| Print command | `project_file` with `param":"Metadata/plate_N.gcode"`, `url":"file:///sdcard/<name>"`, `subtask_name`, `use_ams`, `bed_leveling`, `timelapse`, `flow_cali`, `vibration_cali`, `layer_inspect`, `bed_type":"auto"`, `profile_id`/`project_id`/`subtask_id`/`task_id` = `"0"`, `ams_mapping` |
| Gotchas | A `param` naming a non-existent plate is accepted and prints nothing; `ams_mapping` is a list with one entry per `<filament>` in `slice_info.config` (0–3, no duplicates); `gcode_line` has a silent ~1.8 KB limit; `ledctrl` requires all four fields; `skip_objects` requires `label_object_enabled` |
| Printer-side validation of the `.gcode.3mf` | `plate_N.gcode.md5` in upper-case hex; `slice_info.config` `tray_info_idx` consistent with the gcode's `; filament_ids` (otherwise filament mismatch); the printer also reads `printer_model_id`, `nozzle_diameters`, `filament_maps`. All of this is produced by the inherited Orca engine; `tools/bbl-compat` verifies it |
| Credentials | IP, serial and access code live in the user config under `data_dir()`. FDM-HUB secrets (`.env`, `.credenciales.md`, `config/impresoras.json`, codes extracted from `%APPDATA%\BambuStudio\BambuStudio.conf`) are **never** copied into this repository |

### 3. Dependencies

- **New dependency: `paho.mqtt.c`** (EPL-2.0 / EDL-1.0, AGPL-compatible) as a new entry in `deps/`. Reason: Orca pins Boost 1.84, and Boost.MQTT5 only arrived in Boost 1.88.
- **FTPS needs nothing new:** Orca's vendored libcurl keeps FTP enabled (it only disables LDAP, RTSP, DICT, TELNET, POP3, IMAP), and OpenSSL is already vendored.

### 4. Business model

AGPL-3.0 source plus revenue from brand and services, mirroring Bambu Lab's own posture but without proprietary modules. The NØCTE trademark is the protected asset (§7(e)); anyone may fork the code, nobody may ship it as NØCTE.

### 5. Out of scope for M0–M2

A **user-installed** Bambu plugin — a user choosing, on their own initiative, to install Bambu's network plugin into their own `data_dir()/plugins` — is out of scope for M0–M2. NØCTE neither supports, tests, documents, nor prompts for it in those milestones, and the LAN agent must remain fully functional without it. Cloud printing and Bambu Connect integration are likewise out of scope.

## Consequences

**Positive:** no AGPL §6/§13 exposure from a proprietary blob in our binary; no dependency on an unresolved legal dispute; a printing path we fully own, can test headlessly and can extend to other vendors (Moonraker et al. already exist upstream); credentials stay local and no telemetry or cloud account is required; the print path is verifiable end-to-end in CI-adjacent scripts because the protocol is plain FTPS + MQTT.

**Negative / risks:**

| Risk | Mitigation |
|---|---|
| Developer Mode must be enabled on the printer; some models or firmware builds restrict or remove it (Authorization Control, January 2025) | Document the requirement prominently; detect and report the failure clearly; keep the "open the project in Bambu Studio" fallback, which works because the 3mf is native (ADR-001) |
| No cloud printing, no remote (off-LAN) printing | Explicitly a non-goal; §13 would apply if we ever added a NØCTE-hosted service |
| Bambu may change the LAN protocol or tighten authentication in future firmware | The protocol lives behind `IPrinterAgent` in one file; FDM-HUB gives us a working reference implementation to re-derive behaviour from; firmware versions are recorded alongside test results |
| SSDP discovery conflicts with a running Bambu Studio | Allow manual entry of IP and serial; document the conflict |
| New `paho.mqtt.c` dependency increases build surface | Single small C library, added to `deps/` like any other vendored dep and cached in CI |
| Users may expect the Bambu plugin's features (cloud, AMS auto-refill UX) | Documented as unsupported; LAN feature parity is tracked per milestone |
