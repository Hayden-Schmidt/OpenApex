// Rendering/camera knobs -- hand-edit and reload the page.
// (A plain .json file won't load via fetch() from a file:// page without a
// server; this is a .js file assigning one global object purely so it can
// be loaded with a <script> tag instead, same reason icons-data.js is .js.)
//
// Maneuver SHAPES are not here -- edit
// Google Icons Archive/Icon JSON/maneuvers.json and rerun build_icons.py.
const DEMO_PARAMS = {

  // --- panel --------------------------------------------------------------

  // The REAL target panels, in pixels. One canvas is created per entry, each
  // with a backing store of exactly that size, so the demo is a true pixel
  // preview of the device -- 240/244 is a GC9A01-class round LCD, 460/466 a
  // common round AMOLED. Listing both renders them SIDE BY SIDE from the one
  // shared animation state, which is the only honest way to compare how the
  // same geometry reads at each resolution.
  //
  // Maneuver geometry is authored against a 600-unit circle regardless
  // (build_icons.py's DISPLAY_DIAMETER); these sizes only set how many pixels
  // those 600 units land on. Drop the resolution and the line gets genuinely
  // chunkier, which is exactly the thing worth seeing before committing to a
  // panel.
  //
  // `viewPx` is purely a viewing convenience and has no effect on what is
  // rendered or on what the device would do. It is the on-screen diameter
  // given to the LARGEST panel; `viewMode` decides what the others get:
  //
  //   "proportional" -- scaled by pixel count, so a 244 panel appears at
  //       244/460 of the 460's size. Each device pixel is the same physical
  //       size in both previews, which is what you want for judging whether
  //       the smaller panel is actually big enough.
  //   "matched"      -- every panel at viewPx. Equal apparent size makes it
  //       easy to compare detail, but flatters the low-res panel by blowing
  //       its pixels up bigger than the other's.
  //
  // image-rendering:pixelated keeps the real pixel grid visible either way.
  panel: {
    sizes: [244, 460],
    viewPx: 460,
    viewMode: "proportional",

    // Sub-pixel outward growth applied to every filled piece, in PIXELS.
    //
    // Antialiasing rasterizers -- Canvas2D here, LVGL's on device -- blend
    // each shape's edge against the buffer, so two triangles that merely
    // SHARE an edge each lay down partial coverage along it and the
    // background shows through as a hairline. On the triangulated arrowhead
    // that read as grey lines all over the glyph. Growing each piece by half
    // a pixel makes neighbours overlap instead of abut. Set to 0 to see the
    // artifact; above ~0.75 the glyph visibly fattens.
    seamOverlap: 0.5,
  },

  // Every colour the renderer uses. On device these become lv_color_hex().
  colors: {
    background: "#000000",
    route: "#ffffff",
    secondary: "#6b7280",
    referenceCircle: "#3a3a3a",
  },

  // --- size -------------------------------------------------------------

  // Uniform scale on the entire rendered element, about the centre of the
  // display. 1 = fill the 600-unit viewBox as designed; < 1 shrinks the whole
  // arrow (line, arrowhead and all) inside the same round display, e.g. to
  // leave room for a surrounding bezel or UI ring.
  //
  // This is NOT a per-maneuver zoom: it applies equally to every maneuver, so
  // the size constancy the fixed-scale camera exists to provide is preserved.
  // Values > 1 are allowed but will push geometry outside the display --
  // build_icons.py's check_fit() only guarantees fit at scale 1.
  displayScale: 1.1,

  // Stroke width of the route line, in route units, before displayScale.
  // Keep in sync with LINE_THICKNESS in build_icons.py, which reserves half
  // of this as fit margin (the stroke overhangs the bare centreline).
  lineThickness: 22,

  // Size of the shared arrowhead glyph relative to its authored geometry
  // (the glyph is traced at full Material-Symbol size, which is large).
  // Keep in sync with ARROWHEAD_SCALE in build_icons.py -- it reserves the
  // scaled half-width as fit margin, and demo.js derives the line's head trim
  // from it so the stroke end tucks under the glyph instead of poking through.
  arrowheadScale: 0.25,

  // A thin static circle drawn behind everything as a visual scale reference,
  // as a fraction of the PANEL's radius. It is drawn in pure screen space, so
  // it never rotates or pans with the route -- it is a fixed viewport gauge
  // you can size the arrow against. It is also unaffected by displayScale,
  // which is the point: shrink displayScale and you can see the arrow shrink
  // against a fixed ring.
  referenceCircle: {
    show: false,
    radiusFraction: 2 / 3,
    thickness: 2, // route units, like lineThickness
  },

  // A subtle graduated ring at the dial's outer edge, drawn in pure screen
  // space (unaffected by displayScale/camera, like referenceCircle). Ticks
  // are fixed to the bezel; only the red north mark rotates, tracking where
  // true north currently sits on the heading-up display.
  compass: {
    show: true,
    tickCount: 36, // evenly spaced around the ring; keep divisible by majorEvery
    majorEvery: 3, // every Nth tick is a major mark (36/3 = 12, i.e. every 30 deg)

    // Tick intrusion, as a fraction of the dial radius. Cap is 0.15 -- keep
    // majorLength at or below that.
    minorLength: 0.06,
    majorLength: 0.10,
    minorWidth: 3, // px, unscaled (like referenceCircle.thickness)
    majorWidth: 6,
    color: "#585f68",

    // North mark: filled triangle, base on the screen edge, apex pointing
    // inward. northLength is the apex's intrusion as a fraction of radius
    // (cap 0.20); northWidth is the base half-width, also as a fraction of
    // radius.
    northLength: 0.10,
    northWidth: 0.045,
    northColor: "#ff3b30",
  },

  // --- framing ----------------------------------------------------------

  // Screen-space vertical shift of the framed maneuver, in route units
  // (+Y = down). 0 centres it; nudge positive to sit the entry lower.
  verticalOffset: 1,

  // Layout-only vertical shift of the arrow/route drawing (line, arrowhead,
  // merge lane), as a fraction of the panel size (+ = down, - = up). Unlike
  // verticalOffset above, this is applied AFTER displayScale in raw screen
  // pixels, so it stays predictable regardless of displayScale/route-unit
  // choices, and it deliberately does NOT move the compass ring or
  // referenceCircle -- both stay centred as a fixed bezel. Use this to pull
  // the arrow up and leave clear space at the bottom of the round display for
  // overlaid text (distance, street name, etc).
  arrowVerticalOffset: 0,

  // Per-maneuver framing overrides: the point of the maneuver placed at the
  // centre of the display, in the maneuver's own local frame (origin = its
  // entry point, -Y = straight ahead, +X = right).
  //
  // The values below are the generated geometric bbox centres -- i.e. setting
  // none of them changes nothing. They are listed explicitly so each is easy
  // to nudge by hand where the purely geometric centre doesn't read well.
  // Delete a key to fall back to the baked `frame_center` from icons-data.js.
  //
  //   more negative Y -> pushes the maneuver DOWN in frame (shows further ahead)
  //   more positive Y -> pulls it UP in frame
  //   more positive X -> shifts the maneuver LEFT in frame
  //   more negative X -> shifts it RIGHT
  frameCenter: {
    straight: [0.0, -130.0],
    turn_right: [70.0, -120.0],
    turn_left: [-70.0, -120.0],
    turn_slight_right: [34.6, -145.5],
    turn_sharp_right: [58.0, -112.0],
    turn_sharp_left: [-58.0, -112.0],
    turn_slight_left: [-34.6, -145.5],
    u_turn_right: [55.0, -117.5],
    u_turn_left: [-55.0, -117.5],
    roundabout_right: [40, -145],
    roundabout_straight: [0, -140],
    roundabout_left: [-55, -136.6],
    right_merge: [70.0, -130.0],
    merge_left: [-70.0, -130.0],
  },

  // --- motion -----------------------------------------------------------

  // Reveal pacing: route units per millisecond, clamped. Duration scales with
  // a maneuver's arc length so long and short ones draw at a similar visual
  // rate. There is no window-length knob -- the rendered window is always
  // exactly the current maneuver, so its length comes from the geometry.
  revealSpeed: 0.9,
  minRevealMs: 1000,
  maxRevealMs: 1200,
};
