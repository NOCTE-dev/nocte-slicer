"""One-shot, read-only protocol probe of a Bambu printer in LAN Developer Mode.

Prints a PASS/FAIL checklist with everything the C++ ``NocteLanPrinterAgent``
needs to be written from: TCP reachability, TLS parameters, MQTT CONNACK/SUBACK
behaviour, report shape and cadence, payload sizes, client-id and keepalive
behaviour, and whether a wildcard subscription really does get the client
dropped.

Nothing here changes printer state. The only packets sent are CONNECT,
SUBSCRIBE, PINGREQ, DISCONNECT and two read-only requests (``pushall``,
``get_version``). All output is redacted.

    python a4_probe.py                 # ~60 s
    python a4_probe.py --seconds 30
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import ssl
import tempfile
import time
import uuid

import paho.mqtt.client as mqtt

from credentials import Emitter, Printer, redact, require_printer
from mqtt_listen import (GET_VERSION, PUSHALL, Session, key_structure,
                         tls_context)

MQTT_PORT = 8883
FTPS_PORT = 990


class Checklist:
    def __init__(self, say: Emitter):
        self.say = say
        self.rows: list[tuple[str, str, str]] = []

    def add(self, status: str, name: str, detail: str = "") -> None:
        self.rows.append((status, name, detail))
        self.say(f"[{status:4}] {name}" + (f" -- {detail}" if detail else ""))

    def info(self, name: str, detail: str) -> None:
        self.add("INFO", name, detail)

    def summary(self) -> None:
        self.say("")
        self.say("=" * 72)
        self.say("CHECKLIST")
        self.say("=" * 72)
        for status, name, detail in self.rows:
            self.say(f"[{status:4}] {name}" + (f" -- {detail}" if detail else ""))
        fails = [r for r in self.rows if r[0] == "FAIL"]
        self.say("")
        self.say(f"{len(self.rows)} checks, {len(fails)} FAIL")


# --- layer 1/2: TCP and TLS ---------------------------------------------------

def tcp_probe(check: Checklist, printer: Printer, port: int, label: str) -> bool:
    t0 = time.perf_counter()
    try:
        with socket.create_connection((printer.ip, port), timeout=5):
            dt = (time.perf_counter() - t0) * 1000
        check.add("PASS", f"TCP {port} ({label}) reachable", f"{dt:.0f} ms")
        return True
    except OSError as exc:
        check.add("FAIL", f"TCP {port} ({label}) reachable",
                  f"{type(exc).__name__}: {exc}")
        return False


def _cert_subject(der: bytes) -> str:
    """Decode the peer certificate's subject without a third-party X.509 lib."""
    try:
        pem = ssl.DER_cert_to_PEM_cert(der)
        fd, path = tempfile.mkstemp(suffix=".pem")
        try:
            with os.fdopen(fd, "w") as fh:
                fh.write(pem)
            decoded = ssl._ssl._test_decode_cert(path)   # noqa: SLF001
        finally:
            os.unlink(path)
        subject = "/".join(f"{k}={v}" for rdn in decoded.get("subject", ())
                           for k, v in rdn)
        issuer = "/".join(f"{k}={v}" for rdn in decoded.get("issuer", ())
                          for k, v in rdn)
        return (f"subject={subject} issuer={issuer} "
                f"notAfter={decoded.get('notAfter')} "
                f"selfSigned={subject == issuer}")
    except Exception as exc:                                   # noqa: BLE001
        return f"could not decode ({type(exc).__name__})"


def tls_probe(check: Checklist, printer: Printer, port: int, label: str) -> None:
    ctx = tls_context()
    try:
        raw = socket.create_connection((printer.ip, port), timeout=10)
        t0 = time.perf_counter()
        # No server_hostname at all: if the handshake completes, the server
        # does not demand SNI (matters for boost::asio / OpenSSL setup).
        with ctx.wrap_socket(raw) as tls:
            dt = (time.perf_counter() - t0) * 1000
            cipher = tls.cipher()
            der = tls.getpeercert(binary_form=True)
            check.add("PASS", f"TLS {port} ({label}) handshake without SNI",
                      f"{dt:.0f} ms, {cipher[0]} {cipher[1]} {cipher[2]} bits")
            check.info(f"TLS {port} certificate", _cert_subject(der))
            check.info(f"TLS {port} client certificate demanded", "no "
                       "(handshake completed with no client cert configured)")
    except Exception as exc:                                   # noqa: BLE001
        check.add("FAIL", f"TLS {port} ({label}) handshake without SNI",
                  f"{type(exc).__name__}: {exc}")
        return

    # Same handshake but WITH an SNI name, to prove the server tolerates it.
    try:
        raw = socket.create_connection((printer.ip, port), timeout=10)
        with ctx.wrap_socket(raw, server_hostname="nocte.invalid") as tls:
            check.add("PASS", f"TLS {port} handshake with arbitrary SNI",
                      f"{tls.cipher()[1]}, server ignores the name")
    except Exception as exc:                                   # noqa: BLE001
        check.add("WARN", f"TLS {port} handshake with arbitrary SNI",
                  f"{type(exc).__name__}: {exc}")


# --- layer 3: MQTT ------------------------------------------------------------

def mqtt_probe(check: Checklist, printer: Printer, say: Emitter,
               seconds: float) -> None:
    client_id = f"nocte-probe-{int(time.time())}"
    check.info("MQTT parameters",
               f"protocol=MQTTv311 (level 4), clean_session=True, "
               f"keepalive=60, user=bblp, client_id='{client_id}' "
               f"(FDM-HUB uses 'fdmhub-<id>-<epoch>')")

    session = Session(printer, say, client_id=client_id)
    try:
        if not session.connect():
            check.add("FAIL", "MQTT CONNACK", "no CONNACK within 15 s")
            return
        rc = session.connack
        check.add("PASS" if rc == 0 else "FAIL", "MQTT CONNACK",
                  f"rc={rc} after {(session.connack_at - session.t0) * 1000:.0f} ms"
                  + ("" if rc == 0 else " (4/5 = wrong access code)"))
        if rc != 0:
            return

        session.subscribe()
        t_sub = time.perf_counter()
        session.publish(PUSHALL)
        session.publish(GET_VERSION)

        deadline = time.perf_counter() + seconds
        while time.perf_counter() < deadline:
            time.sleep(0.2)

        if session.granted_qos is not None:
            check.add("PASS", "SUBACK on exact topic device/<serial>/report",
                      f"granted QoS {session.granted_qos} after "
                      f"{(session.suback_at - t_sub) * 1000:.0f} ms")
        else:
            check.add("FAIL", "SUBACK on exact topic", "no SUBACK seen")

        reports = session.decoded()
        if not reports:
            check.add("FAIL", "reports received", f"none in {seconds:.0f} s")
            return

        first_ts, first = reports[0]
        check.add("PASS", "time to first report after pushall",
                  f"{(first_ts - t_sub) * 1000:.0f} ms")
        check.info("top-level keys seen",
                   ", ".join(sorted({k for _, r in reports for k in r})))

        # Is the first answer a full 'print' object?
        full = None
        for ts, report in reports:
            if "print" in report and len(report["print"]) > 20:
                full = (ts, report["print"])
                break
        if full is None:
            check.add("FAIL", "pushall answered with a full 'print' object",
                      "no report carried a large 'print' object")
        else:
            ts, block = full
            is_first = ts == first_ts
            check.add("PASS", "pushall answered with a full 'print' object",
                      f"{len(block)} keys, "
                      f"{'first message' if is_first else 'not the first message'}")
            check.info("keys inside 'print' (names only)",
                       ", ".join(sorted(block.keys())))
            check.info("gcode_state", repr(block.get("gcode_state")))
            ams = block.get("ams")
            if ams is None:
                check.info("ams block", "absent")
            else:
                shape = key_structure({"ams": ams}, max_depth=5)
                check.info("ams block shape (names only)",
                           "; ".join(shape[:40]))

        sizes = [len(p) for _, p in session.messages]
        check.info("message rate",
                   f"{len(session.messages)} messages in {seconds:.0f} s "
                   f"= {len(session.messages) / seconds:.2f} msg/s")
        check.info("payload sizes",
                   f"max {max(sizes)} B, min {min(sizes)} B, "
                   f"median {sorted(sizes)[len(sizes) // 2]} B "
                   f"(>127 B needs multi-byte MQTT 'remaining length')")

        if full is not None and len(reports) > 1:
            full_ts, full_block = full
            later = [len(r["print"]) for ts, r in reports
                     if "print" in r and ts > full_ts]
            smaller = [n for n in later if 0 < n < len(full_block)]
            check.add("PASS" if smaller else "WARN",
                      "subsequent reports are deltas",
                      f"later 'print' objects carry "
                      f"{sorted(set(later))[:8]} keys vs {len(full_block)} "
                      f"in the full one")

        version_reports = [r for _, r in reports if "info" in r]
        if version_reports:
            info = version_reports[0]["info"]
            modules = info.get("module", [])
            check.add("PASS", "get_version answered",
                      f"info.command={info.get('command')!r}, "
                      f"{len(modules)} modules, keys per module: "
                      f"{sorted(modules[0].keys()) if modules else 'n/a'}")
        else:
            check.add("WARN", "get_version answered",
                      "no top-level 'info' report within the window")

        # QoS 1 publish: still only a pushall, still read-only.
        info = session.publish(PUSHALL, qos=1)
        try:
            info.wait_for_publish(timeout=5)
            check.add("PASS" if info.is_published() else "FAIL",
                      "QoS 1 publish accepted (PUBACK)",
                      f"rc={rc_or(info)}, published={info.is_published()}")
        except Exception as exc:                               # noqa: BLE001
            check.add("FAIL", "QoS 1 publish accepted (PUBACK)",
                      f"{type(exc).__name__}: {exc}")

        check.info("disconnect during session",
                   "none" if session.disconnect_rc is None
                   else f"rc={session.disconnect_rc}")
    finally:
        session.stop()


def rc_or(info) -> str:
    return str(getattr(info, "rc", "?"))


def random_client_id_probe(check: Checklist, printer: Printer,
                           say: Emitter) -> None:
    cid = uuid.uuid4().hex[:20]
    session = Session(printer, say, client_id=cid)
    try:
        ok = session.connect()
        check.add("PASS" if ok and session.connack == 0 else "FAIL",
                  "random client id accepted",
                  f"len={len(cid)} hex, CONNACK rc={session.connack}")
    except Exception as exc:                                   # noqa: BLE001
        check.add("FAIL", "random client id accepted",
                  f"{type(exc).__name__}: {exc}")
    finally:
        session.stop()


def keepalive_probe(check: Checklist, printer: Printer, say: Emitter) -> None:
    """Short keepalive so PINGREQ/PINGRESP can be observed in seconds."""
    session = Session(printer, say, client_id=f"nocte-ping-{int(time.time())}",
                      keepalive=5)
    try:
        if not session.connect() or session.connack != 0:
            check.add("FAIL", "PINGREQ keeps the session alive",
                      f"CONNACK rc={session.connack}")
            return
        time.sleep(14)                    # ~2-3 keepalive intervals, idle
        alive = session.disconnect_rc is None
        check.add("PASS" if alive and session.pings["resp"] > 0 else "WARN",
                  "PINGREQ keeps the session alive (keepalive=5, idle 14 s)",
                  f"PINGREQ={session.pings['req']} PINGRESP={session.pings['resp']} "
                  f"disconnect_rc={session.disconnect_rc}")
    finally:
        session.stop()


def wildcard_probe(check: Checklist, printer: Printer, say: Emitter) -> None:
    """ADR-002 claims a wildcard SUBSCRIBE gets the client dropped. Verify.

    Harmless: a SUBSCRIBE changes nothing on the printer. Done last, on its own
    short-lived connection, so a disconnect cannot spoil the measurements above.
    """
    for topic in (f"device/{printer.serial}/+", "device/+/report", "#"):
        session = Session(printer, say,
                          client_id=f"nocte-wild-{int(time.time() * 1000) % 100000}")
        try:
            if not session.connect() or session.connack != 0:
                check.add("FAIL", f"wildcard subscribe {topic!r}",
                          f"CONNACK rc={session.connack}")
                continue
            t0 = time.perf_counter()
            session.subscribe(topic)
            deadline = time.perf_counter() + 5
            while (time.perf_counter() < deadline
                   and session.disconnect_rc is None
                   and session.granted_qos is None):
                time.sleep(0.05)
            if session.disconnect_rc is not None:
                check.add("PASS", f"wildcard subscribe {topic!r} is refused",
                          f"DISCONNECT rc={session.disconnect_rc} after "
                          f"{(session.disconnect_at - t0) * 1000:.0f} ms "
                          f"(ADR-002 predicted rc=128)")
            elif session.granted_qos is not None:
                check.add("WARN", f"wildcard subscribe {topic!r} is refused",
                          f"NOT refused: SUBACK granted {session.granted_qos}, "
                          f"{len(session.messages)} messages - deviates from ADR-002")
            else:
                check.add("WARN", f"wildcard subscribe {topic!r} is refused",
                          "neither SUBACK nor DISCONNECT within 5 s (silently dropped)")
        finally:
            session.stop()
            time.sleep(0.5)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--printer", default="A4")
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="length of the MQTT listening window")
    ap.add_argument("--skip-wildcard", action="store_true")
    ap.add_argument("--skip-keepalive", action="store_true")
    args = ap.parse_args()

    printer = require_printer(args.printer)
    say = Emitter(printer)
    check = Checklist(say)
    say(f"NOCTE LAN probe of {printer!r}")
    import paho.mqtt as paho_pkg
    say(f"paho-mqtt {getattr(paho_pkg, '__version__', 'unknown')} "
        f"(>= 2.0 uses CallbackAPIVersion.VERSION2)")
    say("")

    mqtt_up = tcp_probe(check, printer, MQTT_PORT, "MQTT")
    ftps_up = tcp_probe(check, printer, FTPS_PORT, "FTPS")
    if mqtt_up:
        tls_probe(check, printer, MQTT_PORT, "MQTT")
    if ftps_up:
        tls_probe(check, printer, FTPS_PORT, "FTPS implicit")
    if mqtt_up:
        mqtt_probe(check, printer, say, args.seconds)
        random_client_id_probe(check, printer, say)
        if not args.skip_keepalive:
            keepalive_probe(check, printer, say)
        if not args.skip_wildcard:
            wildcard_probe(check, printer, say)

    check.summary()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
