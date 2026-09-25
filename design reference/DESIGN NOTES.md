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

### Icon assets: SVG → device rasterization ✅

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
3. Include `firmware/gui/icons.hpp` and call `icon_image(ICON_<NAME>)` for the `lv_image_dsc_t*`.
   Do **not** include the generated header directly: its payload is `static const`, so every
   translation unit that includes it gets a private copy. `icons.cpp` is the one file that defines
   `ICON_DATA_IMPL` and instantiates the pixels.

The rasters are **A8 alpha masks**, not colour bitmaps — set the colour at draw time with
`lv_obj_set_style_image_recolor()` + `..._recolor_opa(LV_OPA_COVER)`, taking the colour from
`gui_theme().palette()` rather than a literal.

Prefer a raster over reconstructing an icon from LVGL primitives. The battery indicator on the idle
screen was originally specced as one outline plus a drawn level bar to save memory; measured, the
full `battery_android_0..6` + `full` set is 4.6 kB of flash at 240px (0.1% of a 4 MB part) while
the drawn bar needed four hand-measured interior coordinates that go stale the moment the icon is
re-exported. At 240px the icon's interior is ~10px wide, so a "continuous" bar has no more visible
steps than the eight rasters. Flash is cheap here; hand-measured geometry is not.

---

## Step 3 — UI development

Dev plan: work **one element at a time**, folder by folder through this directory. Each folder
details one "page" and the elements on that page.

| Page | File | Current status |
|------|------|----------------|
| 1. Startup / boot screen | `1. Startup Screen/bootscreen.md` | ⬜ (splash is a Layer-2 screen, not built) |
| 2. Idle screen | `2. Idle Screen/2. idle screen.md` | ✅ (rendered + connect animation; data is fixture-only) |
| 3. Turn-by-turn | `3.Turn by Turn/turn by turn.md` | 🔶 (model + pipeline done, render stub) |
| 4. Odometer page | `4. Odometer page/odometer page.md` | ⬜ (no odometer data in model yet) |

Update this doc and each page doc as you go to reflect current status and outstanding items
(backend and phone app).

### GUI implementation reality check

The `Screen` / `GuiTheme` / `BasicTheme` / `RichTheme` hierarchy from `docs/OpenApex_SPEC.md` §16.6
**is built**. `firmware/gui/` now holds:

- `screen.hpp` — base class; each page owns its own top-level `lv_obj` so LVGL's screen-transition
  API can move between page instances.
- `theme.hpp` + `basic_theme.cpp` / `rich_theme.cpp` — palette and transition style, selected at
  compile time by `BOARD_GFX_TIER`. The theme owns the *how*; the page owns the *what*. Reach it
  from a page via `gui_theme()` in `gui_app.hpp`.
- `gui_app.cpp` — routes `view_state_t` to a page and forwards every frame to it.
- `idle_screen.cpp`, `dial_screen.cpp` — the two pages that exist.
- `icons.hpp` / `icons.cpp` — accessors for the generated raster set.

**Writing a page:** build the whole widget tree in the constructor and only mutate it in
`update()` — `update()` runs every frame and must not allocate. Size everything off
`lv_display_get_horizontal_resolution(lv_display_get_default())`, never `BOARD_DISP_WIDTH`, so one
binary renders correctly at any panel size; express geometry as reference-design pixels against the
240px Figma frame and scale at runtime (see `IdleScreen::px()`). Take colours from
`gui_theme().palette()`, never a literal.

**Reviewing a page:** `firmware/sim_lvgl` runs the real `firmware/gui/` sources against a scripted
fixture. `program.exe --page idle` replays one page's states; `--shot <ms> <file.png>` dumps a frame
for diffing against the Figma SVG. See `docs/DEVELOPMENT_SETUP.md`.

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
2. **Odometer** data model + NVS persistence. `terminal_view_state_t.odometer_meters` exists and the
   idle screen renders it, but nothing writes it.
3. **Runtime config store** (theme colour, unit, 24h/12h, element visibility) — no schema. The idle
   screen hardcodes km as a result.
4. **Compass smoothing** for phone-GNSS-derived heading (data exists, smoothing does not).
5. **Time/RTC**. `terminal_view_state_t.clock` exists and the idle screen renders it (falling back
   to `--:--`), but no source populates it. S3 has a PCF85063 RTC in `board_profile.h`; the C3 has
   none and needs phone-side time sync.
6. **BLE link state**. `terminal_view_state_t.phone_connected` drives the idle screen's whole
   connect animation, but `firmware/main/ble_link.c` only logs connect/disconnect — it must publish
   into `shared_view`.

Also outstanding, GUI-side rather than backend: the RICH/466px tier's clock renders undersized.
LVGL's bundled Montserrat stops at 48px and that layout wants ~85px, so it needs a face generated
with `lv_font_conv`.

Stage commits as you go.