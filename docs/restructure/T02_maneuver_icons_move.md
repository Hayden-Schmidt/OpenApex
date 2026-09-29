# T02 — Move the maneuver icon pipeline and reference renderer to `tools/maneuver_icons/`

Read `00_CONTEXT.md` first.

## What these folders really are

- **`Google Icons Archive/Icon JSON/`** is **live source**, not an archive:
  - `maneuvers.json` is the canonical geometry of every turn arrow.
  - `build_icons.py` generates `firmware/gui/nav_icons_data.h` (shipped in firmware) and
    `Demo/icons-data.js`.
  - `watch.py` re-runs it on save.
  - `icons.md` documents the maneuver set.
- **`Demo/`** is the **reference maneuver renderer** in JS (`demo.js`, `lvgl_shim.js`,
  `params.js`, `index.html`). `firmware/gui/nav_renderer.cpp/.hpp` is a C++ port of it, and its
  comments cite `Demo/params.js`, `Demo/demo.js` and `Demo/lvgl_shim.js` as the source of ported
  constants.

The "Archive" name invites deletion of the only arrow source, so the whole pipeline moves together.

## Target

```
tools/maneuver_icons/
  maneuvers.json
  build_icons.py
  watch.py
  README.md          (was icons.md)
  renderer/          (was Demo/: index.html, demo.js, lvgl_shim.js, params.js, icons-data.js)
```

## Steps

1. Record the current outputs: copy `firmware/gui/nav_icons_data.h` and `Demo/icons-data.js` to a
   temp folder outside the repo.
2. `git mv` the files to the target. Leave `__pycache__/` behind; it is git-ignored, so delete it
   locally.
3. Fix the script paths in `build_icons.py`. Today they are relative to its own folder (`HERE`):
   - `DEMO_JS_OUT = HERE/../../Demo/icons-data.js` → `HERE/renderer/icons-data.js`
   - `FIRMWARE_C_OUT = HERE/../../firmware/gui/nav_icons_data.h` stays at the same depth
     (`tools/maneuver_icons` is two levels down, like before). Confirm, don't assume.

   `watch.py` uses only same-folder paths; confirm.
4. Update text references (comments, docs, the generated-header banner template inside
   `build_icons.py`):
   - `Google Icons Archive/Icon JSON/…` → `tools/maneuver_icons/…`
   - `Demo/…` → `tools/maneuver_icons/renderer/…`

   Known files: `firmware/gui/dial_screen.cpp`, `dial_screen.hpp`, `nav_renderer.cpp`,
   `nav_renderer.hpp`, `compass_ring.cpp`, `nav_icons_data.h` (regenerated, not hand-edited),
   `design reference/nav_icons_svg/README.md`, and the moved files themselves (`demo.js` header,
   `icons.md`, `watch.py` docstring). Also `.gitignore` if it names either folder. Find them all
   with `git grep -n -E "Google Icons Archive|Demo/|icons-data|build_icons"`.
5. Remove the now-empty `Google Icons Archive/` and `Demo/` folders. Anything left in
   `Google Icons Archive/` that is **not** under `Icon JSON/` (check `git ls-files` and the disk):
   list it in the report and leave it for the owner.
6. Run `python tools/maneuver_icons/build_icons.py`. Diff both generated files against the copies
   from step 1. The only allowed differences are path strings in comments. Any geometry or number
   change means a path or ordering mistake: stop and fix it.
7. Open `tools/maneuver_icons/renderer/index.html` in a browser, or at least check its `<script
   src>` tags still resolve (they are same-folder).

## Done when

- `git grep -n -E "Google Icons Archive|\bDemo/"` is empty outside this pack and
  `docs/OpenApex_SPEC.md`. SPEC hits go to carry-over, for T14.
- Regenerated outputs differ from the originals only in comment paths.
- `pio run -d firmware -e prototype_c3` and `pio run -d firmware/sim_lvgl` build.
- `00_CONTEXT.md` is updated and one commit is made.

## Out of scope

- Changing any geometry, framing or renderer code.
- Merging with `design reference/nav_icons_svg/` (a different pipeline: flat status icons,
  rasterised by `tools/build_icon_raster.py`). T03 handles that folder.
