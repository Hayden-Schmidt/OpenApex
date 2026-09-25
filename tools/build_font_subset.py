#!/usr/bin/env python3
"""Build subsetted Montserrat faces for the GUI from design/fonts/fonts_manifest.json.

Why this exists
---------------
LVGL's built-in Montserrat fonts carry all of printable ASCII plus 61 FontAwesome symbols at
every enabled size. Six enabled sizes came to 231 KB -- about 20% of the C3's 1.5 MB app
partition -- while the GUI draws digits, a colon, a degree sign and four compass letters.
Nothing in firmware/gui references LV_SYMBOL_*, so the symbol range is dropped entirely and
each size keeps only the characters its labels can actually produce.

Output is a checked-in .c per size in firmware/gui/generated/, plus a fonts.h of extern
declarations, in the same spirit as build_icon_raster.py: the build never needs node, only a
regeneration does.

Why .c files and not headers
----------------------------
They have to be C. lv_font_conv writes the lv_font_t initializer with its designators in the
order LVGL's own built-in fonts use, which does not match the declaration order of lv_font_t --
legal in C, an error in C++. Including the generated data into a .cpp (in a namespace, to keep
lv_font_conv's identical file-static names from colliding) therefore cannot work. Separate C
translation units solve the name collisions anyway, which is exactly why LVGL ships fonts this
way.

Both builds need the new files listed: firmware/gui/CMakeLists.txt for the device, and
build_src_filter in firmware/sim_lvgl/platformio.ini for the simulator. This tool checks both
and tells you what to add.

Metrics
-------
Subsetting changes what lv_font_conv computes for line_height and base_line -- a digits-only
44px face reports 31/0 where the full face reports 49/9 -- which would silently move every
label. cap_height and x_height are new in LVGL 9.6 and lv_font_conv 1.5.3 does not emit them
at all. All four are therefore scraped from LVGL's own built-in font source for that size and
patched in, so layout stays bit-identical to the full fonts we are replacing.

Usage
-----
    python tools/build_font_subset.py            # all sizes in the manifest
    python tools/build_font_subset.py --size 44  # just one, for a quick iteration

Requires node (lv_font_conv is fetched by npx) and LVGL's sources under
firmware/managed_components/, which `pio run -d firmware -e prototype_c3` downloads.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FONT_DIR = REPO / "design" / "fonts"
MANIFEST = FONT_DIR / "fonts_manifest.json"
OUT_DIR = REPO / "firmware" / "gui" / "generated"
LVGL_FONT_DIRS = (
    REPO / "firmware" / "managed_components" / "lvgl__lvgl" / "src" / "font",
    REPO / "firmware" / "sim_lvgl" / ".pio" / "libdeps" / "sim_lvgl" / "lvgl" / "src" / "font",
)

LV_FONT_CONV = "lv_font_conv@1.5.3"  # pinned: 1.5.x output still matches LVGL 9.6's fmt_txt API

# Metrics we take from LVGL's built-in face rather than from the subset. See the module docstring.
SCRAPED_METRICS = ("line_height", "base_line", "cap_height", "x_height")


def die(msg: str) -> "None":
    sys.exit(f"build_font_subset: {msg}")


def npx() -> list[str]:
    exe = shutil.which("npx") or shutil.which("npx.cmd")
    if not exe:
        die("npx not found on PATH -- install node to regenerate fonts")
    return [exe, "-y", LV_FONT_CONV]


def scrape_metrics(size: int) -> dict[str, int]:
    """Pull the authoritative font metrics out of LVGL's built-in face for `size`."""
    # LVGL's sources are a downloaded dependency in both builds and neither copy is checked in
    # (managed_components is gitignored, and ESP-IDF deletes it while re-resolving), so take
    # whichever is present.
    for base in LVGL_FONT_DIRS:
        src = base / f"lv_font_montserrat_{size}.c"
        if src.is_file():
            break
    else:
        die(
            f"lv_font_montserrat_{size}.c not found in any of:\n"
            + "".join(f"  {d}\n" for d in LVGL_FONT_DIRS)
            + "  Fetch LVGL first: pio run -d firmware -e prototype_c3"
        )
    text = src.read_text(encoding="utf-8", errors="replace")
    metrics: dict[str, int] = {}
    for name in SCRAPED_METRICS:
        m = re.search(rf"^\s*\.{name}\s*=\s*(-?\d+)\s*,", text, re.MULTILINE)
        if not m:
            die(f"could not read .{name} from {src.name}; did LVGL change its font format?")
        metrics[name] = int(m.group(1))
    return metrics


def chars_to_range(chars: str) -> str:
    """lv_font_conv's -r takes decimal/hex codepoints; pass the characters as explicit points.

    Going through codepoints rather than --symbols keeps non-ASCII (the degree sign) working
    regardless of how the shell and node agree on encoding.
    """
    points = sorted({ord(c) for c in chars})
    return ",".join(f"0x{p:X}" for p in points)


def run_conv(source: Path, size: int, bpp: int, rng: str, out: Path) -> None:
    cmd = npx() + [
        "--no-compress",
        "--no-prefilter",
        "--force-fast-kern-format",
        "--bpp", str(bpp),
        "--size", str(size),
        "--font", str(source),
        "-r", rng,
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "-o", str(out),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not out.is_file():
        die(f"lv_font_conv failed for size {size}:\n{proc.stdout}\n{proc.stderr}")


def postprocess(raw: str, size: int, bpp: int, metrics: dict[str, int], spec: dict) -> str:
    """Fix up one lv_font_conv .c file: stable symbol name, inherited metrics, no stray guard."""
    # lv_font_conv echoes its full command line into the file header, which embeds this machine's
    # absolute paths and a fresh temp directory name every run -- so an unchanged font would still
    # show up as a diff. Replace it with the part that actually describes the output.
    raw = re.sub(
        r"^ \* Opts: .*$",
        f" * Opts: --no-compress --no-prefilter --force-fast-kern-format --bpp {bpp} "
        f"--size {size} -r <see below>",
        raw,
        count=1,
        flags=re.MULTILINE,
    )

    # Strip lv_font_conv's enable guard, which it derives from the output filename. Which faces are
    # compiled in is decided by the manifest and the two build files, not by a macro named after a
    # temp file.
    guard = re.search(r"#ifndef (\w+)\n#define \1 1\n#endif\n", raw)
    if not guard:
        die(f"size {size}: could not find lv_font_conv's include guard to strip")
    name = guard.group(1)
    raw = raw.replace(guard.group(0), "")
    raw = re.sub(rf"^#if {name}\n", "", raw, count=1, flags=re.MULTILINE)
    raw = re.sub(rf"^#endif /\*#if {name}\*/\n", "", raw, count=1, flags=re.MULTILINE)

    # Rename the public font object to something stable and obviously ours.
    public = f"gui_font_montserrat_{size}"
    raw = re.sub(rf"\b{re.escape(name.lower())}\b", public, raw)

    # Patch the metrics the subset got wrong or omitted. line_height/base_line are present and
    # wrong; cap_height/x_height are absent in lv_font_conv 1.5.3 and must be inserted.
    for key in ("line_height", "base_line"):
        raw, n = re.subn(
            rf"(\.{key}\s*=\s*)-?\d+(\s*,)",
            rf"\g<1>{metrics[key]}\g<2>",
            raw,
            count=1,
        )
        if n != 1:
            die(f"size {size}: expected exactly one .{key} to patch, found {n}")
    inserted = (
        f"    .cap_height = {metrics['cap_height']},\n"
        f"    .x_height = {metrics['x_height']},\n"
    )
    raw, n = re.subn(
        r"(\.base_line\s*=\s*-?\d+,[^\n]*\n)",
        r"\g<1>" + f"#if LV_VERSION_CHECK(9, 6, 0) || LVGL_VERSION_MAJOR >= 10\n{inserted}#endif\n",
        raw,
        count=1,
    )
    if n != 1:
        die(f"size {size}: could not insert cap_height/x_height after .base_line")

    # LVGL 9.3+ uses .static_bitmap to skip copying glyph data into RAM. The built-in faces set it
    # (the bitmaps are const and uncompressed, which is true of ours too); lv_font_conv 1.5.3 does
    # not emit it, so without this every glyph draw would take the copying path.
    raw, n = re.subn(
        r"(\n)(    \.dsc = &font_dsc,)",
        r"\g<1>#if LV_VERSION_CHECK(9, 3, 0)\n    .static_bitmap = 1,\n#endif\n\g<2>",
        raw,
        count=1,
    )
    if n != 1:
        die(f"size {size}: could not insert static_bitmap before .dsc")

    covered = spec.get("chars") or spec.get("range")
    header = (
        "// GENERATED by tools/build_font_subset.py from design/fonts/fonts_manifest.json.\n"
        "// Do not edit: rerun the tool instead.\n"
        "//\n"
        f"// Montserrat-Medium {size}px, subsetted to: {covered!r}\n"
        f"// {spec.get('why', '')}\n"
        "//\n"
        "// Any character not listed above renders as an empty box at this size. This is a C file and\n"
        "// must stay one: the lv_font_t initializer's designator order is legal C and a C++ error.\n"
        "//\n"
        f"// line_height/base_line/cap_height/x_height are taken from LVGL's full\n"
        f"// lv_font_montserrat_{size} ({metrics}) so that subsetting cannot move any label.\n"
    )
    return header + raw.lstrip("\n")


def write_decls(specs: list[dict]) -> None:
    """Emit the extern declarations gui_font.cpp consumes."""
    lines = [
        "// GENERATED by tools/build_font_subset.py from design/fonts/fonts_manifest.json.",
        "// Do not edit: rerun the tool instead.",
        "//",
        "// Subsetted Montserrat faces, one per generated font_montserrat_<size>.c. Each carries only",
        "// the characters the manifest lists for it -- anything else draws as an empty box. Read by",
        "// gui_font.cpp; nothing else should touch these directly.",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for spec in specs:
        size = int(spec["size"])
        covered = spec.get("chars") or spec.get("range")
        lines.append(f"// {covered!r}")
        lines.append(f"extern const lv_font_t gui_font_montserrat_{size};")
    lines += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    (OUT_DIR / "fonts.h").write_text("\n".join(lines), encoding="utf-8", newline="\n")


def check_build_files(sizes: list[int]) -> None:
    """Warn if a generated .c is not in the device or simulator build.

    Both builds need to be told about these files explicitly -- ESP-IDF via an SRCS entry, the
    simulator via build_src_filter, whose default glob only picks up ../../gui/*.cpp. Forgetting
    either fails at link time with a missing gui_font_montserrat_<size>, which is a confusing way to
    find out, so say it here instead.
    """
    cmake = REPO / "firmware" / "gui" / "CMakeLists.txt"
    ini = REPO / "firmware" / "sim_lvgl" / "platformio.ini"
    cmake_text = cmake.read_text(encoding="utf-8") if cmake.is_file() else ""
    ini_text = ini.read_text(encoding="utf-8") if ini.is_file() else ""
    problems = []
    for size in sizes:
        rel = f"generated/font_montserrat_{size}.c"
        if rel not in cmake_text:
            problems.append(f'  add "{rel}" to the SRCS in {cmake.relative_to(REPO)}')
    if "generated/*.c" not in ini_text and "generated/" not in ini_text:
        problems.append(
            f"  add +<../../gui/generated/*.c> to build_src_filter in {ini.relative_to(REPO)}"
        )
    if problems:
        print("build_font_subset: WARNING -- the fonts are generated but not built:")
        print("\n".join(problems))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", type=int, action="append", help="only build this size (repeatable)")
    args = ap.parse_args()

    if not MANIFEST.is_file():
        die(f"{MANIFEST} not found")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    source = FONT_DIR / manifest["source"]
    if not source.is_file():
        die(f"{source} not found")
    bpp = int(manifest.get("bpp", 4))

    specs = manifest["fonts"]
    if args.size:
        wanted = set(args.size)
        specs = [s for s in specs if int(s["size"]) in wanted]
        missing = wanted - {int(s["size"]) for s in manifest["fonts"]}
        if missing:
            die(f"size(s) {sorted(missing)} are not in the manifest")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for spec in specs:
            size = int(spec["size"])
            if ("chars" in spec) == ("range" in spec):
                die(f"size {size}: give exactly one of \"chars\" or \"range\"")
            rng = spec["range"] if "range" in spec else chars_to_range(spec["chars"])

            scratch = Path(tmp) / f"montserrat_{size}.c"
            run_conv(source, size, bpp, rng, scratch)
            out = OUT_DIR / f"font_montserrat_{size}.c"
            out.write_text(
                postprocess(scratch.read_text(encoding="utf-8"), size, bpp, scrape_metrics(size), spec),
                encoding="utf-8",
                newline="\n",
            )
            print(f"  {out.relative_to(REPO)}  {out.stat().st_size / 1024:.1f} KB of source")

    # The declarations header always covers the whole manifest, not just --size, so a partial
    # rebuild cannot leave it describing a different set of faces than the manifest does.
    all_sizes = [int(s["size"]) for s in manifest["fonts"]]
    write_decls(manifest["fonts"])
    check_build_files(all_sizes)

    print(f"build_font_subset: wrote {len(specs)} font(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
