"""Copies the SDL2 runtime DLL next to program.exe after linking, so `program.exe` can be run
directly from .pio/build/sim_lvgl/ without manually locating the DLL. Mirrors the vendored
third_party/SDL2_* devel package this env links against (see platformio.ini)."""

import shutil
from pathlib import Path

Import("env")  # noqa: F821 (PlatformIO injects this)


def copy_sdl2_dll(source, target, env):
    project_dir = Path(env["PROJECT_DIR"])
    build_dir = Path(env.subst("$BUILD_DIR"))
    src = project_dir / "third_party" / "SDL2_bin" / "SDL2.dll"
    dst = build_dir / "SDL2.dll"
    if src.exists():
        shutil.copy2(src, dst)


env.AddPostAction("$BUILD_DIR/${PROGNAME}${PROGSUFFIX}", copy_sdl2_dll)
