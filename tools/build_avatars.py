#!/usr/bin/env python3
"""Build the preset avatars (docs/OpenApex_SPEC.md §5.5) from the design reference sheets.

The sheets are mock-ups, not pixel-exact art, so each character is classified at source
resolution and then resampled onto the target grid:
  0 transparent  background, and dark interior features (eyes, mouth) so they read as holes
  1 dim          outline and shadow
  2 body
  3 highlight
The terminal tints these four levels with the accent colour, so the art carries no colour.

Usage: python tools/build_avatars.py [--preview out.png]
Writes firmware/gui/avatars_data.h.
"""
import argparse
import pathlib

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHEETS = ROOT / "design reference" / "Characters"
OUT = ROOT / "firmware" / "gui" / "avatars_data.h"
NAMES = ["crab", "owl", "dino", "octopus", "cat", "chick", "slime", "bear", "dragon", "robot"]


BG = (229, 230, 239)  # sheet background, measured


def is_fg(c):
    return sum(abs(a - b) for a, b in zip(c[:3], BG)) > 60


def lum(c):
    r, g, b = c[:3]
    return 0.299 * r + 0.587 * g + 0.114 * b


def sprite_boxes(im, y0):
    """Horizontal runs of foreground columns below the title, widest ten (drops border specks)."""
    w, h = im.size
    p = im.load()
    cols = [any(is_fg(p[x, y]) for y in range(y0, h - 3)) for x in range(w)]
    runs, s = [], None
    for x, c in enumerate(cols + [False]):
        if c and s is None:
            s = x
        elif not c and s is not None:
            runs.append([s, x - 1])
            s = None
    # Merge runs split by a small gap (the dragon's horns, the robot's antenna).
    merged = []
    for r in runs:
        if merged and r[0] - merged[-1][1] <= 12:
            merged[-1][1] = r[1]
        else:
            merged.append(r)
    merged = [r for r in merged if r[1] - r[0] > 20]
    assert len(merged) == len(NAMES), merged
    boxes = []
    for a, b in merged:
        ys = [y for y in range(y0, h - 3) if any(is_fg(p[x, y]) for x in range(a, b + 1))]
        boxes.append((a, ys[0], b + 1, ys[-1] + 1))
    return boxes


def classify(sprite):
    """Per-source-pixel level map for one cropped sprite."""
    w, h = sprite.size
    p = sprite.load()
    fg = [[is_fg(p[x, y]) for x in range(w)] for y in range(h)]
    dark = [[fg[y][x] and lum(p[x, y]) < 75 for x in range(w)] for y in range(h)]
    # Dark pixels reachable from the background through dark pixels are outline; the rest are
    # enclosed features.
    outline = [[False] * w for _ in range(h)]
    stack = [(x, y) for y in range(h) for x in range(w) if not fg[y][x]]
    seen = [[not fg[y][x] for x in range(w)] for y in range(h)]
    while stack:
        x, y = stack.pop()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and dark[ny][nx]:
                seen[ny][nx] = True
                outline[ny][nx] = True
                stack.append((nx, ny))
    body = sorted(lum(p[x, y]) for y in range(h) for x in range(w) if fg[y][x] and not dark[y][x])
    mid = body[len(body) // 2]
    lv = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if not fg[y][x]:
                continue
            if dark[y][x]:
                lv[y][x] = 1 if outline[y][x] else 0
                continue
            L = lum(p[x, y])
            lv[y][x] = 3 if L > mid * 1.25 else 1 if L < mid * 0.8 else 2
    return lv, [[fg[y][x] and not (dark[y][x] and not outline[y][x]) for x in range(w)]
                for y in range(h)]


def resample(lv, n):
    """Square-fit the sprite and take a vote per target cell; holes and background win ties so
    eyes survive the downscale."""
    h, w = len(lv), len(lv[0])
    side = max(w, h)
    ox, oy = (side - w) / 2, (side - h)  # bottom-aligned, centred horizontally
    out = []
    for gy in range(n):
        row = []
        for gx in range(n):
            votes = [0, 0, 0, 0]
            bg = 0
            y0, y1 = gy * side / n - oy, (gy + 1) * side / n - oy
            x0, x1 = gx * side / n - ox, (gx + 1) * side / n - ox
            # Sample the inner 60% of the cell to dodge the mock-up's resampling blur.
            for sy in range(5):
                for sx in range(5):
                    fy = y0 + (y1 - y0) * (0.2 + 0.15 * sy)
                    fx = x0 + (x1 - x0) * (0.2 + 0.15 * sx)
                    iy, ix = int(fy), int(fx)
                    if 0 <= iy < h and 0 <= ix < w and lv[iy][ix] is not None:
                        votes[lv[iy][ix]] += 1
                    else:
                        bg += 1
            votes[0] += bg
            best = max(range(4), key=lambda k: (votes[k], k == 0))
            row.append(best)
        out.append(row)
    return out


def resample_native(lv, pitch, n):
    """For a sheet drawn on a known pixel pitch: read each art pixel at its centre, then
    nearest-neighbour squeeze anything wider or taller than n, bottom-aligned and centred."""
    h, w = len(lv), len(lv[0])
    cw, ch = max(1, round(w / pitch)), max(1, round(h / pitch))
    art = [[lv[min(h - 1, int((y + 0.5) * h / ch))][min(w - 1, int((x + 0.5) * w / cw))]
            for x in range(cw)] for y in range(ch)]
    tw, th = min(cw, n), min(ch, n)
    art = [[art[y * ch // th][x * cw // tw] for x in range(tw)] for y in range(th)]
    ox, oy = (n - tw) // 2, n - th
    out = [[0] * n for _ in range(n)]
    for y in range(th):
        for x in range(tw):
            out[oy + y][ox + x] = art[y][x]
    return out


def extract(sheet, y0, fit):
    im = Image.open(SHEETS / sheet).convert("RGB")
    return [fit(classify(im.crop(box))[0]) for box in sprite_boxes(im, y0)]


def pack_a2(grid):
    """Row-major, 4 px per byte, first pixel in the high bits."""
    flat = [v for row in grid for v in row]
    return bytes((flat[i] << 6) | (flat[i + 1] << 4) | (flat[i + 2] << 2) | flat[i + 3]
                 for i in range(0, len(flat), 4))


def write_header(big, small):
    lines = [
        "// Generated by tools/build_avatars.py from design reference/Characters. Do not edit.",
        "// A2 alpha, row-major, 4 px per byte, first pixel in the high bits (spec §5.5).",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define AVATAR_PRESET_COUNT {len(NAMES)}",
        "",
        "// [preset][16x16 A2 (64 B) | 8x8 A2 (16 B)], preset i is settings avatar value i + 1.",
        f"static const uint8_t kAvatarPresets[{len(NAMES)}][80] = {{",
    ]
    for name, b, s in zip(NAMES, big, small):
        data = pack_a2(b) + pack_a2(s)
        lines.append(f"    // {name}")
        lines.append("    {")
        for i in range(0, len(data), 16):
            lines.append("        " + ", ".join(f"0x{v:02X}" for v in data[i:i + 16]) + ",")
        lines.append("    },")
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", newline="\n")


def preview(big, small, path):
    shade = [(0, 0, 0), (0, 64, 85), (0, 129, 170), (0, 194, 255)]
    scale = 8
    im = Image.new("RGB", (len(NAMES) * 20 * scale, 30 * scale), (0, 0, 0))
    for i, (b, s) in enumerate(zip(big, small)):
        for grid, oy, sc in ((b, 0, scale), (s, 18 * scale, scale)):
            for y, row in enumerate(grid):
                for x, v in enumerate(row):
                    x0, y0 = i * 20 * scale + x * sc, oy + y * sc
                    im.paste(shade[v], (x0, y0, x0 + sc, y0 + sc))
    im.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview")
    a = ap.parse_args()
    big = extract("Full Res Characters.png", 38, lambda lv: resample(lv, 16))
    # The quarter sheet is drawn on a ~10 px grid, so read it natively rather than voting.
    small = extract("Quarter Res Characters.png", 30, lambda lv: resample_native(lv, 10.1, 8))
    write_header(big, small)
    if a.preview:
        preview(big, small, a.preview)


if __name__ == "__main__":
    main()
