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

### Measured protocol sequence (2026-09-17, A1, Developer Mode)

The table above was a hypothesis ported from FDM-HUB. It was validated against a
real Bambu Lab A1 with an AMS Lite, in LAN Developer Mode, idle (`gcode_state`
`FINISH`), by the probes in [`tools/nocte/lan`](../../tools/nocte/lan/README.md)
(`a4_probe.py --seconds 30`, `ftps_upload.py`). Firmware at the time of the run:
`ota 01.08.01.00`, `esp32 01.16.33.15`, `mc 00.01.30.58`, `th 00.01.07.70`,
`ams_f1/0 00.00.08.15`. Every credential-bearing value below is redacted by the
probes themselves; no address, serial or access code appears in this repository.

**Transport.** TCP 8883 accepts in ~40 ms, TCP 990 in ~110 ms. Both ports speak
**TLS 1.2** with `ECDHE-RSA-AES256-GCM-SHA384` (256-bit). A handshake costs
**≈0.85–0.9 s** on both ports — the dominant cost of every connection, and the
reason a per-command reconnect design would feel broken. The printer demands
**no SNI** (handshake completes with no `server_hostname` at all) and ignores an
arbitrary SNI name; it demands **no client certificate**. Its certificate is
**not self-signed**: subject `CN=<printer serial>`, issuer
`C=CN, O=BBL Technologies Co., Ltd, CN=BBL CA`, valid to 2035-08-12. Since the
subject CN is the serial and we connect by IP, hostname verification can never
succeed — peer verification has to be disabled, or the BBL CA pinned and the
hostname check waived.

**MQTT.** Protocol level 4 (MQTT 3.1.1), `clean_session=1`, keepalive 60,
user `bblp`, password = access code. CONNACK `rc=0` arrives ~850 ms after
`connect()` (TLS included). The client id is free-form: FDM-HUB's
`fdmhub-<id>-<epoch>` and a random 20-hex-character id are both accepted.
`SUBACK` for the exact topic `device/<serial>/report` grants **QoS 0** in ~10 ms.
After `{"pushing":{"sequence_id":"1","command":"pushall"}}` the **first** message
back, ~60 ms later, is a full `print` object with **65 keys**:

> `ams`, `ams_rfid_status`, `ams_status`, `bed_target_temper`, `bed_temper`,
> `big_fan1_speed`, `big_fan2_speed`, `cali_version`, `chamber_temper`,
> `command`, `cooling_fan_speed`, `fan_gear`, `filam_bak`, `flag3`,
> `force_upgrade`, `gcode_file`, `gcode_file_prepare_percent`, `gcode_state`,
> `heatbreak_fan_speed`, `hms`, `home_flag`, `hw_switch_state`, `info`,
> `ipcam`, `k`, `layer_num`, `lifecycle`, `lights_report`, `mc_percent`,
> `mc_print_line_number`, `mc_print_stage`, `mc_print_sub_stage`,
> `mc_remaining_time`, `mess_production_state`, `msg`, `net`, `nozzle_diameter`,
> `nozzle_target_temper`, `nozzle_temper`, `nozzle_type`, `online`,
> `print_error`, `print_type`, `profile_id`, `project_id`, `queue_est`,
> `queue_number`, `queue_sts`, `queue_total`, `s_obj`, `sdcard`, `sequence_id`,
> `spd_lvl`, `spd_mag`, `stg`, `stg_cur`, `subtask_id`, `subtask_name`,
> `task_id`, `total_layer_num`, `upgrade_state`, `upload`, `vt_tray`,
> `wifi_signal`, `xcam`

`net`, `ipcam` and `info` carry identity and network data; the C++ agent should
not log them. The `ams` block is `ams.ams[]` (one unit here), each unit with
`id`, `chip_id`, `humidity`, `humidity_raw`, `temp`, `dry_time`, `info` and a
`tray[4]` array, alongside the unit-level bitmask strings `ams_exist_bits`,
`tray_exist_bits`, `tray_is_bbl_bits`, `tray_tar`, `tray_now`, `tray_pre`,
`tray_read_done_bits`, `tray_reading_bits`, plus `version`, `insert_flag`,
`power_on_flag`. The external spool appears separately as `print.vt_tray`.

Subsequent reports are indeed **deltas**: 4–5 keys each. On an idle machine the
rate is **0.37 msg/s** (11 messages in 30 s). Payloads measured **86 B minimum,
88 B median, 3362 B maximum** — so the fixed-header *remaining length* is
multi-byte in practice and a naive one-byte decoder breaks on the `pushall`
answer. `{"info":{"sequence_id":"2","command":"get_version"}}` answers on the
same topic with a top-level `info` object carrying `module[]` (5 modules here),
each with `name`, `product_name`, `hw_ver`, `sw_ver`, `loader_ver`, `sn`,
`flag`, `visible`. With `keepalive=5` and an idle link, PINGREQ/PINGRESP flow
normally (2/2 over 14 s) and the session is not dropped.

**FTPS.** Implicit TLS on 990, as predicted. Banner `220 BBL-P003 FTP Server`,
then `331` → `230` for `USER bblp` / `PASS <code>`, `200` for `TYPE`/`PBSZ`/
`PROT P`, `227` passive mode (the reply repeats the printer's own address in
`h1,h2,h3,h4,p1,p2` form — a redaction trap), `150` … `226` for `NLST`
(~1.0–1.6 s for 65 entries), `250` for `DELE`. Connect + login + `PROT P` costs
~0.9–1.2 s. Uploading a 63,131-byte `.gcode.3mf` pushed every byte and then
**never sent `226`**: `ftplib` raised `TimeoutError` after its 60 s socket
timeout on a file that had arrived intact, confirmed by `NLST`. The transfer
itself is fast; the 60 s is pure reply-wait.

#### Deviations from the hypothesised table

| Hypothesis | Measured |
|---|---|
| "self-signed certificate accepted" | Not self-signed: issued by a private `BBL CA`, subject `CN=<serial>`. Verification must still be disabled (or the CA pinned **and** hostname checking waived), but for a different reason — the CN is a serial, never the IP we dial |
| TLS session reuse required on the FTPS data channel | **Not required** by this firmware. A data connection negotiating a fresh TLS session listed the SD card fine (1.6 s vs 1.0 s with reuse). Keep reuse — it is the default in CPython's `ftplib` and in libcurl — but it is not the cause of a failure |
| Wildcards "cause DISCONNECT rc=128" | Confirmed exactly, for all three shapes: `device/<serial>/+` (20 ms), `device/+/report` (27 ms), `#` (12 ms) |
| "The server frequently omits the 226" | Confirmed on the one upload measured — omitted, not merely late: nothing arrived in 60 s |
| CONNACK "waited up to 15 s" | Generous; measured ~850 ms. Keep 15 s for a busy machine, but a failure is usually immediate |
| `pushall` every 300 s "because reports are partial" | Confirmed: deltas of 4–5 keys. 300 s is a policy choice, not a protocol requirement |
| QoS unspecified | **Publish at QoS 0 only.** A QoS 1 publish *is* delivered (the printer answered a QoS 1 `pushall`) but **no PUBACK is ever returned** — the message stays in flight forever, and an in-flight window of 1 would stall the client permanently |
| Serial "discovered via SSDP" | Not exercised; manual entry was used. SSDP remains the only discovery path and still conflicts with a running Bambu Studio (UDP 2021) |

#### Implications for the C++ agent

- **OpenSSL / TLS:** allow TLS 1.2 (do not require 1.3), `SSL_VERIFY_NONE`, no
  hostname verification, no client certificate, SNI optional. Set connect and
  handshake timeouts ≥ 5 s: a healthy handshake already costs ~0.9 s.
- **paho.mqtt.c:** `MQTTVERSION_3_1_1`, `cleansession = 1`,
  `keepAliveInterval = 60`, username `bblp`, password = access code,
  `MQTTClient_SSLOptions.enableServerCertAuth = 0`. Any unique client id works;
  make it unique per process so two NØCTE instances do not evict each other.
- **Buffers:** size the receive buffer for at least 64 KB even though 3.4 KB was
  the maximum seen — a multi-plate job with four AMS units and a long `hms`
  array is much larger. Decode the *remaining length* as the full 1–4 byte
  varint; the very first `pushall` answer already needs two bytes.
- **Topics:** subscribe to the exact `device/<serial>/report` and nothing else.
  A wildcard is a hard disconnect in ~20 ms, which will look like a credential
  failure if it is not special-cased.
- **Publishing:** QoS 0. Re-issue `pushall` on connect and periodically, and
  merge deltas into a cached state object rather than replacing it.
- **libcurl FTPS** (option names are *candidates, unverified* until the C++ side
  is built): `CURLOPT_USE_SSL = CURLUSESSL_ALL` for `PROT P`; an `ftps://` URL
  on port 990 for implicit TLS — confirm libcurl really selects implicit mode
  from the port rather than trying `AUTH TLS`, since getting this wrong is the
  classic hang; `CURLOPT_SSL_VERIFYPEER = 0` and `CURLOPT_SSL_VERIFYHOST = 0`;
  leave `CURLOPT_FTP_SSL_CCC` **off** (clearing the command channel is the
  opposite of what `PROT P` asks for); `CURLOPT_SSL_SESSIONID_CACHE` can stay at
  its default, since data-channel session reuse is not required here;
  passive mode is what the printer offers, so do not force `CURLOPT_FTPPORT`.
- **Upload completion:** never treat a missing `226` as failure and never let
  the UI block on it. Bound the post-transfer wait (a `CURLOPT_FTP_RESPONSE_TIMEOUT`
  of ~10 s rather than the 60 s socket timeout), then decide success by
  re-listing the SD root, exactly as `ftps_upload.py` does.

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
