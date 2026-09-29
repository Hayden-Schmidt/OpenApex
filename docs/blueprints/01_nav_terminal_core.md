# 01 — Nav terminal core (C3 notification + GNSS display)

**Status:** IN PROGRESS  
**Last reconciled with code:** 2026-09-29 (light pass: file evidence, no hardware re-test)  
**Reference docs:** [system_overview](../reference/system_overview.md), [nav_pipeline](../reference/nav_pipeline.md), [ble_link](../reference/ble_link.md), [gui_and_display](../reference/gui_and_display.md), [hardware](../reference/hardware.md)

| Phase | Name | Status | Evidence |
|---|---|---|---|
| 01.1 | Firmware skeleton and BLE packet path | DONE | `firmware/main/ble_link.c`, `packet.c`, `packet_merge.c`; host suites `packet_normalize_test.c`, `packet_kotlin_fixture_test.c` |
| 01.2 | Android source adapter | DONE | `phone/androidApp/.../NavNotificationRelayService.kt`; telemetry via `phone/shared/.../TelemetryAggregator.kt` (packet v6) |
| 01.3 | Countdown | DONE | `firmware/main/countdown.c`, `pipeline.c`; `countdown_test.c`, `pipeline_test.c` |
| 01.4 | On-device GUI | DONE | `firmware/gui/` (splash, idle, dial, arrived, odometer, call, friend screens); `gui_app_init()` wired in `firmware/main/main.c`; on-device session [2026-09-26](../investigations/2026-09-26_on_device_session.md) |
| 01.5 | Physical display and mechanical prototype | PARTIAL | Display and touch run on the delivered board (`display_driver.c`, `touch_driver.c`, `BOARD_HAS_TOUCH 1`). Weather-resistant mechanical prototype: not started |

## Current status (2026-09-29)

Everything in this blueprint's scope runs on the delivered C3 board except the mechanical
prototype. The iOS source path is built on the firmware side but belongs to
[blueprint 03](03_ios_multiplatform.md) and is unverified on an iPhone. Work that grew out of this
blueprint now has its own homes: heading fusion ([02](02_heading_sensor_fusion.md)), the rider
mesh ([reference](../reference/rider_mesh_v1.md), [04](04_rider_mesh.md)).

**Next for this blueprint:** the weather-resistant mechanical prototype; the open decisions below
(notification formats and localisation; BLE disconnect churn re-test; CDM cold-wake mitigation).
The exit test has not been formally re-run end to end since the GUI landed; run it before
marking 01.5 DONE.

## Scope

*Was SPEC §2.*

Phase 1 is the first implementation target and is intentionally narrow.

### Does

*Was SPEC §2.1.*

- Targets the incoming generic ESP32-C3 1.28-inch GC9A01 round display.
- Proves BLE connectivity and live rendering on the C3 screen.
- Reads Google Maps, Apple Maps, and later Waze navigation notifications through phone-side
  platform adapters.
- Normalizes notification data into a common maneuver/distance/street model.
- Passes phone GNSS-derived speed, heading, fix state, and freshness data to the terminal.
- Performs local distance countdown processing on the C3 between notification updates.
- Renders maneuver icon, distance to maneuver, speed, connection state, and stale-data state.
- Supports touch and non-touch C3 board variants through compile-time configuration.

### Does not

*Was SPEC §2.2.*

- No local mapping of any kind.
- No map tiles, raster maps, vector maps, route polylines, or road-context geometry.
- No route calculation, rerouting, geocoding, or map matching on the phone companion layer or C3.
- No terminal-side navigation authority.
- No onboard terminal GNSS as a second navigation source.
- No terminal IMU fusion into the navigation solution.
- No offline map or offline routing data.
- No audio, microphone, speaker, or codec feature.
- No persistent ride history or route storage.

Phase 1 is a notification and phone-GNSS passthrough proof of concept. The terminal is proving the
BLE, parser, countdown, and display mechanisms, not proving local navigation.

## Checklist and exit test

*Was SPEC §11 Phase 1.*

- [x] Confirm the C3 board variant, pins, touch option, reset, backlight, and brownout behavior
      (bare dev board confirmed; display module not yet in hand).
- [x] Create ESP-IDF project structure and compile-time C3 profile.
- [x] Disable unused audio and unrelated silicon where present.
- [x] Implement BLE reception and normalized packet decoding.
- [x] Implement Android NotificationListenerService source adapter for Google Maps.
- [x] Implement the iOS/ANCS source contract and adapter path for Apple Maps. *(2026-09-29: firmware side done — `ancs_core.c`, `ancs_nav.c`, `apple_client.c`; unverified on an iPhone, tracked in blueprint 03.)*
- [x] *(2026-09-29: firmware done — `call_state.c`, `call_packet.c`, `call_test.c`; iPhone-unverified.)* Implement ANCS incoming-call support ([ble_link](../reference/ble_link.md)): IncomingCall category, Title attribute, Accept via Control Point.
- [x] Pass phone GNSS speed, heading, validity, and freshness through the packet. *(2026-09-29: packet v3 heading, v6 telemetry-only packet; `packet_merge.c`.)*
- [x] Implement C3-side speed smoothing and distance countdown between notifications.
- [x] *(2026-09-29: done — wired via `gui_app_init()` in `main.c`; the note below is history.)* Implement the basic LVGL maneuver/distance/speed/stale UI on-device (prototyped in
      `firmware/sim_lvgl`, not yet wired into `firmware/main/main.c`'s `gui_task`). GUI architecture
      is now shared-baseline + tier-gated per [gui_and_display](../reference/gui_and_display.md) — see "Recommended next steps" item 2 above for
      the sequencing.
- [ ] Build the first weather-resistant mechanical prototype only after the display path works.

**Phase 1 exit test:** Google Maps navigation notification plus phone GNSS data reaches the C3,
the screen renders the maneuver and distance, the distance counts down smoothly between source
updates, stale/disconnect states are visible, and the board recovers from power removal. No map,
tile, route, polyline, local navigation, or terminal GNSS dependency is permitted.

## Acceptance criteria

*Was SPEC §12.*

- The C3 prototype builds with touch enabled or disabled without changing navigation logic. *(DEFERRED — owner: no non-touch board support at this stage.)*
- Android Google Maps notification data can drive the C3 maneuver display.
- Apple Maps ANCS data maps to the same normalized maneuver model when the iOS path is enabled.
- Phone GNSS speed and heading arrive with validity and freshness information.
- The C3 countdown continues between notifications without pretending stale data is fresh.
- Unsupported, malformed, or stale packets cannot produce a false zero-distance maneuver.
- No map, route, tile, polyline, or local navigation code is required for Phase 1.
- A BLE disconnect, phone GNSS loss, or abrupt power cut does not crash the display.
- C3, C6, and S3 profiles do not assume the same core count, peripherals, memory, or pins.
- The production reference target keeps audio disabled and preserves RF antenna clearance.

## Open decisions

*Was SPEC §13 items 2, 7, 8 (two items numbered 8) and 9.*

2. Confirm Android notification formats and localization behavior for Google Maps, Apple Maps, and
   Waze test fixtures.

7. Decide whether the embedded project remains under `firmware/` in this repository.

8. **[2026-09-24 UPDATE]** Service-lifetime churn is resolved, and it was not a radio problem.
   `OpenApexCompanionService.relayStop()` called `stopService()` on every CDM "device disappeared"
   event, so a momentary BLE drop destroyed and recreated `RelayService` — seven lifetimes in 70
   minutes on the 2026-09-24 capture, `bleDisconnected -> serviceDestroy` within 35 ms each time.
   It now leaves the service running to reconnect itself. The NimBLE reason-531 question below is
   separate and still open.

8. Root-cause the ~9-30s BLE central/peripheral disconnect churn (NimBLE reason 531) previously
   observed between the C3 and an Android peripheral (tested on a OnePlus 15), before roles were
   flipped in [decisions](../reference/decisions.md) ADR-5. A `ble_gap_update_params()` call from the central made disconnects happen
   faster and was reverted; suspected OEM (OnePlus/ColorOS) peripheral-stack issue. **Needs
   re-testing** now that the C3 is the peripheral and the phone is the central — the disconnect
   source may no longer apply, since the flaky peripheral role moved off the OEM phone stack.

9. **CONFIRMED RISK, root cause narrowed**: `CompanionDeviceManager` cold-process wake is unreliable
   on the OnePlus 15 test device. Five candidate root causes were investigated and ruled out:
   - Service UUID placed in scan response instead of the primary advert payload — ruled out;
     `ble_gap_adv_set_fields()` is the only call setting advertising data in `ble_link.c` and it sets
     the primary payload (no device name is included specifically so the UUID fits in the 31-byte
     legacy primary payload).
   - ESP32 MAC address instability (RPA/rotating address) — ruled out; the C3's MAC
     (`88:56:a6:29:76:8a`) was identical across every reset/boot tested, and no RPA code exists in
     the firmware (`ble_hs_util_ensure_addr`/`ble_hs_id_infer_auto` give a stable public/static
     address).
   - `CompanionDeviceService` manifest/binding declaration — ruled out; confirmed correct via direct
     file read and `dumpsys activity services` showing correct binding with the right permission and
     intent-filter.
   - OxygenOS/ColorOS OS-level background restriction (Doze whitelist, `RUN_ANY_IN_BACKGROUND`,
     `START_FOREGROUND`, app's own "allow background activity" toggle) — ruled out; all confirmed
     allowed via `dumpsys deviceidle`/`dumpsys appops`, device was not in Doze during test windows,
     and the user confirmed "allow background activity" is on with no separate "auto-launch" toggle
     present in Settings on this device.
   - Slow BLE advertising interval — a real bug, now fixed: `adv_params` in `start_advertising()`
     (`firmware/main/ble_link.c`) was zero-initialized, leaving NimBLE's default (~1.28s) interval,
     which could cause Android's low-power background scanner to miss the advertisement for multiple
     scan cycles. Changed to `itvl_min=32`/`itvl_max=64` (20-40ms). Also found and cleaned up 10
     duplicate CDM associations for the same MAC accumulated from repeated test-cycle relaunches
     (`adb shell cmd companiondevice disassociate`), which could have added noise/races to the
     presence state machine.
   - **Re-tested after the fast-advertising fix and with a single clean association**
     (`scripts/test-ble-link.sh COM5 90`): still fails — `onDeviceAppeared` does not fire and no BLE
     connection occurs within a 90s cold-process window. Critically, `dumpsys companiondevice`
     captured immediately after the window shows the terminal listed under "Nearby BLE Devices" for
     the association — i.e. **CDM's own presence-detection engine does correctly detect the
     terminal as nearby**, isolating the failure to one specific step: the `onDeviceAppeared`
     wake-up broadcast to the killed app's `CompanionDeviceService` is not being delivered (or is
     delayed well beyond 90s). This points at an OEM (OxygenOS/ColorOS)-level restriction on waking
     third-party app components from a killed state that has no discoverable toggle in Settings or
     `adb shell dumpsys`/`cmd` output — not a bug in this project's BLE/CDM implementation.
   - **API 36 deprecation checked and ruled out as the cause.** The test device runs Android 16
     (API 36), which deprecates `startObservingDevicePresence(String)`/`onDeviceAppeared(String)` in
     favor of `startObservingDevicePresence(ObservingDevicePresenceRequest)` +
     `onDevicePresenceEvent(DevicePresenceEvent)`. Migrated `MainActivity.kt` and
     `OpenApexCompanionService.kt` to use the new API on API 36+ (falling back to the old API on
     API < 36 via an `SDK_INT` check, so minSdk 26 devices are unaffected) and re-tested with a
     freshly cleaned single association: **identical failure** — `onDevicePresenceEvent` also never
     fires from a killed process, and `dumpsys companiondevice` still shows the terminal correctly
     detected as "Nearby." Confirms this is not an API-deprecation issue.
   - **External research** (Google Issue Tracker #432207962) confirms this is a known, currently
     unresolved AOSP-level bug/regression in `onDeviceAppeared`/`onDevicePresenceEvent` delivery,
     and OnePlus/OxygenOS/ColorOS devices are widely documented (dontkillmyapp.com and others) as
     among the most aggressive at killing background processes and silently reverting
     battery-optimization whitelisting, with no programmatic (adb/API) way to fix it from app code.
   - **Practical implication / accepted fix:** per community and Android-team consensus, CDM
     cold-wake should not be relied on alone on affected OEMs. The recommended pattern — and what
     this project is implementing — is CDM (still useful on OEMs where it works) backed by a
     `BOOT_COMPLETED` receiver that starts a persistent foreground service, so the phone reconnects
     to the terminal on boot without ever depending on the CDM wake callback firing from a killed
     process.

## Resolved decisions

*Was SPEC §13 items 1 and 5.*

1. ~~Confirm the exact C3 board revision and delivered touch/non-touch pinout.~~ **RESOLVED**
   (2026-09-26): touch variant; CST816 at 0x15 on SDA GPIO4 / SCL GPIO5 responds, `BOARD_HAS_TOUCH 1`.

5. ~~Choose the ESP-IDF version and LVGL major version for the firmware project.~~ **RESOLVED**
   (see [decisions](../reference/decisions.md) ADR-2): ESP-IDF 5.x + LVGL 9.

## Status history

### Current status (2026-09-21)

*Was SPEC §11 Current status.*

BLE roles have been flipped (see [decisions](../reference/decisions.md) ADR-5): the C3 terminal is now the BLE peripheral/GATT server, and
the Android phone is the central/client using `CompanionDeviceManager` (CDM) observer mode. This
replaces the previous central/peripheral assignment and the disconnect-churn issue described below
is being re-tested under the new roles ([01_nav_terminal_core](01_nav_terminal_core.md) (Open decisions, item 8)).

Confirmed working end-to-end on real hardware (bare ESP32-C3 dev board on COM5, no display module
yet, plus a OnePlus 15 running the Android relay app):

- NimBLE peripheral (`firmware/main/ble_link.c`) advertises the OpenApex nav service, accepts one
  bonded central, and decodes incoming `RawNotifPacket` writes. Builds and boots cleanly (RAM 4.8%,
  Flash 45.6%); serial log confirms clean advertising with no crashes.
- Android central (`RelayBleClient.kt`, `OpenApexCompanionService.kt`, `NavNotificationRelayService.kt`,
  `RelayService.kt`) associates with the terminal via CDM, registers presence observation
  (`CompanionDeviceManager.startObservingDevicePresence`, which is required in addition to
  `associate()` and needs the `REQUEST_OBSERVE_COMPANION_DEVICE_PRESENCE` manifest permission — see
  [decisions](../reference/decisions.md) ADR-5), and has been observed end-to-end to fire `onDeviceAppeared` and start `RelayService` as a
  foreground service from a killed app process on a real device reboot cycle.
- `firmware/sim_lvgl` (PlatformIO native + SDL2) added as the LVGL GUI design/iteration surface:
  opens a real SDL window at the exact `board_profile.h` resolution (240x240), driven by a scripted
  fixture sequence in the absence of live hardware/data. Screen-building code lives in
  `firmware/gui/`, not `firmware/main/`, so it never gets pulled into the real ESP-IDF
  `prototype_c3` build. The original plain-C screen content (`gui_screens.c`/`.h`) has since been
  removed and is being rebuilt on the C++ architecture in [gui_and_display](../reference/gui_and_display.md) (see [01_nav_terminal_core](01_nav_terminal_core.md) "Recommended next steps"
  item 2); `sim_lvgl` currently opens an empty window pending that rebuild.
- `pio run -e native` and `pio run -d firmware -e prototype_c3` build clean. `sim_lvgl` builds and
  opens its window but has no screen content until [gui_and_display](../reference/gui_and_display.md) is implemented.

**Open items:** the full GATT connect/write path from the Android central to the C3 peripheral
(RelayBleClient discovering the service and writing a live packet) has not yet been observed in a
single continuous session — see [01_nav_terminal_core](01_nav_terminal_core.md) (Open decisions) items 8 and 9. The previous ~9-30s disconnect-churn issue (NimBLE
reason 531) was observed under the old central/peripheral assignment and needs re-testing now that
the flaky peripheral role has moved off the OEM phone Bluetooth stack.

### Recommended next steps

*Was SPEC §11 Recommended next steps.*

1. Confirm the full GATT connect/write path (phone central to C3 peripheral) with live Google Maps
   navigation data, and re-test the disconnect-churn scenario under the new roles ([01_nav_terminal_core](01_nav_terminal_core.md) (Open decisions) items 8, 9).
2. GUI rebuild from scratch on the C++ architecture (see [gui_and_display](../reference/gui_and_display.md)). The original Phase 1 plain-C
   `gui_screens.c`/`.h` and the SDL sim's textual-include bridge have been removed; `sim_lvgl`
   currently opens an empty window pending this work:
   a. Add `BOARD_GFX_TIER_BASIC`/`BOARD_GFX_TIER_RICH`, capability flags (`BOARD_HAS_SPLASH`,
      `BOARD_HAS_MAP_RENDER`, ...), and a 466x466 S3 profile stub to `board_profile.h` (no real S3
      pin/panel data needed yet).
   b. Stand up the build wiring: `firmware/gui` as its own ESP-IDF component; `sim_lvgl`'s
      `platformio.ini` switched from the old single-file bridge to a normal multi-TU
      `build_src_filter` build; `gui_app.hpp`/`.cpp` skeleton with the `extern "C"`
      `gui_app_init()`/`gui_app_update()` seam, loading an empty screen. Confirm both the ESP-IDF
      build and the native `sim_lvgl` build compile clean before writing any screen content.
   c. Second `firmware/sim_lvgl` PlatformIO env (`sim_lvgl_s3`) for the S3 profile, sharing the
      same `gui_app.cpp`/`DialScreen` build as the C3 env — done ahead of screen content so both
      windows (240x240 and 466x466) run side by side while designing Layer 1, catching scaling
      issues live instead of in a later pass.
   d. Implement `Screen`, `GuiTheme` interfaces, `BasicTheme`, and `RichTheme` (`RichTheme` can
      start visually identical to `BasicTheme`, richer styling deferred); implement `DialScreen`
      from scratch — a fresh visual design, not a port of the removed `gui_screens.c` content —
      relatively laid out (`LV_PCT`) so it scales without a layout fork. Built item-by-item,
      checked against both running `sim_lvgl` windows after each piece, not pre-mocked.
3. Finish the Phase 1 UI content against both sim envs: real maneuver icons (replace text
   placeholders), close-turn visual emphasis, a `DiagnosticsScreen` (Layer 2), all 6 [gui_and_display](../reference/gui_and_display.md) states —
   then wire `gui_app_init`/`gui_app_update` into the real `gui_task` on the C3 firmware, driven by
   the now-working `ble_link`/`countdown` pipeline instead of the simulator's scripted fixture.
4. Source and wire the physical GC9A01 display module once available; port the SPI display driver
   (deferred — no module in hand as of this status update).
5. Implement the iOS/ANCS source adapter (Android-only so far).
6. Add phone motion/GNSS telemetry (fused location + fused orientation + raw accel/gyro) per the
   Phase 1 roadmap below — not yet started.
7. `BOARD_GFX_TIER_RICH` implementation (animated splash, richer transitions, map-following once
   [06_road_context](06_road_context.md) exists) tracks against real S3 hardware in Phase 2 — additive, does not block Phase 1.
