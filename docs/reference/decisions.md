# Architecture decision records

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §16 (restructure, 2026-09-29)  
**Last verified against code:** not yet

Consolidated decisions that shape the implementation. These are authoritative; where they revise
an earlier section, they supersede it.

ADR-6 (GUI architecture) lives in [gui_and_display.md](gui_and_display.md) because it is the
living description of the GUI.

## ADR-1 — Normalization on the terminal (see [system_overview](system_overview.md))

*Was SPEC §16.1.*

The phone is a dumb relay; the ESP32 normalizes notification text into the maneuver model. One C
normalizer serves both Android and iOS/ANCS.

## ADR-2 — Firmware language split

*Was SPEC §16.2.*

**Decision:** hybrid C/C++ with a fixed boundary.

- **C** — hardware drivers, protocol (BLE GATT, packet decoder), and pure domain logic
  (`normalize.c`, `countdown.c`, `pipeline.c`). No `extern "C"` wrapper fatigue; straight ESP-IDF
  APIs. Data contracts crossing into the GUI (`view_state.h`, `nav_model.h`, `board_profile.h`) stay
  plain C headers — POD structs and macros only, includable from both C and C++.
- **C++** — the entire LVGL GUI layer (`firmware/gui/`): screen classes, the theme/tier interface,
  and view-state handling on the render side. RAII for widget and mutex-guard lifetime. See [gui_and_display](gui_and_display.md)
  for the concrete class layout — this supersedes the single plain-C `gui_screens.c` file from the
  original Phase 1 slice, binned in favor of the architecture below once it became clear the GUI
  would grow multiple screens across multiple board tiers (C3 and S3, both permanently supported,
  not a prototype-then-replace relationship).
- **Disciplined constraints:** `-fno-exceptions`, `-fno-rtti`, no heap-heavy STL inside the render
  loop; `std::array` and fixed pools only. (These are ESP-IDF defaults; do not add conflicting
  `build_flags`.)
- **Boundary:** POD C structs (`nav_payload_t`, `terminal_view_state_t`) cross the C→C++ seam.

The pure C modules (`packet`, `normalize`, `countdown`, `pipeline`) are host-testable under the
native/MSVC test harness independent of ESP-IDF.

## ADR-3 — Toolchain

*Was SPEC §16.3.*

- PlatformIO (`espressif32` platform) drives the ESP-IDF CMake build; no custom toolchain.
- Framework: **ESP-IDF 5.x** (required by LVGL 9), **LVGL 9**.
- BLE: **NimBLE** (lightweight, correct fit for single-core C3).

## ADR-4 — Hardware-free development path

*Was SPEC §16.4.*

The browser simulator (`simulator/index.html`) and PlatformIO `native` target share the same
`countdown.c`/`normalize.c` sources. Browser = visual target; `native` + host tests = code target.

> Superseded in part (2026-09-29): the browser simulator and the `native` target were retired.
> `firmware/sim_lvgl` is the visual target; `scripts/run-host-tests.ps1` runs the host tests.

## ADR-5 — BLE role flip: terminal as peripheral/GATT server, phone as central/client

*Was SPEC §16.5.*

**Decision:** the ESP32 terminal advertises and hosts the GATT server (peripheral); the Android
phone connects, bonds, and writes navigation packets (central/client). This reverses the original
Phase 1 assumption in [ble_link](ble_link.md).

**Why:** every surveyed open-source BLE wearable/accessory precedent (Gadgetbridge, InfiniTime/
PineTime, and consumer earbuds/fitness bands) puts the small, battery- and memory-constrained device
in the peripheral role and lets the phone's mature BLE stack own scanning, bonding, and reconnection
retry logic — that logic is expensive to reimplement correctly on an ESP32 and cheap on Android.
Peripheral-role NimBLE (advertise + accept one bonded central) is also the simpler, lower-power
mode for a single-core C3.

This also unlocks Android `CompanionDeviceManager` (CDM) observer mode: the OS can wake a
`CompanionDeviceService` from a fully killed app process when it detects the terminal's
advertisement, which is what gives the "bike on -> terminal boots -> phone reconnects" flow without
the user manually opening the app. That flow is not available (or far less reliable) with the phone
as peripheral, since CDM observer mode watches for BLE advertisements, not for GATT server clients.

Bonds persist in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST=y`) so the terminal does not require re-pairing
across power cycles. Pairing uses Just Works (`BLE_SM_IO_CAP_NO_IO`) with Secure Connections
(`sm_sc=1`); the navigation characteristic requires an encrypted link (`BLE_GATT_CHR_F_WRITE_ENC`).

**Implementation notes:**

- Firmware: `firmware/main/ble_link.c`/`.h` (replaces the former `ble_central.c`/`.h`) implement the
  peripheral/GATT-server role; `firmware/sdkconfig.defaults` sets `ROLE_PERIPHERAL`/
  `ROLE_BROADCASTER` (not `ROLE_CENTRAL`/`ROLE_OBSERVER`).
- Android: `RelayBleClient.kt` (replaces `RelayBleServer.kt`) implements the GATT client;
  `OpenApexCompanionService.kt` is the CDM observer entry point; `MainActivity.kt` builds the CDM
  `AssociationRequest` and — critically — must also call
  `CompanionDeviceManager.startObservingDevicePresence(macAddress)` for each associated device.
  `associate()` alone does not enable `onDeviceAppeared`/`onDeviceDisappeared` callbacks. This call
  requires the `android.permission.REQUEST_OBSERVE_COMPANION_DEVICE_PRESENCE` manifest permission
  (normal protection level, no runtime grant needed) — omitting it throws a `SecurityException` at
  the call site on modern Android (confirmed on a OnePlus 15, Android 15/16-era ROM).
- This structure leaves room for a later second BLE central (e.g. an ESP32-to-ESP32 sensor link)
  connecting to the same terminal peripheral without any role renegotiation (see [ble_link](ble_link.md)).
- Validated end-to-end on real hardware (C3 + OnePlus 15) with a warm app process: CDM association
  approved, presence observed, `OpenApexCompanionService.onDeviceAppeared` fired, `RelayService`
  started as a foreground service. Cold-process wake did not reproduce in scripted testing — see
  [01_nav_terminal_core](../blueprints/01_nav_terminal_core.md) (Open decisions, item 9). Full GATT connect/write path from the phone to the C3 and item 8's disconnect-churn
  re-test are tracked as open items ([01_nav_terminal_core](../blueprints/01_nav_terminal_core.md) (Open decisions), [01_nav_terminal_core](../blueprints/01_nav_terminal_core.md) (Open decisions)).
- `scripts/test-ble-link.sh` automates this test: it resets the C3, force-stops (but does not
  relaunch) the Android app, captures the C3 serial log and filtered phone logcat concurrently for
  a fixed window, and prints a lean pass/fail summary. Raw logs land in `logs/` (gitignored).
