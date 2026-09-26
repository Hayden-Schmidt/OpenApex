"""
Generates maneuver centerlines for the ESP32 round-display nav demo by
interpreting `maneuvers.json` -- the canonical, hand-edited source of truth.
Each maneuver there is just a list of two primitives ("straight"/"curve")
chained tip-to-tail; this script is a generic walker over that list, not a
per-maneuver code path, so adding/tuning a maneuver never requires touching
this file, only maneuvers.json.

The old approach (extracting centerlines by averaging the two stroke edges
of a traced Google Material Symbol outline) produced non-monotonic,
zigzagging point data (artifacts near corners/the arrowhead notch where the
two edges fell out of correspondence) -- SVG_PATH_PACK/Google Icons Archive
are visual reference only now, never parsed for geometry.

Coordinate convention (matches SVG: x right, y DOWN):
  - Every maneuver starts at local origin (0, 0) heading "up the screen"
    (heading angle -90 deg, i.e. direction vector (0, -1)).
  - A "curve" with dir:"right" bends the path toward +x; dir:"left" toward
    -x (equivalently: heading angle increases for a right curve).
  - `main_path`'s last point IS `head_junction` -- the shared arrowhead
    glyph (ARROWHEAD, below) is placed TIP-first exactly there, so a new
    maneuver continues from precisely where the arrowhead's tip sits (no
    gap/overlap at the join). The line ITSELF is rendered short of that
    point by ARROWHEAD's `head_base_depth` (the glyph is already at
    full width `head_base_depth` back from its tip) -- but only for
    whichever maneuver is currently live; see Demo/demo.js.
  - `rotation`: net heading change (deg, signed, normalized to (-180, 180])
    from first to last point -- for viewport-transform use, not chaining
    (chaining derives its own heading delta from the points, so it's
    robust to any shape).
  - Framing is NOT baked in here. Demo/demo.js frames each maneuver from
    its own bounding box, measured in the rotated camera frame, so the
    whole maneuver is centred on the round display at a FIXED scale (no
    camera zoom -- size constancy matters for a glanceable motorcycle
    display). What this file must guarantee in exchange is that every
    maneuver actually FITS that one frame: see check_fit() below, which
    fails the build if one doesn't.

Run: python build_icons.py   (reads maneuvers.json, writes
Demo/icons-data.js)
"""
import json, math, os

HERE = os.path.dirname(__file__)
MANEUVERS_PATH = os.path.join(HERE, "maneuvers.json")
DEMO_JS_OUT = os.path.join(HERE, "..", "..", "Demo", "icons-data.js")
FIRMWARE_C_OUT = os.path.join(HERE, "..", "..", "firmware", "gui", "nav_icons_data.h")

# Hand-tuned framing overrides, mirrored from Demo/params.js's DEMO_PARAMS.frameCenter --
# demo.js's computePose() prefers this table over each icon's baked (purely geometric)
# frame_center, and several of these differ substantially from the geometric default (e.g.
# roundabout_left: baked bbox centre (-101.4, -83.9) vs the tuned (-55, -136.6)). Kept in sync by
# hand since it lives in a JS-only params file; update both if you retune framing there.
FRAME_CENTER_OVERRIDES = {
    "straight": [0.0, -137.0],
    "turn_right": [70.0, -120.0],
    "turn_left": [-70.0, -120.0],
    "turn_slight_right": [34.6, -125.5],
    # Sharp pair: the baked bbox centre is (82.5, -87.5), but a 125-degree bend puts the exit leg
    # low and outboard, so the geometric centre sits below and right of where the eye reads the
    # maneuver. Pulled in and up by the same proportion turn_right is (its baked centre is
    # (100, -95) against a tuned (70, -120)), which keeps the two arrows optically centred the
    # same way when the display cuts between them.
    "turn_sharp_right": [58.0, -112.0],
    "turn_sharp_left": [-58.0, -112.0],
    "turn_slight_left": [-34.6, -125.5],
    "u_turn_right": [55.0, -117.5],
    "u_turn_left": [-55.0, -117.5],
    "roundabout_right": [40.0, -135.0],
    "roundabout_straight": [0.0, -140.0],
    "roundabout_left": [-55.0, -116],
    "right_merge": [70.0, -130.0],
    "merge_left": [-70.0, -130.0],
}

# one sample point roughly every this many units of straight, or this many
# degrees of curve -- tessellation density is a rendering concern, not part
# of a maneuver's geometric definition, so it isn't in maneuvers.json.
UNITS_PER_SAMPLE = 30.0
DEGREES_PER_SAMPLE = 7.0


def norm_angle(a):
    """normalize degrees to (-180, 180]"""
    a = a % 360
    if a > 180:
        a -= 360
    return a


def heading_vec(deg):
    r = math.radians(deg)
    return (math.cos(r), math.sin(r))


def add(p, v, s=1.0):
    return (p[0] + v[0] * s, p[1] + v[1] * s)


def line_points(p0, heading_deg, length, n):
    """n points, evenly spaced, from p0 along heading_deg for `length`."""
    v = heading_vec(heading_deg)
    return [add(p0, v, length * k / (n - 1)) for k in range(n)]


def arc_points(p0, heading_deg, radius, turn_deg, n):
    """
    n points along a circular arc starting at p0 with initial heading
    heading_deg, turning by turn_deg (signed: + = right/toward +x, per the
    module-level convention), constant radius. Returns (points, final_heading_deg).
    """
    sign = 1.0 if turn_deg >= 0 else -1.0
    h0 = math.radians(heading_deg)
    nx0, ny0 = -math.sin(h0), math.cos(h0)
    cx, cy = p0[0] + sign * radius * nx0, p0[1] + sign * radius * ny0
    pts = []
    for k in range(n):
        h = math.radians(heading_deg + turn_deg * k / (n - 1))
        nx, ny = -math.sin(h), math.cos(h)
        pts.append((cx - sign * radius * nx, cy - sign * radius * ny))
    return pts, heading_deg + turn_deg


def chain(p0, heading_deg, steps):
    """
    steps: list of ("line", length, n) | ("arc", radius, turn_deg, n)
    Returns (all_points, final_heading_deg) with each step's shared
    endpoint de-duplicated.
    """
    pts = [p0]
    h = heading_deg
    cur = p0
    for step in steps:
        if step[0] == "line":
            _, length, n = step
            seg = line_points(cur, h, length, n)
        else:
            _, radius, turn_deg, n = step
            seg, h = arc_points(cur, h, radius, turn_deg, n)
        pts.extend(seg[1:])
        cur = seg[-1]
    return pts, h


def round_pts(pts):
    return [[round(x, 1), round(y, 1)] for x, y in pts]


# --- roundabout solver -----------------------------------------------------

def solve_roundabout(r, R, net):
    """
    Expands a "roundabout" step into the CURVED CORE of a roundabout --
    transition arc, circulating loop, transition arc -- with the loop's
    centre solved so it lands on the entry centreline.

    The approach and departure straights are NOT part of this step: they are
    ordinary "straight" steps written either side of it in maneuvers.json, so
    their lengths are independently editable framing knobs. This is sound
    because the entry straight's length cancels out of the solution below --
    it only slides the whole construction further along the centreline.

    The entry runs straight up x=0, so it is RADIAL to the loop circle, not
    tangent -- which is why a transition arc is needed at all, and why its
    sweep cannot be picked by hand (the old hand-written 90deg entry arc is
    exactly what pushed the loop centre off-axis by r).

    Take the step's start as the origin, heading up. Let the loop be centred
    at (0, -d) with radius R, and the transition arc have radius r, leaving
    the entry line at the origin. The transition curves LEFT while the loop
    curves RIGHT -- opposite curvature, so their circles are EXTERNALLY
    tangent and their centres are (r + R) apart. The transition's centre sits
    at (-r, 0), giving

        d^2 + r^2 = (r + R)^2
          =>  d   = sqrt(R^2 + 2rR)                 centre distance ahead
          =>  psi = atan2(sqrt(R^2 + 2rR), r)       transition sweep

    and the loop must then sweep `net + 2*psi`, since the two transitions
    (entry and exit, symmetric) each take back psi of heading.

    Returns (ops, k) where k is that distance d -- the caller places the
    centre k ahead of wherever the step actually starts.
    """
    k = math.sqrt(R * R + 2 * r * R)
    psi = math.degrees(math.atan2(k, r))
    loop_sweep = net + 2 * psi

    def n_arc(angle):
        return max(2, round(abs(angle) / DEGREES_PER_SAMPLE) + 1)

    return [
        ("arc", r, -psi, n_arc(psi)),
        ("arc", R, loop_sweep, n_arc(loop_sweep)),
        ("arc", r, -psi, n_arc(psi)),
    ], k


# --- maneuvers.json interpreter --------------------------------------------

def steps_from_json(json_steps):
    """Converts maneuvers.json's declarative steps into chain()'s ops,
    auto-computing sample density (see UNITS_PER_SAMPLE/DEGREES_PER_SAMPLE).

    Returns (ops, loop_center) -- loop_center is None unless the maneuver
    contains a "roundabout" step, in which case it's that step's solved loop
    centre in local coordinates (see solve_roundabout)."""
    ops = []
    loop_center = None
    for s in json_steps:
        if s["type"] == "roundabout":
            rb_ops, k = solve_roundabout(
                s["transition_radius"], s["loop_radius"], s["net"]
            )
            # The solver works in the step's own frame, so walk everything
            # before it to find where that frame sits; the centre is then
            # simply k straight ahead of the step's start.
            before, h = chain((0.0, 0.0), -90.0, ops)
            loop_center = add(before[-1], heading_vec(h), k)
            ops.extend(rb_ops)
            continue
        if s["type"] == "straight":
            length = s["length"]
            n = max(2, round(length / UNITS_PER_SAMPLE) + 1)
            ops.append(("line", length, n))
        elif s["type"] == "curve":
            angle = s["angle"]
            turn_deg = angle if s["dir"] == "right" else -angle
            n = max(2, round(abs(angle) / DEGREES_PER_SAMPLE) + 1)
            ops.append(("arc", s["radius"], turn_deg, n))
        else:
            raise ValueError(f"unknown step type {s['type']!r}")
    return ops, loop_center


def frame_center(pts, secondary_path=None):
    """
    The maneuver's local-frame bounding-box centre -- the point demo.js puts
    at the middle of the round display when framing this maneuver.

    Local frame == camera frame: every maneuver is authored starting at (0,0)
    heading up, and the camera rotates so the entry heading points up, so a
    local-space centre is directly a screen-space framing target regardless of
    where the maneuver ends up chained in world space.

    This is only a DEFAULT -- purely geometric, which is not always what reads
    best. Demo/params.js carries a `frameCenter` override table to hand-tune
    any maneuver whose automatic centre sits badly.
    """
    allpts = list(pts) + list(secondary_path or [])
    xs = [p[0] for p in allpts]
    ys = [p[1] for p in allpts]
    return [round((min(xs) + max(xs)) / 2, 1), round((min(ys) + max(ys)) / 2, 1)]


def segments_from_ops(p0, heading_deg, ops):
    """Walks the same op list chain() walks, but keeps each op as an exact
    primitive instead of tessellating it into sample points -- this is what the
    firmware renders (one lv_draw_arc per curve, one quad per straight), so no
    sampling density and therefore no quad-to-quad seams. `n` (sample count) is
    dropped; it is purely a JS-canvas-path concern.

    Returns a list of dicts: {"type": "line"|"arc", "p0", "heading0",
    "length"|("radius","turn_deg"), "start_dist", "end_dist"} with distances
    cumulative along the maneuver from its start (arc length =
    radius * |turn_deg| * pi/180), which is what the reveal animation slices on.
    """
    segs = []
    cur, h, d = p0, heading_deg, 0.0
    for step in ops:
        if step[0] == "line":
            _, length, _n = step
            seg = {"type": "line", "p0": cur, "heading0": h,
                   "length": float(length), "radius": 0.0, "turn_deg": 0.0}
            cur = add(cur, heading_vec(h), length)
        else:
            _, radius, turn_deg, _n = step
            length = radius * abs(turn_deg) * math.pi / 180.0
            seg = {"type": "arc", "p0": cur, "heading0": h,
                   "length": length, "radius": float(radius),
                   "turn_deg": float(turn_deg)}
            pts, h = arc_points(cur, h, radius, turn_deg, 2)
            cur = pts[-1]
        seg["start_dist"] = d
        d += seg["length"]
        seg["end_dist"] = d
        segs.append(seg)
    return segs


def round_segs(segs):
    out = []
    for s in segs:
        r = dict(s)
        r["p0"] = [round(s["p0"][0], 2), round(s["p0"][1], 2)]
        for k in ("heading0", "length", "radius", "turn_deg", "start_dist", "end_dist"):
            r[k] = round(s[k], 4)
        out.append(r)
    return out


def build_from_steps(name, json_steps, secondary_path=None):
    ops, loop_center = steps_from_json(json_steps)
    pts, final_h = chain((0.0, 0.0), -90.0, ops)
    icon = {
        "name": name,
        "segments": round_segs(segments_from_ops((0.0, 0.0), -90.0, ops)),
        "main_path": round_pts(pts),
        "rotation": round(norm_angle(final_h - (-90.0)), 1),
        "head_junction": round_pts([pts[-1]])[0],
        "frame_center": frame_center(pts, secondary_path),
        "secondary_path": round_pts(secondary_path) if secondary_path else [],
    }
    if loop_center is not None:
        icon["loop_center"] = [round(loop_center[0], 1), round(loop_center[1], 1)]
    return icon


def mirror_x(icon, new_name):
    m = json.loads(json.dumps(icon))
    m["name"] = new_name
    m["main_path"] = [[-x, y] for x, y in m["main_path"]]
    # Reflecting about x=0 maps direction (cos h, sin h) -> (-cos h, sin h),
    # i.e. h -> 180 - h, and flips every turn's handedness.
    for s in m["segments"]:
        s["p0"] = [-s["p0"][0], s["p0"][1]]
        s["heading0"] = round(norm_angle(180.0 - s["heading0"]), 4)
        s["turn_deg"] = -s["turn_deg"]
    m["head_junction"] = [-m["head_junction"][0], m["head_junction"][1]]
    m["secondary_path"] = [[-x, y] for x, y in m["secondary_path"]]
    m["frame_center"] = [-m["frame_center"][0], m["frame_center"][1]]
    if "loop_center" in m:
        m["loop_center"] = [-m["loop_center"][0], m["loop_center"][1]]
    m["rotation"] = round(-m["rotation"], 1)
    return m


# --- display fit check -----------------------------------------------------
# demo.js renders at a fixed scale into a round display of DISPLAY_DIAMETER
# units (index.html's viewBox), framing each maneuver on its own bbox centre.
# A maneuver fits iff its bbox -- inflated by half the stroke width and the
# arrowhead's scaled half-width, which both overhang the bare centreline --
# has a diagonal no longer than that diameter (the box's circumscribed
# circle must sit inside the display's). Keep these in sync with
# Demo/params.js + Demo/index.html.
DISPLAY_DIAMETER = 600.0
LINE_THICKNESS = 18.0
ARROWHEAD_SCALE = 0.5


def check_roundabouts(icons, maneuvers):
    """
    Guards the two invariants solve_roundabout() exists to establish, so they
    can't silently rot if someone retunes a radius:
      1. the loop's centre sits on the entry centreline (local x == 0);
      2. the net heading change is exactly the declared exit direction.
    Also re-derives the loop centre from the emitted POINTS rather than
    trusting the solver's own arithmetic -- the points are what ships.
    """
    failed = []
    print("")
    print("roundabout alignment:")
    for key, icon in icons.items():
        if "loop_center" not in icon or "steps" not in maneuvers[key]:
            continue  # mirrored roundabouts carry no steps of their own
        step = next(s for s in maneuvers[key]["steps"] if s["type"] == "roundabout")
        R = step["loop_radius"]
        cx, cy = icon["loop_center"]
        # every point on the circulating loop must be exactly R from the centre
        on_loop = [
            p for p in icon["main_path"]
            if abs(math.hypot(p[0] - cx, p[1] - cy) - R) < 0.5
        ]
        radii = [math.hypot(p[0] - cx, p[1] - cy) for p in on_loop]
        worst_r = max(abs(v - R) for v in radii) if radii else float("inf")
        net_err = abs(norm_angle(icon["rotation"] - step["net"]))
        # >=4 loop points is just a sanity floor proving we actually matched the
        # circulating arc; the short left-exit sweep legitimately yields few.
        ok = abs(cx) < 0.05 and net_err < 0.05 and worst_r < 0.5 and len(on_loop) >= 4
        print(
            f"  {key:22} centre=({cx:6.1f},{cy:7.1f})  |x|={abs(cx):.3f}  "
            f"net={icon['rotation']:+7.1f} (want {step['net']:+d})  "
            f"loop pts={len(on_loop):3d} radius err={worst_r:.3f}  "
            + ("ok" if ok else "FAIL")
        )
        if not ok:
            failed.append(key)
    if failed:
        raise SystemExit(
            "ROUNDABOUT CHECK FAILED for: " + ", ".join(failed)
            + "\nThe loop centre must lie on the entry centreline (x=0) and the "
            "net heading must match the declared exit."
        )


def check_segments(icons):
    """The firmware renders `segments`, the JS demo renders `main_path`, and both claim to be the
    same maneuver -- so prove it here rather than by eyeballing two screens. Walks each segment
    analytically (the same arithmetic nav_renderer.cpp's eval_segment does) and checks every
    tessellated main_path point lies on it, and that cumulative distance matches the polyline."""
    failed = []
    print("")
    print("segment/point agreement:")
    for key, icon in icons.items():
        segs = icon["segments"]

        def at(d):
            s = next((s for s in segs if d <= s["end_dist"]), segs[-1])
            t = (d - s["start_dist"]) / s["length"] if s["length"] else 0.0
            t = min(max(t, 0.0), 1.0)
            h = s["heading0"]
            if s["type"] == "line":
                return add(s["p0"], heading_vec(h), s["length"] * t)
            sign = 1.0 if s["turn_deg"] >= 0 else -1.0
            h0 = math.radians(h)
            cx = s["p0"][0] + sign * s["radius"] * -math.sin(h0)
            cy = s["p0"][1] + sign * s["radius"] * math.cos(h0)
            hr = math.radians(h + s["turn_deg"] * t)
            return (cx - sign * s["radius"] * -math.sin(hr),
                    cy - sign * s["radius"] * math.cos(hr))

        pts = icon["main_path"]
        cum, worst = 0.0, 0.0
        for i, p in enumerate(pts):
            if i:
                cum += math.dist(pts[i - 1], p)
            worst = max(worst, math.dist(at(cum), p))
        len_err = abs(cum - segs[-1]["end_dist"])
        # Tolerance is dominated by the tessellation itself: a chord cuts the corner off its arc by
        # radius*(1-cos(DEGREES_PER_SAMPLE/2)) ~ 0.1 units here, plus main_path's 0.1 rounding.
        ok = worst < 0.5 and len_err < 1.0
        print(f"  {key:22} {len(segs)} segs  max point dev={worst:.3f}  "
              f"length err={len_err:.3f}  " + ("ok" if ok else "FAIL"))
        if not ok:
            failed.append(key)
    if failed:
        raise SystemExit(
            "SEGMENT CHECK FAILED for: " + ", ".join(failed)
            + "\nThe firmware's primitives no longer trace the same shape as the demo's points."
        )


def check_fit(icons):
    margin = LINE_THICKNESS / 2 + arrowhead["head_base_half_width"] * ARROWHEAD_SCALE
    report, failed = [], []
    for key, icon in icons.items():
        pts = icon["main_path"] + icon["secondary_path"]
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        w = (max(xs) - min(xs)) + 2 * margin
        h = (max(ys) - min(ys)) + 2 * margin
        diag = math.hypot(w, h)
        report.append((diag, key, w, h))
        if diag > DISPLAY_DIAMETER:
            failed.append((key, diag))
    report.sort(reverse=True)
    print("")
    print(f"fit check (margin {margin:.1f}, display {DISPLAY_DIAMETER:.0f}):")
    for diag, key, w, h in report:
        flag = "FAIL" if diag > DISPLAY_DIAMETER else "ok"
        print(f"  {key:22} {w:6.0f} x {h:6.0f}  diag {diag:6.0f}  {flag}")
    if failed:
        detail = "\n".join(
            f"  {k}: diagonal {d:.0f} > {DISPLAY_DIAMETER:.0f}" for k, d in failed
        )
        raise SystemExit(
            "FIT CHECK FAILED -- these maneuvers overflow the round display:" + "\n"
            + detail
            + "\n" + "Shrink their radii/lengths in maneuvers.json."
        )


with open(MANEUVERS_PATH, encoding="utf-8") as f:
    maneuvers = json.load(f)

order = [k for k in maneuvers.keys() if not k.startswith("_")]

icons = {}
for key in order:
    entry = maneuvers[key]
    if "mirror_of" in entry:
        continue  # built in the second pass, once its source exists
    icons[key] = build_from_steps(entry["name"], entry["steps"], entry.get("secondary_path"))

for key in order:
    entry = maneuvers[key]
    if "mirror_of" in entry:
        icons[key] = mirror_x(icons[entry["mirror_of"]], entry["name"])

# --- shared arrowhead glyph -----------------------------------------------
# Traced directly from the real Google Material Symbol
# (straight_150dp_..._wght700..svg), not reconstructed -- this is the actual
# flared/barbed head shape, not a plain triangle. Original path vertices
# (tip at 480,-866; left half only, path order shaft->tip):
#   (433,-686) (388,-641) (343,-596) (310,-629) (277,-662)
#   (344.67,-730) (412.33,-798) (480,-866)=tip
# Re-expressed tip-at-origin, local space = (x - 480, y + 866): tip (0,0)
# is "forward" (localForward = -90deg, i.e. -y); positive y = back toward
# the shaft. Right half is the exact mirror (Google's glyph is symmetric).
# The (+-47, 180) pair is where the shaft meets the head -- main_path's
# head_junction should sit at that same depth for a seamless join.
_HEAD_LEFT = [
    (-47.0, 180.0),
    (-92.0, 225.0),
    (-137.0, 270.0),
    (-170.0, 237.0),
    (-203.0, 204.0),
    (-135.33, 136.0),
    (-67.67, 68.0),
]

def simplify_collinear(pts, tol=1.0):
    """Drops vertices that sit on the straight run between their neighbours.

    Material Symbols paths subdivide straight edges into several L commands,
    so the traced glyph above carries 15 points where the shape has only 7
    genuine corners -- e.g. (-92,225) is exactly halfway along
    (-47,180)->(-137,270), and (-135.33,136)/(-67.67,68) merely subdivide the
    long wing edge.

    Those pass-through points are not free. They cost triangles (ear clipping
    turned 15 points into 13 triangles instead of 5), and worse, because the
    traced coordinates are rounded they are only NEARLY collinear -- which
    makes the outward offset used for seam suppression solve a pair of almost
    parallel edges and throw the mitred corner a long way out, as visible
    spikes off the glyph. Removing them fixes the cause rather than the
    symptom.

    `tol` is the maximum perpendicular deviation, in glyph units, for a point
    to count as on the line. The glyph is ~400 units across and drawn at
    ARROWHEAD_SCALE, so 1.0 is far below a pixel on any target panel.
    """
    out = []
    n = len(pts)
    for i in range(n):
        ax, ay = pts[(i - 1) % n]
        bx, by = pts[i]
        cx, cy = pts[(i + 1) % n]
        ux, uy = cx - ax, cy - ay
        seg = math.hypot(ux, uy)
        if seg < 1e-9:
            continue
        # perpendicular distance of b from the line a->c
        dev = abs((bx - ax) * uy - (by - ay) * ux) / seg
        if dev > tol:
            out.append(pts[i])
    return out


_HEAD_PERIMETER = simplify_collinear(
    _HEAD_LEFT
    + [(0.0, 0.0)]
    + [(-x, y) for x, y in reversed(_HEAD_LEFT)]
)

arrowhead = {
    "name": "Master Arrowhead",
    "tip": [0.0, 0.0],
    "head_base_half_width": 47.0,
    "head_base_depth": 180.0,
    "perimeter": _HEAD_PERIMETER,
}
arrowhead["perimeter"] = [[round(x, 2), round(y, 2)] for x, y in arrowhead["perimeter"]]

# --- bundle into Demo/icons-data.js (offline file:// use, no fetch) ------
lines = ["// Auto-generated by Google Icons Archive/Icon JSON/build_icons.py from",
         "// maneuvers.json -- do not edit by hand, edit maneuvers.json instead.",
         "const ICONS = {"]
for i, key in enumerate(order):
    comma = "," if i < len(order) - 1 else ""
    # "segments" is firmware-only (see the C output below); the JS canvas demo keeps
    # consuming the dense sampled main_path, so its file stays exactly as it was.
    js_icon = {k: v for k, v in icons[key].items() if k != "segments"}
    lines.append(f"  {json.dumps(key)}: {json.dumps(js_icon, indent=2)}{comma}")
lines.append("};")
lines.append("const ARROWHEAD = " + json.dumps(arrowhead, indent=2) + ";")

with open(DEMO_JS_OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("wrote", DEMO_JS_OUT)

# --- bundle into firmware/gui/nav_icons_data.h (C port, Demo/demo.js's twin) ----------------
# Same `icons`/`arrowhead` dicts as the JS output above, so the two outputs cannot drift.
# ARROWHEAD ships as its bare 7-point perimeter: the firmware rasterizes it once at boot into an
# alpha mask and draws that as a single rotated image, so it needs no triangulation (which is what
# caused the anti-aliased seams down the glyph) and none is emitted.


def c_pts(pts):
    return ", ".join(f"{{{x:.2f}f, {y:.2f}f}}" for x, y in pts)


def c_segs(segs):
    out = []
    for s in segs:
        kind = "NAV_SEG_LINE" if s["type"] == "line" else "NAV_SEG_ARC"
        out.append(
            f"    {{{kind}, {{{s['p0'][0]:.2f}f, {s['p0'][1]:.2f}f}}, {s['heading0']:.4f}f, "
            f"{s['length']:.4f}f, {s['radius']:.4f}f, {s['turn_deg']:.4f}f, "
            f"{s['start_dist']:.4f}f, {s['end_dist']:.4f}f}},"
        )
    return out



c_lines = [
    "// Auto-generated by Google Icons Archive/Icon JSON/build_icons.py from",
    "// maneuvers.json -- do not edit by hand, edit maneuvers.json instead.",
    "// C twin of Demo/icons-data.js -- see Demo/demo.js for the renderer this data feeds.",
    "#pragma once",
    "",
    "#include <stddef.h>",
    "#include <stdint.h>",
    "",
    "typedef struct { float x, y; } nav_pt_t;",
    "",
    "// The maneuver body ships as EXACT primitives (the same straights/curves maneuvers.json",
    "// declares), not as sampled points: the firmware renders a curve with one lv_draw_arc, so",
    "// there are no quad-to-quad joints inside it to seam. Demo/icons-data.js still gets the",
    "// tessellated point path -- the JS canvas demo strokes a Path2D and has no such problem.",
    "// Angles are degrees, x right / y DOWN (SVG convention, matching maneuvers.json).",
    "// heading0 = travel direction at p0; turn_deg is signed (+ = right/toward +x) and 0 for",
    "// lines; length is arc length for arcs (radius * |turn_deg| * pi/180); start_dist/end_dist",
    "// are cumulative along the maneuver, which is what the reveal animation slices on.",
    "typedef enum { NAV_SEG_LINE = 0, NAV_SEG_ARC = 1 } nav_seg_type_t;",
    "",
    "typedef struct {",
    "    nav_seg_type_t type;",
    "    nav_pt_t p0;",
    "    float heading0;",
    "    float length;",
    "    float radius;",
    "    float turn_deg;",
    "    float start_dist;",
    "    float end_dist;",
    "} nav_segment_t;",
    "",
    "typedef struct {",
    "    const nav_segment_t *main_segments;",
    "    size_t main_segment_count;",
    "    const nav_pt_t *secondary_path;",
    "    size_t secondary_count;",
    "    nav_pt_t frame_center;",
    "} nav_icon_data_t;",
    "",
    "typedef enum {",
]
for key in order:
    c_lines.append(f"    NAV_RENDER_{key.upper()},")
c_lines.append("    NAV_RENDER_COUNT,")
c_lines.append("} nav_render_icon_t;")
c_lines.append("")

for key in order:
    icon = icons[key]
    c_lines.append(f"static const nav_segment_t nav_icon_{key}_main[] = {{")
    c_lines.extend(c_segs(icon["segments"]))
    c_lines.append("};")
    if icon["secondary_path"]:
        c_lines.append(
            f"static const nav_pt_t nav_icon_{key}_secondary[] = {{{c_pts(icon['secondary_path'])}}};"
        )
c_lines.append("")
c_lines.append("static const nav_icon_data_t NAV_ICON_DATA[NAV_RENDER_COUNT] = {")
for key in order:
    icon = icons[key]
    sec = f"nav_icon_{key}_secondary" if icon["secondary_path"] else "NULL"
    sec_count = len(icon["secondary_path"])
    # Matches demo.js's computePose(): frameCenterOverrides[key] || data.frame_center.
    fc = FRAME_CENTER_OVERRIDES.get(key, icon["frame_center"])
    c_lines.append(
        f"    [NAV_RENDER_{key.upper()}] = {{ nav_icon_{key}_main, "
        f"{len(icon['segments'])}, {sec}, {sec_count}, {{{fc[0]:.1f}f, {fc[1]:.1f}f}} }},"
    )
c_lines.append("};")
c_lines.append("")
c_lines.append("// Longest maneuver, so the renderer can size its route buffer off the data.")
c_lines.append(
    f"#define NAV_ICON_MAX_SEGMENTS {max(len(icons[k]['segments']) for k in order)}"
)
c_lines.append("")

c_lines.append(f"static const nav_pt_t NAV_ARROWHEAD_PERIMETER[] = {{{c_pts(arrowhead['perimeter'])}}};")
c_lines.append(f"#define NAV_ARROWHEAD_VERT_COUNT {len(arrowhead['perimeter'])}")
c_lines.append(f"static const nav_pt_t NAV_ARROWHEAD_TIP = {{{arrowhead['tip'][0]:.1f}f, {arrowhead['tip'][1]:.1f}f}};")
c_lines.append(f"static const float NAV_ARROWHEAD_HEAD_BASE_HALF_WIDTH = {arrowhead['head_base_half_width']:.1f}f;")
c_lines.append(f"static const float NAV_ARROWHEAD_HEAD_BASE_DEPTH = {arrowhead['head_base_depth']:.1f}f;")
c_lines.append("")

with open(FIRMWARE_C_OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(c_lines) + "\n")
print("wrote", FIRMWARE_C_OUT)

print("\nrotations:", {k: icons[k]["rotation"] for k in order})
check_segments(icons)
check_fit(icons)
check_roundabouts(icons, maneuvers)

# Paste-ready seed for Demo/params.js's frameCenter override table.
print("")
print("frame centers (paste into Demo/params.js frameCenter to hand-tune):")
for key in order:
    fc = icons[key]["frame_center"]
    print(f"    {key}: [{fc[0]}, {fc[1]}],")
