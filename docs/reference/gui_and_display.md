# GUI and display

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §9, §16.6 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## Display Behavior

*Was SPEC §9.*

The Phase 1 C3 renderer is a high-contrast dial:

- Idle/waiting: connection state and no fabricated navigation values.
- Active: large maneuver icon, countdown distance, speed, and optional street text.
- Close turn: visual countdown emphasis as distance approaches zero.
- Stale: last-known maneuver de-emphasized with a stale-link or stale-GNSS indicator.
- Arrived: completion glyph when the source notification indicates arrival.
- Diagnostics: source platform/application, packet age, GNSS validity, speed, heading, and profile.

The C3 UI should use partial redraws and bounded memory. The 1.75 AMOLED may use larger type,
double buffering in PSRAM, and later road-context graphics, but it must share the same normalized
navigation model.

## GUI architecture (was ADR 16.6)

*Was SPEC §16.6.*

**Decision:** one shared GUI composition drives every board profile, split into two layers, so the
C3 and S3 (and any later profile) present the same product with the same states and meaning, while
each device's UI quality scales with what its hardware can actually support — without capping the
richer targets to the leanest one or letting the profiles drift into unrelated UIs. Implemented as
C++ classes (per [decisions](decisions.md) ADR-2), not the plain-C single-file `gui_screens.c` from the original Phase 1
slice — that file has been removed. Reasons for the C++ rewrite:

- Layer 2 screens are created/destroyed dynamically (splash plays once and unloads; map screen loads
  only on demand) — plain C has no compiler-enforced cleanup path for that; C++ RAII does.
- A missing theme implementation for a board profile is a compile error with a virtual interface,
  not a null-pointer crash discovered on whichever device happens to hit the unfilled hook.
- The two-file, single-screen POC scope that justified plain C no longer holds now that C3 and S3
  are both permanently supported products, not a prototype-then-replace pair.

**Layer 1 — shared baseline (identical logic across all profiles).**

- `Screen` (`firmware/gui/screen.hpp`) — abstract base: `init()`, `update(const terminal_view_state_t&)`,
  virtual destructor. Every screen, Layer 1 or Layer 2, implements this.
- `DialScreen : Screen` (`firmware/gui/dial_screen.hpp/.cpp`) — owns the state machine
  (`view_state_t` -> what is shown) and the core dial layout (state banner, maneuver/distance
  readout, street line, presence ring). Widgets are members, created in `init()`, released in the
  destructor (RAII — no manual `lv_obj_del()` bookkeeping). Layout is expressed in relative terms
  (`LV_PCT`, DPI-relative font choice), so the same class scales from 240x240 (C3/GC9A01) to 466x466
  (S3/AMOLED) without forking layout code.
- `GuiTheme` (`firmware/gui/theme.hpp`) — abstract interface: `draw_icon(nav_icon_t, size)` (icon
  asset fidelity), `palette(...)` (color richness), `apply_state_change(prev, next)` (instant
  mutation vs. animated/crossfade transition, e.g. via `lv_style_transition_dsc_t` or
  `lv_screen_load_anim`). `DialScreen` holds a `GuiTheme&` and never branches on tier itself — the
  state machine and the meaning of each state never change per tier, only how a change is rendered.
  Two concrete implementations: `BasicTheme` (C3, instant/flat) and `RichTheme` (S3, animated).
  Selected per board profile via `BOARD_GFX_TIER_BASIC`/`BOARD_GFX_TIER_RICH` in `board_profile.h`,
  resolved to a concrete type at compile time (e.g. a type alias picked by `#if`) — no runtime
  branching, no unused theme code in either build.

**Layer 2 — capability-gated additive screens (opt-in per profile, never a ceiling on richer
hardware).** Each is a separate `Screen` subclass in its own file, compiled in only when a profile
declares the capability it needs — same pattern already used for `BOARD_HAS_TOUCH` compiling the
touch driver out: `SplashScreen` (`#if BOARD_HAS_SPLASH`), `MapScreen` (`#if BOARD_HAS_MAP_RENDER`,
once the polyline stream in [06_road_context](../blueprints/06_road_context.md) exists), a richer `DiagnosticsScreen` with live graphs. A leaner
profile simply never compiles or links these — they cannot bloat or slow down its build.

**C++/C seam.** `firmware/gui/gui_app.hpp` exposes the only points the C side calls, as
`extern "C"` functions taking POD structs: `gui_app_init()`, `gui_app_update(const
terminal_view_state_t*)`. `gui_app.cpp` owns the active `Screen` instance(s) and the selected
`GuiTheme`, and is the only file that constructs concrete screen/theme types. `firmware/main/main.c`
(`gui_task`) calls only these two functions and never sees a C++ type — matching the existing
POD-struct C→C++ boundary rule in [decisions](decisions.md) ADR-2.

**Build wiring.**

- ESP-IDF: `firmware/gui/` becomes its own component (`CMakeLists.txt` with
  `idf_component_register(SRCS ... INCLUDE_DIRS "." REQUIRES main)`), referenced from
  `firmware/CMakeLists.txt` via `EXTRA_COMPONENT_DIRS`. `firmware/main`'s `CMakeLists.txt` gains
  `gui` under `REQUIRES` (or `PRIV_REQUIRES`) instead of listing GUI sources directly.
- `firmware/sim_lvgl` (PlatformIO `native`): replaces the previous single-file textual include
  (`gui_screens_bridge.c` including `../../gui/gui_screens.c` directly) with a normal multi-TU C++
  build — `build_src_filter` in `platformio.ini` adds `../gui/*.cpp` alongside `src/*.cpp`.

**Dev-loop consequence:** `firmware/sim_lvgl` gains a second PlatformIO env built against an S3
profile stub (466x466, `BOARD_GFX_TIER_RICH`) alongside the existing C3 env. Every Layer 1 change is
checked in both simulator windows before being considered done, so tier drift is caught immediately
rather than discovered after S3 hardware exists. Layer 2 screens are simulated only under the S3 env
since leaner profiles never compile them.

**Why:** without this split, either every profile is limited to what fits on the leanest board
(240px round dial, bounded memory, no animation budget), or profiles diverge into separately
maintained UIs that drift out of sync over time. The split keeps exactly one state machine and one
layout contract, while leaving S3-class hardware fully open to animated transitions, a splash
screen, and map-following — none of which require touching or reworking the shared baseline.

**Dev-loop consequence:** `firmware/sim_lvgl` gains a second PlatformIO env built against an S3
profile stub (466x466, `BOARD_GFX_TIER_RICH`) alongside the existing C3 env. Every Layer 1 change is
checked in both simulator windows before being considered done, so tier drift is caught immediately
rather than discovered after S3 hardware exists. Layer 2 screens are simulated only under the S3 env
since leaner profiles never compile them.

**Resolution model:** every choice above (tier, capability flags, which Layer 2 screens exist) is
resolved at compile time via `board_profile.h` `#if`/`#define` selection, the same mechanism already
used for `BOARD_HAS_TOUCH`. A C3 build contains no S3-only screen code and no animated-transition
path; nothing is a runtime branch. Final builds stay as lean as they are today.
