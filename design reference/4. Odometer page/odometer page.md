# Page 4 — Odometer Page

Shows lifetime travel distance with OpenApex, and **reuses the compass element** from page design 3.

## Elements

### a. Compass heading element

Shows current orientation as text, e.g.:

```text
330°
NW
```

- Updates at the same frequency as the compass, depending on the simple vs rich profile.
- May need the same smoothing noted for the compass element.
- Text in the theme colour.

### b. Compass element

Reused from page design 3.

### c. Odometer

A set of small rounded-corner boxes reflecting a classic odometer gauge — one per digit, **5
digits** — showing total distance travelled with OpenApex in km.

- **Per-device** odometer stored **on the device**.
- **Per-account** odometer (across all devices) stored **on the app** as a collation of all
  odometer readings from connected devices.

**Open design decision (needs discussion before implementing behaviour):** whether the device
odometer is **per-phone** (connect my device to a friend's phone → shows the km *he* rode, not
mine, and switches back when I reconnect), or a single **total device miles** figure. Possibly
both — a toggleable screen between **total device miles**, **total miles *you* have ridden with it**
(keyed by phone MAC), and **total account miles** (app-stored). UI can be built now without hooking
up the behaviour.

- **Simple:** odometer is static; updates once each new kilometre passes.
- **Rich:** odometer is static except the **last digit**, whose background slowly fills with the
  theme colour until it ticks over and starts again. May need text-colour inversion on that digit.

### d. Time

Show time in **standard or 24h** format, configurable in the app (once built).

- **Simple:** a simple digital time, e.g. `12:55am` or `22:18`, updating when the minute changes.
- **Rich:** the same, but with morphing digits — each number smoothly morphs into the next as time
  changes. See https://www.morphicons.com/ *only* if SVG is the best path and usable; don't default
  to morphicons if it doesn't make sense.

## Design notes

I really like Beeline's example here — happy to copy that page almost exactly. Reference added to
the folder.

---

## Current code status: ⬜ not started (no data model)

**Nothing exists yet.** The odometer page is fully unbuilt, and its data has no home:

- `terminal_view_state_t` (`firmware/main/view_state.h`) has **no odometer field** — lifetime
  distance is not tracked anywhere in the model.
- No NVS persistence for a device odometer (`firmware/main/ble_link.c` uses NVS for BLE bonding
  only; there is no odometer store).
- No account/app-side aggregation on Android (`android/.../RelayService.kt` and the relay packet
  `RawNotifPacket.kt` carry no odometer data).
- `heading_deg` (needed for elements a/b) **is** already in `terminal_view_state_t`, but the
  compass rendering itself is unbuilt (see page 3).

**Backend gaps flagged (all post-UI-sprint):**
1. **Odometer data model** — a lifetime-distance field + increment logic (driven by
   `speed_kmh_x10`/GNSS) on the device.
2. **NVS persistence** — store the device odometer across reboots.
3. **Account aggregation** — phone app collation of odometer readings across devices.
4. **Per-phone vs total-device semantics** — unresolved; needs the design decision above.
5. **Time/RTC source** — C3 has no RTC; S3 has PCF85063 (`board_profile.h`). Phone time sync needed
   for the clock.
6. **Compass smoothing** — shared with page 3's compass element.
