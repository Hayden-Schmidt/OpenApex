"""The WinGet-installed mingw64 toolchain lives under a path containing a space
(...AppData\Local\Microsoft\WinGet\Packages\...\Hayden Schmidt\...), which breaks ld's manifest
object lookup on link (unquoted path split at the space). C:\\mingw64 is the same toolchain
(MinGW-W64 UCRT, gcc 16.1.0) at a space-free path -- prepend it so SCons' compiler autodetection
picks it before the WinGet copy on PATH. Scoped to this build only; does not touch the OS PATH."""

import os

Import("env")  # noqa: F821 (PlatformIO injects this)

os.environ["PATH"] = "C:\\mingw64\\bin" + os.pathsep + os.environ["PATH"]
