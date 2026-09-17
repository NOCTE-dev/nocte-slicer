# `tools/nocte/lan` — LAN Developer Mode protocol probes

Python probes that measure what a Bambu printer in **LAN Developer Mode**
actually does, so that `NocteLanPrinterAgent` (C++, `src/slic3r/Utils/Nocte/`)
can be written against measurements instead of folklore. See
[ADR-002](../../../docs/ADR/ADR-002-network-agpl-and-lan-developer-mode.md),
section *Measured protocol sequence*, for the results.

These scripts are developer tooling. They are not built, shipped or called by
the slicer.

## Requirements

- Python 3.12, user scope. No admin rights needed.
- `paho-mqtt` — **2.x** (measured with 2.1.0). The callbacks in `mqtt_listen.py`
  are written to fit both the 1.x and 2.x callback APIs: they take `*args` and
  read the reason code positionally, because 2.x inserts a `properties`
  argument and changes `on_disconnect`'s arity. Under 2.x the client is built
  with `CallbackAPIVersion.VERSION2`.
- Everything else is standard library (`ftplib`, `ssl`, `socket`, `zipfile`,
  `xml.etree`).

## Safety model

| Script | What it sends to the printer | State change |
|---|---|---|
| `credentials.py` | nothing | none |
| `mqtt_listen.py` | CONNECT, SUBSCRIBE, `pushall`, `get_version`, DISCONNECT | none |
| `a4_probe.py` | the same, plus PINGREQ and three wildcard SUBSCRIBEs | none |
| `ftps_upload.py` | `NLST`, `STOR`, `DELE` | writes/removes one file on the SD card |
| `print_job.py` | `project_file` — **only** past two locks | starts a print |

- `pushall` and `get_version` are read-only requests. No `gcode_line`, no
  `ledctrl`, no pause/resume/stop, no temperature command exists in this
  directory at all.
- `ftps_upload.py delete` refuses any name that does not start with `nocte_`,
  so a probe run cannot remove a real job from the SD card.
- `print_job.py` is `--dry-run` by default. Publishing needs **both**
  `--i-really-mean-it` and `NOCTE_LAN_ALLOW_PRINT=1`, and then still refuses
  unless a live `pushall` reports `gcode_state` in `IDLE`/`FINISH`/`FAILED`.

## Redaction guarantees

Credentials are resolved at runtime and never written to this repository.

1. `credentials.load_printer()` resolves, in order:
   - `NOCTE_LAN_IP` / `NOCTE_LAN_SERIAL` / `NOCTE_LAN_ACCESS_CODE` (all three), then
   - the user's FDM-HUB checkout (`%USERPROFILE%\Documents\FDM-HUB`, override
     with `NOCTE_FDMHUB_DIR`), read-only, with FDM-HUB's own lookup:
     `config/impresoras.json` for `ip`/`serie`, `.env` for `FDM_<ID>_CODIGO`
     (and optionally `FDM_<ID>_IP` / `FDM_<ID>_SERIE`, which win).
2. `Printer.__repr__` is redacted: `ip=<ip>`, `serial='039...'` (three
   characters), `access_code=<access-code>`. The access code is never printed,
   not even as a masked length.
3. Every script prints through `credentials.Emitter`, which routes each line
   through `redact()`. That replaces the known values first, then applies
   pattern rules for values that arrive **from the printer**: IPv4, the
   `h1,h2,h3,h4,p1,p2` form inside an FTP `227` reply (measured: without this
   rule the printer's own address reaches stdout on every data transfer),
   MAC addresses, 12–20 character upper-case alphanumeric serials, and 8-digit
   codes.
4. MQTT payloads are summarised by `key_structure()` as **key paths and types**.
   Leaf values print only for an allowlist of enum-ish fields (`gcode_state`,
   temperatures, layer counters …). Everything identity-bearing — `sn`, `net`,
   `ipcam`, Wi-Fi names, cloud URLs — is never rendered.
5. Raw payloads exist only if `--raw` is passed, land in `_captures/`, and that
   directory is gitignored. Raw captures are **not** scrubbed: never paste one
   into a document, an issue or a commit.

Known over-redaction: dotted quads are masked, so firmware versions would be
too. They are exempted only in their canonical Bambu form (four two-digit
groups with at least one leading zero, e.g. `01.08.01.00`), a shape no
canonically written IPv4 address has.

## Usage

```bash
cd tools/nocte/lan

python credentials.py                 # which credentials resolve (no values)
python a4_probe.py --seconds 30       # the full PASS/FAIL checklist, ~70 s
python mqtt_listen.py --seconds 30    # report shapes only
python ftps_upload.py list
python ftps_upload.py upload ../../bbl-compat/corpus/fdmhub/P4_FLUJO_A1.gcode.3mf \
                      --as nocte_lan_probe.gcode.3mf
python ftps_upload.py delete nocte_lan_probe.gcode.3mf
python print_job.py --file ../../bbl-compat/corpus/fdmhub/P4_FLUJO_A1.gcode.3mf \
                    --sd-name nocte_lan_probe.gcode.3mf     # dry run
```

`--printer NAME` selects another entry from the FDM-HUB inventory; the default
is `A4`.

## What each script proves

- **`credentials.py`** — that credentials can be found without being stored,
  and that nothing downstream can print them. Run it alone to check a machine
  is configured before blaming the network.
- **`a4_probe.py`** — the whole hypothesis in ADR-002's protocol table, layer by
  layer: TCP 8883/990 reachability, TLS version/cipher/certificate/SNI/client
  certificate, MQTT CONNACK and SUBACK, time to first report, whether `pushall`
  really answers with a full `print` object, the key inventory of that object,
  `gcode_state`, the shape of `ams`, message rate, payload sizes, whether later
  reports are deltas, whether a random client id and QoS 1 are accepted, that
  PINGREQ keeps an idle session alive, and — last, on throwaway connections —
  that a wildcard subscription is refused.
- **`mqtt_listen.py`** — the report format itself, when you need to know which
  fields a UI can bind to.
- **`ftps_upload.py`** — that implicit TLS on 990 works, what every reply code
  is, how long each phase takes, and that upload success can only be decided by
  re-listing.
- **`print_job.py`** — that a local `.gcode.3mf` satisfies the printer-side
  invariants (`Metadata/plate_N.gcode` present, `.md5` upper-case hex and
  matching, `slice_info.config` filament count) and what the exact
  `project_file` payload looks like. It is the executable specification of the
  command the C++ agent must emit.
