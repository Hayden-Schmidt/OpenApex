"""Stops the two sim environments from silently linking each other's object files.

`build_src_filter = +<../../gui/*.cpp>` reaches outside the project dir, so PlatformIO puts those
objects in `.pio/build/gui/` -- *beside* the per-env `.pio/build/sim_lvgl{,_s3}/` dirs rather than
inside them. Both envs therefore share one set of firmware/gui objects, while compiling them with
different `-DBOARD_PROFILE_*`. Whichever env built last wins, and the other one links a binary
built for the wrong panel: wrong icon raster sizes (the generated header is chosen by board
profile) and the wrong GuiTheme. It fails silently -- the build succeeds and the window opens, the
artwork is just quietly the wrong size.

This records which profile the shared objects were built with and wipes them when it changes, so a
switch between envs forces a rebuild instead of reusing mismatched objects.
"""

import re
import shutil
from pathlib import Path

Import("env")  # noqa: F821 (PlatformIO injects this)

shared_dir = Path(env.subst("$PROJECT_BUILD_DIR")) / "gui"
stamp = shared_dir / ".board_profile"

# The board profile is what selects the icon set and the theme, so it is the flag that matters.
# Read it out of the raw -D flags: by this point PlatformIO has parsed build_flags into CPPDEFINES
# as (name, value) tuples, but the exact shape varies by version, and getting this wrong silently
# disables the guard -- so match the flag text instead.
flags = re.findall(r"-D\s*(BOARD_PROFILE_\w+)", env.subst("$BUILD_FLAGS"))
if not flags:
    raise SystemExit("guard_shared_objs: no -DBOARD_PROFILE_* in build_flags -- "
                     "the guard cannot tell the sim environments apart, refusing to build")
profile = flags[0]

previous = stamp.read_text(encoding="utf-8").strip() if stamp.is_file() else None
if previous is not None and previous != profile:
    print(f"guard_shared_objs: firmware/gui objects were built for {previous}, this env needs "
          f"{profile} -- discarding them so they are rebuilt")
    shutil.rmtree(shared_dir, ignore_errors=True)

shared_dir.mkdir(parents=True, exist_ok=True)
stamp.write_text(profile + "\n", encoding="utf-8")
