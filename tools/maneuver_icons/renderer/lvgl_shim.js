// A deliberately thin, LVGL-shaped drawing layer over HTML5 Canvas2D.
//
// WHY THIS EXISTS
// The demo used to draw with SVG elements -- <path d="..."> with
// stroke-linejoin:round, a <polygon>, and a transform attribute on a <g>.
// None of that exists on an ESP32/STM32 running LVGL, so the old render code
// could not be ported, only rewritten. This file is the rewrite: every
// function below maps 1:1 onto an LVGL call, and nothing is used that LVGL
// cannot do.
//
// The rules that fall out of that, and that the rest of the demo now obeys:
//
//   * IMMEDIATE MODE. No retained scene graph, no element pooling. Clear the
//     buffer, draw the frame, done -- exactly LVGL's model.
//   * NO TRANSFORM STACK. LVGL has no camera. The caller transforms points
//     itself and hands over device-space coordinates, which is what the C
//     port will do.
//   * NO STROKED POLYLINES. LVGL's line draw has no round join, so a
//     tessellated curve drawn as thick line segments shows notches on the
//     outside of every bend. strokePolyline() below instead emits one convex
//     quad per segment plus a disc at each interior vertex -- all convex, all
//     primitives LVGL fills reliably.
//   * NO CONCAVE POLYGON FILL. LVGL's polygon fill is only dependable for
//     convex shapes, and the arrowhead is a barbed (concave) glyph. It is
//     triangulated ONCE at startup and drawn as triangles thereafter.
//
// LVGL EQUIVALENTS
//   clear()            -> lv_canvas_fill_bg()
//   fillTriangle()     -> lv_draw_triangle()  / lv_canvas_draw_polygon(3)
//   fillConvexQuad()   -> lv_canvas_draw_polygon(4)
//   fillCircle()       -> lv_draw_arc(0..360, filled)
//   strokeArcRing()    -> lv_draw_arc(width=w)
//   strokePolyline()   -> composite of the above (no LVGL primitive exists)
//
// SEAMS
// Any antialiasing rasterizer -- Canvas2D here, LVGL's on device -- blends
// each shape's edge against whatever is already in the buffer. Two triangles
// that merely SHARE an edge therefore each lay down ~50% coverage along it,
// and the background shows through as a hairline. On the arrowhead that read
// as grey lines all over the glyph. inflate() below pushes every piece out by
// a sub-pixel amount so neighbours overlap instead of abut, which is the same
// fix the C port needs; it is not a Canvas-specific workaround.

const LV = (() => {
  let ctx = null;
  let size = 0;

  // Creates a draw target. Multiple targets can coexist (the demo renders the
  // same frame at several panel resolutions side by side); use() selects one.
  function target(canvas) {
    return { ctx: canvas.getContext("2d"), size: canvas.width, canvas };
  }

  function use(t) {
    ctx = t.ctx;
    size = t.size;
  }

  function attach(canvas) {
    const t = target(canvas);
    use(t);
    return t;
  }

  // -- seam suppression ---------------------------------------------------

  // How far a mitred corner may travel from its original vertex, as a
  // multiple of the offset distance. At a shallow corner the two offset edges
  // are nearly parallel and their intersection runs away to infinity -- which
  // rendered as thin spikes shooting off the arrowhead. The glyph itself no
  // longer has such corners (build_icons.py strips the traced pass-through
  // points), but geometry is data and this keeps a bad corner bounded instead
  // of catastrophic.
  const MITER_LIMIT = 2.5;

  // Offsets a convex polygon outward by `d` pixels: each edge is pushed along
  // its outward normal and consecutive offset edges are re-intersected. Exact
  // for any convex shape, and unlike scaling about the centroid it does not
  // under-expand thin slivers -- which matters, because ear clipping produces
  // plenty of those.
  function inflate(pts, d) {
    if (!d) return pts;
    const n = pts.length;
    let cx = 0, cy = 0;
    for (const p of pts) { cx += p[0]; cy += p[1]; }
    cx /= n; cy /= n;

    const lines = [];
    for (let i = 0; i < n; i++) {
      const p = pts[i], q = pts[(i + 1) % n];
      let dx = q[0] - p[0], dy = q[1] - p[1];
      const len = Math.hypot(dx, dy);
      if (len < 1e-9) { lines.push(null); continue; }
      dx /= len; dy /= len;
      let nx = -dy, ny = dx;
      if (nx * (p[0] - cx) + ny * (p[1] - cy) < 0) { nx = -nx; ny = -ny; }
      lines.push({ px: p[0] + nx * d, py: p[1] + ny * d, dx, dy });
    }

    const out = [];
    for (let i = 0; i < n; i++) {
      const a = lines[(i + n - 1) % n], b = lines[i];
      if (!a || !b) { out.push(pts[i]); continue; }
      const cross = a.dx * b.dy - a.dy * b.dx;
      if (Math.abs(cross) < 1e-9) { out.push(pts[i]); continue; } // parallel
      const t = ((b.px - a.px) * b.dy - (b.py - a.py) * b.dx) / cross;
      const mx = a.px + a.dx * t, my = a.py + a.dy * t;
      // Clamp the miter: past the limit, fall back to moving the vertex
      // straight out along the bisector by `d`, which never spikes.
      const ox = mx - pts[i][0], oy = my - pts[i][1];
      const run = Math.hypot(ox, oy);
      if (run > d * MITER_LIMIT) {
        const k = (d * MITER_LIMIT) / run;
        out.push([pts[i][0] + ox * k, pts[i][1] + oy * k]);
      } else {
        out.push([mx, my]);
      }
    }
    return out;
  }

  // -- primitives ---------------------------------------------------------

  // lv_canvas_fill_bg(&canvas, color, LV_OPA_COVER)
  function clear(color) {
    ctx.fillStyle = color;
    ctx.fillRect(0, 0, size, size);
  }

  function fillPoly(pts, color) {
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
    ctx.closePath();
    ctx.fill();
  }

  // lv_draw_triangle(): the one polygon primitive LVGL is always safe with.
  function fillTriangle(a, b, c, color, grow = 0) {
    fillPoly(inflate([a, b, c], grow), color);
  }

  // lv_canvas_draw_polygon() with 4 points. Caller guarantees convexity --
  // every quad we emit is a rectangle in some rotated frame, so it holds.
  function fillConvexQuad(a, b, c, d, color, grow = 0) {
    fillPoly(inflate([a, b, c, d], grow), color);
  }

  // lv_draw_arc() sweeping the full 360 with the radius as its width.
  function fillCircle(cx, cy, r, color) {
    if (r <= 0) return;
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fill();
  }

  // lv_draw_arc() with arc_dsc.width = w, start 0 end 360.
  function strokeArcRing(cx, cy, r, w, color) {
    if (r <= 0 || w <= 0) return;
    ctx.strokeStyle = color;
    ctx.lineWidth = w;
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.stroke();
  }

  // lv_draw_line(): one straight stroked segment. Unlike strokePolyline this
  // needs no join workaround -- a single segment has no interior vertex to
  // notch -- so it maps straight onto LVGL's line draw with no composition.
  function strokeLine(x1, y1, x2, y2, width, color) {
    if (width <= 0) return;
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.lineCap = "butt";
    ctx.beginPath();
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    ctx.stroke();
  }

  // -- composites ---------------------------------------------------------

  // The replacement for SVG's stroke-linejoin:round on a thick polyline.
  //
  // One quad per segment, offset by the segment's own normal, plus a disc at
  // each INTERIOR vertex to fill the wedge the two quads leave open on the
  // outside of the bend. Ends stay flat -- the old SVG used
  // stroke-linecap:butt, and the head end deliberately tucks under the
  // arrowhead glyph, so caps would poke out past its point.
  //
  // Cost is linear in segment count (<= 63 points for the worst maneuver) and
  // uses no trig at all: the normal is just the perpendicular of the
  // normalized segment vector.
  function strokePolyline(pts, width, color, grow = 0) {
    if (!pts || pts.length < 2 || width <= 0) return;
    const h = width / 2;
    for (let i = 1; i < pts.length; i++) {
      const a = pts[i - 1], b = pts[i];
      const dx = b[0] - a[0], dy = b[1] - a[1];
      const len = Math.hypot(dx, dy);
      if (len < 1e-9) continue;
      const nx = (-dy / len) * h, ny = (dx / len) * h;
      fillConvexQuad(
        [a[0] + nx, a[1] + ny],
        [b[0] + nx, b[1] + ny],
        [b[0] - nx, b[1] - ny],
        [a[0] - nx, a[1] - ny],
        color, grow
      );
    }
    // discs are drawn at the full half-width plus the same growth, so they
    // swallow the quad ends rather than meeting them edge-to-edge
    for (let i = 1; i < pts.length - 1; i++) fillCircle(pts[i][0], pts[i][1], h + grow, color);
  }

  // Ear clipping, run ONCE on the arrowhead at startup. The glyph is rigid,
  // so the triangle INDICES never change -- per frame we only transform the
  // 15 vertices and redraw the same triangle list. On device this becomes a
  // const index array in flash and no triangulation code ships at all.
  function triangulate(poly) {
    const n = poly.length;
    if (n < 3) return [];
    const idx = [...Array(n).keys()];
    const area2 = (a, b, c) =>
      (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    let signedArea = 0;
    for (let i = 0; i < n; i++) {
      const p = poly[i], q = poly[(i + 1) % n];
      signedArea += p[0] * q[1] - q[0] * p[1];
    }
    if (signedArea < 0) idx.reverse(); // normalize to CCW
    const inside = (a, b, c, p) =>
      area2(a, b, p) >= 0 && area2(b, c, p) >= 0 && area2(c, a, p) >= 0;

    const tris = [];
    let guard = 0;
    while (idx.length > 3 && guard++ < n * n) {
      let clipped = false;
      for (let i = 0; i < idx.length; i++) {
        const i0 = idx[(i + idx.length - 1) % idx.length];
        const i1 = idx[i];
        const i2 = idx[(i + 1) % idx.length];
        const a = poly[i0], b = poly[i1], c = poly[i2];
        if (area2(a, b, c) <= 0) continue; // reflex, not an ear
        let clean = true;
        for (const j of idx) {
          if (j === i0 || j === i1 || j === i2) continue;
          if (inside(a, b, c, poly[j])) { clean = false; break; }
        }
        if (!clean) continue;
        tris.push([i0, i1, i2]);
        idx.splice(i, 1);
        clipped = true;
        break;
      }
      if (!clipped) break; // degenerate input; emit what we have
    }
    if (idx.length === 3) tris.push([idx[0], idx[1], idx[2]]);
    return tris;
  }

  // Draws a pre-triangulated polygon given freshly transformed vertices.
  // `grow` is the seam overlap -- without it the shared edges between these
  // triangles show as hairlines of background colour straight through the
  // glyph.
  function fillTriangulated(verts, tris, color, grow = 0) {
    for (const [a, b, c] of tris) fillTriangle(verts[a], verts[b], verts[c], color, grow);
  }

  return {
    target,
    use,
    attach,
    clear,
    inflate,
    fillTriangle,
    fillConvexQuad,
    fillCircle,
    strokeArcRing,
    strokeLine,
    strokePolyline,
    triangulate,
    fillTriangulated,
    get size() { return size; },
  };
})();

if (typeof module !== "undefined") module.exports = { LV };
