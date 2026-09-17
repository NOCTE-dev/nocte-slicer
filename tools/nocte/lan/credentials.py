"""Credential resolution and output redaction for the NOCTE LAN probes.

Two jobs, both security-critical:

1. Find the IP / serial / access code of a printer at RUNTIME, never storing
   them in this repository.
2. Make it impossible for those values to reach stdout, a log, a report or a
   committed file: every script in this directory routes its output through
   :func:`redact`.

Resolution order for ``load_printer("A4")``:

  a. Environment: ``NOCTE_LAN_IP`` / ``NOCTE_LAN_SERIAL`` /
     ``NOCTE_LAN_ACCESS_CODE`` (all three must be set).
  b. The user's existing FDM-HUB tool, read-only, at
     ``%USERPROFILE%\\Documents\\FDM-HUB`` (override with ``NOCTE_FDMHUB_DIR``),
     using FDM-HUB's own lookup (``fdmhub/config.py``):
       * ``config/impresoras.json`` -> ``impresoras[]`` with ``id``/``ip``/``serie``
       * ``.env`` -> ``FDM_<ID>_CODIGO`` (access code), and optionally
         ``FDM_<ID>_IP`` / ``FDM_<ID>_SERIE``, which win over the JSON.

Nothing here writes anything back to FDM-HUB.
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

# --- masks -------------------------------------------------------------------

IP_MASK = "<ip>"
CODE_MASK = "<access-code>"
SERIAL_MASK = "<serial>"

# Generic fallbacks, applied even when we do not know the exact value: they
# catch values that arrive from the printer itself (MQTT payloads name serials
# and IPs of their own accord).
_RE_IPV4 = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
# Bambu firmware versions are dotted quads too ("01.04.00.00"), so the IPv4 rule
# swallows them. They are exempted only in their canonical form: four groups of
# exactly two digits with at least one leading zero, which no canonically
# written IPv4 address has.
_RE_FIRMWARE = re.compile(r"^(?=.*\b0\d\b)\d{2}\.\d{2}\.\d{2}\.\d{2}$")
_RE_SERIAL = re.compile(r"\b(?=[0-9A-Z]*[0-9])(?=[0-9A-Z]*[A-Z])[0-9A-Z]{12,20}\b")
_RE_CODE = re.compile(r"\b\d{8}\b")
_RE_MAC = re.compile(r"\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b")
# FTP's 227 PASV reply spells the address as h1,h2,h3,h4,p1,p2 - the dotted-quad
# pattern above does not see it. MEASURED: without this rule the printer's own
# IP reaches stdout on every data transfer.
_RE_PASV = re.compile(r"\b(?:\d{1,3},){3}\d{1,3}(?:,\d{1,3},\d{1,3})?\b")


@dataclass
class Printer:
    """A LAN printer. Never print the raw fields; use ``redact`` or ``repr``."""

    name: str
    ip: str
    serial: str
    access_code: str = field(repr=False, default="")
    source: str = "unknown"

    # --- safe presentation ---------------------------------------------------

    @property
    def serial_hint(self) -> str:
        """First three characters of the serial, the rest elided."""
        return (self.serial[:3] + "...") if self.serial else "<none>"

    def __repr__(self) -> str:  # noqa: D105 - redacted on purpose
        return (f"Printer(name={self.name!r}, ip={IP_MASK}, "
                f"serial={self.serial_hint!r}, access_code={CODE_MASK}, "
                f"source={self.source!r})")

    __str__ = __repr__

    def complete(self) -> bool:
        return bool(self.ip and self.serial and self.access_code)

    def missing(self) -> list[str]:
        return [n for n, v in (("ip", self.ip), ("serial", self.serial),
                               ("access_code", self.access_code)) if not v]


# --- redaction ---------------------------------------------------------------

def redact(text: object, printer: Printer | None = None) -> str:
    """Return ``text`` with every credential-shaped value masked.

    Exact values of ``printer`` are replaced first (longest first, so that a
    substring never shadows a longer match), then the generic patterns.
    """
    s = text if isinstance(text, str) else repr(text)

    if printer is not None:
        exact: list[tuple[str, str]] = []
        if printer.access_code:
            exact.append((printer.access_code, CODE_MASK))
        if printer.serial:
            exact.append((printer.serial, SERIAL_MASK))
        if printer.ip:
            exact.append((printer.ip, IP_MASK))
        for value, mask in sorted(exact, key=lambda kv: -len(kv[0])):
            s = s.replace(value, mask)

    if printer is not None and printer.ip:
        # The same IP as h1,h2,h3,h4 inside a PASV reply.
        s = s.replace(printer.ip.replace(".", ","), "<pasv-host>")
    s = _RE_MAC.sub("<mac>", s)
    s = _RE_PASV.sub("<pasv-addr>", s)
    s = _RE_IPV4.sub(lambda m: m.group(0) if _RE_FIRMWARE.match(m.group(0))
                     else "x.x.x.x", s)
    s = _RE_SERIAL.sub(SERIAL_MASK, s)
    s = _RE_CODE.sub(CODE_MASK, s)
    return s


class Emitter:
    """``print`` that cannot leak. Every script uses this instead of print."""

    def __init__(self, printer: Printer | None = None):
        self.printer = printer

    def __call__(self, *parts: object) -> None:
        line = " ".join(p if isinstance(p, str) else repr(p) for p in parts)
        print(redact(line, self.printer), flush=True)


# --- lookup ------------------------------------------------------------------

def _fdmhub_dir() -> Path:
    override = os.environ.get("NOCTE_FDMHUB_DIR")
    if override:
        return Path(override)
    home = os.environ.get("USERPROFILE") or str(Path.home())
    return Path(home) / "Documents" / "FDM-HUB"


def _read_env_file(path: Path) -> dict[str, str]:
    """FDM-HUB's ``.env`` format: KEY=VALUE, ``#`` comments, optional quotes."""
    values: dict[str, str] = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def _from_fdmhub(name: str) -> Printer:
    root = _fdmhub_dir()
    ident = name.upper()
    env = _read_env_file(root / ".env")
    # Real environment wins, exactly as FDM-HUB's cargar_env() does.
    for key, value in os.environ.items():
        if key.startswith("FDM_"):
            env[key] = value

    ip = env.get(f"FDM_{ident}_IP", "")
    serial = env.get(f"FDM_{ident}_SERIE", "")

    inventory = root / "config" / "impresoras.json"
    if inventory.exists():
        try:
            data = json.loads(inventory.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            data = {}
        for entry in data.get("impresoras", []):
            if str(entry.get("id", "")).upper() != ident:
                continue
            ip = ip or str(entry.get("ip", ""))
            serial = serial or str(entry.get("serie", ""))
            break

    return Printer(name=name, ip=ip, serial=serial,
                   access_code=env.get(f"FDM_{ident}_CODIGO", ""),
                   source=f"fdmhub:{root.name}")


def load_printer(name: str = "A4") -> Printer:
    """Resolve a printer's credentials. Raises nothing; check ``complete()``."""
    env_ip = os.environ.get("NOCTE_LAN_IP", "")
    env_serial = os.environ.get("NOCTE_LAN_SERIAL", "")
    env_code = os.environ.get("NOCTE_LAN_ACCESS_CODE", "")
    if env_ip and env_serial and env_code:
        return Printer(name=name, ip=env_ip, serial=env_serial,
                       access_code=env_code, source="env:NOCTE_LAN_*")
    return _from_fdmhub(name)


def require_printer(name: str = "A4") -> Printer:
    printer = load_printer(name)
    if not printer.complete():
        raise SystemExit(
            f"missing credentials for {name}: {', '.join(printer.missing())}. "
            "Set NOCTE_LAN_IP / NOCTE_LAN_SERIAL / NOCTE_LAN_ACCESS_CODE, or "
            "point NOCTE_FDMHUB_DIR at an FDM-HUB checkout.")
    return printer


if __name__ == "__main__":  # pragma: no cover - manual smoke test
    import argparse

    ap = argparse.ArgumentParser(description="Show which credentials resolve "
                                             "(values are never printed).")
    ap.add_argument("--printer", default="A4")
    args = ap.parse_args()
    p = load_printer(args.printer)
    say = Emitter(p)
    say(repr(p))
    say("complete:", str(p.complete()), "missing:", str(p.missing()))
