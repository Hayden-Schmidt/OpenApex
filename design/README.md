# Design

Reference designs for the terminal and the phone app. The owner designs in Figma and exports here.
Terminal pages are authored against a 240 px reference frame; the firmware scales them per board.

| Folder | Holds |
|---|---|
| `screens/01-startup/` … `screens/06-add-friend/` | One folder per terminal page: the Figma export(s) and a `NOTES.md` with the page's intent |
| `screens/07-rider-mesh/` | Rider-mesh terminal designs (blueprint 04, U1-U12) |
| `phone-app/` | Phone app design system: tokens, style sheet, notes |
| `characters/` | Preset avatar pixel art (the firmware transcribes these into `firmware/gui/avatars_data.h`) |
| `icons/` | Flat status/marker SVGs plus `icons_manifest.json`, rasterised per screen by `tools/icons/build_icon_raster.py` |
| `DESIGN_NOTES.md` | Cross-page design notes and per-page implementation status |
| `SCREEN_TRANSITIONS.md` | How pages enter and leave, and the shared easing rules |

Turn arrows are not here: they are geometry, not artwork. See `tools/maneuver_icons/`.

## Old paths (before the 2026-09-29 restructure)

| Old | New |
|---|---|
| `design/screens/01-startup/NOTES.md` | `screens/01-startup/NOTES.md` |
| `design/screens/02-idle/NOTES.md` | `screens/02-idle/NOTES.md` |
| `design/screens/03-turn-by-turn/NOTES.md` | `screens/03-turn-by-turn/NOTES.md` |
| `design/screens/04-odometer/NOTES.md` | `screens/04-odometer/NOTES.md` |
| `design/screens/05-calling/` | `screens/05-calling/` |
| `design/screens/06-add-friend/` | `screens/06-add-friend/` |
| `design/phone-app/` | `phone-app/` |
| `design/characters/` | `characters/` |
| `design/icons/` | `icons/` |
| `design/DESIGN_NOTES.md` | `DESIGN_NOTES.md` |
| `design/SCREEN_TRANSITIONS.md` | `SCREEN_TRANSITIONS.md` |
