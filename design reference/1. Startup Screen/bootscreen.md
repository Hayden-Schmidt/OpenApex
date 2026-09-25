# Page 1 — Startup / Boot Screen

## Requirements

- Display for at least as long as the app takes to load and background processes to start —
  **minimum 2 seconds**.
- **Simple:** static screen with a logo. *Maybe* a fade-in if the C3 can handle it.
- **Rich:** logo animation (logo TBC — defer).

## Reference

Beeline Moto 2 boot screen: plain black with a simple white logo — high contrast, easy to see.



## Baked in or software driven? — investigated, software

The logo cannot be baked into the panel on either board:

- **C3 / GC9A01** is a dumb SPI panel — no nonvolatile storage, no boot-logo feature. Its GRAM is
  uninitialised SRAM at power-on and the display stays off until the MCU clocks out the init
  sequence. There is nothing to bake a logo *into*.
- **S3 / CO5300 AMOLED** is the same class of part with the same answer.
- The only genuine pre-app option on ESP-IDF is a custom second-stage bootloader component that
  brings up SPI + the panel and blits a raw frame before the app image loads. That means
  reimplementing panel bring-up in an environment with no heap and no DMA setup, plus flash for a
  second framebuffer, to win roughly 200-300ms. Rejected.

So the boot screen is **software driven on both tiers**, and `BOARD_HAS_SPLASH` is 1 for the C3
profile too: here the splash is not a richness feature, it is what stands between power-on and a
visible glitch.

The investigation surfaced two real boot-order defects, both now fixed:

1. `display_driver_init()` raised the backlight *before* `esp_lcd_panel_init()` and before any pixel
   was written, so power-on showed a burst of uninitialised GRAM. The backlight now starts off and
   is raised by a separate `display_driver_backlight_on()`, which `gui_task` calls only after
   `lv_refr_now()` has flushed the complete first frame.
2. `gui_task` was created *last* in `app_main()`, after `ble_link_init()` brought up NVS and the
   whole NimBLE host — hundreds of milliseconds of dark screen. It is now created first, so that
   startup work happens behind the logo rather than ahead of it.

## Current code status: ✅ rendered

`firmware/gui/splash_screen.{hpp,cpp}`. Logo geometry verified against `Boot Screen.svg` at 240x240:
ink lands at x 89-150, y 85-154 (reference 89.375-150.625, 85-155) — within half a pixel.

The splash is the first 2s of any simulator run:

```powershell
firmware/sim_lvgl/.pio/build/sim_lvgl/program.exe --page idle
```

**Implementation notes:**

- The 2s hold lives in `gui_app.cpp`, not in `SplashScreen`, because it is a *routing* decision: the
  splash is not a `view_state_t`, it is what is shown before any state is honoured. It is a floor,
  not an added delay — real startup work runs concurrently behind it.
- The logo is an ordinary manifest icon, so it is rasterized per board profile like every other
  glyph, and `lv_obj_center()` places it (the SVG has it dead-centre, so no reference-pixel maths).
- **Swapping the logo is a manifest edit only.** The manifest key is `logo`, deliberately not named
  after the current art, so it generates `ICON_LOGO` and `splash_screen.cpp` never has to change.
  Point `icons.logo.svg` at the new file, set `size_240` to the desired box size at the 240px
  reference, re-run `python tools/build_icon_raster.py`, rebuild. Two constraints from the pipeline:
  the source must have a **square viewBox** (the generator hard-fails otherwise), and it is
  rasterized to an **A8 alpha mask**, so only the silhouette survives — a multi-colour logo renders
  as a flat theme-coloured shape. A multi-colour logo would need a different pixel format and a
  different accessor.
- The placeholder SVG fills the logo `#D0D000`. That is a Figma placeholder and contradicts the
  "default theme colour is not yellow" rule in `DESIGN NOTES.md`, so the mask is recoloured from
  `gui_theme().palette()` and the boot screen tracks the brand colour automatically.
- Fade-in (400ms) is on for both tiers. The requirements above allowed it "maybe, if the C3 can
  handle it" — it can: one object-opacity value over a 70px image, blended into an area LVGL is
  already redrawing.

**Outstanding:**

- **Logo asset is still the `cat-svgrepo-com.svg` placeholder.** See the swap procedure above.
- **RICH-tier logo animation deferred**, per the requirements — no point animating a placeholder.
  Both tiers currently render the same fade-in.
- **Theme-change boot animation** (`DESIGN NOTES.md` Step 2: a pseudo-reboot animating the logo from
  the old theme colour to the new one when connecting to another rider's device) needs the runtime
  config store, which does not exist.
