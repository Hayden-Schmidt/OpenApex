# Page 3 — Turn-by-Turn (main navigation screen)

Moved to from the idle screen once navigation has started on the phone. This is the main screen
for pass one.

## Elements (build independently)

### 1. The arrow

A large, white, chunky, clear arrow in the middle of the page reflecting the next navigation
instruction — turn left/right, roundabout (with different exit profiles), U-turn, rerouting (if we
get that data from any maps app; if not, flag for later), and any other standard-navigation
maneuvers.

- **Simple:** static arrow icons — solid, unmoving, shown when called by navigation data.
- **Rich:** morphing design — each arrow morphs seamlessly into the next (straight bends into a
  turn; straight/bend skinner and bend round into a U-turn or roundabout, etc.). Full morphing
  suite of directional arrows with smooth transitions. See https://www.morphicons.com/ for the
  math *if* SVG is chosen. Consider whether this can be kept for Simple too.

### 2. The compass exterior

A small ring of subtle grey markers around the exterior of the dial — static, subtle, must not
impede the main design. Accompanied by a small **red north marker** (default: a circle placed on
the screen border so only the inner half shows) at the screen edge.

- **Simple:** default compass marker only, updated from tracked phone GNSS data (may need to flag
  backend setup for after UI dev), at a rate appropriate for the processor.
- **Rich:** marker has a few options (customisable in the app in later dev), updated live via
  phone GNSS, onboard GPS (S3-with-GPS), or magnetometer data (external module, scoped for
  testing). Spec the UI, hook up existing architecture, flag additional backend dev for post-UI
  sprint.
- **Note:** compass is **not page-specific** — it will be reused on other screens, so keep it
  subtle but seeable.
- **Note 2:** may need **smoothing** on compass data, especially phone-GNSS data, which can stutter
  or move too quickly to read.

### 3. Distance marker

Text under the direction arrow showing the distance telegraphed by notification data, mixed with
GPS interpolation (**already set up, ready to hook in**). Goal: simple metres display, 1 m
resolution, counting down live as you approach the corner.

- **Simple:** if the C3 can't handle "live" updates, throttle update speed appropriately;
  otherwise leave live.
- **Rich:** fully live metre countdown, plus a configurable backend setting to increase
  granularity by a size in metres until a set distance — e.g. roll up to nearest 10 m until within
  20 m. Later a configurable item in phone-app settings.
- **Note:** hopefully no backend change required; if so, make small changes now and flag larger
  ones for post-UI sprint.

## Design direction

Simple, easy to read at high speed on a motorcycle, not overcrowded — every element needs space to
breathe and must follow the theme. This page will get the most tweaking; set it up to configure
easily, as it will absolutely be revisited.

## Reference

Beeline odometer page (photo in this dir) — reference for the **compass only**. Nice, maybe a
*little* too intrusive.

---

## Current code status: ✅ rendered (traffic ring has no data source)

`firmware/gui/dial_screen.{hpp,cpp}` + `nav_renderer.cpp` + `compass_ring.cpp` + `trip_arc.cpp`.
Laid out against `OpenApex Hardware Design Ref.svg`.

```powershell
firmware/sim_lvgl/.pio/build/sim_lvgl/program.exe              # compass variant, all states
firmware/sim_lvgl/.pio/build/sim_lvgl/program.exe --page trip  # traffic-ring variant
```

### Element 1 — the arrow

Real vector geometry, not glyphs: `nav_renderer.cpp` strokes the exact straights/arcs from
`nav_icons_data.h` with one `lv_draw_arc` per curve, and tweens between maneuvers. The
"morphing suite" the Rich tier asks for is effectively already here for both tiers.

Its vertical position is `kVerticalOffset` in `nav_renderer.cpp`, now -28.545 route units so the
arrow clears the street/distance stack, matching the design ref.

### Element 2 — the compass exterior

`compass_ring.{hpp,cpp}`, shared with the odometer page (page 4 reuses the same object, it is not a
lookalike). Revised for this design:

- **72 ticks**, up from 36. `kMajorEvery` went 3 -> 6 so the majors still land every 30 deg, on the
  same twelve compass points — leaving it at 3 would have doubled the majors too.
- Majors are now the **same width** as minors and **full white** instead of twice as thick and grey.
- North marker resized to the design's 16x16 (`M128 0H112 L120 16`).
- The marker's point carries a **small radius**. LVGL has no corner radius on a filled triangle, so
  the point is truncated at the circle's tangent points and the corner filled with that circle —
  stamping a circle onto the full triangle would leave the sharp point poking through it.

### Element 3 — distance marker

Live text, 20px, under the street name, per the design ref. Greyed rather than hidden in
`VIEW_STALE`: a held distance reads better on the road than an empty dial, but must not look live.

### Street name and ETA

Both were already carried in `terminal_view_state_t` and produced by `normalize.c`, but nothing drew
them. Now rendered. **The street name must be a SHORT name** ("Elm St", not "Elm Street North") —
it has ~47px of the 240 frame. It is currently whatever the maps app supplies, ellipsized to fit.

### Traffic ring (the "Google Trip Data" variant)

`trip_arc.{hpp,cpp}` — an alternative to the compass in the same slot, swapped live via
`gui_app_set_dial_outer()`. Google's notification progress bar bent around the screen: coloured runs
of free/slow/heavy/stopped, clockwise from lower-left over the top to lower-right, consumed from the
START as the ride progresses, with the ETA in the gap at the bottom.

The gap is **not** a mask. The design ref cuts it with a black circle (r=44 at (120,246)); the arc
simply stops at the two angles where that circle crosses the ring — 110.366 deg and 429.634 deg —
which is geometrically identical and leaves clean rounded ends instead of square-chopped ones.

With no traffic data the ring draws one neutral grey arc, never "all clear": absent data must not
look like a clear road.

## Backend gaps flagged

1. **Google traffic data is not parsed or transmitted.** `terminal_view_state_t.traffic[]`,
   `traffic_count` and `trip_progress_permille` exist and the ring renders them, but the Android
   relay does not read the notification's progress section and it is not in the packet format at
   all. The simulator fixture is the only writer. **This is the largest gap on this page.**
2. **No input to swap the outer element.** `gui_app_set_dial_outer()` works, but how the rider picks
   (screen swipe, phone toggle) is undecided and the C3 models no input hardware. Same gap as the
   odometer page's routing.
3. **Traffic colours are unconfirmed.** Only `#009AA6` is pinned down by the design ref; the
   slow/heavy/stopped colours follow Google's palette by eye and need checking against a real
   notification capture.
4. **Street short-name.** Nothing shortens what the maps app supplies; long names ellipsize.
5. **Arrival: built, but not a "pop up".** `arrived_screen.{hpp,cpp}` renders
   `Arrived Pop Up.svg` as a full screen on `VIEW_ARRIVED`. If it should animate IN over the dial
   rather than replace it, that is a transition — `GuiTheme::apply_state_change` is the hook and is
   still an instant `lv_screen_load` on both tiers.
6. **Rerouting** maneuver: no `nav_icon_t` value, not surfaced by any maps app adapter yet.
7. **Distance granularity rolling** (Rich: round to 10m until within 20m) needs the runtime config
   store.

## Tooling

`tools/export_page_svg.py` re-exports this page as an editable SVG from the live firmware geometry,
for redesign work. `--list` shows the maneuvers. It covers a single settled maneuver only — not
chaining, not the reveal tween.
