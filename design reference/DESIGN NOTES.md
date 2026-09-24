# OpenApex UI — Design Notes

A circular navigation display for motorcycle handlebars. High-contrast, readable at speed and in
sunlight, and implementable across every supported board tier.

> **Status legend** used throughout this folder:
> - ✅ implemented in code (see reference)
> - 🔶 partially implemented / stub
> - ⬜ not started / blocked on hardware or backend

---

## Step 1 — Design template / characteristics

The following are the fixed design rules every UI element (device **and** later phone app) must
follow:

- **High contrast** — easy to see in the sun and at speed. Default theme colour is **not yellow**;
  final value TBC.
- **Simple to implement** and runnable on all platforms (C3 BASIC tier and S3 RICH tier).
- **Practically scalable** — simple for C3 hardware, rich for S3 hardware (see
  `docs/OpenApex_SPEC.md` §16.6 for the tier model).
- **Consistent look** — fonts, corner radii, and other design tokens must be shared across the
  device UI and the later phone app.

---

## Step 2 — Infrastructure

A lot of UI will be configurable not just at compile time but **at runtime via the phone app**
(theme colours, hiding small elements like the compass, etc.). The UI infrastructure needs a
central reference plan for these configs so they can be made dynamic/live later.

**Open decision (needs discussion):** whether configs are stored on the **app** or on the
**device**. Ideal: stored on the device so they persist across runs and load on startup. But if a
rider connects to a *friend's* device, connecting should trigger a visible theme change — either an
idle-screen animation highlighting the theme colour loaded from the app, or a pseudo-reboot where
the logo page animates from the old theme colour to the new one (requires the splash to show theme
colour). **Not yet implemented** — this is backend/config infra, not UI.

Current compile-time config surface lives in `firmware/main/board_profile.h`:

| Flag | C3 `PROTOTYPE_C3_GC9A01` | S3 `S3_AMOLED_175` |
|------|--------------------------|--------------------|
| `BOARD_GFX_TIER` | `BASIC` | `RICH` |
| `BOARD_HAS_SPLASH` | 0 | 1 |
| `BOARD_HAS_MAP_RENDER` | 0 | 0 (flips when §5.4 polyline stream lands) |
| `BOARD_HAS_TOUCH` | 0 | 1 |
| `BOARD_HAS_GNSS` | 0 | 0 (GPS variant not modeled yet) |
| `BOARD_HAS_IMU` | 0 | 1 |
| display | 240×240 GC9A01 | 466×466 AMOLED (stub) |

There is **no runtime config store yet** (no NVS schema, no phone→device config packet). Flagged
for post-UI-sprint backend work.

### Icon assets: SVG → device rasterization ✅ (pipeline only, no page consumes it yet)

Status/marker icons (battery, bluetooth, location pins, warning, etc. — **not** the turn-by-turn
maneuver arrows, which stay procedural, see `firmware/gui/nav_renderer.cpp`) are authored as SVG in
`design reference/nav_icons_svg/` and rasterized at build time to per-board-profile LVGL bitmaps.
Full pipeline docs live in `design/icons/README.md`; the short version, when a page in this folder
needs a status icon:

1. Add the SVG to `design reference/nav_icons_svg/` (square viewBox) and register it in
   `design/icons/icons_manifest.json` with its render size at the 240px reference profile.
2. `python -m pip install -r tools/requirements-icons.txt` (first time only), then
   `python tools/build_icon_raster.py` — writes `firmware/gui/generated/icons_<profile>.h` for
   every board profile in `firmware/main/board_profile.h`, correctly scaled per screen.
3. Reference `&ICON_DESC[ICON_<NAME>]` from the generated header as an `lv_image_dsc_t*` in the
   page's GUI code.

---

## Step 3 — UI development

Dev plan: work **one element at a time**, folder by folder through this directory. Each folder
details one "page" and the elements on that page.

| Page | File | Current status |
|------|------|----------------|
| 1. Startup / boot screen | `1. Startup Screen/bootscreen.md` | ⬜ (splash is a Layer-2 screen, not built) |
| 2. Idle screen | `2. Idle Screen/2. idle screen.md` | ⬜ (`VIEW_IDLE` state exists, not rendered) |
| 3. Turn-by-turn | `3.Turn by Turn/turn by turn.md` | 🔶 (model + pipeline done, render stub) |
| 4. Odometer page | `4. Odometer page/odometer page.md` | ⬜ (no odometer data in model yet) |

Update this doc and each page doc as you go to reflect current status and outstanding items
(backend and phone app).

### GUI implementation reality check

The entire GUI layer is currently a **stub** (`firmware/gui/gui_app.cpp`):

```cpp
// TODO (#3): replace with DialScreen once Screen/GuiTheme/BasicTheme exist (§16.6 Layer 1).
lv_obj_t *s_screen = nullptr;
```

- `gui_app_init()` creates a single black `lv_obj` and loads it.
- `gui_app_update()` is a no-op (`(void)state;`).

The `Screen` / `GuiTheme` / `BasicTheme` / `RichTheme` / `DialScreen` class hierarchy described in
`docs/OpenApex_SPEC.md` §16.6 has **not yet been written** — `firmware/gui/` contains only
`gui_app.cpp`, `gui_app.hpp`, `CMakeLists.txt`, and `idf_component.yml`. The C/C++ seam
(`gui_app_init` / `gui_app_update`) is in place and correct; the screen content behind it is the
next work item.

The data side *is* complete and host-tested:

- `firmware/main/view_state.h` — `terminal_view_state_t` + `view_state_t` (the states the GUI
  renders).
- `firmware/main/pipeline.c` — `view_state_apply_packet()` / `view_state_tick()`.
- `firmware/main/countdown.c` — countdown interpolation engine.
- `firmware/main/normalize.c` — text→maneuver/street/distance normalization.
- `firmware/sim_lvgl/src/main.cpp` — SDL simulator that drives the GUI with a scripted fixture of
  all four `view_state_t` states.

---

## Step 4 — Backend hookup plan

All discovered elements needing further backend implementation are flagged in each page doc below,
and should be collated into a new doc in `docs/` once the UI sprint finishes. Current known
backend gaps:

1. **Rerouting** maneuver (no `nav_icon_t` value, not surfaced by any maps app adapter yet).
2. **Odometer** data model + NVS persistence (no field in `terminal_view_state_t`).
3. **Runtime config store** (theme colour, unit, 24h/12h, element visibility) — no schema.
4. **Compass smoothing** for phone-GNSS-derived heading (data exists, smoothing does not).
5. **Time/RTC** for the odometer page (S3 has PCF85063 RTC in `board_profile.h`; C3 has none).

Stage commits as you go.