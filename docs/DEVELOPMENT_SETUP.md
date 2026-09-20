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

Install ESP-IDF 5.x using Espressif's official Windows installer or VS Code ESP-IDF extension.
Load the ESP-IDF export environment in the terminal so `idf.py` is available, then configure and
build the C3 target:

```powershell
.\scripts\setup-firmware.ps1
```

The firmware skeleton targets `esp32c3` and intentionally contains no S3-only assumptions. Add
board profiles and display drivers after the C3 prototype pinout is confirmed.

### Host countdown test

A native C compiler is required for the pure countdown test. Install LLVM/Clang or MinGW, then run:

```powershell
.\scripts\build-countdown.ps1
```

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

## Phase 1 boundary

Phase 1 is Google Maps/Apple Maps notification ingestion, phone GNSS passthrough, BLE transport,
C3-side distance countdown processing, and LVGL rendering. It does not include maps, map tiles,
route calculation, polylines, offline routing, or terminal-side navigation.
