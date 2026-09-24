# Maneuver Set

Canonical source: `maneuvers.json` in this folder. Every maneuver is a list of two
primitives -- `straight` (length) and `curve` (dir, angle, radius) -- chained
tip-to-tail from `(0,0)` heading up the screen. `build_icons.py` is a generic
walker over that list, not a per-maneuver code path, so adding or tuning a
maneuver only ever means editing `maneuvers.json` and rerunning the script.

`SVG path pack/` and `Google Icons Archive/*.svg` are **visual reference only** and
are never parsed for geometry. An earlier pipeline did derive centrelines from
those traced outlines by averaging the two stroke edges; it produced
non-monotonic, zigzagging points wherever the two edges fell out of
correspondence (corners, and the arrowhead notch), and it has been removed.

Coordinate convention matches SVG: x right, **y down**. A `curve` with
`dir: "right"` bends toward +x.

| Key | Name | Secondary path? | Notes |
|---|---|---|---|
| straight | Straight | no | 240-unit vertical |
| turn_left / turn_right | Turn Left / Right | no | 90 deg, radius 80 |
| turn_slight_left / turn_slight_right | Slight Left / Right | no | 35 deg, radius 80 |
| turn_sharp_left / turn_sharp_right | Sharp Left / Right | no | 125 deg, radius 45, short exit leg |
| u_turn_left / u_turn_right | U-Turn Left / Right | no | 180 deg, radius 100 |
| roundabout_left | Roundabout Left | no | 90 deg loop sweep -- net exit left |
| roundabout_straight | Roundabout Straight | no | 180 deg loop sweep -- net exit straight |
| roundabout_right | Roundabout Right | no | 270 deg loop sweep -- net exit right |
| merge_left / right_merge | Merge Left / Right | yes | main = through lane |

Entries with `mirror_of` are generated as the exact x-mirror of their source, so
the natural left/right pairs are defined once. The three roundabouts are each
written out in full: they share a rotational sense and their entry/exit kinks but
are not mirrors of one another, just three sweep amounts on the same shape.

## Generated fields

- `main_path` -- the centreline, base to tip. Its **last point is
  `head_junction`**: the shared arrowhead glyph is placed tip-first exactly
  there, so a following maneuver continues from precisely where the previous
  arrowhead's tip sat, with no gap or overlap at the join.
- `head_junction` -- duplicate of that last point, kept for consumers that only
  want the join.
- `rotation` -- net heading change in degrees, signed, normalized to (-180, 180].
  For viewport-transform use. Chaining does **not** use it (it derives its own
  heading delta from the points, so it stays robust to any shape).
- `secondary_path` -- merges only; a side artifact, not a navigable line.
- `ARROWHEAD` (emitted alongside `ICONS`) -- the shared glyph, traced from the
  real Google Material Symbol (so it is the actual flared/barbed head, not a
  plain triangle), re-expressed tip-at-origin pointing up. Its vertices are
  hand-authored in `build_icons.py` (`_HEAD_LEFT`), not derived from anything.

Framing is deliberately **not** baked in here. `Demo/demo.js` frames each
maneuver from its own bounding box, measured in the rotated camera frame, at a
**fixed scale** -- there is no camera zoom, because size constancy matters more
than tight fit on a display glanced at for a fraction of a second. (An earlier
`center_offset_x` field served the old base-following camera and has been
removed.)

What this file must guarantee in exchange is that every maneuver actually fits
that one frame. `check_fit()` in `build_icons.py` enforces it: it inflates each
bbox by half the stroke width plus the arrowhead's scaled half-width and fails
the build if the diagonal exceeds the 600-unit display. Keep its
`DISPLAY_DIAMETER` / `LINE_THICKNESS` / `ARROWHEAD_SCALE` constants in sync with
`Demo/params.js` and `Demo/index.html`.

Regenerate `Demo/icons-data.js` with `python build_icons.py`, or leave
`python watch.py` running to rebuild on every save of `maneuvers.json`.

`Demo/icons-data.js` is the **only** build output. The script used to also write
a per-maneuver `*.json` beside this file (plus an `arrowhead.json`); nothing ever
read them -- they were duplicates of what the bundle already carries -- so they
were deleted and the writes removed.
