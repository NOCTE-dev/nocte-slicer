"""FTPS against a Bambu printer in LAN Developer Mode (port 990, implicit TLS).

Why the subclass: port 990 speaks **implicit** TLS - the handshake starts on
the first byte. ``ftplib.FTP_TLS`` does *explicit* TLS (plaintext connect, then
``AUTH TLS``), so without wrapping the socket inside ``connect()`` the symptom
is a hang until timeout, not a readable error. Ported from FDM-HUB
``fdmhub/enlace/bambu_ftps.py``.

TLS session reuse on the data channel comes for free once ``self.sock`` is a
real ``ssl.SSLSocket``: CPython's ``FTP_TLS.ntransfercmd`` wraps the data
connection with ``session=self.sock.session``. The Bambu server requires it -
a data connection negotiating a fresh session is dropped.

Success of an upload is decided by re-listing (``NLST``), never by the reply
code: the server frequently omits the final ``226``.

Usage (all output redacted):
    python ftps_upload.py list
    python ftps_upload.py upload <path> [--as nocte_lan_probe.gcode.3mf]
    python ftps_upload.py delete nocte_lan_probe.gcode.3mf
"""

from __future__ import annotations

import argparse
import ftplib
import ssl
import time
from pathlib import Path

from credentials import Emitter, Printer, redact, require_printer

USER = "bblp"
PORT = 990
TIMEOUT = 60
BLOCK = 32768
EXTENSION = ".3mf"
# Only names this tool created may be deleted, so a probe run can never remove
# a real job from the SD card.
DELETE_PREFIX = "nocte_"


class ImplicitFTP_TLS(ftplib.FTP_TLS):
    """FTP_TLS that wraps the control socket on connect, not afterwards."""

    def __init__(self, *args, **kwargs):
        self._sock = None
        self.replies: list[str] = []
        super().__init__(*args, **kwargs)

    @property
    def sock(self):
        return self._sock

    @sock.setter
    def sock(self, value):
        if value is not None and not isinstance(value, ssl.SSLSocket):
            value = self.context.wrap_socket(value)
        self._sock = value

    def getresp(self):  # record every server reply, redacted by the caller
        resp = super().getresp()
        self.replies.append(resp.splitlines()[0][:200])
        return resp


def tls_context() -> ssl.SSLContext:
    """Self-signed printer certificate; the channel never leaves the LAN."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def connect(printer: Printer, say: Emitter) -> ImplicitFTP_TLS:
    t0 = time.perf_counter()
    ftp = ImplicitFTP_TLS(context=tls_context(), timeout=TIMEOUT)
    ftp.connect(printer.ip, PORT)
    t_conn = time.perf_counter()
    ftp.login(USER, printer.access_code)
    t_login = time.perf_counter()
    ftp.prot_p()                       # encrypt the data channel too
    t_prot = time.perf_counter()

    cipher = ftp.sock.cipher() if isinstance(ftp.sock, ssl.SSLSocket) else None
    say(f"  connect {(t_conn - t0) * 1000:.0f} ms | login "
        f"{(t_login - t_conn) * 1000:.0f} ms | PROT P "
        f"{(t_prot - t_login) * 1000:.0f} ms")
    if cipher:
        say(f"  tls: {cipher[1]} cipher={cipher[0]} bits={cipher[2]}")
    return ftp


def _close(ftp: ftplib.FTP) -> None:
    try:
        ftp.quit()
    except Exception:                                          # noqa: BLE001
        try:
            ftp.close()
        except Exception:                                      # noqa: BLE001
            pass


def do_list(printer: Printer, say: Emitter, quiet: bool = False) -> list[str]:
    ftp = connect(printer, say)
    try:
        t0 = time.perf_counter()
        entries = ftp.nlst()
        dt = (time.perf_counter() - t0) * 1000
    finally:
        replies = list(ftp.replies)
        _close(ftp)
    if not quiet:
        say(f"  NLST {dt:.0f} ms, {len(entries)} entries")
        for name in sorted(entries):
            say(f"    {name}")
        say("  replies: " + " | ".join(replies))
    return entries


def do_upload(printer: Printer, say: Emitter, source: Path,
              name: str | None = None) -> bool:
    source = Path(source)
    if not source.is_file():
        raise SystemExit(f"no such file: {source}")
    name = name or source.name
    if not name.lower().endswith(EXTENSION):
        raise SystemExit("the printer only starts .gcode.3mf files")
    if "/" in name or "\\" in name:
        raise SystemExit("name must not contain a path")

    size = source.stat().st_size
    sent = [0]

    def progress(chunk: bytes) -> None:
        sent[0] += len(chunk)

    say(f"  STOR {name} ({size} bytes, {BLOCK} B blocks)")
    ftp = connect(printer, say)
    failure = None
    t0 = time.perf_counter()
    try:
        with source.open("rb") as fh:
            ftp.storbinary(f"STOR {name}", fh, blocksize=BLOCK, callback=progress)
    except Exception as exc:                                   # noqa: BLE001
        failure = exc
    dt = time.perf_counter() - t0
    replies = list(ftp.replies)
    _close(ftp)

    say(f"  transfer {dt:.2f} s, {sent[0]} bytes pushed"
        f"{'' if failure is None else ' (exception: ' + type(failure).__name__ + ')'}")
    if failure is not None:
        say("  note: exception during STOR is expected when the server omits 226")
    say("  replies: " + " | ".join(replies))

    # Success is decided here, not by the reply code.
    present = name in do_list(printer, say, quiet=True)
    say(f"  verified by NLST: {'YES' if present else 'NO'}")
    return present


def do_delete(printer: Printer, say: Emitter, name: str) -> bool:
    if not name.startswith(DELETE_PREFIX):
        raise SystemExit(f"refusing to delete {name!r}: this tool may only "
                         f"delete files it created ({DELETE_PREFIX}*)")
    if "/" in name or "\\" in name:
        raise SystemExit("name must not contain a path")
    ftp = connect(printer, say)
    try:
        ftp.delete(name)
    finally:
        replies = list(ftp.replies)
        _close(ftp)
    say("  replies: " + " | ".join(replies))
    gone = name not in do_list(printer, say, quiet=True)
    say(f"  verified gone by NLST: {'YES' if gone else 'NO'}")
    return gone


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--printer", default="A4")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    up = sub.add_parser("upload")
    up.add_argument("file")
    up.add_argument("--as", dest="as_name", default=None)
    rm = sub.add_parser("delete")
    rm.add_argument("name")
    args = ap.parse_args()

    printer = require_printer(args.printer)
    say = Emitter(printer)
    say(f"FTPS {printer!r} port {PORT} user {USER}")
    try:
        if args.cmd == "list":
            do_list(printer, say)
        elif args.cmd == "upload":
            ok = do_upload(printer, say, Path(args.file), args.as_name)
            return 0 if ok else 1
        else:
            ok = do_delete(printer, say, args.name)
            return 0 if ok else 1
    except Exception as exc:                                   # noqa: BLE001
        say(f"FAILED {type(exc).__name__}: {redact(str(exc), printer)}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
