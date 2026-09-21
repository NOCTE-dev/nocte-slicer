# W5 — NØCTE LAN Developer Mode printer agent (C++)

Implements ADR-002 §2 in C++: `NocteLanPrinterAgent` over an in-tree MQTT 3.1.1 client and
libcurl FTPS, plus a headless diagnostics probe. No new dependency; nothing under `deps/**`
changed. **Not compiled here — there is no local toolchain.**

## Files

New, all under `src/slic3r/Utils/Nocte/`:

| File | Lines | Contents |
|---|---|---|
| `NocteMqttClient.hpp` / `.cpp` | 125 / 797 | minimal MQTT 3.1.1 over `boost::asio` + `boost::asio::ssl`, redaction helpers |
| `NocteFtps.hpp` / `.cpp` | 90 / 431 | implicit FTPS on 990 over the vendored libcurl |
| `NocteLanPrinterAgent.hpp` / `.cpp` | 139 / 664 | the `IPrinterAgent` implementation |
| `NocteLanProbe.hpp` / `.cpp` | 28 / 415 | `Slic3r::Nocte::run_lan_probe()` |

Edited (marked, minimal):

- `src/slic3r/Utils/NetworkAgentFactory.cpp` — include, a shared-instance factory, two
  registrations (`// NOCTE-BEGIN nocte-lan`).
- `src/OrcaSlicer.cpp` — include + `attach_console_on_demand()` declaration in the
  `SLIC3R_GUI` block; the `NOCTE_LAN_PROBE=1` short circuit at the top of `CLI::run`; the
  stale `NOCTE-TODO(W5)` anchor comment replaced with the reason the probe is not a CLI action.

### For the integrator: `src/slic3r/CMakeLists.txt`

Eight paths into `SLIC3R_GUI_SOURCES`, next to the other `Utils/` entries (~`:727-740`), in
their own marked block:

```cmake
# NOCTE-BEGIN nocte-lan
    Utils/Nocte/NocteMqttClient.cpp
    Utils/Nocte/NocteMqttClient.hpp
    Utils/Nocte/NocteFtps.cpp
    Utils/Nocte/NocteFtps.hpp
    Utils/Nocte/NocteLanPrinterAgent.cpp
    Utils/Nocte/NocteLanPrinterAgent.hpp
    Utils/Nocte/NocteLanProbe.cpp
    Utils/Nocte/NocteLanProbe.hpp
# NOCTE-END
```

No link-line change: `libslic3r_gui` already links `libcurl OpenSSL::SSL OpenSSL::Crypto`
(`src/slic3r/CMakeLists.txt:892`, unchanged), and Boost.Asio's SSL layer is header-only. Boost 1.84 is
already used in-tree for Asio (`src/slic3r/Utils/TCPConsole.cpp:1-6`,
`MoonrakerPrinterAgent.cpp:13-14`); `boost/asio/ssl.hpp` is the one Asio header this tree had
not used before.

## MQTT packet layouts

Encoded by hand in `NocteMqttClient.cpp`; cross-checked against what `tools/nocte/lan/mqtt_listen.py`
actually observed (ADR-002 "Measured protocol sequence").

*Remaining length* (`encode_remaining_length`, `:56`) is the full 1–4 byte varint, and the
decoder (`read_remaining_length`, `:392`) reads continuation bytes one at a time up to four.
The `pushall` answer measured 3362 B, so two bytes are needed on the very first message; a
one-byte decoder desynchronises the stream permanently.

**CONNECT** — `0x10`, remaining length, then:

```
00 04 'M' 'Q' 'T' 'T'     protocol name        (MQTT-3.1.2.1)
04                        protocol level 4     = MQTT 3.1.1
C2                        user name | password | clean session
00 3C                     keep alive = 60 s
<len><client id>          20 random hex characters, unique per process
<len>"bblp"               user name
<len><access code>        password
```

**CONNACK** — `0x20 02 <session present> <rc>`. rc 0 accepted; **rc 4 / 5 = wrong access
code**, reported as such and the session torn down.

**SUBSCRIBE** — `0x82` (type 8 with the reserved flags `0010`), remaining length, then
`<packet id:2> <len>"device/<serial>/report" 00`. Exactly one filter, requested QoS 0; no
wildcard is ever sent, because all three shapes were confirmed to be a hard disconnect in
12–27 ms.

**SUBACK** — `0x90`, `<packet id:2> <granted qos>`; `0x80` is surfaced through
`GetSubscribeFailureFn`.

**PUBLISH (out)** — `0x30`, remaining length, `<len><topic>`, payload. **No packet identifier
and no QoS 1, ever**: a QoS 1 publish is delivered but never PUBACKed, so an in-flight window
of one would stall the client permanently.

**PUBLISH (in)** — QoS taken from bits 1–2 of byte 1; the packet-id field is skipped when
QoS > 0 and a PUBACK (`0x40 02 <id>`) is returned at QoS 1. Defensive only: the SUBACK grants 0.

**PINGREQ / PINGRESP / DISCONNECT** — `C0 00`, `D0 00`, `E0 00`. PINGREQ goes out every
keep-alive / 2 (30 s); a second one firing while the first is unanswered fails the session.

## FTPS

`ftps://<ip>:990/` — scheme and port are built inside the class and never taken from a caller,
because explicit TLS on 990 is the classic hang. Options set in `apply_common_options()`:
`CURLOPT_USE_SSL = CURLUSESSL_ALL` (→ `PROT P`), `CURLOPT_SSL_VERIFYPEER 0`,
`CURLOPT_SSL_VERIFYHOST 0`, `CURLOPT_USERNAME`/`CURLOPT_PASSWORD`,
`CURLOPT_FTP_SKIP_PASV_IP 1`, `CURLOPT_CONNECTTIMEOUT`, `CURLOPT_TIMEOUT`,
`CURLOPT_FTP_RESPONSE_TIMEOUT` (10 s, not the 60 s socket timeout), `CURLOPT_NOSIGNAL 1`,
`CURLOPT_ERRORBUFFER`. `CURLOPT_FTP_USE_EPSV` is left at its default (EPSV then PASV) and
`CURLOPT_FTP_SSL_CCC` is left off. All exist in curl 7.75 (`deps/CURL/CURL.cmake` pins
`curl-7_75_0`, FTP enabled, OpenSSL backend).

- `list()` — `CURLOPT_DIRLISTONLY 1` (NLST), output collected into a string.
- `upload_file()` / `upload_memory()` — `CURLOPT_UPLOAD 1` + `CURLOPT_READFUNCTION`
  (`boost::nowide::fopen` for files, a buffer cursor for memory) + `CURLOPT_INFILESIZE_LARGE`.
  A non-`CURLE_OK` result with `CURLINFO_SIZE_UPLOAD_T` equal to the file size is **not** a
  failure: the printer omits the final `226`. Success is decided by re-listing and finding the
  name — exactly as `ftps_upload.py` does.
- `remove()` — `CURLOPT_QUOTE` `"DELE <name>"` + `CURLOPT_NOBODY 1`, then confirmed by NLST.
- `is_safe_remote_name()` gates every name: bare ASCII, no separators, wildcards, spaces or
  leading `-`/`.`, and `.3mf`/`.txt` only.

## The print gate

Three places could publish `project_file`; all three consult `m_allow_print`
(`std::atomic<bool>`, **default false**):

1. `publish_command()` — parses the JSON and refuses any `{"print":{"command":"project_file"}}`.
   A payload that fails to parse but contains the string `project_file` is refused too: the gate
   errs towards refusing. This covers `send_message()` and `send_message_to_printer()`, i.e.
   every command the GUI sends of its own accord.
2. `start_local_print()` — uploads, then stops before publishing, emits
   `PrintingStageFinished / 100 / "printing is disabled in this build"` and returns
   `BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED`.
3. `start_sdcard_print()` — same.

The only way to open it is `set_allow_print(bool)`, which nothing in this build calls and which
logs at `warning` when used. `NocteLanProbe` has no reference to the agent at all, so the probe
cannot reach the gate even indirectly.

## Probe

```
set NOCTE_LAN_PROBE=1
set NOCTE_LAN_IP=…
set NOCTE_LAN_SERIAL=…
set NOCTE_LAN_ACCESS_CODE=…
nocte-slicer.exe
```

It runs at the very top of `CLI::run` — before option parsing, config load and GUI — calls
`attach_console_on_demand()` (`src/OrcaSlicer.cpp:~7990`) and returns its own exit code.
`nocte-slicer.exe` is a GUI-subsystem binary, so the checklist is **always** written to
`./nocte_lan_probe.txt` as well as stdout.

Checks: credentials present · TCP+TLS on 8883 and 990 with the negotiated version, cipher and
timing · CONNACK received and rc 0 (4/5 named as a bad access code) · TLS 1.2 · SUBACK granted
QoS 0 · at least one report · first `print` report size, top-level keys and `print` key count ·
`get_version` module count · largest payload > 127 B (the multi-byte remaining length) · message
rate over 20 s · NLST entry count · a `nocte_lan_probe.gcode.3mf` built in memory with
`mz_zip_writer_init_heap` / `add_mem("Metadata/plate_1.gcode")` / `finalize_heap_archive`
(`libslic3r/miniz_extension.hpp`), STOR'd, confirmed by NLST, DELE'd and confirmed gone.
Exit 0 / 1.

Every line goes through a two-stage redactor that mirrors `tools/nocte/lan/credentials.py`:
the known IP, serial and access code first (longest first), then MAC, `h1,h2,h3,h4,p1,p2`,
dotted-quad (with the firmware-version exemption), serial-shaped and 8-digit-code patterns.
The access code has no "masked" form anywhere — it is simply never logged.

## Agent selection

`GUI_App::resolve_printer_agent_id()` (`:3976-3982`) maps an empty `printer_agent` on a Bambu
Lab preset to `BBL_PRINTER_AGENT_ID`, and `switch_printer_agent()` (`:4003-4011`) clears the
live agent when that id is unregistered. Since the BBL agent is no longer registered, every
existing Bambu preset would have had no agent at all.

`GUI_App.cpp` is at its hunk budget and owned by another PR, so this is fixed in
`NetworkAgentFactory.cpp`, which W5 owns: the agent is registered under **both**
`Slic3r::Nocte::LAN_AGENT_ID` and `"bbl"`. `register_printer_agent(id, display_name, factory)`
takes the id explicitly, so two ids for one class is supported as-is. Both registrations share
`create_nocte_lan_agent()`, which hands out one instance via a `static std::weak_ptr` — without
that, `create_printer_agent_by_id()`'s per-id cache would build a second agent, and a second
MQTT session against the same printer, the first time the user moved between the two entries.
`canonical_printer_agent_id()` keeps storing the empty default for the `"bbl"` row, so no
preset is rewritten.

Cost: two rows in the Preferences dropdown ("NØCTE LAN (Developer Mode)" and "Bambu Lab (NØCTE
LAN Developer Mode)"). The intended replacement, for the PR that owns `GUI_App.cpp`, is three
lines in `resolve_printer_agent_id()`:

```cpp
    return (preset_bundle && preset_bundle->is_bbl_vendor()) ? Slic3r::Nocte::LAN_AGENT_ID
                                                             : ORCA_PRINTER_AGENT_ID;
```

…after which the `"bbl"` alias registration can be deleted.

Also worth knowing: `GUI_App::select_machine()` returns early for `BBL_PRINTER_AGENT_ID`
(`:4056-4062`), so the alias path does not get the automatic machine selection the other
agents do; and this block implements **no SSDP**, so `start_discovery()` is a no-op and the
printer has to reach `MachineObject::connect()` (`DeviceManager.cpp:2632`) by another route.

## Compile risks, worst first

1. **`boost::asio::ssl` on clang-cl.** No file in this tree includes `boost/asio/ssl.hpp`
   today, so this is the only genuinely new header. It is header-only against OpenSSL 1.1.1w
   (`deps/OpenSSL/OpenSSL.cmake:57`) and both `OpenSSL::SSL` and `OpenSSL::Crypto` are already
   linked into `libslic3r_gui`, so the link should be clean; the risk is Windows header
   collisions (`wincrypt.h` `X509_NAME`) pulled in through `<openssl/ssl.h>`.
2. **`ssl::stream` teardown semantics.** Deliberately *not* using `async_shutdown()`: the
   printer drops TCP without `close_notify`. Teardown cancels, shuts down and closes the
   lowest layer with an `error_code` and tolerates every error. One real hazard was found and
   fixed while writing it — a graceful `DISCONNECT` must not start a second `async_write` on a
   stream that already has one in flight, and must not clear the deque that owns the in-flight
   buffer; `write_in_flight` plus a separate `disconnect_packet` buffer guards both.
3. **`executor_work_guard` has no `operator=`**, so the design relies on posting the first
   handler *before* the thread starts and on `io_context::stop()` in `finish_shutdown()` to end
   `run()`. If a future edit removes that post, `run()` can return immediately.
4. **curl option availability in 7.75.** `CURLOPT_XFERINFOFUNCTION` (7.32),
   `CURLINFO_SIZE_UPLOAD_T` (7.55), `CURLOPT_FTP_SKIP_PASV_IP` (7.14.2) and
   `CURLOPT_DIRLISTONLY` (7.17) all predate the pin. `curl_easy_setopt` is varargs — every
   numeric argument is passed as an explicit `long` or `curl_off_t`.
5. **`<atomic>` / `<thread>` / `<system_error>`** are included explicitly rather than inherited
   from `pchheader.hpp`, which not every build configuration uses.
6. **`[[nodiscard]]`-shaped calls.** Timer `cancel()` is called in its non-`error_code` form
   (the `error_code` overload is the deprecated one); socket `cancel`/`shutdown`/`close`/
   `set_option` use the `error_code` overloads, which return `BOOST_ASIO_SYNC_OP_VOID`.
7. **Narrowing.** Every byte written into a packet goes through an explicit
   `static_cast<uint8_t>` then `static_cast<char>`; no braced `uint8_t{int}` anywhere.
8. **`(std::min)` / `(std::max)`** are parenthesised throughout, against `windows.h` macros.
9. **UTF-8 literals.** "NØCTE LAN (Developer Mode)" reaches the UI through
   `_(std::string)` → `wxString(s, wxConvUTF8)` (`GUI/I18N.hpp:45`), and MSVC gets `/utf-8`
   (`CMakeLists.txt:706-707`). Files are UTF-8 without BOM, matching `encoding_check`.
