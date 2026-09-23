"""The bundled third_party/SDL2_lib libs are 64-bit (x86_64-w64-mingw32), so the build needs a
64-bit MinGW-w64 g++ -- PlatformIO's own platform_packages = toolchain-gccmingw32 is a 32-bit
(i686) toolchain and will link but fail at link time with undefined SDL2/WinMain references.
WinGet's BrechtSanders.WinLibs.POSIX.UCRT package (MinGW-W64 UCRT, gcc 16.1.0) is the matching
64-bit toolchain; prepend its bin dir so SCons' compiler autodetection picks it up. Scoped to
this build only; does not touch the OS PATH."""

import os

Import("env")  # noqa: F821 (PlatformIO injects this)

mingw_bin = os.path.join(
    os.path.expanduser("~"), "AppData", "Local", "Microsoft", "WinGet", "Packages",
    "BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe",
    "mingw64", "bin")
os.environ["PATH"] = mingw_bin + os.pathsep + os.environ["PATH"]
