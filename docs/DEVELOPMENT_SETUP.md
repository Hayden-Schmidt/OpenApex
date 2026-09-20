# OpenApex Development Setup

## Repository layout

- `android/`: minimal Android Auto host-facing app module.
- `firmware/`: ESP-IDF C3 terminal skeleton and profile-independent countdown module.
- `simulator/`: hardware-free browser display and PlatformIO native harness.
- `scripts/`: setup and host-test helpers.
- `docs/OpenApex_SPEC.md`: active architecture and Phase 1 contract.

## Required tools

### Android

- JDK 21.
- Android SDK command-line tools.
- Android SDK Platform 36.
- Android SDK Build Tools 36.0.0.
- Android Platform Tools.
- Gradle is supplied by the repository wrapper: Gradle 8.13.
- Android Studio is optional; VS Code plus the Android SDK is sufficient for command-line builds.

Install the SDK into `%LOCALAPPDATA%\Android\Sdk`, set `ANDROID_HOME` and
`ANDROID_SDK_ROOT`, and create `android/local.properties` if Gradle cannot discover the SDK:

```properties
sdk.dir=C:\\Users\\<user>\\AppData\\Local\\Android\\Sdk
```

`android/local.properties` is ignored and must not be committed.

Build the Android Auto module from the repository root:

```powershell
.\gradlew.bat -p android assembleDebug
```

The Android Auto shell uses `androidx.car.app:app:1.7.0`, a `CarAppService`, a navigation-category
service declaration, and a minimal `PaneTemplate`. Notification ingestion, GNSS normalization, and
BLE publishing are the next implementation slice.

### ESP-IDF

PlatformIO's `espressif32` platform already bundles `framework-espidf` plus the RISC-V/Xtensa
toolchains — a separate Espressif installer is not required. `idf.py` is the documented build
entry point (`.vscode/tasks.json` "Firmware: build C3", `firmware/README.md`) because `firmware/`
is a hand-rolled ESP-IDF CMake project. PlatformIO can also build it directly, through a second,
nested project at `firmware/platformio.ini` (kept separate from the repo-root `platformio.ini`
because PlatformIO only supports one `src_dir` per project, and the C3/native targets compile
disjoint source trees):

```powershell
pio run -d firmware -e prototype_c3
```

Use this path for the PlatformIO IDE's IntelliSense/upload UI; use `idf.py` for the canonical
CI/task build. Keep both working — do not let them diverge.

If `idf.py` isn't already on PATH from a prior ESP-IDF install, export it from the
PlatformIO-bundled copy instead of installing a second toolchain:

```powershell
& "$env:PLATFORMIO_CORE_DIR\packages\framework-espidf\export.ps1"
```

Then configure and build the C3 target:

```powershell
.\scripts\setup-firmware.ps1
```

The firmware skeleton targets `esp32c3` and intentionally contains no S3-only assumptions. Add
board profiles and display drivers after the C3 prototype pinout is confirmed.

**Windows path-with-space warning:** ESP-IDF's CMake build refuses to run if *any* path involved
— project dir, toolchain dir, or PlatformIO core dir — contains a space. If your Windows user
folder has a space in it (e.g. `C:\Users\First Last\`), the default `%USERPROFILE%\.platformio`
core dir will break every espidf build with `Error: Detected a whitespace character in project
paths.`, even from a clean, space-free project directory. Fix: relocate PlatformIO's core dir to a
space-free path once, globally:

```powershell
setx PLATFORMIO_CORE_DIR "C:\PlatformIO"
```

Re-open your terminal/VS Code after setting this so the new session picks it up. The `espressif32`
platform and its packages will re-download under the new location on first use.

### Host countdown test

A native C compiler is required for the pure countdown test. Install LLVM/Clang or MinGW, then run:

```powershell
.\scripts\build-countdown.ps1
```

`build-countdown.ps1` falls back to MSVC (`vcvars64.bat`) when no `cc`/`gcc`/`clang` is on PATH, so
it works without installing MinGW. The PlatformIO `native` simulator environment (below) does need
a real g++, since PlatformIO's `native` platform only drives GCC/Clang, not MSVC.

If you hit the same space-in-username problem with a per-user MinGW install (e.g. winget installing
to `C:\Users\First Last\AppData\...`, which breaks `ld.exe` the same way), install MinGW to a
space-free path instead, e.g. `C:\mingw64`, and add `C:\mingw64\bin` to your user `PATH`.

**After changing PATH, the PlatformIO IDE extension needs its background Home server killed, not
just VS Code restarted.** The extension spawns a persistent `pio home` server process
(`python.exe ... --port 45824`, visible in Task Manager) that outlives individual VS Code restarts
and keeps whatever PATH it was first launched with. If PlatformIO IDE builds still can't find
`g++`/`gcc` after a PATH change even from a fully-quit-and-reopened VS Code, kill any `python.exe`
processes running from your `PLATFORMIO_CORE_DIR` (e.g. `C:\PlatformIO\penv\...`) and retry — the
extension respawns the server fresh with the current environment. A plain integrated terminal
doesn't have this problem since it's a new process each time.

No Python virtual environment is currently required. Python becomes necessary only if we add
asset-generation, packet-fixture, or firmware tooling that uses Python.

## VS Code tasks

- `Android Auto: assembleDebug`
- `Firmware: host countdown test`
- `Firmware: build C3`
- `Simulator: open round display`
- `Simulator: PlatformIO native`

## Hardware-free screen development

Open `simulator/index.html` directly or run `Simulator: open round display`. It provides a round
466x466 canvas with maneuver, distance, speed, GNSS validity, pause/resume, and new-notification
controls. This is the fastest way to iterate on the C3 dial before the board arrives.

The PlatformIO `native` environment compiles `firmware/main/countdown.c` directly through
`simulator/native/countdown_bridge.cpp`, so the countdown logic is shared rather than copied.
The native target prints a deterministic countdown trace; the browser target supplies visual
feedback. PlatformIO's CLI may be exposed only inside the VS Code PlatformIO terminal, even when
`pio` is not available in an ordinary PowerShell session.

## Verified working (2026-09-21)

- `pio run -e native` (root project): builds and runs the native simulator via g++/MinGW.
- `pio run -d firmware -e prototype_c3`: builds the C3 firmware through PlatformIO's espidf
  framework (RAM 3.3%, Flash 14.3% of an esp32c3-devkitm-1).
- `idf.py -C firmware build`: builds the same C3 firmware directly.
- A physical ESP32-C3 (no display, dev board only) is reachable on COM5 for flashing.

## Phase 1 boundary

Phase 1 is Google Maps/Apple Maps notification ingestion, phone GNSS passthrough, BLE transport,
C3-side distance countdown processing, and LVGL rendering. It does not include maps, map tiles,
route calculation, polylines, offline routing, or terminal-side navigation.
