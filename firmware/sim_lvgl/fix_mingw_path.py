"""The bundled third_party/SDL2_lib libs are 64-bit (x86_64-w64-mingw32), so the build needs a
64-bit MinGW-w64 g++ -- PlatformIO's own platform_packages = toolchain-gccmingw32 is a 32-bit
(i686) toolchain and will link but fail at link time with undefined SDL2/WinMain references.
WinLibs' MinGW-W64 UCRT (gcc 16.1.0) is the matching 64-bit toolchain; prepend its bin dir so
SCons' compiler autodetection picks it up. Scoped to this build only; does not touch the OS PATH.

It lives at C:\\mingw64, relocated out of the WinGet package dir: that path runs through the user
profile, and the space in it breaks gcc's link step (ld is handed default-manifest.o unquoted and
fails with "cannot find C:/Users/Hayden"). The WinGet location is kept as a fallback only."""

import os

Import("env")  # noqa: F821 (PlatformIO injects this)

candidates = [
    r"C:\mingw64\bin",
    os.path.join(
        os.path.expanduser("~"), "AppData", "Local", "Microsoft", "WinGet", "Packages",
        "BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe",
        "mingw64", "bin"),
]
mingw_bin = next((d for d in candidates if os.path.isfile(os.path.join(d, "g++.exe"))), None)
if mingw_bin is None:
    raise SystemExit("fix_mingw_path.py: no 64-bit MinGW g++ found in " + ", ".join(candidates))
os.environ["PATH"] = mingw_bin + os.pathsep + os.environ["PATH"]
env.PrependENVPath("PATH", mingw_bin)  # noqa: F821 -- SCons runs tools from env['ENV'], not os.environ
