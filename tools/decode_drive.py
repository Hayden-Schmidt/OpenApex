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
import collections
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
BLE_EVENTS = {1: "CONNECTED", 2: "DISCONNECTED", 3: "DECODE_FAILED", 4: "QUEUE_FULL",
              # detail = negotiated ATT MTU; anything below RAW_NOTIF_PACKET_SIZE + 3 (149)
              # means the central never negotiated up and every write is being truncated.
              5: "MTU",
              # detail = the (too short) write length actually received.
              6: "TRUNCATED"}


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


def pair_by_sequence(records):
    """Pairs each RAW record with the MODEL the terminal derived from it.

    Keyed on (boot_id, seq), never on seq alone: `sequence` restarts at 1 on every phone-side
    service start, so across a multi-session capture the same seq belongs to several different
    packets. Joining on seq alone silently attributes one drive's title to another drive's icon.
    """
    models = {(r["boot_id"], r["seq"]): r for r in records if r["kind"] == "MODEL"}
    for raw in (r for r in records if r["kind"] == "RAW"):
        yield raw, models.get((raw["boot_id"], raw["seq"]))


def report_glyphs(records) -> int:
    """Lists every maneuver-glyph angle in the capture and what the terminal made of it.

    The terminal's angle->maneuver tables are a lookup over observed glyph bitmaps, so the way to
    find a glyph they don't know yet is to look for an angle that produced UNKNOWN. No extra
    firmware logging is needed for this: the RAW record already carries icon_rotation_deg for every
    packet and the MODEL record carries the resulting icon.
    """
    seen = {}
    for raw, model in pair_by_sequence(records):
        angle = raw["icon_rotation_deg"]
        icon = model["icon"] if model else "(no model)"
        entry = seen.setdefault(angle, {"count": 0, "icons": collections.Counter(), "titles": set()})
        entry["count"] += 1
        entry["icons"][icon] += 1
        if raw["title"]:
            entry["titles"].add(raw["title"])

    print(f"{'angle':>7}  {'n':>5}  resolved-to")
    unseen = []
    for angle in sorted(seen, key=lambda a: (a is None, a)):
        entry = seen[angle]
        icons = ", ".join(f"{name}x{n}" for name, n in entry["icons"].most_common())
        label = "none" if angle is None else str(angle)
        print(f"{label:>7}  {entry['count']:>5}  {icons}")
        for title in sorted(entry["titles"])[:3]:
            print(f"{'':>16}{title!r}")
        # An angle is "unseen by the glyph tables" only if it exists AND produced UNKNOWN. A null
        # angle producing UNKNOWN just means the notification had no maneuver icon to measure.
        if angle is not None and entry["icons"].get("UNKNOWN"):
            unseen.append((angle, entry["icons"]["UNKNOWN"]))

    if unseen:
        print("\nAngles NOT in the terminal's glyph tables (add to normalize.c after confirming "
              "the maneuver from the titles above):", file=sys.stderr)
        for angle, count in unseen:
            print(f"  {angle} ({count} packet(s) resolved to UNKNOWN)", file=sys.stderr)
    else:
        print("\nevery maneuver glyph in this capture is known to the terminal", file=sys.stderr)
    return 0


def report_link(records) -> int:
    """Per-boot BLE link health: negotiated MTU, and whether packets actually decoded.

    A session with writes arriving but nothing decoding is the truncating-MTU failure; a session
    with an MTU below RAW_NOTIF_PACKET_SIZE + 3 names the cause outright.
    """
    min_usable_mtu = 146 + 3
    boots = []
    for r in records:
        if r["boot_id"] not in boots:
            boots.append(r["boot_id"])

    for boot in boots:
        rs = [r for r in records if r["boot_id"] == boot]
        kinds = collections.Counter(r["kind"] for r in rs)
        events = collections.Counter(r.get("event") for r in rs if r["kind"] == "BLE")
        mtus = [r["detail"] for r in rs if r.get("event") == "MTU"]
        mtu = mtus[-1] if mtus else None
        decoded = kinds.get("RAW", 0)
        failed = events.get("DECODE_FAILED", 0) + events.get("TRUNCATED", 0)

        print(f"{boot}  decoded={decoded:<5} failed={failed:<6} mtu={mtu if mtu else 'not negotiated'}")
        if mtu is not None and mtu < min_usable_mtu:
            print(f"    MTU {mtu} < {min_usable_mtu}: every packet is truncated and discarded")
        elif mtu is None and failed:
            print("    no MTU exchange logged and writes failed to decode — likely truncation "
                  "(pre-fix firmware does not log MTU, so this may be an older capture)")
        elif decoded == 0 and failed:
            print("    link delivered writes but nothing decoded")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("esp_log", type=Path, help="raw partition dump from esptool read_flash")
    parser.add_argument("--phone", type=Path, help="phone-side JSONL log to merge on `seq`")
    parser.add_argument("--json", action="store_true", help="emit JSONL instead of a text timeline")
    parser.add_argument("--kind", action="append",
                        help="only this record kind (repeatable): BOOT BLE RAW MODEL VIEW")
    parser.add_argument("--glyphs", action="store_true",
                        help="report which maneuver-glyph angles appeared and what each resolved "
                             "to; flags angles missing from the terminal's glyph tables")
    parser.add_argument("--link", action="store_true",
                        help="report ATT MTU and packet-decode health per boot session")
    args = parser.parse_args()

    records = decode_records(args.esp_log.read_bytes())
    if args.kind:
        wanted = {k.upper() for k in args.kind}
        records = [r for r in records if r["kind"] in wanted]

    if args.glyphs:
        return report_glyphs(records)
    if args.link:
        return report_link(records)

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
