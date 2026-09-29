#!/usr/bin/env python3
"""Decode the ANCS tier of an OpenApex drive capture, and turn it into a host-test fixture.

On iOS the terminal reads notifications straight off the iPhone over ANCS, because no phone app
can see them (docs/reference/ios_ancs.md). What iOS actually sends -- whether Google Maps modifies
one long-lived notification per turn or posts new ones, what its Title/Subtitle/Message hold,
whether Apple Maps says anything at all -- is not documented anywhere and is answered by capture.
So firmware/main/drive_log.c records every ANCS byte in BOTH directions, unparsed, and this reads
them back.

    esptool.py --port COM30 read_flash 0x210000 0x100000 esp_log.bin
    python tools/decode_ancs.py esp_log.bin                 # timeline
    python tools/decode_ancs.py esp_log.bin --raw           # every frame, hex
    python tools/decode_ancs.py esp_log.bin --fixture out.c # regression fixture

The fixture is the point of the whole exercise: a drive becomes a permanent host test, the same
way tools/decode_drive.py + firmware/test_host/replay.c already work for the Android path.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

RECORD_SIZE = 128
MAGIC = 0xA7C3
VERSION = 1

HEADER = struct.Struct("<HBBIII")  # magic, version, kind, record_index, uptime_ms, boot_id
KIND_ANCS = 6

# firmware/main/drive_log.h drive_log_ancs_t
ANCS_HDR = struct.Struct("<BBH")  # kind, len, total_len

# firmware/main/ancs_core.h ancs_raw_kind_t
RAW_NS, RAW_DS, RAW_CP = 0, 1, 2
RAW_NAMES = {RAW_NS: "NS", RAW_DS: "DS", RAW_CP: "CP->"}

EVENT_IDS = {0: "Added", 1: "Modified", 2: "Removed"}
CATEGORIES = {
    0: "Other", 1: "IncomingCall", 2: "MissedCall", 3: "Voicemail", 4: "Social",
    5: "Schedule", 6: "Email", 7: "News", 8: "HealthAndFitness", 9: "BusinessAndFinance",
    10: "Location", 11: "Entertainment",
}
ATTRS = {
    0: "AppIdentifier", 1: "Title", 2: "Subtitle", 3: "Message", 4: "MessageSize",
    5: "Date", 6: "PositiveActionLabel", 7: "NegativeActionLabel",
}
FLAGS = [(0x01, "silent"), (0x02, "important"), (0x04, "pre-existing"),
         (0x08, "positive-action"), (0x10, "negative-action")]


def decode_records(blob: bytes):
    """Yields (uptime_ms, boot_id, kind, len, total_len, data) for the ANCS tier, in ring order."""
    out = []
    for off in range(0, len(blob) - RECORD_SIZE + 1, RECORD_SIZE):
        rec = blob[off:off + RECORD_SIZE]
        magic, version, kind, index, uptime, boot = HEADER.unpack_from(rec)
        if magic != MAGIC or version != VERSION or kind != KIND_ANCS:
            continue
        rk, n, total = ANCS_HDR.unpack_from(rec, HEADER.size)
        data = rec[HEADER.size + ANCS_HDR.size:][:n]
        out.append((index, uptime, boot, rk, n, total, data))
    out.sort(key=lambda r: r[0])
    return out


def describe_flags(flags: int) -> str:
    on = [name for bit, name in FLAGS if flags & bit]
    return ",".join(on) if on else "-"


def parse_notification_source(data: bytes) -> str:
    if len(data) < 8:
        return f"(short, {len(data)}B)"
    event, flags, category, count = data[0], data[1], data[2], data[3]
    uid = struct.unpack_from("<I", data, 4)[0]
    return (f"{EVENT_IDS.get(event, event):<8} uid={uid:<10} "
            f"cat={CATEGORIES.get(category, category)}({category}) count={count} "
            f"flags={describe_flags(flags)}")


def parse_control_point(data: bytes) -> str:
    if not data:
        return "(empty)"
    cmd = data[0]
    if cmd == 0 and len(data) >= 5:
        uid = struct.unpack_from("<I", data, 1)[0]
        return f"GetNotificationAttributes uid={uid}"
    if cmd == 2 and len(data) >= 6:
        uid = struct.unpack_from("<I", data, 1)[0]
        action = "POSITIVE" if data[5] == 0 else "NEGATIVE"
        return f"PerformNotificationAction uid={uid} {action}"
    return f"cmd={cmd} {data.hex()}"


class DataSourceAssembler:
    """Reassembles Data Source frames exactly the way firmware/main/ancs_core.c does.

    Deliberately a second, independent implementation: if this and the firmware disagree about a
    real capture, that disagreement is the finding.
    """

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        """Returns a list of (uid, {attr_name: value}) for every response that completed."""
        self.buf.extend(data)
        done = []
        while True:
            parsed = self._try_parse()
            if parsed is None:
                return done
            consumed, uid, attrs = parsed
            del self.buf[:consumed]
            done.append((uid, attrs))

    def _try_parse(self):
        b = self.buf
        if len(b) < 5:
            return None
        if b[0] != 0:  # only GetNotificationAttributes responses are ever requested
            del self.buf[:1]  # resync
            return None
        uid = struct.unpack_from("<I", b, 1)[0]
        off = 5
        attrs = {}
        # The firmware asks for five attributes; a response is complete when five have arrived.
        while len(attrs) < 5:
            if len(b) - off < 3:
                return None
            aid = b[off]
            alen = struct.unpack_from("<H", b, off + 1)[0]
            if len(b) - off < 3 + alen:
                return None
            val = bytes(b[off + 3:off + 3 + alen])
            attrs[ATTRS.get(aid, f"attr{aid}")] = val.decode("utf-8", errors="replace")
            off += 3 + alen
        return off, uid, attrs


def c_escape(s: str) -> str:
    out = []
    for ch in s:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            out.append("".join(f"\\x{b:02x}" for b in ch.encode("utf-8")))
    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", type=Path, help="flash image read off the log partition")
    ap.add_argument("--raw", action="store_true", help="print every frame as hex")
    ap.add_argument("--fixture", type=Path,
                    help="write the reassembled notifications as a C fixture table")
    args = ap.parse_args()

    blob = args.capture.read_bytes()
    records = decode_records(blob)
    if not records:
        print("no ANCS records in this capture", file=sys.stderr)
        print("(is the firmware built with -DOPENAPEX_DRIVE_LOG=1, and did a phone connect?)",
              file=sys.stderr)
        return 1

    asm = DataSourceAssembler()
    notifications = []
    clipped = 0
    counts = {RAW_NS: 0, RAW_DS: 0, RAW_CP: 0}

    for _index, uptime, _boot, rk, n, total, data in records:
        counts[rk] = counts.get(rk, 0) + 1
        if n != total:
            clipped += 1
        stamp = f"{uptime / 1000.0:9.3f}s"
        if args.raw:
            note = "" if n == total else f"  [CLIPPED {n}/{total}B]"
            print(f"{stamp} {RAW_NAMES.get(rk, rk):<4} {data.hex()}{note}")
        if rk == RAW_NS:
            if not args.raw:
                print(f"{stamp} NS   {parse_notification_source(data)}")
        elif rk == RAW_CP:
            if not args.raw:
                print(f"{stamp} CP-> {parse_control_point(data)}")
        elif rk == RAW_DS:
            if n != total:
                # A clipped frame desyncs the stream; say so rather than reassemble nonsense.
                print(f"{stamp} DS   [CLIPPED {n}/{total}B -- reassembly reset]")
                asm = DataSourceAssembler()
                continue
            for uid, attrs in asm.feed(data):
                notifications.append((uptime, uid, attrs))
                if not args.raw:
                    app = attrs.get("AppIdentifier", "")
                    print(f"{stamp} DS   uid={uid} app={app}")
                    for key in ("Title", "Subtitle", "Message", "Date"):
                        if attrs.get(key):
                            print(f"{'':10}        {key}: {attrs[key]}")

    print()
    print(f"{len(records)} ANCS records: "
          f"{counts.get(RAW_NS, 0)} notification-source, {counts.get(RAW_DS, 0)} data-source, "
          f"{counts.get(RAW_CP, 0)} control-point")
    print(f"{len(notifications)} notifications reassembled"
          + (f", {clipped} records clipped by the 108-byte log slot" if clipped else ""))

    apps = {}
    for _t, _uid, attrs in notifications:
        apps[attrs.get("AppIdentifier", "?")] = apps.get(attrs.get("AppIdentifier", "?"), 0) + 1
    if apps:
        print("apps seen: " + ", ".join(f"{a} x{c}" for a, c in sorted(apps.items())))

    if args.fixture:
        lines = [
            "// Generated by tools/decode_ancs.py from a real iPhone capture. Do not hand-edit:",
            "// regenerate it from the capture instead, so the fixture always says what the phone",
            "// actually sent rather than what we later assumed it sent.",
            "//",
            f"// source: {args.capture.name}, {len(notifications)} notifications",
            "",
            "static const struct {",
            "    uint32_t uptime_ms;",
            "    uint32_t uid;",
            "    const char *app_id;",
            "    const char *title;",
            "    const char *subtitle;",
            "    const char *message;",
            "    const char *date;",
            "} kAncsCapture[] = {",
        ]
        for uptime, uid, attrs in notifications:
            lines.append(
                "    {%u, %u, \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"}," % (
                    uptime, uid,
                    c_escape(attrs.get("AppIdentifier", "")),
                    c_escape(attrs.get("Title", "")),
                    c_escape(attrs.get("Subtitle", "")),
                    c_escape(attrs.get("Message", "")),
                    c_escape(attrs.get("Date", "")),
                ))
        lines.append("};")
        lines.append("")
        args.fixture.write_text("\n".join(lines), encoding="utf-8")
        print(f"wrote {args.fixture} ({len(notifications)} rows)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
