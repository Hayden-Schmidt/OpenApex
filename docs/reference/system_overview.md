# System overview

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md title block, §1, §2.3, §2.4, §8, §8.1 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## Scope

**Status:** Active specification  
**Scope:** Phone navigation-source adapters and a BLE motorcycle terminal  
**First hardware:** Generic ESP32-C3 1.28-inch GC9A01 round display  
**Reference hardware:** Waveshare ESP32-S3-Touch-AMOLED-1.75 and GPS variant

## System Boundary

*Was SPEC §1.*

OpenApex is a high-visibility, weather-sealed handlebar display. A paired smartphone remains
responsible for heavy navigation work and navigation-app integration. The terminal receives
navigation events and phone GNSS telemetry over Bluetooth Low Energy, performs small amounts of
local display processing, and renders a high-contrast vector UI.

The terminal is not a phone replacement. It does not load maps, calculate routes, geocode
locations, or perform map matching in the first development pass.

```text
+--------------------------- Smartphone --------------------------+
| Google Maps / Apple Maps / Waze                                 |
|        |                                                        |
| Android NotificationListenerService / iOS ANCS                 |
|        |                                                        |
| phone GNSS: speed, heading, fix state                          |
|        |                                                        |
| raw-notification + telemetry packet (relay only, no parsing)   |
+-----------------------------+-----------------------------------+
                              | BLE
                              v
+---------------------- Motorcycle terminal ----------------------+
| packet decoder -> normalizer -> countdown -> C++ LVGL renderer   |
| optional board sensors: GNSS, IMU, RTC, buttons, touch           |
| high-contrast maneuver, distance, speed, connection UI           |
+------------------------------------------------------------------+
```

The phone is a **dumb relay**: it forwards raw navigation notification fields and GNSS telemetry
without interpreting them. All text parsing (maneuver type, street name, distance) happens on the
terminal in a single pure-C normalizer shared by both the Android and iOS/ANCS paths. See
[system_overview](system_overview.md) "Normalization ownership".

## Platform source adapters

*Was SPEC §2.3.*

**Android:** A companion/background helper uses `NotificationListenerService` to observe supported
navigation notifications from Google Maps, Apple Maps where available, Waze, and later OsmAnd.
The helper extracts title, subtitle, message, source application, and notification update time.

**iOS:** The terminal can use Apple Notification Center Service (ANCS) as a BLE client to receive
supported notification events from the iPhone. The firmware or an iOS-side adapter filters active
navigation applications and extracts notification attributes. A custom iOS app is not required for
basic ANCS events, but richer GNSS and normalization behavior may require an iOS companion later.

Both source paths must produce the same normalized navigation model before BLE transmission. A
GNSS-only source can provide telemetry and diagnostics but cannot populate the maneuver display
without a navigation notification.

## Normalization ownership (architectural decision)

*Was SPEC §2.4.*

**Decision:** normalization lives on the **ESP32 terminal**, not the phone. The phone is a dumb
relay that forwards raw notification fields and GNSS telemetry; it does no text parsing,
classification, or street extraction.

**Rationale:**

- iOS/ANCS delivers notification text directly to the terminal (the terminal is the BLE GATT
  client to the iPhone; there is no phone-side app to parse). C-side normalization is therefore
  unavoidable for the iOS path. Writing it once on the terminal and routing Android through it is
  the only way to get a single implementation instead of two (one Kotlin, one C) that would drift.
- The locale/phrasing matching is exactly the fragile part the spec flags ([nav_pipeline](nav_pipeline.md)). Duplicating it in
  two languages guarantees divergence; one C normalizer is a single source of truth.
- The C normalizer is host-testable (like `countdown.c`): pure function, no BLE/LVGL dependency,
  exercised under the native/host test harness, not only on the C3.

**Consequences:**

- The BLE payload carries a bounded raw title string (and optional numeric distance/remaining
  fields) rather than a pre-classified maneuver enum.
- The terminal pipeline is a clean chain: `packet decoder -> normalizer -> countdown -> renderer`.
  The iOS/ANCS path adds a raw-text input stage in front of the same decoder/normalizer; the
  decode/countdown/render stages are shared and unchanged.

## Firmware Runtime

*Was SPEC §8.*

Use ESP-IDF and FreeRTOS. Feature tasks compile out when their hardware provider is disabled.

| Task | Priority | Core | Responsibility | Phase 1 |
|---|---:|---:|---|---:|
| `gui_task` | 5 | profile-defined | LVGL rendering and display flush | yes |
| `ble_handler_task` | 4 | profile-defined | BLE peripheral/GATT-server connection, packet reception, parsing (see [ble_link](ble_link.md)) | yes |
| `countdown_task` | 4 | profile-defined | speed smoothing and distance interpolation | yes |
| `gnss_parser_task` | 3 | profile-defined | terminal NMEA input | no |
| `sensor_fusion_task` | 3 | profile-defined | IMU/motion/COG processing | no |
| `system_manager_task` | 2 | profile-defined | PMIC, buttons, watchdog, diagnostics | yes |

The GUI reads a `TerminalViewState`; it never calls BLE, UART, I2C, or GNSS APIs directly.

### Concurrency model (architectural decision)

*Was SPEC §8.1.*

**Decision:** two shared boundaries, each with exactly one writer.

- `raw_packet_queue` (bounded FreeRTOS queue): single-producer (`ble_handler_task`) / single-consumer
  (`countdown_task`) transfer of decoded `raw_notif_t`. `ble_handler_task` must never block on a full
  queue beyond a short bounded wait — BLE callbacks must not stall on countdown or render work.
- `shared_view` (`terminal_view_state_t`, guarded by `view_mutex`): single-writer (`countdown_task`
  only) / multi-reader (`gui_task`, and later `system_manager_task` diagnostics). Every reader other
  than `countdown_task` must go through a snapshot accessor (`view_state_snapshot()` on the C3) that
  copies the struct under `view_mutex`; no task may read `shared_view` fields directly without holding
  the mutex.

**Rationale:** `countdown.c`'s own internal state (`current`, `filtered_speed_kmh`, `has_baseline`) is
plain file-scope static state with no locking of its own. That is only safe because exactly one task
(`countdown_task`) ever calls `countdown_accept`/`countdown_estimate`/`view_state_tick`. If a second
task ever needs countdown state directly (rather than through the `shared_view` snapshot), it must
gain its own queue or mutex — do not call `countdown.c` from more than one task.

**Consequences:** this pattern is required for any future producer added on a second core ([hardware](hardware.md) below):
one queue or one mutex-guarded struct per boundary, exactly one designated writer, and all other
readers going through an explicit snapshot/copy function rather than direct field access.
