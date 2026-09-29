# Phone App: design reference

Design tokens extracted from the terminal UI (`firmware/gui/`) so the phone app matches the device.

| File | Use |
|---|---|
| `openapex.tokens.json` | W3C design tokens. Import into Figma as Variables (Tokens Studio plugin, or any DTCG variables importer). Later generates the Compose `ui/theme`. |
| `DESIGN.md` | Design system brief for agent tools (Open Design) and for building the Compose theme. |
| `openapex-style-sheet.svg` | Visual sheet. Drag into Figma as a reference frame. |

## Where each token comes from
- Accent `#00FFE8`, success `#1E8D3E`: `basic_theme.cpp` / `rich_theme.cpp`
- Traffic colours: `trip_arc.cpp`
- Compass colours: `compass_ring.cpp`
- Greys: `dial_screen.cpp` (stale), `odometer_screen.cpp` (chrome), `arrived_screen.cpp` (icon)
- Type sizes: glyph-height constants in `dial_screen.cpp`, `idle_screen.cpp`, `odometer_screen.cpp`
- Durations: `k*Ms` constants in the same files plus `gui_app.cpp`. Easing rules from `../SCREEN_TRANSITIONS.md`.

## Notes for phone design
- The font sizes are terminal pixels at the 240px reference. Keep the ratios and the phone scale is up to you.
- The bezier values are CSS approximations of the firmware's hand-written curves (`ease.hpp`).
- The accent is runtime-themeable (`../DESIGN_NOTES.md` step 2), so design every screen with it as a variable, never a literal.
- If a token changes, update it here and in firmware together.
