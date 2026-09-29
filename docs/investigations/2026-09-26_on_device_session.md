# 2026-09-26 — On-device session: transitions, touch, text normalisation

**Kind:** Investigation log — frozen; do not update except to add a closing note.  
**Moved from:** OpenApex_SPEC.md §19 (restructure, 2026-09-29)

## 2026-09-26 on-device session: transitions, touch, text normalisation

*Was SPEC §19.*

**Screen transitions** ("design/SCREEN_TRANSITIONS.md") are implemented. Boot always
passes splash -> idle -> nav, even when navigation is already running, with idle's 2 s hold floor.
Nav entry is staged: the arrow lands fully, then the text slides up from the bottom, then the ring
zooms in. The dial root is not scrollable (a scrollbar showed while the arrow was off-screen). The
idle odometer bubble enters ease-out-expo and leaves ease-in-expo, 470 ms each way.

**Boot flash.** The backlight GPIO is driven low first thing in `app_main` and the panel stays
display-off until the first frame is ready, so stale panel RAM no longer flashes before the logo.
If it ever reappears, a hardware pull-down on GPIO3 is the remaining fix.

**Touch.** A 2 s press-and-hold anywhere on the nav page swaps the outer ring between compass and
trip progress: the current ring plays its entry in reverse, then the other enters; the ETA label
slides off/on from the bottom with the trip arc. Root children are non-clickable so the hold reaches
the root. This is a test control, not final UX.

**Nav gating.** Only packets carrying navigation content (title, distance, progress or icon) enter
the countdown; telemetry-only packets (time, battery, GNSS) keep the terminal on idle.

**Text normalisation.** Street extraction no longer eats the first letter after " on " ("alliser
Lane"). Street suffixes and compass words are abbreviated as whole words (Street->St, Lane->Ln,
North->N, ...), never the first word. ETA drops "arrive"/"arrival at". The label is configurable
(`pipeline_set_eta_format()`, default `PIPELINE_ETA_FORMAT_DEFAULT`): arrival time ("ETA 03:06"), or time
left only ("15 min", the default). Maps sends only the arrival clock, and trip
progress is a fraction with no duration, so time left is arrival minus the phone's synced clock.

**Trip arc.** Outer radius 116 (was 117), stroke 7 (was 5), gap widened to 116..424 deg.

**BLE stall.** `RelayBleClient.writeInFlight` was left set when the link dropped mid-write, so every
later write was skipped. It is now cleared on disconnect and on service discovery.

**Revisit later: stationary U-turn flicker.** When stopped, Maps alternates "Make a U-turn" and
"Head west" notifications, so the arrow repeatedly animates a U-turn and back. A 3-packet hold was
rejected (packets arrive about 1 Hz, so every turn would lag). Candidate fix: debounce maneuver
changes only below walking speed.
