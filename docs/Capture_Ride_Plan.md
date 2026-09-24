# Targeted capture ride plan

The terminal's maneuver rendering is a **glyph lookup**, not geometry: Google Maps ships a distinct
pre-rendered bitmap per maneuver, and `NavNotificationRelayService.extractIconRotationDeg` measures
a per-glyph constant (see `firmware/main/normalize.c`). A maneuver class the tables have never seen
resolves to `UNKNOWN` and renders nothing useful — so coverage is bounded by what has actually been
ridden, and the only way to extend it is to go and provoke the missing maneuvers.

This plan exists because the 2026-09-24 capture exercised only 6 of the 13 normalized maneuver
classes, and because two whole categories of defect (km-scale distance, non-metre units) have
correct-looking code that has never once run on real data.

## Coverage as of 2026-09-24

Counts are MODEL records from the 2026-09-24 capture (`--glyphs` report).

| Maneuver | Glyph angle | Status |
|---|---|---|
| `STRAIGHT` | 0, 181 | 0 confirmed (17). 181 ("Merge onto…") from 2026-09-23 only |
| `TURN_LEFT` | 113 | confirmed (91) |
| `TURN_RIGHT` | 247 | confirmed (154) |
| `ROUNDABOUT_STRAIGHT` | 169 | confirmed (148) |
| `ROUNDABOUT_LEFT` | 135 | confirmed (22) |
| `ARRIVED` | 133 | confirmed (8) |
| `SHARP_RIGHT` | 283 | 2026-09-23 only, not re-observed |
| `SLIGHT_LEFT` | 325 | 2026-09-23 only, not re-observed |
| `SLIGHT_RIGHT` | — | **no glyph entry**; text matching only |
| `SHARP_LEFT` | — | **no glyph entry**; text matching only |
| `U_TURN` | — | **no glyph entry**; text matching only |
| `ROUNDABOUT_RIGHT` | — | **no glyph entry**; text matching only |

A class with "no glyph entry" survives only while Maps phrases the title in English in a way
`derive_maneuver()` recognises. It has no second witness, so a locale change or a rephrasing takes
it straight to `UNKNOWN`.

## What to ride

Each item below provokes one gap. Ride them in any order; the recorder segments by session, so
stopping and restarting between them is fine and actually makes the capture easier to read.

1. **Motorway on-ramp and off-ramp** — `SLIGHT_LEFT` / `SLIGHT_RIGHT`, and the `181` merge glyph.
   Maps phrases these as "Use the left 2 lanes to take exit …", "Merge onto …", "Keep right at the
   fork". Take an exit *and* an entrance; they are different glyphs.
2. **A U-turn** — `U_TURN`. Easiest to provoke deliberately: start navigation to somewhere behind
   you and let Maps route the U-turn, or miss a turn on a divided road.
3. **A multi-exit roundabout, 2nd and 3rd exits** — the `take the Nth exit` phrasing carries no
   direction at all, so these depend entirely on the glyph. Today only the 1st exit (135) is known.
4. **A right-hand roundabout exit** — `ROUNDABOUT_RIGHT`. In NZ left-hand traffic this is a
   right-hand exit off a roundabout, i.e. going most of the way around.
5. **A leg longer than 1 km between maneuvers** — km-scale distance parsing. `parse_distance_metres`
   handles `"1.1 km"` and is covered by host tests, but **zero** km distances appear in any capture
   to date; all 351 were metres. A motorway leg gives a "1.2 km"-style countdown.
6. **A sharp left and a sharp right** — `SHARP_LEFT` has no glyph entry at all; `SHARP_RIGHT` (283)
   needs confirming a second time.

A ride that covers 1, 3 and 5 together (suburban roundabouts → motorway → exit) gets most of the
value in one trip.

## Before setting off

```bash
adb shell dumpsys package org.openapex.androidauto | grep BACKGROUND_LOCATION
```

Must print `granted=true`. If it does not, the terminal gets **no speed, heading or compass** for
the entire ride and the capture is worth far less — this is exactly what cost four of the seven
sessions on 2026-09-24. The app now posts a high-priority "No speed or compass" notification in
this state; do not ride past it.

Also confirm the phone and terminal are on the current build — a capture is only a regression
fixture if you know which code produced it.

## After the ride

```bash
# Phone side (MSYS_NO_PATHCONV=1 in Git Bash, or adb mangles the remote path)
adb pull /sdcard/Android/data/org.openapex.androidauto/files/drivelogs/

# Terminal side (ESP32-C3 native USB port, VID 303A)
python -m esptool --port COM30 --baud 921600 read-flash 0x190000 0x100000 esp_log.bin

# Did the link actually work? Check this FIRST — a truncating link means no usable capture.
python tools/decode_drive.py esp_log.bin --link

# Which maneuver glyphs appeared, and which are still unknown to the terminal?
python tools/decode_drive.py esp_log.bin --glyphs

# Full merged timeline
python tools/decode_drive.py esp_log.bin --phone drive-*.jsonl
```

`--link` should now report a negotiated MTU of at least 149 for every session. "not negotiated"
with failures means the truncation bug is back.

`--glyphs` prints any angle that resolved to `UNKNOWN` along with the titles that produced it. For
each one, read the titles, decide the maneuver, and add the `(angle, icon)` pair to `MANEUVER_GLYPHS`
or `ROUNDABOUT_GLYPHS` in `firmware/main/normalize.c`. The host test
`test_glyph_tables_are_separable()` will fail if a new entry sits close enough to an existing one
for the match tolerance to confuse them.

## Then prove nothing regressed

```bash
gcc -std=c11 -I firmware/main -o replay firmware/test_host/replay.c \
    firmware/main/normalize.c firmware/main/packet.c firmware/main/countdown.c \
    firmware/main/pipeline.c -lm
./replay esp_log.bin
```

Every captured drive is a regression fixture: the replay re-derives each packet through the current
normalizer and countdown and compares against what the terminal actually displayed at the time.
After adding glyph entries, replay **every** capture you have, not just the new one — the report
should show differences only for the packets you meant to change.
