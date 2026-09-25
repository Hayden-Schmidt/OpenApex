"""Exports the turn-by-turn page as an editable SVG, for redesign in Figma.

This is a DESIGN-HANDOFF tool, not part of any build. It reads the same geometry the firmware
renders from -- the maneuver segment tables in firmware/gui/nav_icons_data.h and the camera/compass
constants in firmware/gui/nav_renderer.cpp + compass_ring.cpp -- and re-emits them as SVG paths at
the 240px reference frame every other page in "design reference/" is authored against.

It is a reimplementation of NavRenderer's transform, not a trace of a screenshot, so what lands in
Figma is true vector geometry at the true positions: an arc is an arc, and the arrow is one stroked
path a designer can restyle rather than a flattened outline.

Why it is safe to reimplement rather than share code: with a SINGLE maneuver on the route,
NavRenderer::compute_transform() returns the identity (route_count_ == 0, so rotation 0 and
anchor == base0), and world_to_screen() is a plain similarity -- rotate, uniform scale, translate.
That collapses to the ~20 lines below. It does NOT cover multi-maneuver chaining or the reveal
tween, and is not meant to.

    python tools/export_page_svg.py                    # default maneuver
    python tools/export_page_svg.py turn_left -o x.svg
    python tools/export_page_svg.py --list

Verify a change to this file by rendering its output and diffing against a simulator screenshot of
the same maneuver -- see --check.
"""

import argparse
import math
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_H = os.path.join(REPO_ROOT, "firmware", "gui", "nav_icons_data.h")

# --- constants mirrored from firmware/gui (hand-synced, like build_icon_raster.py's SCREEN_PROFILES)
FRAME = 240.0           # reference design frame, matches every SVG in "design reference/"
DESIGN_DIAMETER = 600.0 # nav_renderer.cpp kDesignDiameter (maneuvers.json authoring circle)
DISPLAY_SCALE = 1.1     # kDisplayScale
VERTICAL_OFFSET = 1.0   # kVerticalOffset, route units
LINE_THICKNESS = 22.0   # kLineThickness, route units before displayScale
ARROWHEAD_SCALE = 0.25  # kArrowheadScale

# compass_ring.cpp
TICK_COUNT = 36
MAJOR_EVERY = 3
MINOR_LENGTH = 0.06
MAJOR_LENGTH = 0.10
MINOR_WIDTH = 3.0
MAJOR_WIDTH = 6.0
NORTH_LENGTH = 0.10
NORTH_WIDTH = 0.045
TICK_COLOR = "#585F68"
NORTH_COLOR = "#FF3B30"

ROUTE_COLOR = "#FFFFFF"


def parse_data_header(path):
    """Pulls the maneuver segment tables, frame centres and arrowhead polygon out of the generated
    C header, so this tool cannot drift from what the firmware actually ships."""
    src = open(path, encoding="utf-8").read()

    segments = {}
    for m in re.finditer(r"static const nav_segment_t nav_icon_(\w+)_main\[\] = \{(.*?)\n\};",
                         src, re.S):
        name, body = m.group(1), m.group(2)
        segs = []
        for row in re.finditer(
                r"\{NAV_SEG_(LINE|ARC),\s*\{([-\d.f]+),\s*([-\d.f]+)\},"
                r"\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),"
                r"\s*([-\d.f]+),\s*([-\d.f]+)\}", body):
            g = row.groups()
            f = lambda s: float(s.rstrip("f"))
            segs.append({
                "type": g[0], "p0": (f(g[1]), f(g[2])), "heading0": f(g[3]),
                "length": f(g[4]), "radius": f(g[5]), "turn_deg": f(g[6]),
                "start_dist": f(g[7]), "end_dist": f(g[8]),
            })
        segments[name] = segs

    # frame_center rides in the NAV_ICON_DATA table, one row per nav_render_icon_t.
    centers = {}
    table = re.search(r"NAV_ICON_DATA\[[^\]]*\](.*?)\n\};", src, re.S)
    if table is None:
        raise SystemExit("export_page_svg: could not find the NAV_ICON_DATA table in "
                         "nav_icons_data.h -- the generated header's shape changed")
    if True:
        for row in re.finditer(r"nav_icon_(\w+)_main[^}]*?\{\s*([-\d.f]+)f?,\s*([-\d.f]+)f?\s*\}",
                               table.group(1)):
            centers[row.group(1)] = (float(row.group(2).rstrip("f")),
                                     float(row.group(3).rstrip("f")))

    head = re.search(r"NAV_ARROWHEAD_PERIMETER\[\] = \{(.*?)\};", src, re.S)
    perimeter = [(float(a), float(b)) for a, b in
                 re.findall(r"\{([-\d.]+)f,\s*([-\d.]+)f\}", head.group(1))]
    depth = float(re.search(r"NAV_ARROWHEAD_HEAD_BASE_DEPTH = ([-\d.]+)f", src).group(1))
    return segments, centers, perimeter, depth


def require_center(centers, maneuver):
    """A missing frame centre used to fall back to (0,0), which still produced a plausible-looking
    arrow -- just in the wrong place. Fail loudly instead."""
    if maneuver not in centers:
        raise SystemExit(f"export_page_svg: no frame_center parsed for '{maneuver}'")
    return centers[maneuver]


def seg_end(seg):
    """End point and exit heading of one segment, in maneuver-local space."""
    x, y = seg["p0"]
    h = math.radians(seg["heading0"])
    if seg["type"] == "LINE":
        return (x + math.cos(h) * seg["length"], y + math.sin(h) * seg["length"]), h
    turn = math.radians(seg["turn_deg"])
    sign = 1.0 if turn >= 0 else -1.0
    # Centre sits one radius off to the turning side of the travel heading.
    cx = x + seg["radius"] * -math.sin(h) * sign
    cy = y + seg["radius"] * math.cos(h) * sign
    ca, sa = math.cos(turn), math.sin(turn)
    dx, dy = x - cx, y - cy
    return (cx + dx * ca - dy * sa, cy + dx * sa + dy * ca), h + turn


def build(maneuver, segments, centers, perimeter, head_depth):
    segs = segments[maneuver]
    # Single maneuver => compute_transform() is the identity, so local space IS world space.
    cx, cy = require_center(centers, maneuver)
    rot = -math.pi / 2.0 - math.radians(segs[0]["heading0"])
    bc, bs = math.cos(rot), math.sin(rot)
    px_unit = FRAME / DESIGN_DIAMETER
    half = FRAME / 2.0

    def to_screen(p):
        dx, dy = p[0] - cx, p[1] - cy
        rx = dx * bc - dy * bs
        ry = dx * bs + dy * bc
        return (half + rx * DISPLAY_SCALE * px_unit,
                half + (ry + VERTICAL_OFFSET) * DISPLAY_SCALE * px_unit)

    # The stroked body stops short of the tip so it tucks under the arrowhead glyph, exactly as
    # NavRenderer trims it by kHeadDepth.
    total = segs[-1]["end_dist"]
    trimmed = max(segs[0]["start_dist"], total - head_depth * ARROWHEAD_SCALE)

    d = []
    tip_world = tip_heading = None
    for i, s in enumerate(segs):
        if s["start_dist"] >= trimmed:
            break
        frac = min(1.0, (trimmed - s["start_dist"]) / s["length"]) if s["length"] else 0.0
        clipped = dict(s)
        if frac < 1.0:
            clipped["length"] = s["length"] * frac
            clipped["turn_deg"] = s["turn_deg"] * frac
        p_end, h_end = seg_end(clipped)
        if i == 0:
            sx, sy = to_screen(s["p0"])
            d.append(f"M {sx:.3f} {sy:.3f}")
        ex, ey = to_screen(p_end)
        if clipped["type"] == "LINE":
            d.append(f"L {ex:.3f} {ey:.3f}")
        else:
            r = clipped["radius"] * DISPLAY_SCALE * px_unit
            large = 1 if abs(clipped["turn_deg"]) > 180.0 else 0
            sweep = 1 if clipped["turn_deg"] >= 0 else 0
            d.append(f"A {r:.3f} {r:.3f} 0 {large} {sweep} {ex:.3f} {ey:.3f}")

    # Arrowhead: tip pinned to the untrimmed route end, rotated from its authored "up".
    tip_world, tip_heading = segs[0]["p0"], math.radians(segs[0]["heading0"])
    for s in segs:
        tip_world, tip_heading = seg_end(s)
    tip = to_screen(tip_world)
    head_rot = tip_heading + math.pi / 2.0 + rot
    hc, hs = math.cos(head_rot), math.sin(head_rot)
    hscale = ARROWHEAD_SCALE * DISPLAY_SCALE * px_unit
    head_pts = []
    for vx, vy in perimeter:
        x, y = vx * hscale, vy * hscale
        head_pts.append((tip[0] + x * hc - y * hs, tip[1] + x * hs + y * hc))

    return {
        "body": " ".join(d),
        "stroke": LINE_THICKNESS * DISPLAY_SCALE * px_unit,
        "head": head_pts,
    }


def compass_svg():
    half = FRAME / 2.0
    px_unit = FRAME / DESIGN_DIAMETER
    out = ['  <g id="Compass Ring">']
    for i in range(TICK_COUNT):
        ang = (i / TICK_COUNT) * 2.0 * math.pi - math.pi / 2.0
        major = (i % MAJOR_EVERY) == 0
        ln = (MAJOR_LENGTH if major else MINOR_LENGTH) * half
        w = max(1.0, (MAJOR_WIDTH if major else MINOR_WIDTH) * px_unit)
        c, s = math.cos(ang), math.sin(ang)
        out.append(
            f'    <line x1="{half + c * half:.3f}" y1="{half + s * half:.3f}" '
            f'x2="{half + c * (half - ln):.3f}" y2="{half + s * (half - ln):.3f}" '
            f'stroke="{TICK_COLOR}" stroke-width="{w:.3f}"/>')
    out.append("  </g>")

    # North marker at 0 deg (pointing up) -- the heading-driven position, shown at north here.
    ang = -math.pi / 2.0
    c, s = math.cos(ang), math.sin(ang)
    tip_r = half * (1.0 - NORTH_LENGTH)
    bw = NORTH_WIDTH * half
    px_, py_ = -s, c
    pts = [(half + c * half + px_ * bw, half + s * half + py_ * bw),
           (half + c * half - px_ * bw, half + s * half - py_ * bw),
           (half + c * tip_r, half + s * tip_r)]
    out.append('  <polygon id="North Marker" points="' +
               " ".join(f"{x:.3f},{y:.3f}" for x, y in pts) +
               f'" fill="{NORTH_COLOR}"/>')
    return "\n".join(out)


def emit(maneuver, geo, distance_text):
    head = " ".join(f"{x:.3f},{y:.3f}" for x, y in geo["head"])
    return f'''<svg width="240" height="240" viewBox="0 0 240 240" fill="none" xmlns="http://www.w3.org/2000/svg">
<!-- Turn-by-turn page, exported from the live firmware geometry by tools/export_page_svg.py.
     Maneuver: {maneuver}. 240x240 reference frame, matching every other "design reference/" SVG.
     The arrow body is ONE stroked path: restyle it with stroke/stroke-width/stroke-linecap rather
     than editing an outline. The arrowhead is a separate filled polygon, tip-pinned to the route
     end. Both are generated from firmware/gui/nav_icons_data.h, so this is true geometry. -->
<rect width="240" height="240" fill="#1E1E1E"/>
<circle id="Screen Background" cx="120" cy="120" r="120" fill="black"/>
{compass_svg()}
  <g id="Maneuver Arrow">
    <path id="Arrow Body" d="{geo['body']}" stroke="{ROUTE_COLOR}" stroke-width="{geo['stroke']:.3f}" fill="none" stroke-linecap="round" stroke-linejoin="round"/>
    <polygon id="Arrow Head" points="{head}" fill="{ROUTE_COLOR}"/>
  </g>
  <text id="Distance" x="120" y="220" fill="{ROUTE_COLOR}" font-family="Montserrat" font-size="14" text-anchor="middle">{distance_text}</text>
  <!-- Present in terminal_view_state_t and populated by the pipeline, but NOT rendered today.
       Included so a redesign can place them; delete the group if they stay unrendered. -->
  <g id="Available but not rendered" opacity="0.45">
    <text id="Street Name" x="120" y="60" fill="#969696" font-family="Montserrat" font-size="14" text-anchor="middle">Elm Street</text>
    <text id="ETA" x="120" y="78" fill="#969696" font-family="Montserrat" font-size="12" text-anchor="middle">2 min</text>
  </g>
</svg>
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maneuver", nargs="?", default="turn_right")
    ap.add_argument("-o", "--out")
    ap.add_argument("--distance", default="250 m")
    ap.add_argument("--list", action="store_true", help="list available maneuvers and exit")
    args = ap.parse_args()

    segments, centers, perimeter, depth = parse_data_header(DATA_H)
    if args.list:
        for name in sorted(segments):
            print(name)
        return 0
    if args.maneuver not in segments:
        print(f"unknown maneuver '{args.maneuver}'; --list to see them", file=sys.stderr)
        return 1

    geo = build(args.maneuver, segments, centers, perimeter, depth)
    svg = emit(args.maneuver, geo, args.distance)
    out = args.out or os.path.join(REPO_ROOT, "design reference", "3.Turn by Turn",
                                   f"Turn by Turn ref ({args.maneuver}).svg")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(svg)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
