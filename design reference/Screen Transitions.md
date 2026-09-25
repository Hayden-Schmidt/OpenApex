# Screen Transitions

Planned animated flow between pages, and the parts of the GUI layer it touches. This is
high-level intent — the exact curves and timings below are the target, not yet implemented.

## Guiding rules

- All per-element motion uses **S-curve (ease-in-out) or inverse-exponential** easing. No linear.
- Bluetooth link state is **independent** of page animation state: it must be readable from the
  moment the page loads, even while a slide/fade is still in flight. Animation clocks are driven
  off page-load time so the live value is shown accurately, never frozen behind a tween.
- A "minimum hold" is always a **floor**, never a delay — if the underlying system is ready sooner,
  the hold is what keeps the element visible.
- Outgoing = the incoming sequence played **in reverse**.

## Flow

### 1. Loading screen (splash)

Shown for as long as the stack takes to load, with a **minimum of 2 s** (matching the current
`kSplashMinMs` floor in `gui_app.cpp`). Exit by fading out — the same fade curve as its fade-in
(no scaling or sliding).

### 2. Idle screen

Initial state after the splash.

1. **Odometer bubble** slides up onto screen from the bottom (0.7 s, S-curve).
2. **Bluetooth icon** slides out from behind the odometer bubble (0.4 s, S-curve — needs masking so
   it emerges from under the bubble).
3. Bluetooth state is tracked independently of step 1–2 (see guiding rules), so connecting during
   load is reflected accurately.
4. Once the phone connection is established, play the existing page-text slide-in.
   - On initial load, even if Bluetooth is already connected, the bubble and BT icon must **land
     fully first**, then play the text slide-in (this animation already exists).
5. Once the full "connected" page is displayed and all animations have finished, **wait for
   navigation to start**. Even if navigation already began, hold **a minimum of 2 s** from the
   moment the animations finish before leaving.
6. To leave: play text slide, then BT icon slide, then odometer bubble slide — in reverse.

### 3. Navigation page (default = turn-by-turn, compass config)

The next page is whichever the rider has set as their default nav page; starting with turn-by-turn
in compass configuration.

Incoming sequence:

1. **Arrow** slides in from the bottom. Prefer routing this through the existing
   NavRenderer morphing pipeline (`NavRenderer::add_maneuver`): start as a straight (or two
   straights) positioned off-page, with the camera already at the first real maneuver's position,
   then let the arrow *run* onto the page.
2. **Text** (street / distance / ETA) slides on along the bottom.
3. **Compass ring** (or Google-traffic `TripArc` + ETA text) enters from the outside: drawn
   oversized, centred on the page centre, then shrinks down to its final position so it reads as a
   slide-in from beyond the edge. S-curve / inverse-exponential.

Secondary option (config): fade the whole page in over 0.4 s. We will try both and pick the
cleaner.

## Parts this touches

| Layer / file                                                                  | Role                                                                 |
| ----------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| `gui/gui_app.cpp` — `screen_for_state()` + min-hold timers                    | Routing + the "minimum N s" floors (splash, nav-leave hold)           |
| `gui/theme.hpp` / `basic_theme.cpp` / `rich_theme.cpp` — `apply_state_change` | The transition hook. Basic is instant `lv_screen_load`; animation lands here (lvgl `lv_screen_load_anim` or per-element tweens) |
| `gui/idle_screen.hpp/.cpp`                                                    | Bubble / BT icon / text slide-in and reverse — per-widget tweens + masking |
| `gui/dial_screen.hpp/.cpp`                                                    | Nav-page entry: arrow, text, compass/trip-arc reveal                  |
| `gui/nav_renderer.hpp/.cpp` — `add_maneuver`                                  | Morphing pipeline the arrow entry rides through                       |
| `gui/compass_ring.hpp`, `gui/trip_arc.hpp`                                    | The "outer" element that oversized-zoom-enters                        |

## Open question

- Cleanest mechanism per element: LVGL avoid `lv_anim_t` keyed to each widget vs. a single
  page-level timeline. The BT-icon "independent of page animation" rule suggests the link status
  (data) and the slide (presentation) must be decoupled — a data publisher + a presentation
  timeline, rather than driving both from one state machine.