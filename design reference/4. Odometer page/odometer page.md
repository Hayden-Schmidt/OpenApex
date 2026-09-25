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

## Current code status: ✅ rendered (data is fixture-only)

`firmware/gui/odometer_screen.{hpp,cpp}`. Verified against `Odometer page ref.svg` at 240x240 --
every element lands within ~1px, the rounding floor at that resolution.

Review it with:

```powershell
firmware/sim_lvgl/.pio/build/sim_lvgl/program.exe --page odometer
```

which sweeps the heading a full turn every 20s (so the compass ring and the degree/cardinal readout
are both visibly live) and climbs the odometer about a tenth of a km per second.

**Compass element (b): genuinely reused, not reimplemented.** The ring lived inside `NavRenderer`
as a private `draw_compass_ring()`, so it was not reusable as written. It is now
`firmware/gui/compass_ring.{hpp,cpp}` and `NavRenderer` delegates to it -- one implementation, one
set of constants, and a heading that cannot drift between the two pages. Verified non-regressing by
diffing a rendered dial frame before and after the extraction: 0 differing pixels. (The first
attempt was 19 pixels off along the north marker's anti-aliased edge, because the original
`to_lv()` truncated where the extracted copy rounded. The truncation is now deliberately preserved,
with a comment saying why.)

**Departure from the requirements above: FOUR digits, not five.** The SVG draws three full-size
boxes plus a smaller tenths box beside "km"; the prose says five. The SVG is the Figma reference and
wins, the same call made for page 2's km-vs-miles. Changing it is `kDigitCount` plus the `kBoxX`
table.

**Implementation notes:**

- Geometry is authored as reference-design pixels against the SVG's 240px frame and scaled at
  runtime, so the page renders at any panel size.
- The compass ring draws into an `LV_EVENT_DRAW_MAIN` layer rather than owning widgets -- 36 ticks
  would otherwise be 36 `lv_obj_t` for something never hit-tested or individually styled. Same
  approach `DialScreen` uses.
- `montserrat_at_most()` moved out of `idle_screen.cpp` into the shared `gui_font.{hpp,cpp}` and now
  enumerates every Montserrat size. Its previous hand-written subset silently rounded this page's
  38px digits down to 28.
- "km" takes an explicit 18px rather than being derived like the other labels: it is the only run
  with no capitals, so its 13.09px in the SVG is a lowercase *ascender*, a larger fraction of the em
  than a cap height. Deriving it rounded down to 14px and rendered the unit visibly undersized.
- Montserrat 18/28/38 were enabled in both `firmware/sim_lvgl/include/lv_conf.h` and
  `firmware/sdkconfig.defaults`. **Those two lists must stay mirrored** or the simulator renders a
  page at a different size than the device does.

**Routing:** the odometer is a page the rider *picks*, not a `view_state_t` -- no packet or
countdown result can produce it. Rather than adding a bogus value to the protocol enum, it is
reached through `gui_app_set_page_override(GUI_PAGE_ODOMETER)` (`gui_app.hpp`). An active maneuver
always takes the screen back and clears the selection: being shown the odometer instead of the turn
you are about to miss is a safety problem, not a UX one.

**Backend gaps flagged:**

1. **Page switching has no input source.** `gui_app_set_page_override()` exists and works, but
   nothing on the device calls it -- the C3 profile models no buttons and `BOARD_HAS_TOUCH` is 0.
   `firmware/sim_lvgl`'s `--page` flag is the only caller today. This blocks the page from being
   reachable on hardware at all.
2. **Odometer data model** -- `terminal_view_state_t.odometer_meters` exists (added for page 2) and
   this page renders it, but nothing writes it. Capacity is part of this decision: four boxes hold
   999.9km, which is not a lifetime figure.
3. **NVS persistence** for the device odometer.
4. **Account aggregation** -- phone-app collation across devices.
5. **Per-phone vs total-device semantics** -- still unresolved; see the open design decision above.
   The UI is built and does not depend on which way it goes.
6. **Time/RTC source** -- `clock` renders `--:--` until phone time sync exists. C3 has no RTC.
7. **Compass smoothing** -- shared with page 3's compass element; `heading_deg` is fused
   (`heading_fusion.c`) but nothing populates it into the view state.

**Not built (deferred, per the requirements):**

- **Rich tier's last-digit fill** -- the tenths box's background slowly filling with the theme
  colour as it approaches rollover. Needs the odometer data model first; filling from a fixture
  would be animating a number nothing produces.
- **Rich tier's morphing clock digits.** Both tiers render a static clock today.
- **12h vs 24h toggle** -- needs the runtime config store. The clock renders whatever string the
  view state carries.
