#!/usr/bin/env python3
"""Decode an OpenApex drive capture and, optionally, merge it with the phone-side log.

The terminal writes fixed 128-byte records into a dedicated flash partition while driving (see
firmware/main/drive_log.h). Pull the partition over USB after the drive -- no laptop needed in the
vehicle:

    esptool.py --port COM30 read_flash 0x190000 0x100000 esp_log.bin
    python tools/decode_drive.py esp_log.bin
    python tools/decode_drive.py esp_log.bin --phone relay-*.jsonl

The two sides are joined on the packet `sequence`, which both already record -- the phone supplies
wall-clock time, the terminal supplies uptime, so a merged timeline needs no clock sync and no
change to the v2 packet format.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

RECORD_SIZE = 128
MAGIC = 0xA7C3
VERSION = 1

HEADER = struct.Struct("<HBBIII")  # magic, version, kind, record_index, uptime_ms, boot_id

KIND_BOOT, KIND_BLE, KIND_RAW, KIND_MODEL, KIND_VIEW = 1, 2, 3, 4, 5
KIND_NAMES = {KIND_BOOT: "BOOT", KIND_BLE: "BLE", KIND_RAW: "RAW",
              KIND_MODEL: "MODEL", KIND_VIEW: "VIEW"}

RAW = struct.Struct("<IhHHBBii64s16s12s")
MODEL = struct.Struct("<IBiiHH64s")
VIEW = struct.Struct("<IBBBIHHB")
BLE = struct.Struct("<Bi")

# firmware/main/nav_model.h, in enum order. Protocol values, not platform ordinals.
ICONS = ["STRAIGHT", "TURN_LEFT", "TURN_RIGHT", "SLIGHT_LEFT", "SLIGHT_RIGHT",
         "SHARP_LEFT", "SHARP_RIGHT", "ROUNDABOUT_LEFT", "ROUNDABOUT_RIGHT",
         "ROUNDABOUT_STRAIGHT", "U_TURN", "ARRIVED", "UNKNOWN"]
# firmware/main/view_state.h
STATES = ["IDLE", "ACTIVE", "STALE", "ARRIVED"]
BLE_EVENTS = {1: "CONNECTED", 2: "DISCONNECTED", 3: "DECODE_FAILED", 4: "QUEUE_FULL"}


def cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def name(table, index):
    return table[index] if 0 <= index < len(table) else f"?{index}"


def unknown(value, sentinel):
    """Sentinels are 'unknown', never a fabricated zero -- keep that distinction in the output."""
    return None if value == sentinel else value


def decode_records(blob: bytes):
    """Yields every valid record, in ring order (by record_index, which is monotonic across boots)."""
    records = []
    for offset in range(0, len(blob) - RECORD_SIZE + 1, RECORD_SIZE):
        chunk = blob[offset:offset + RECORD_SIZE]
        magic, version, kind, index, uptime, boot_id = HEADER.unpack_from(chunk)
        if magic != MAGIC or version != VERSION:
            continue  # erased slot, or a torn write
        payload = chunk[HEADER.size:]
        record = {"index": index, "uptime_ms": uptime, "boot_id": f"0x{boot_id:08x}",
                  "kind": KIND_NAMES.get(kind, f"?{kind}")}
        try:
            record.update(decode_payload(kind, payload))
        except struct.error:
            record["error"] = "truncated payload"
        records.append(record)
    records.sort(key=lambda r: r["index"])
    return records


def decode_payload(kind: int, payload: bytes) -> dict:
    if kind == KIND_BOOT:
        return {"what": cstr(payload)}
    if kind == KIND_BLE:
        event, detail = BLE.unpack_from(payload)
        return {"event": BLE_EVENTS.get(event, f"?{event}"), "detail": detail}
    if kind == KIND_RAW:
        (seq, rot, speed, heading, battery, fix, progress, progress_max,
         title, distance, eta) = RAW.unpack_from(payload)
        return {
            "seq": seq,
            "icon_rotation_deg": unknown(rot, 0x7FFF),
            "speed_kmh": None if speed == 0xFFFF else speed / 10.0,
            "heading_deg": unknown(heading, 0xFFFF),
            "battery_percent": unknown(battery, 0xFF),
            "gnss_fix_valid": bool(fix),
            "progress": unknown(progress, -1),
            "progress_max": unknown(progress_max, -1),
            "title": cstr(title),
            "distance_str": cstr(distance),
            "eta": cstr(eta),
        }
    if kind == KIND_MODEL:
        seq, icon, distance, remaining, speed, heading, street = MODEL.unpack_from(payload)
        return {
            "seq": seq,
            "icon": name(ICONS, icon),
            "distance_m": unknown(distance, -1),
            "remaining_m": unknown(remaining, -1),
            "speed_kmh": None if speed == 0xFFFF else speed / 10.0,
            "heading_deg": unknown(heading, 0xFFFF),
            "street": cstr(street),
        }
    if kind == KIND_VIEW:
        seq, state, icon, stale, distance, speed, heading, battery = VIEW.unpack_from(payload)
        return {
            "seq": seq,
            "state": name(STATES, state),
            "icon": name(ICONS, icon),
            "stale": bool(stale),
            "distance_m": distance,
            "speed_kmh": None if speed == 0xFFFF else speed / 10.0,
            "heading_deg": unknown(heading, 0xFFFF),
            "battery_percent": unknown(battery, 0xFF),
        }
    return {}


def load_phone(path: Path) -> dict:
    """Indexes the phone-side JSONL by packet sequence, so ESP records can be joined onto it."""
    by_seq = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        seq = event.get("seq")
        if seq is not None:
            by_seq.setdefault(seq, []).append(event)
    return by_seq


def format_record(record: dict) -> str:
    head = f"[{record['index']:>6}] {record['uptime_ms']/1000:>9.3f}s {record['kind']:<5}"
    body = " ".join(f"{k}={v!r}" for k, v in record.items()
                    if k not in ("index", "uptime_ms", "kind", "boot_id"))
    return f"{head} {body}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("esp_log", type=Path, help="raw partition dump from esptool read_flash")
    parser.add_argument("--phone", type=Path, help="phone-side JSONL log to merge on `seq`")
    parser.add_argument("--json", action="store_true", help="emit JSONL instead of a text timeline")
    parser.add_argument("--kind", action="append",
                        help="only this record kind (repeatable): BOOT BLE RAW MODEL VIEW")
    args = parser.parse_args()

    records = decode_records(args.esp_log.read_bytes())
    if args.kind:
        wanted = {k.upper() for k in args.kind}
        records = [r for r in records if r["kind"] in wanted]

    phone = load_phone(args.phone) if args.phone else {}
    if phone:
        for record in records:
            seq = record.get("seq")
            if seq in phone:
                record["phone"] = phone[seq]

    if args.json:
        for record in records:
            print(json.dumps(record))
    else:
        for record in records:
            print(format_record(record))

    if not records:
        print("no valid records found — partition erased, or dumped from the wrong offset?",
              file=sys.stderr)
        return 1

    boots = {r["boot_id"] for r in records}
    print(f"\n{len(records)} records across {len(boots)} boot session(s)", file=sys.stderr)
    if args.phone and not phone:
        print("phone log had no `seq` fields — nothing to join on", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
