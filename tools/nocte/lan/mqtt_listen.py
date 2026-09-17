"""Listen to a Bambu printer's MQTT report topic in LAN Developer Mode.

Read-only. It connects (TLS 8883, MQTT 3.1.1, user ``bblp``, password = access
code, self-signed certificate accepted, keepalive 60), subscribes to EXACTLY
``device/<serial>/report`` - wildcards get the client disconnected - publishes
``pushall`` and ``get_version``, and prints a REDACTED key-structure summary of
what comes back. Values are never printed except for a small allowlist of
enum-ish fields that carry no identity.

Requires paho-mqtt (tested with 2.1.0; the callbacks below are written to work
with both the 1.x and 2.x callback APIs).

    python mqtt_listen.py --seconds 30
    python mqtt_listen.py --seconds 30 --raw   # raw payloads -> _captures/ (gitignored)
"""

from __future__ import annotations

import argparse
import json
import ssl
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from credentials import Emitter, Printer, redact, require_printer

USER = "bblp"
PORT = 8883
KEEPALIVE = 60
CONNACK_WAIT = 15

PUSHALL = {"pushing": {"sequence_id": "1", "command": "pushall"}}
GET_VERSION = {"info": {"sequence_id": "2", "command": "get_version"}}

# Leaf values safe to print: enums and counters with no identity content.
SAFE_VALUES = {
    "gcode_state", "print_type", "mc_percent", "mc_remaining_time",
    "layer_num", "total_layer_num", "print_error", "command", "msg",
    "sequence_id", "stg_cur", "bed_temper", "bed_target_temper",
    "nozzle_temper", "nozzle_target_temper", "wifi_signal", "spd_lvl",
    "ams_status", "ams_rfid_status", "hw_switch_state", "home_flag",
}

CAPTURE_DIR = Path(__file__).resolve().parent / "_captures"


def tls_context() -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def make_client(printer: Printer, client_id: str, clean_session: bool = True,
                protocol=mqtt.MQTTv311):
    """A paho client configured the way the printer expects it."""
    try:                                    # paho 2.x
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=client_id, protocol=protocol,
                             clean_session=clean_session)
    except AttributeError:                  # paho 1.x
        client = mqtt.Client(client_id=client_id, protocol=protocol,
                             clean_session=clean_session)
    client.username_pw_set(USER, printer.access_code)
    client.tls_set_context(tls_context())
    return client


def rc_value(rc) -> int:
    """paho 2.x hands back a ReasonCode object; 1.x an int."""
    return int(getattr(rc, "value", rc) if rc is not None else -1)


def key_structure(obj, prefix: str = "", depth: int = 0,
                  max_depth: int = 4) -> list[str]:
    """Key paths only, with types. Values appear only for SAFE_VALUES leaves."""
    out: list[str] = []
    if isinstance(obj, dict):
        for key in obj:
            path = f"{prefix}.{key}" if prefix else key
            value = obj[key]
            if isinstance(value, (dict, list)) and depth < max_depth:
                kind = "object" if isinstance(value, dict) else f"array[{len(value)}]"
                out.append(f"{path}: {kind}")
                out.extend(key_structure(value, path, depth + 1, max_depth))
            elif key in SAFE_VALUES and not isinstance(value, (dict, list)):
                out.append(f"{path} = {value!r}")
            else:
                out.append(f"{path}: {type(value).__name__}")
    elif isinstance(obj, list):
        if obj and depth < max_depth:
            out.extend(key_structure(obj[0], f"{prefix}[0]", depth + 1, max_depth))
    return out


class Session:
    """One MQTT connection, with everything the probe needs to measure."""

    def __init__(self, printer: Printer, say: Emitter, client_id: str,
                 clean_session: bool = True, keepalive: int = KEEPALIVE,
                 topic: str | None = None, verbose: bool = False):
        self.printer = printer
        self.say = say
        self.keepalive = keepalive
        self.topic = topic or f"device/{printer.serial}/report"
        self.request_topic = f"device/{printer.serial}/request"
        self.verbose = verbose

        self.client = make_client(printer, client_id, clean_session)
        self.connack: int | None = None
        self.connack_at: float | None = None
        self.granted_qos: list | None = None
        self.suback_at: float | None = None
        self.disconnect_rc: int | None = None
        self.disconnect_at: float | None = None
        self.messages: list[tuple[float, bytes]] = []
        self.pings = {"req": 0, "resp": 0}
        self.connected = threading.Event()
        self.t0 = time.perf_counter()

        self.client.on_connect = self._on_connect
        self.client.on_subscribe = self._on_subscribe
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.client.on_log = self._on_log

    # -- callbacks (written to fit both paho callback APIs) -------------------

    def _on_connect(self, *args) -> None:
        self.connack = rc_value(args[3])
        self.connack_at = time.perf_counter()
        self.connected.set()

    def _on_subscribe(self, *args) -> None:
        granted = args[3]
        self.granted_qos = [rc_value(g) for g in granted] if isinstance(
            granted, (list, tuple)) else [rc_value(granted)]
        self.suback_at = time.perf_counter()

    def _on_message(self, *args) -> None:
        msg = args[2]
        self.messages.append((time.perf_counter(), msg.payload))

    def _on_disconnect(self, *args) -> None:
        rc = args[2] if len(args) == 3 else args[3]
        self.disconnect_rc = rc_value(rc)
        self.disconnect_at = time.perf_counter()

    def _on_log(self, *args) -> None:
        text = args[-1]
        if "PINGREQ" in text:
            self.pings["req"] += 1
        elif "PINGRESP" in text:
            self.pings["resp"] += 1
        if self.verbose:
            self.say("    log:", redact(str(text), self.printer))

    # -- lifecycle -----------------------------------------------------------

    def connect(self) -> bool:
        self.t0 = time.perf_counter()
        self.client.connect(self.printer.ip, PORT, keepalive=self.keepalive)
        self.client.loop_start()
        return self.connected.wait(CONNACK_WAIT)

    def subscribe(self, topic: str | None = None, qos: int = 0):
        return self.client.subscribe(topic or self.topic, qos=qos)

    def publish(self, payload: dict, qos: int = 0):
        return self.client.publish(self.request_topic, json.dumps(payload), qos=qos)

    def stop(self) -> None:
        try:
            self.client.disconnect()
        except Exception:                                      # noqa: BLE001
            pass
        self.client.loop_stop()

    # -- derived measurements ------------------------------------------------

    def decoded(self) -> list[tuple[float, dict]]:
        out = []
        for ts, payload in self.messages:
            try:
                out.append((ts, json.loads(payload.decode("utf-8"))))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
        return out

    def max_payload(self) -> int:
        return max((len(p) for _, p in self.messages), default=0)


def save_raw(printer: Printer, session: Session, say: Emitter) -> None:
    """Raw payloads go ONLY to the gitignored capture directory."""
    CAPTURE_DIR.mkdir(exist_ok=True)
    path = CAPTURE_DIR / f"report-{time.strftime('%Y%m%dT%H%M%S')}.jsonl"
    with path.open("w", encoding="utf-8") as fh:
        for ts, payload in session.messages:
            fh.write(json.dumps({"t": round(ts - session.t0, 3),
                                 "payload": payload.decode("utf-8", "replace")}) + "\n")
    say(f"raw capture written to _captures/{path.name} (gitignored, NOT scrubbed)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--printer", default="A4")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--raw", action="store_true",
                    help="also write raw payloads to the gitignored _captures/")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    printer = require_printer(args.printer)
    say = Emitter(printer)
    say(f"MQTT {printer!r} port {PORT} topic device/<serial>/report")

    session = Session(printer, say, client_id=f"nocte-listen-{int(time.time())}",
                      verbose=args.verbose)
    if not session.connect():
        say("FAIL: no CONNACK within", str(CONNACK_WAIT), "s")
        session.stop()
        return 2
    say(f"CONNACK rc={session.connack} after "
        f"{(session.connack_at - session.t0) * 1000:.0f} ms")
    if session.connack != 0:
        say("FAIL: rc 4/5 means the access code is wrong")
        session.stop()
        return 2

    session.subscribe()
    session.publish(PUSHALL)
    session.publish(GET_VERSION)

    deadline = time.perf_counter() + args.seconds
    while time.perf_counter() < deadline:
        time.sleep(0.2)

    reports = session.decoded()
    say(f"{len(reports)} reports in {args.seconds:.0f} s, "
        f"max payload {session.max_payload()} bytes")
    for index, (ts, report) in enumerate(reports):
        top = ", ".join(report.keys())
        say(f"--- report {index} at t+{ts - session.t0:.2f}s top-level: {top}")
        if index < 2:
            for line in key_structure(report):
                say("    " + line)
        else:
            say("    keys: " + ", ".join(key_structure(report)[:40]))
    if args.raw:
        save_raw(printer, session, say)
    session.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
