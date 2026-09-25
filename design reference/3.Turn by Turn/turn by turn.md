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

## Current code status: 🔶 model complete, placeholder render (all 3 elements landed)

### What exists (data / pipeline)

The entire maneuver→countdown→view-state path is implemented and host-tested:

- `nav_icon_t` (`firmware/main/nav_model.h`) — the normalized maneuver set, **including**
  `NAV_ICON_SHARP_LEFT`/`NAV_ICON_SHARP_RIGHT` (the original spec enum was missing them):
  `STRAIGHT, TURN_LEFT, TURN_RIGHT, SLIGHT_LEFT, SLIGHT_RIGHT, SHARP_LEFT, SHARP_RIGHT,
  ROUNDABOUT, U_TURN, ARRIVED, UNKNOWN`.
- `normalize.c` (`derive_maneuver`) — English-keyword matching on the raw title text → `nav_icon_t`.
  Brittle across locales/phrasings; the terminal-side single source of truth.
- `countdown.c` — speed-filtered distance interpolation (10 s max hold → `stale`).
- `pipeline.c` — `view_state_apply_packet()` / `view_state_tick()` producing
  `terminal_view_state_t` with `distance_meters`, `speed_kmh_x10`, `heading_deg`, `street_name`,
  `eta`, `stale`.
- `view_state.h` — `view_state_t` states: `VIEW_IDLE`, `VIEW_ACTIVE`, `VIEW_STALE`, `VIEW_ARRIVED`.
- Host tests: `firmware/test_host/pipeline_test.c`, `packet_normalize_test.c`, `countdown_test.c`.

### What exists (render)

`firmware/gui/dial_screen.{hpp,cpp}` now renders all three elements, Simple-tier only, as
placeholders (no final art/theme pass):

- **Distance marker** — live text, `distance_meters`, blanked outside `VIEW_ACTIVE`.
- **Arrow** — LVGL built-in symbol glyphs (no custom art yet): `LV_SYMBOL_UP` rotated per
  `nav_icon_t` for STRAIGHT/SLIGHT/TURN/SHARP/U_TURN; fixed glyphs for ROUNDABOUT (`LOOP`),
  ARRIVED (`OK`), UNKNOWN (`WARNING`).
- **Compass ring** — grey circular ring + red north-marker dot, heading-up (whole ring rotates by
  raw `heading_deg`, no smoothing — see gap #2 below); marker hidden when heading is unknown
  (`0xFFFF`).

### What does not exist yet (render)

- **Real arrow/compass art** — current glyphs/shapes are LVGL-drawn placeholders, not final icon
  assets.

### Backend gaps flagged

1. arrival screen missing, to be developed 25.09
