# SVG -> device icon rasterization pipeline

Scope: turning the flat status/marker SVGs in `design reference/nav_icons_svg/` into pixel data
firmware can draw, at the correct size for each physical screen. **Not** in scope: wiring the
generated icons into any actual UI page (there's only the nav dial screen today), and **not** the
turn-by-turn maneuver arrows -- those are a separate, deliberately non-bitmap system (see
"Relationship to the maneuver arrow pipeline" below).

## Pieces

- **`icons_manifest.json`** -- hand-authored source of truth. One entry per icon: which SVG file,
  and its render size in pixels (`size_240`), authored against a 240px-wide reference screen.
- **`../../tools/build_icon_raster.py`** -- reads the manifest, rasterizes each icon at the correct
  pixel size for every screen profile, and writes one generated C header per profile to
  `../../firmware/gui/generated/icons_<profile>.h`.
- **Generated headers** (`firmware/gui/generated/icons_*.h`) -- checked into git, like
  `firmware/gui/nav_icons_data.h`. Never hand-edited; re-run the script and commit the diff instead.

## Screen profiles

The script carries its own `SCREEN_PROFILES` table, a hand-synced Python twin of
`firmware/main/board_profile.h`'s `BOARD_DISP_WIDTH`/`BOARD_DISP_HEIGHT`:

| profile id             | board_profile.h macro              | size    |
|-------------------------|-------------------------------------|---------|
| `prototype_c3_gc9a01`   | `BOARD_PROFILE_PROTOTYPE_C3_GC9A01` | 240x240 |
| `s3_amoled_175`         | `BOARD_PROFILE_S3_AMOLED_175`       | 466x466 |

There's no automated way to pull those numbers out of the C header from Python without running a
compiler, so — same call as `build_icons.py`'s `FRAME_CENTER_OVERRIDES` table — this is kept in
sync by hand. **When you add or resize a board profile in `board_profile.h`, update
`SCREEN_PROFILES` in `build_icon_raster.py` to match, then re-run the script.**

An icon's `size_240` in the manifest is its render size on the 240px reference profile; every other
profile scales it by `profile.width / 240` (identical convention to how `nav_renderer.cpp` scales
maneuver geometry off its 600-unit design diameter). One manifest entry therefore fans out to one
correctly-sized bitmap per screen profile automatically — an icon used on multiple screens needs no
extra manifest work, just a reference in code on each screen.

## Pixel format: A8 alpha mask, not RGBA

Every source icon here is a flat, single-colour Material Symbols glyph. Shape lives entirely in the
alpha channel, so each icon is rasterized to `LV_COLOR_FORMAT_A8` (1 byte/px) rather than RGBA
(4 bytes/px) or RGB565 (2 bytes/px, and useless without alpha for a non-rectangular icon anyway).
Colour is applied at draw time via LVGL's image recolor/tint, the same technique
`NavRenderer::build_arrowhead_mask()` already uses for the shared arrowhead glyph — one raster
serves any theme colour or state tint (e.g. a battery icon recoloured red at low charge) with no
extra asset variants. This also keeps flash cost down on the C3 target (16 icons at both profiles
combined is ~45 KB total; RGBA would be 4x that).

## Why the SVGs aren't decoded on-device

`lv_conf.h` has `LV_USE_LODEPNG 0` and `LV_USE_TJPGD 0` — no PNG/JPEG decoder is compiled in, and
there is no SVG decoder in this build at all. Every image LVGL draws must already be an
`lv_image_dsc_t` with raw pixel data. Rasterizing SVG -> PNG -> A8 array happens entirely offline in
`build_icon_raster.py`, using `resvg-py` (a prebuilt `resvg` binary wheel — no system Cairo/Inkscape
dependency, which matters on Windows dev machines) and Pillow to lift the alpha channel out of the
rendered PNG.

## Relationship to the maneuver arrow pipeline

`Google Icons Archive/Icon JSON/build_icons.py` + `firmware/gui/nav_icons_data.h` +
`firmware/gui/nav_renderer.cpp` are a *different, older* system: turn arrows are drawn as vector
line/arc primitives at runtime, specifically to avoid `LV_USE_FLOAT`/ThorVG's RAM cost on the C3
(no PSRAM, no FPU — see `docs/OpenApex_SPEC.md` §16.6). That system stays as-is. This pipeline is
for icons whose shape doesn't reduce to a handful of drawable primitives — status glyphs, pins,
battery/bluetooth states — where offline rasterization is the documented fallback for exactly that
reason (see the `docs/OpenApex_SPEC.md` note on `build_arrowhead_mask()`-style pre-rasterization).
The two pipelines deliberately do not share a manifest or an icon ID space.

## Usage

```
python -m pip install -r tools/requirements-icons.txt   # first time only
python tools/build_icon_raster.py
```

Adding a new icon:
1. Drop the SVG into `design reference/nav_icons_svg/` (square viewBox; non-square source art will
   fail the build).
2. Add an entry to `icons_manifest.json` with its `size_240`.
3. Re-run the script, review the diff to `firmware/gui/generated/`, commit both.

Consuming an icon in firmware: include the profile's generated header (selected the same way
`board_profile.h` is, via the active `BOARD_PROFILE_*` build flag) and reference
`&ICON_DESC[ICON_<NAME>]` as an `lv_image_dsc_t*` with `lv_image_set_src`. Wiring this into an
actual screen is out of scope here — no page other than the nav dial exists yet.

## Open decisions made while building this (see also the plan discussion above)

- **Rasterizer**: `resvg-py` — prebuilt binary wheel, installed cleanly with no native toolchain or
  system library, unlike `cairosvg` (needs system Cairo) or Inkscape/ImageMagick CLIs (not present
  on this machine and heavier to provision in CI).
- **Pixel format**: A8 alpha mask (see above), not `LVGLImage.py`'s general-purpose RGB/indexed
  formats — our source art doesn't need them, and A8 is a fifth the size of the smallest colour
  format that would still carry alpha.
- **Generated headers are checked in**, not gitignored build output — matches `nav_icons_data.h`'s
  precedent so the firmware build never gains a Python/resvg/Pillow dependency.
