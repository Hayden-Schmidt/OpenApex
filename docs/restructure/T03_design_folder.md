# T03 — Reorganise `design reference/` → `design/`

Read `00_CONTEXT.md` first.

The owner designs terminal and phone screens in Figma and exports reference SVGs, notes and art
here. The owner approved renames and organising only: **no content changes, and no renaming of the
exported files themselves** (except where listed).

## Target

| Today | New |
|---|---|
| `design reference/` | `design/` |
| `design reference/DESIGN NOTES.md` | `design/DESIGN_NOTES.md` |
| `design reference/Screen Transitions.md` | `design/SCREEN_TRANSITIONS.md` |
| `design reference/1. Startup Screen/` | `design/screens/01-startup/` |
| `design reference/2. Idle Screen/` | `design/screens/02-idle/` |
| `design reference/3.Turn by Turn/` | `design/screens/03-turn-by-turn/` |
| `design reference/4. Odometer page/` | `design/screens/04-odometer/` |
| `design reference/6. Calling/` | `design/screens/05-calling/` |
| `design reference/7. Add Freind Screen/` | `design/screens/06-add-friend/` |
| (new, empty + README) | `design/screens/07-rider-mesh/` — home for the rider-mesh terminal designs (U1-U12 in blueprint 04) |
| `design reference/5. Phone App/` | `design/phone-app/` |
| `design reference/Characters/` | `design/characters/` |
| `design reference/nav_icons_svg/` | `design/icons/` |

Files inside keep their names. Exception: the per-screen notes files (`bootscreen.md`,
`2. idle screen.md`, `turn by turn.md`, `odometer page.md`) become `NOTES.md` in their folder, so
every screen folder reads the same.

`design/README.md` (new, short) holds:

- one line per folder
- "Figma exports go in the numbered screen folder; the terminal pages are 240 px reference frames"
- the old → new table above, so old links in history still make sense

`design/screens/07-rider-mesh/README.md` (new) says: "Designs for blueprint 04 (rider mesh). One
file or subfolder per U-number (U1-U12), matching the U-table in `docs/blueprints/04_rider_mesh.md`
(`docs/RiderMesh_Phase2_Plan.md` until T13)."

## Steps

1. Record the current outputs of the icon rasteriser: copy `firmware/gui/generated/*.h` to a temp
   folder.
2. `git mv` everything per the table. On Windows, a case-only or space-only rename can need a
   two-step `git mv` via a temp name.
3. Fix paths in code and tools, using `git grep -n -F "design reference"`. Today that finds:
   - `tools/build_icon_raster.py` — `MANIFEST_PATH` (a path join with "design reference",
     "nav_icons_svg"), plus the docstring and generated-banner text
   - `tools/export_page_svg.py` — comments/strings naming "design reference/"
   - `design/icons/icons_manifest.json` and `design/icons/README.md` (self-references and
     `../../tools/…` relative paths: re-check the depth, `design/icons` is one level shallower
     than `design reference/nav_icons_svg`, so `../../` becomes `../`)
   - `firmware/gui/*.cpp/.hpp`, `firmware/gui/avatars_data.h`, `firmware/main/board_profile.h`,
     `nav_model.h`, `view_state.h`, `firmware/sdkconfig.defaults`, `firmware/sim_lvgl/src/main.cpp`
     (comments)
   - `firmware/README.md`, `docs/DEVELOPMENT_SETUP.md`, `design/phone-app/README.md`,
     `design/DESIGN_NOTES.md`, the SVG with an embedded path (`Turn by Turn ref (turn_right).svg`)
   - `.gitignore` lines 65-69 (`design reference/Characters/**` patterns) → `design/characters/**`
   - also `nav_icons_svg`, `DESIGN NOTES`, `Screen Transitions`, `Characters` and each old screen
     folder name: grep each.
4. Run `python tools/build_icon_raster.py`. Diff `firmware/gui/generated/*.h` against step 1. Only
   comment paths may differ.
5. Check the `.gitignore` patterns still ignore what they did: `git status --ignored design/` should
   list the same kinds of files the old paths ignored.

## Done when

- `git grep -n -F "design reference"` is empty outside this pack, `docs/OpenApex_SPEC.md` (log SPEC
  hits for T14) and `design/README.md`'s old → new table.
- Regenerated icon headers differ only in comment paths.
- `pio run -d firmware -e prototype_c3` and `pio run -d firmware/sim_lvgl` build.
- `00_CONTEXT.md` is updated and one commit is made.

## Out of scope

Editing any design content; deleting any asset (list anything that looks orphaned in the report);
the `webp`/`jpg` inspiration images stay where they are.
