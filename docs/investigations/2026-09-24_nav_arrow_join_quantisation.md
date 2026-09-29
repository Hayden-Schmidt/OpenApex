# 2026-09-24 — Nav arrow join quantisation

**Kind:** Investigation log — frozen; do not update except to add a closing note.  
**Moved from:** OpenApex_SPEC.md §18 (restructure, 2026-09-29)

## Nav arrow join quantisation (2026-09-24)

*Was SPEC §18.*

### Symptom

At every place a straight ran into a curve, the curve appeared shifted slightly sideways from the
straight, leaving a visible lip in the silhouette. Roundabouts were worst.

### Cause

`NavRenderer` carries maneuvers as primitives (line, arc) rather than sampled points, and hands
each one to LVGL separately. Consecutive primitives are exactly tangent in maneuver space and stay
tangent through `world_to_screen()`, which is a similarity. Everything is lost at the LVGL handoff:
with `LV_USE_FLOAT 0`, `lv_value_precise_t` is an integer, so a line takes integer endpoints while
an arc takes an integer centre, an integer radius and whole-degree end angles. Those quantisations
were independent of each other, and an arc quantises as a *rigid body* -- rounding its centre or
its radius translates the whole curve, and against a vertical stem the radius at the tangent point
is horizontal, which is why it read as the arc being pushed to one side.

Four distinct defects, in descending order of how much each contributed:

1. **The stroke width disagreed between primitives.** `stroke_line()` truncated the float width
   while `stroke_arc()` rounded it. At a 240px display the width is `22 * 1.1 * 240/600 = 9.68px`,
   so straights drew 9px wide and curves 10px. A whole-pixel width difference offsets one edge of
   the silhouette across the entire join.
2. **Line endpoints were truncated, arc centres rounded** -- a systematic half-pixel disagreement.
3. **The arc's radius was double-rounded.** `dsc.radius` came from `lround(radius + width/2)` and
   LVGL then fills `[radius - width, radius]`, leaving the band centreline on a half pixel whenever
   rounding the width changed its parity.
4. **Arc end angles were truncated** to whole degrees, so both ends pulled inward, and because
   `dsc.rounded` centres a semicircular cap on the endpoint, the cap bulged past its neighbour.

### Fix

`stroke_width_px()` is now the single width every primitive uses: whole pixels, and always even so
that an arc's `radius - width/2` centreline stays integral. All rounding is `lround`, never a cast.
`dsc.radius` is `lround(radius) + width/2`, so the band centreline is exact.

`stroke_arc()` takes an `anchor` -- the pixel the preceding segment was actually drawn to -- and
places its centre at `round(anchor) + r_center * unit` rather than rounding the ideal float centre.
The curve then translates bodily to meet its neighbour. This direction matters: a circle moved half
a pixel is still a circle, whereas a straight moved half a pixel off its axis anti-aliases along its
whole length. For the same reason `stroke_line()`, when anchored, translates both endpoints by the
same delta rather than dragging one across, which would tilt it.

`draw()` chains the real drawn endpoint from each segment into the next, because a curve's drawn end
is `integer centre + integer radius` at a whole-degree angle -- not the rounded ideal joint. An
earlier pass that anchored every segment to the ideal joint fixed straight-to-curve but left
curve-to-curve mismatched.

An even earlier pass stamped a disc at each interior joint to cover the step. That is removed: a
disc has the same rounded-centre problem it was meant to hide, and it had to be recomputed per frame
from values that move as `tick()` eases `cur_.rot` and `cur_.base_dist`, so it could pop a pixel
mid-animation.

### What remains, and why it was not fixed

Straight-to-curve is now clean. **Curve-to-curve is still imperfect**, and cannot be made exact by
this approach. Where a tangent is axis-aligned -- the usual resting case of a vertical stem meeting
a curve -- the anchor arithmetic is integer plus integer and the join is exact. A roundabout's
transition arcs meet the loop at oblique angles, where no choice of integer centre and integer
radius puts the tangent point on an integer pixel. The residual is up to half a pixel per join, and
a roundabout chains four of them.

The real fix is to stop composing quantised primitives and hand LVGL one path. LVGL 9.6 has the
API for exactly this in `include/lvgl/draw/lv_draw_vector.h`: `lv_fpoint_t` is float, and
`lv_vector_path_move_to()` / `lv_vector_path_line_to()` / `lv_vector_path_append_arc()` build a
single path that `lv_draw_vector_dsc_set_stroke_width()` strokes in one pass with a proper stroker
handling the joins. That would delete `stroke_line()`, `stroke_arc()`, the anchor plumbing and the
width-parity rule outright.

It is not viable on this hardware. `LV_USE_VECTOR_GRAPHIC` requires `LV_USE_MATRIX`
(`lv_conf_internal.h:5271`), which requires `LV_USE_FLOAT` (`:5395`), and the software backend is
ThorVG -- 2.3 MB of vendored C++ source. The target is an `esp32-c3-devkitm-1`: single RISC-V core,
~400 KB SRAM, no PSRAM, no FPU. `LV_USE_FLOAT` turns every LVGL coordinate operation into a
soft-float call, and ThorVG's software rasterizer wants a full ARGB8888 canvas -- 240x240x4 =
225 KB before its own allocations. `dial_screen.cpp` already records rejecting an `lv_canvas` over
that same 225 KB.

The fallback, if the join quality ever matters enough: pre-rasterize each maneuver's stroked path to
an alpha mask offline in `build_icons.py`, where a real float renderer is available, and blit it at
runtime -- the same trick `build_arrowhead_mask()` already uses for the arrowhead. The cost is that
the route is currently animated (`tick()` slides `base_dist`, rotates the camera, and clips segments
per frame), and a baked mask cannot do that without baking frames or giving up the tween.

Accepted as-is: a sub-pixel defect on the oblique joins of roundabouts only.
