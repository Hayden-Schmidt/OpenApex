# MotoNav Technical Specification

**Status:** Active specification  
**Scope:** Phone navigation-source adapters and a BLE motorcycle terminal  
**First hardware:** Generic ESP32-C3 1.28-inch GC9A01 round display  
**Reference hardware:** Waveshare ESP32-S3-Touch-AMOLED-1.75 and GPS variant

## 1. System Boundary

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
§2.4 "Normalization ownership".

## 2. Phase 1: C3 Notification and GNSS Passthrough POC

Phase 1 is the first implementation target and is intentionally narrow.

### 2.1 Phase 1 does

- Targets the incoming generic ESP32-C3 1.28-inch GC9A01 round display.
- Proves BLE connectivity and live rendering on the C3 screen.
- Reads Google Maps, Apple Maps, and later Waze navigation notifications through phone-side
  platform adapters.
- Normalizes notification data into a common maneuver/distance/street model.
- Passes phone GNSS-derived speed, heading, fix state, and freshness data to the terminal.
- Performs local distance countdown processing on the C3 between notification updates.
- Renders maneuver icon, distance to maneuver, speed, connection state, and stale-data state.
- Supports touch and non-touch C3 board variants through compile-time configuration.

### 2.2 Phase 1 explicitly does not do

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

### 2.3 Platform source adapters

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

### 2.4 Normalization ownership (architectural decision)

**Decision:** normalization lives on the **ESP32 terminal**, not the phone. The phone is a dumb
relay that forwards raw notification fields and GNSS telemetry; it does no text parsing,
classification, or street extraction.

**Rationale:**

- iOS/ANCS delivers notification text directly to the terminal (the terminal is the BLE GATT
  client to the iPhone; there is no phone-side app to parse). C-side normalization is therefore
  unavoidable for the iOS path. Writing it once on the terminal and routing Android through it is
  the only way to get a single implementation instead of two (one Kotlin, one C) that would drift.
- The locale/phrasing matching is exactly the fragile part the spec flags (§3). Duplicating it in
  two languages guarantees divergence; one C normalizer is a single source of truth.
- The C normalizer is host-testable (like `countdown.c`): pure function, no BLE/LVGL dependency,
  exercised under the native/host test harness, not only on the C3.

**Consequences:**

- The BLE payload carries a bounded raw title string (and optional numeric distance/remaining
  fields) rather than a pre-classified maneuver enum.
- The terminal pipeline is a clean chain: `packet decoder -> normalizer -> countdown -> renderer`.
  The iOS/ANCS path adds a raw-text input stage in front of the same decoder/normalizer; the
  decode/countdown/render stages are shared and unchanged.

## 3. Notification Normalization

Notification text is inconsistent across applications and locales. Source adapters must normalize
it before transmission where practical; the C3 parser must remain small and deterministic.

The normalized model is:

```c
// Values are stable protocol values, not platform enum ordinals.
typedef enum {
    NAV_ICON_STRAIGHT = 0,
    NAV_ICON_TURN_LEFT,
    NAV_ICON_TURN_RIGHT,
    NAV_ICON_SLIGHT_LEFT,
    NAV_ICON_SLIGHT_RIGHT,
    NAV_ICON_SHARP_LEFT,
    NAV_ICON_SHARP_RIGHT,
    NAV_ICON_ROUNDABOUT,
    NAV_ICON_U_TURN,
    NAV_ICON_ARRIVED,
    NAV_ICON_UNKNOWN
} nav_icon_t;

typedef struct {
    nav_icon_t icon_type;
    char distance_str[16];
    char street_name[64];
    uint32_t distance_meters;
    uint16_t current_speed_kmh;
    uint16_t heading_degrees;
    uint8_t gnss_fix_valid;
    uint8_t phone_battery_percent;
    uint32_t notification_age_ms;
    uint32_t sequence;
} nav_payload_t;
```

Unknown numeric values use explicit sentinels. A missing field must never become a fabricated zero.
The source application and source platform should be included in the packet or diagnostics data.

The initial POC may use compact binary payloads. JSON is acceptable between a phone helper and an
intermediate service, but the terminal link should use a bounded binary representation for reliable
BLE notifications and simple C3 parsing.

## 4. C3 Countdown Processing

Notification distances update discretely. The terminal must continue showing a slowly decreasing
value between notification updates instead of freezing the last displayed number.

On every valid navigation update, the C3 stores:

- notification distance in metres;
- local monotonic timestamp;
- latest phone GNSS speed;
- speed validity and freshness;
- maneuver sequence number.

While the maneuver sequence is unchanged, it estimates:

```text
estimated_distance_m = max(
    0,
    notification_distance_m - integrated_speed_mps * elapsed_seconds
)
```

Requirements:

- Convert phone speed from km/h to m/s before integration.
- Smooth speed changes to avoid visible jumps.
- Apply a maximum elapsed-time hold window; do not count down forever on stale data.
- Clamp the estimate at zero.
- Replace the baseline when a new notification distance arrives.
- Reset the baseline when the maneuver or source navigation state changes.
- Mark the estimate stale when BLE, notification, or GNSS freshness expires.
- Never use a missing speed as zero motion without exposing the stale/unknown state.
- Do not claim this is dead reckoning or route navigation; it is display interpolation only.

The phone remains authoritative for the next maneuver and notification distance. The C3 only makes
the visible countdown smoother between authoritative updates.

## 5. BLE Architecture

The final architecture supports both platform paths while keeping the terminal firmware source-
agnostic.

### 5.1 Android path

The C3 terminal is the BLE peripheral: it advertises a custom GATT service containing a navigation
characteristic and accepts one bonded central. The Android helper is the BLE central/GATT client —
it associates with the terminal via `CompanionDeviceManager` (CDM) observer mode, connects, and
writes each normalized navigation packet into the characteristic (see §16.5 for the rationale).

This matches the role split used by essentially every consumer BLE accessory (fitness bands,
earbuds, Gadgetbridge/InfiniTime-class open trackers): the small, single-purpose, battery-constrained
device advertises and lets the phone do discovery, bonding, and reconnection management. It also
gives the "bike on -> terminal boots -> phone reconnects automatically" UX without opening the app:
CDM observer mode wakes a `CompanionDeviceService` from a killed process when the terminal's
advertisement is seen again, which starts the relay and connects.

It also leaves room for a later ESP32-to-ESP32 link (e.g. a handlebar terminal and a helmet or TPMS
sensor unit): with the terminal already acting as peripheral/GATT-server, a second ESP32 can attach
as another BLE central against the same or a second service, with no role renegotiation on the
terminal side.

### 5.2 iOS/ANCS path

For ANCS, the C3 acts as a BLE GATT client subscribing to the iPhone's notification source. The
terminal filters supported navigation applications and maps notification attributes into the same
normalized model. Phone GNSS telemetry may be supplied by a later iOS companion path or a platform-
specific telemetry characteristic. (This path keeps the terminal as central for ANCS specifically,
since ANCS requires the terminal to be the GATT client of the iPhone's notification service; it is
unrelated to the Android path's peripheral/central roles in §5.1.)

### 5.3 Project BLE identifiers

The Android implementation currently defines:

- Service UUID: `c9c6d0a0-0001-4f0a-9c8e-2f6b1a2d3e4f`
- Navigation characteristic UUID: `c9c6d0a0-0002-4f0a-9c8e-2f6b1a2d3e4f`

The navigation characteristic is `WRITE` / `WRITE_NO_RESPONSE`, encrypted (`BLE_GATT_CHR_F_WRITE_ENC`),
not `NOTIFY` — the phone (central) writes, the terminal (peripheral/GATT server) receives. No write
acknowledgement is required because a stale packet is always superseded by the next update rather
than retried.

The terminal must reject unsupported protocol versions and malformed lengths. BLE callbacks must
not block on rendering. Parsed packets are copied into a bounded queue or immutable state buffer.

### 5.4 Future polyline stream

Route context is Phase 2 or later. A future smartphone companion may downsample a 500 m to 1 km
route-ahead buffer into local `(delta_x, delta_y)` points and stream them in BLE chunks every 1-2
seconds. This is explicitly excluded from Phase 1.

## 6. Hardware Profiles

Firmware uses compile-time profiles. Board-specific pins and drivers must not leak into the
navigation model or packet decoder. Every profile declares its SoC, display, touch, GNSS, IMU, RTC,
PMIC, buttons, memory, and power capabilities.

### 6.1 Phase 1 prototype: `PROTOTYPE_C3_GC9A01` generic C3 round display

Target listing: AliExpress item `1005006194839720`.

- ESP32-C3, single-core RISC-V target. Seller descriptions calling it dual-core are incorrect.
- 1.28-inch round 240x240 GC9A01 IPS LCD.
- Touch and non-touch product variants.
- No onboard GNSS or IMU assumed.
- Phone GNSS passthrough is therefore the only Phase 1 telemetry source.

Buyer-reported touch-variant pins are provisional until checked against the delivered board:

| Function | Reported pin |
|---|---:|
| GC9A01 SCLK | GPIO6 |
| GC9A01 MOSI | GPIO7 |
| GC9A01 DC | GPIO2 |
| GC9A01 CS | GPIO10 |
| GC9A01 reset | board-dependent / not confirmed |
| LCD backlight | GPIO3 |
| CST816/CST816D SDA | GPIO4 |
| CST816/CST816D SCL | GPIO5 |
| CST816/CST816D interrupt | GPIO0 |
| CST816/CST816D reset | GPIO1 |

The touch driver must compile out for the non-touch variant. GPIO maps must remain configurable.
Brownout behavior, reset behavior, backlight control, and actual display orientation are Phase 1
bring-up checks.

### 6.2 Single-core expansion targets

The firmware architecture must also support other single-core ESP32 families, including ESP32-C6,
through separate profiles such as `GENERIC_C6_GC9A01`. C3/C6 support is not allowed to assume dual-
core scheduling or S3-only peripherals. Unsupported peripherals must compile out cleanly.

### 6.3 Reference production target: Waveshare 1.75 AMOLED

Target board: `ESP32-S3-Touch-AMOLED-1.75` and `ESP32-S3-Touch-AMOLED-1.75-G` GPS variant.

- ESP32-S3R8, Xtensa LX7 dual-core, up to 240 MHz.
- 8 MB stacked PSRAM and 16 MB external NOR flash.
- 1.75-inch circular AMOLED, 466x466, approximately 700 cd/m2.
- CO5300 display driver over QSPI.
- CST9217 capacitive touch over I2C.
- QMI8658 6-axis IMU over I2C.
- PCF85063 RTC with backup power path.
- AXP2101 power-management IC.
- TF/microSD slot for later logging or data features.
- GPS variant includes onboard Quectel LC76G and IPEX1 antenna connection.

The GPS variant is not required for Phase 1. Its GNSS driver belongs to the later telemetry phase.

## 7. Optional GNSS and Sensor Hardware

### 7.1 Terminal GNSS

Some profiles may enable `GNSS_LC76G_UART` or `GNSS_EXTERNAL_NMEA_UART`. The LC76G supports
GPS, GLONASS, Galileo, and BeiDou reception. Terminal GNSS is supplemental telemetry and later
verification input; it is not the Phase 1 navigation authority.

### 7.2 Heading

QMI8658 is a 6-axis accelerometer/gyroscope and is not an absolute compass. Absolute heading is
provided by phone magnetometer data or GNSS course-over-ground while moving above approximately
3 km/h. Below that threshold, heading is unknown or held with an explicit stale indicator.

### 7.3 Dormant audio and peripherals

The 1.75 board contains ES7210/ES8311-class audio hardware, microphones, echo-cancellation
circuitry, and speaker connections. Audio is permanently disabled in firmware configuration. It is
not initialized, scheduled, rendered, or used for navigation.

## 8. Firmware Runtime

Use ESP-IDF and FreeRTOS. Feature tasks compile out when their hardware provider is disabled.

| Task | Priority | Core | Responsibility | Phase 1 |
|---|---:|---:|---|---:|
| `gui_task` | 5 | profile-defined | LVGL rendering and display flush | yes |
| `ble_handler_task` | 4 | profile-defined | BLE peripheral/GATT-server connection, packet reception, parsing (see §5.1) | yes |
| `countdown_task` | 4 | profile-defined | speed smoothing and distance interpolation | yes |
| `gnss_parser_task` | 3 | profile-defined | terminal NMEA input | no |
| `sensor_fusion_task` | 3 | profile-defined | IMU/motion/COG processing | no |
| `system_manager_task` | 2 | profile-defined | PMIC, buttons, watchdog, diagnostics | yes |

The GUI reads a `TerminalViewState`; it never calls BLE, UART, I2C, or GNSS APIs directly.

### 8.1 Concurrency model (architectural decision)

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

**Consequences:** this pattern is required for any future producer added on a second core (§10 below):
one queue or one mutex-guarded struct per boundary, exactly one designated writer, and all other
readers going through an explicit snapshot/copy function rather than direct field access.

## 9. Display Behavior

The Phase 1 C3 renderer is a high-contrast dial:

- Idle/waiting: connection state and no fabricated navigation values.
- Active: large maneuver icon, countdown distance, speed, and optional street text.
- Close turn: visual countdown emphasis as distance approaches zero.
- Stale: last-known maneuver de-emphasized with a stale-link or stale-GNSS indicator.
- Arrived: completion glyph when the source notification indicates arrival.
- Diagnostics: source platform/application, packet age, GNSS validity, speed, heading, and profile.

The C3 UI should use partial redraws and bounded memory. The 1.75 AMOLED may use larger type,
double buffering in PSRAM, and later road-context graphics, but it must share the same normalized
navigation model.

## 10. Power and Mechanical Design

```text
switched motorcycle 12 V -> 2 A fuse -> external waterproof 12-to-5 V buck
                         -> weather-sealed handlebar pogo dock -> terminal
```

- No buck converter or voltage transformation occurs inside the sealed terminal.
- Power is coupled to switched ignition; key-off removes dock power.
- Use a 2-pin or 4-pin recessed spring-loaded pogo interface. Mechanical bayonet or twist-lock
  retention carries wind and vibration loads; pogo contacts carry electrical current only.
- Use a dummy cap or low-side MOSFET arrangement to prevent wet exposed contacts shorting.
- The terminal has no internal LiPo requirement. The 3.7 V battery header on some development
  boards is not part of the motorcycle terminal assembly.
- Immediate power loss must be safe. Active navigation uses RAM state and does not require a
  shutdown sequence or filesystem transaction.
- ASA or polycarbonate is preferred for the shell. A metal bezel requires an RF-transparent window
  over the ESP32 antenna region.
- A 10 mm sun hood may reduce overhead glare but must not block the display, antenna, touch surface,
  or dock retention.

## 11. Development Roadmap

### Current status (2026-09-21)

BLE roles have been flipped (see §16.5): the C3 terminal is now the BLE peripheral/GATT server, and
the Android phone is the central/client using `CompanionDeviceManager` (CDM) observer mode. This
replaces the previous central/peripheral assignment and the disconnect-churn issue described below
is being re-tested under the new roles (§13 item 8).

Confirmed working end-to-end on real hardware (bare ESP32-C3 dev board on COM5, no display module
yet, plus a OnePlus 15 running the Android relay app):

- NimBLE peripheral (`firmware/main/ble_link.c`) advertises the OpenApex nav service, accepts one
  bonded central, and decodes incoming `RawNotifPacket` writes. Builds and boots cleanly (RAM 4.8%,
  Flash 45.6%); serial log confirms clean advertising with no crashes.
- Android central (`RelayBleClient.kt`, `OpenApexCompanionService.kt`, `NavNotificationRelayService.kt`,
  `RelayService.kt`) associates with the terminal via CDM, registers presence observation
  (`CompanionDeviceManager.startObservingDevicePresence`, which is required in addition to
  `associate()` and needs the `REQUEST_OBSERVE_COMPANION_DEVICE_PRESENCE` manifest permission — see
  §16.5), and has been observed end-to-end to fire `onDeviceAppeared` and start `RelayService` as a
  foreground service from a killed app process on a real device reboot cycle.
- `firmware/sim_lvgl` (PlatformIO native + SDL2) added as the LVGL GUI design/iteration surface:
  opens a real SDL window at the exact `board_profile.h` resolution (240x240) and renders the
  shared `firmware/gui/gui_screens.c` widget code, driven by a scripted fixture sequence in the
  absence of live hardware/data. Screen-building code lives in `firmware/gui/`, not
  `firmware/main/`, so it never gets pulled into the real ESP-IDF `prototype_c3` build.
- `pio run -e native`, `pio run -d firmware -e prototype_c3`, and the new `sim_lvgl` env all build
  clean.

**Open items:** the full GATT connect/write path from the Android central to the C3 peripheral
(RelayBleClient discovering the service and writing a live packet) has not yet been observed in a
single continuous session — see §13 items 8 and 9. The previous ~9-30s disconnect-churn issue (NimBLE
reason 531) was observed under the old central/peripheral assignment and needs re-testing now that
the flaky peripheral role has moved off the OEM phone Bluetooth stack.

### Recommended next steps

1. Confirm the full GATT connect/write path (phone central to C3 peripheral) with live Google Maps
   navigation data, and re-test the disconnect-churn scenario under the new roles (§13 items 8, 9).
2. Wire the LVGL screens already prototyped in `firmware/sim_lvgl` into the real `gui_task` on the
   C3 firmware, driven by the now-working `ble_link`/`countdown` pipeline instead of the simulator's
   scripted fixture.
3. Source and wire the physical GC9A01 display module once available; port the SPI display driver
   (deferred — no module in hand as of this status update).
4. Implement the iOS/ANCS source adapter (Android-only so far).
5. Add phone motion/GNSS telemetry (fused location + fused orientation + raw accel/gyro) per the
   Phase 1 roadmap below — not yet started.

### Phase 1: C3 notification/GNSS display POC

- [x] Confirm the C3 board variant, pins, touch option, reset, backlight, and brownout behavior
      (bare dev board confirmed; display module not yet in hand).
- [x] Create ESP-IDF project structure and compile-time C3 profile.
- [x] Disable unused audio and unrelated silicon where present.
- [x] Implement BLE reception and normalized packet decoding.
- [x] Implement Android NotificationListenerService source adapter for Google Maps.
- [ ] Implement the iOS/ANCS source contract and adapter path for Apple Maps.
- [ ] Pass phone GNSS speed, heading, validity, and freshness through the packet.
- [x] Implement C3-side speed smoothing and distance countdown between notifications.
- [ ] Implement the basic LVGL maneuver/distance/speed/stale UI on-device (prototyped in
      `firmware/sim_lvgl`, not yet wired into `firmware/main/main.c`'s `gui_task`).
- [ ] Build the first weather-resistant mechanical prototype only after the display path works.

**Phase 1 exit test:** Google Maps navigation notification plus phone GNSS data reaches the C3,
the screen renders the maneuver and distance, the distance counts down smoothly between source
updates, stale/disconnect states are visible, and the board recovers from power removal. No map,
tile, route, polyline, local navigation, or terminal GNSS dependency is permitted.

### Phase 2: advanced telemetry and production reference board

- Add the Waveshare 1.75 AMOLED S3 profile and CO5300/CST9217 drivers.
- Add LC76G UART support on the 1.75-GPS variant.
- Add QMI8658 motion wake, tilt, and optional COG smoothing.
- Add PCF85063 RTC and AXP2101 power controls where needed.
- Add optional phone/terminal telemetry comparison and diagnostics.
- Add dock power testing and weather/UV/vibration enclosure testing.

### Phase 3: road context

Only after Phase 1 display reliability and Phase 2 hardware support are complete:

- stream downsampled vector route coordinates from the smartphone;
- draw a heading-relative road-context line on the terminal;
- negotiate/version polyline chunks separately from the core notification packet.

Full offline maps remain out of scope for the terminal. Offline routing and map selection, if used,
remain smartphone-side features.

## 12. Acceptance Criteria

- The C3 prototype builds with touch enabled or disabled without changing navigation logic.
- Android Google Maps notification data can drive the C3 maneuver display.
- Apple Maps ANCS data maps to the same normalized maneuver model when the iOS path is enabled.
- Phone GNSS speed and heading arrive with validity and freshness information.
- The C3 countdown continues between notifications without pretending stale data is fresh.
- Unsupported, malformed, or stale packets cannot produce a false zero-distance maneuver.
- No map, route, tile, polyline, or local navigation code is required for Phase 1.
- A BLE disconnect, phone GNSS loss, or abrupt power cut does not crash the display.
- C3, C6, and S3 profiles do not assume the same core count, peripherals, memory, or pins.
- The production reference target keeps audio disabled and preserves RF antenna clearance.

## 13. Open Decisions

1. Confirm the exact C3 board revision and delivered touch/non-touch pinout.
2. Confirm Android notification formats and localization behavior for Google Maps, Apple Maps, and
   Waze test fixtures.
3. Confirm the iOS ANCS subscription and notification-attribute flow on the intended iPhone.
4. Finalize packet version 1 byte layout, sentinels, and whether source identifiers fit in the first
   packet or use a second characteristic.
5. ~~Choose the ESP-IDF version and LVGL major version for the firmware project.~~ **RESOLVED**
   (see §16.2): ESP-IDF 5.x + LVGL 9.
6. Confirm dock pin count, wake/ID pin requirements, and external buck converter packaging.
7. Decide whether the embedded project remains under `firmware/` in this repository.
8. Root-cause the ~9-30s BLE central/peripheral disconnect churn (NimBLE reason 531) previously
   observed between the C3 and an Android peripheral (tested on a OnePlus 15), before roles were
   flipped in §16.5. A `ble_gap_update_params()` call from the central made disconnects happen
   faster and was reverted; suspected OEM (OnePlus/ColorOS) peripheral-stack issue. **Needs
   re-testing** now that the C3 is the peripheral and the phone is the central — the disconnect
   source may no longer apply, since the flaky peripheral role moved off the OEM phone stack.
9. Confirm the `CompanionDeviceManager` `BluetoothLeDeviceFilter`/`ScanFilter` match remains
   reliable across OEM ROMs beyond the OnePlus 15 test device — CDM presence detection (`
   onDeviceAppeared`) is OEM/ROM dependent and was observed to be sensitive to repeated rapid
   app-process kill/restart cycles during manual testing (see §16.5); real-world behavior is a
   single boot-time transition, not rapid cycling, so this is expected to be a testing-only
   artifact but has not been fully confirmed.

## 16. Architecture Decision Records

Consolidated decisions that shape the implementation. These are authoritative; where they revise
an earlier section, they supersede it.

### 16.1 Normalization on the terminal (see §2.4)

The phone is a dumb relay; the ESP32 normalizes notification text into the maneuver model. One C
normalizer serves both Android and iOS/ANCS.

### 16.2 Firmware language split

**Decision:** hybrid C/C++ with a fixed boundary.

- **C** — hardware drivers, protocol (BLE GATT, packet decoder), and pure domain logic
  (`normalize.c`, `countdown.c`, `pipeline.c`). No `extern "C"` wrapper fatigue; straight ESP-IDF
  APIs.
- **C++** — LVGL UI screens (`NavScreen`, `IdleScreen`, etc.) and view-state encapsulation. RAII
  for mutex guards.
- **Disciplined constraints:** `-fno-exceptions`, `-fno-rtti`, no heap-heavy STL inside the render
  loop; `std::array` and fixed pools only. (These are ESP-IDF defaults; do not add conflicting
  `build_flags`.)
- **Boundary:** POD C structs (`nav_payload_t`, `terminal_view_state_t`) cross the C→C++ seam.

The pure C modules (`packet`, `normalize`, `countdown`, `pipeline`) are host-testable under the
native/MSVC test harness independent of ESP-IDF.

### 16.3 Toolchain

- PlatformIO (`espressif32` platform) drives the ESP-IDF CMake build; no custom toolchain.
- Framework: **ESP-IDF 5.x** (required by LVGL 9), **LVGL 9**.
- BLE: **NimBLE** (lightweight, correct fit for single-core C3).

### 16.4 Hardware-free development path

The browser simulator (`simulator/index.html`) and PlatformIO `native` target share the same
`countdown.c`/`normalize.c` sources. Browser = visual target; `native` + host tests = code target.

### 16.5 BLE role flip: terminal as peripheral/GATT server, phone as central/client

**Decision:** the ESP32 terminal advertises and hosts the GATT server (peripheral); the Android
phone connects, bonds, and writes navigation packets (central/client). This reverses the original
Phase 1 assumption in §5.1.

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
  connecting to the same terminal peripheral without any role renegotiation (see §5.1).
- Validated end-to-end on real hardware (C3 + OnePlus 15): CDM association approved, presence
  observed, `OpenApexCompanionService.onDeviceAppeared` fired, `RelayService` started as a
  foreground service. Full GATT connect/write path from the phone to the C3 and item 8's disconnect-
  churn re-test are tracked as open items (§13.8, §13.9).

## 14. Source and Licensing Notes

- MotoNav application and firmware code are intended to remain under the repository's chosen open
  source license.
- The terminal renders data received from supported smartphone navigation applications; it does not
  reproduce their maps or perform local map extraction in Phase 1.
- OpenStreetMap attribution is required for any OSM-backed phone-side service used later.
- Platform notification APIs, ANCS, and navigation-application terms must be reviewed before public
  distribution. The Phase 1 notification path is a technical proof of concept and must not assume
  that every application permits redistribution or persistent storage of its notification data.

## 15. Existing Code Porting References

This section maps the Phase 1 work onto code preserved in the historical
[MotoNav repository](https://github.com/Hayden-Schmidt/MotoNav). File links are references for
implementation, not claims that the old code already satisfies this specification.

### 15.1 BLE transport and packet precedent

- [BleLink.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ride/BleLink.kt) contains the existing Android
  GATT server, service/characteristic UUIDs, notification subscription handling, advertising, and
  packet publication lifecycle. Reuse its permission checks and lifecycle shape where the Android
  helper remains the GATT server.
- [RideStatePacket.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ride/RideStatePacket.kt) contains the
  existing versioned 12-byte little-endian packet packer, explicit unknown sentinels, clamping, and
  byte-layout documentation. Adapt the model for normalized notification fields and GNSS freshness;
  do not silently change the existing packet meaning.
- [RideStatePacketTest.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/test/java/com/motonav/app/ride/RideStatePacketTest.kt) is the
  pattern for pure JVM wire-format tests. Add fixtures for Google Maps, Apple Maps/ANCS, idle,
  arrived, stale GNSS, malformed packets, and countdown baselines.
- [RideSessionService.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ride/RideSessionService.kt) owns the
  existing long-lived foreground-service lifecycle and currently starts BLE, sensors, and packet
  publication. Reuse its lifecycle/wake-lock lessons, but do not copy its Ferrostar route ownership
  into the Phase 1 notification adapter.

### 15.2 Phone GNSS and heading

- [RideSessionService.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ride/RideSessionService.kt) already
  requests fused location updates, converts location speed from m/s to km/h, and combines sensor
  values before BLE publication. This is the starting point for the phone GNSS passthrough path.
- [RideSensorsStateHolder.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/location/RideSensorsStateHolder.kt)
  is the existing small shared holder for nullable speed and heading. Extend the data contract with
  fix validity and freshness rather than fabricating zeros.
- [CompassTracker.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/location/CompassTracker.kt) contains
  phone rotation-vector heading, magnetic-declination correction, smoothing, and GPS-bearing fallback.
  Reuse the heading math for phone telemetry; the terminal QMI8658 is not an equivalent absolute
  compass.
- [CompassMath.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/location/CompassMath.kt) and its tests are
  the pure-math reference for wrap-safe angle smoothing.

### 15.3 Notification scraping gap

There is currently **no active NotificationListenerService or ANCS implementation** in the main
source tree. The earlier notification-era architecture was removed or archived during the OSM
rebuild. A developer implementing Phase 1 must add a new source-adapter boundary rather than
assuming a parser already exists.

Recommended ownership:

- Android `NotificationListenerService`: phone-side source adapter for Google Maps, Apple Maps,
  Waze, and later OsmAnd.
- iOS ANCS: terminal-side BLE client path or a future iOS companion adapter, depending on the
  chosen phone/terminal division.
- Shared normalization: pure parser/model code with fixture tests, independent of Android UI and
  ESP-IDF headers.
- BLE publisher: reuse the existing `BleLink` lifecycle, but publish normalized notification plus
  GNSS telemetry rather than Ferrostar-only `RideState`.

Do not revive the archived notification implementation without checking its assumptions. It was
notification-era exploratory code, not a verified production parser or a current Phase 1 contract.

### 15.4 C3 terminal countdown and renderer

There is no ESP32 firmware in the repository yet. The following phone UI code is still useful for
the embedded renderer contract:

- [NavDial.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ui/dial/NavDial.kt) defines the existing idle,
  active, rerouting, arrived, speed, and stale-display concepts to port as behavior, not as Compose
  code.
- [DialLayout.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ui/dial/DialLayout.kt) stores fractional
  layout constants suitable for transcription into C/LVGL profile-independent constants.
- [NavDialConfig.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ui/dial/NavDialConfig.kt) shows the
  existing display-toggle model for speed and other elements.
- [RouteLine.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ui/dial/RouteLine.kt) and
  [RouteGeometry.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/nav/RouteGeometry.kt) are **not Phase 1
  dependencies**. They belong to the later vector road-context phase and must not pull route
  geometry into the C3 POC.

The new firmware should add a pure host-testable countdown module. Its inputs are normalized
notification distance, phone speed, packet age, maneuver sequence, and local monotonic time. Its
output is estimated distance plus freshness/state flags. Keep this math independent of LVGL and BLE.

### 15.5 Offline maps and routing: existing code, explicitly later

Offline functionality exists in the Android app, but none of it belongs in Phase 1 or the C3:

- [OfflineMapTiles.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/map/OfflineMapTiles.kt) implements the
  phone-side MapLibre `pmtiles://file://` loading path for the route-selection map.
- [RouteSelectionMapScreen.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/map/RouteSelectionMapScreen.kt)
  is the phone-side route-selection map UI, not a terminal navigation surface.
- [OfflineTileDownloader.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ride/OfflineTileDownloader.kt)
  downloads the phone-side Valhalla tile tarball using a temporary file and atomic rename.
- [LocalRouteProvider.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ride/LocalRouteProvider.kt) is the
  phone-side local Valhalla bridge. It is not firmware code and must not be ported to the C3.
- [OfflineMapTilesTest.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/test/java/com/motonav/app/map/OfflineMapTilesTest.kt) is the
  reference for testing the phone-side PMTiles path.

These files are references for later smartphone offline work only. Phase 1 must not import their
dependencies, data formats, tile files, or route geometry into the embedded project.

### 15.6 Navigation models and later migration

- [Maneuver.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/nav/Maneuver.kt) contains the current maneuver
  bucketing concept. Reuse the stable bucket meanings when mapping notification text, but do not
  rely on Kotlin enum ordinals as the long-term wire contract.
- [Geocoding.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/nav/Geocoding.kt) is phone-side destination
  search logic and is out of Phase 1.
- [Elevation.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/nav/Elevation.kt) is phone-side Valhalla
  elevation logic and is out of Phase 1.
- [RouterBackend.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/ride/RouterBackend.kt) and
  [SettingsStore.kt](https://github.com/Hayden-Schmidt/MotoNav/blob/main/app/src/main/java/com/motonav/app/settings/SettingsStore.kt) describe
  configurable phone routing providers. They are later phone-app references, not C3 requirements.

### 15.7 Porting rule

When implementing Phase 1, the dependency direction must remain:

```text
phone source adapter -> normalized notification/GNSS model -> BLE packet
                                                        -> C3 decoder
                                                        -> C3 countdown processor
                                                        -> LVGL renderer
```

Routing, maps, geocoding, PMTiles, Valhalla, Ferrostar, and route geometry must remain outside this
path. A future developer should be able to build and test the C3 POC without downloading map data
or understanding the phone's later offline-routing implementation.
