"""Build (and, under protest, publish) the Bambu ``project_file`` command.

This is the ONE script here that can change printer state, so it is built to
refuse by default:

* ``--dry-run`` is the default: it validates the local ``.gcode.3mf``, prints
  the exact redacted JSON that would be published, and exits.
* Publishing requires BOTH ``--i-really-mean-it`` AND ``NOCTE_LAN_ALLOW_PRINT=1``
  in the environment, and it additionally refuses if the printer is not IDLE or
  FINISH according to a live ``pushall``.

Payload fields follow ADR-002 and FDM-HUB ``fdmhub/enlace/comandos.py``
(``imprimir``). The file must already be on the SD card (see ftps_upload.py);
``url`` addresses it as ``file:///sdcard/<name>``.

    python print_job.py --file ../../bbl-compat/corpus/fdmhub/P4_FLUJO_A1.gcode.3mf \\
                        --sd-name nocte_lan_probe.gcode.3mf
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import time
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

from credentials import Emitter, Printer, require_printer

PLATE_RE = re.compile(r"^Metadata/plate_(\d+)\.gcode$")


class ValidationError(RuntimeError):
    pass


def validate_3mf(path: Path, plate: int) -> dict:
    """Check the printer-side invariants before anything is published.

    * ``Metadata/plate_N.gcode`` exists,
    * its ``.md5`` sibling exists, is upper-case hex and matches the gcode,
    * ``slice_info.config`` is present; its ``<filament>`` count sizes
      ``ams_mapping``.
    """
    if not path.is_file():
        raise ValidationError(f"no such file: {path}")
    if not path.name.lower().endswith(".3mf"):
        raise ValidationError("the printer only starts .gcode.3mf files")

    with zipfile.ZipFile(path) as zf:
        names = set(zf.namelist())
        plates = sorted(int(m.group(1)) for m in
                        (PLATE_RE.match(n) for n in names) if m)
        if not plates:
            raise ValidationError("no Metadata/plate_N.gcode inside: this is a "
                                  "model .3mf, not a sliced one. The printer "
                                  "accepts the job and prints nothing.")
        if plate not in plates:
            raise ValidationError(f"plate {plate} not in this file (has {plates}); "
                                  "a param naming a missing plate is accepted "
                                  "and prints nothing")
        gcode_name = f"Metadata/plate_{plate}.gcode"
        md5_name = gcode_name + ".md5"
        if md5_name not in names:
            raise ValidationError(f"{md5_name} missing; the printer rejects the job")

        declared = zf.read(md5_name).decode("ascii", "replace").strip()
        actual = hashlib.md5(zf.read(gcode_name)).hexdigest().upper()
        if declared != declared.upper():
            raise ValidationError("plate md5 is not upper-case hex")
        if declared != actual:
            raise ValidationError("plate md5 does not match the gcode")

        filaments: list[dict] = []
        meta: dict[str, str] = {}
        if "Metadata/slice_info.config" in names:
            root = ET.fromstring(zf.read("Metadata/slice_info.config"))
            for plate_el in root.findall("plate"):
                index = next((m.get("value") for m in plate_el.findall("metadata")
                              if m.get("key") == "index"), None)
                if index is not None and int(index) != plate:
                    continue
                meta = {m.get("key"): m.get("value")
                        for m in plate_el.findall("metadata")}
                filaments = [dict(f.attrib) for f in plate_el.findall("filament")]
                break
        else:
            raise ValidationError("Metadata/slice_info.config missing")

    return {
        "plate": plate,
        "plates": plates,
        "gcode": gcode_name,
        "md5": declared,
        "size": path.stat().st_size,
        "filaments": filaments,
        "printer_model_id": meta.get("printer_model_id"),
        "nozzle_diameters": meta.get("nozzle_diameters"),
        "filament_maps": meta.get("filament_maps"),
        "label_object_enabled": meta.get("label_object_enabled"),
    }


def build_payload(sd_name: str, plate: int, *, use_ams: bool = False,
                  ams_mapping: list[int] | None = None,
                  bed_leveling: bool = True, timelapse: bool = False,
                  flow_cali: bool = False, vibration_cali: bool = True,
                  layer_inspect: bool = False,
                  sequence_id: str | None = None) -> dict:
    if "/" in sd_name or "\\" in sd_name:
        raise ValidationError("the SD name must not contain a path")
    if not sd_name.lower().endswith(".3mf"):
        raise ValidationError("the printer only starts .gcode.3mf files")

    payload = {
        "sequence_id": sequence_id or str(int(time.time()) % 100000),
        "command": "project_file",
        "param": f"Metadata/plate_{plate}.gcode",
        "url": f"file:///sdcard/{sd_name}",
        "subtask_name": sd_name,
        "use_ams": use_ams,
        "bed_leveling": bed_leveling,
        "timelapse": timelapse,
        "flow_cali": flow_cali,
        "vibration_cali": vibration_cali,
        "layer_inspect": layer_inspect,
        "bed_type": "auto",
        # The job does not come from Bambu's cloud: all four ids are "0".
        "profile_id": "0",
        "project_id": "0",
        "subtask_id": "0",
        "task_id": "0",
    }
    if use_ams:
        if not ams_mapping:
            raise ValidationError("use_ams needs an ams_mapping: one tray per "
                                  "<filament> in slice_info.config, in order")
        if any(not 0 <= t <= 3 for t in ams_mapping):
            raise ValidationError("tray indices run 0-3")
        if len(set(ams_mapping)) != len(ams_mapping):
            raise ValidationError("duplicate trays in ams_mapping: each filament "
                                  "of the file comes from a different tray")
        payload["ams_mapping"] = ams_mapping
    return {"print": payload}


def printer_is_idle(printer: Printer, say: Emitter) -> tuple[bool, str]:
    """Live gcode_state check; only used on the real-publish path."""
    from mqtt_listen import PUSHALL, Session

    session = Session(printer, say, client_id=f"nocte-guard-{int(time.time())}")
    try:
        if not session.connect() or session.connack != 0:
            return False, f"no usable MQTT session (CONNACK rc={session.connack})"
        session.subscribe()
        session.publish(PUSHALL)
        deadline = time.perf_counter() + 15
        state = None
        while time.perf_counter() < deadline:
            for _, report in session.decoded():
                candidate = report.get("print", {}).get("gcode_state")
                if candidate:
                    state = candidate
            if state:
                break
            time.sleep(0.25)
        if state is None:
            return False, "no gcode_state seen in 15 s"
        return state in ("IDLE", "FINISH", "FAILED"), f"gcode_state={state}"
    finally:
        session.stop()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--printer", default="A4")
    ap.add_argument("--file", required=True, help="local .gcode.3mf to validate")
    ap.add_argument("--sd-name", default=None,
                    help="name on the SD card (default: the local file name)")
    ap.add_argument("--plate", type=int, default=1)
    ap.add_argument("--use-ams", action="store_true")
    ap.add_argument("--tray", type=int, action="append", default=None,
                    help="repeat once per filament, in file order")
    ap.add_argument("--no-bed-leveling", action="store_true")
    ap.add_argument("--timelapse", action="store_true")
    ap.add_argument("--flow-cali", action="store_true")
    ap.add_argument("--no-vibration-cali", action="store_true")
    ap.add_argument("--layer-inspect", action="store_true")
    ap.add_argument("--dry-run", action="store_true", default=True)
    ap.add_argument("--i-really-mean-it", action="store_true",
                    help="together with NOCTE_LAN_ALLOW_PRINT=1, actually publish")
    args = ap.parse_args()

    printer = require_printer(args.printer)
    say = Emitter(printer)
    local = Path(args.file)
    sd_name = args.sd_name or local.name

    try:
        facts = validate_3mf(local, args.plate)
    except ValidationError as exc:
        raise SystemExit(f"invalid job file: {exc}")
    say(f"validated {local.name}: plates={facts['plates']} "
        f"gcode={facts['gcode']} md5=OK size={facts['size']} B")
    say(f"  printer_model_id={facts['printer_model_id']} "
        f"nozzle_diameters={facts['nozzle_diameters']} "
        f"filament_maps={facts['filament_maps']} "
        f"label_object_enabled={facts['label_object_enabled']}")
    say(f"  {len(facts['filaments'])} filament(s): " + ", ".join(
        f"id={f.get('id')} tray_info_idx={f.get('tray_info_idx')} "
        f"type={f.get('type')}" for f in facts['filaments']))

    mapping = args.tray
    if args.use_ams and mapping is None:
        mapping = [int(f.get("id", 1)) - 1 for f in facts["filaments"]]
        say(f"  ams_mapping defaulted from slice_info filament ids: {mapping}")
    if args.use_ams and len(mapping) != len(facts["filaments"]):
        raise SystemExit(f"ams_mapping needs exactly {len(facts['filaments'])} "
                         f"entries, got {len(mapping)}")

    payload = build_payload(
        sd_name, args.plate, use_ams=args.use_ams, ams_mapping=mapping,
        bed_leveling=not args.no_bed_leveling, timelapse=args.timelapse,
        flow_cali=args.flow_cali, vibration_cali=not args.no_vibration_cali,
        layer_inspect=args.layer_inspect)

    say("")
    say(f"would publish to device/<serial>/request (QoS 0):")
    say(json.dumps(payload, indent=2, ensure_ascii=False))
    say("")

    allowed = (args.i_really_mean_it
               and os.environ.get("NOCTE_LAN_ALLOW_PRINT") == "1")
    if not allowed:
        say("DRY RUN - nothing was published. Real publishing needs both "
            "--i-really-mean-it and NOCTE_LAN_ALLOW_PRINT=1.")
        return 0

    idle, why = printer_is_idle(printer, say)
    if not idle:
        say(f"REFUSED: printer is not idle ({why})")
        return 1

    from mqtt_listen import Session

    session = Session(printer, say, client_id=f"nocte-print-{int(time.time())}")
    try:
        if not session.connect() or session.connack != 0:
            say(f"REFUSED: CONNACK rc={session.connack}")
            return 1
        session.publish(payload)
        time.sleep(2)
        say(f"published ({why})")
    finally:
        session.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
