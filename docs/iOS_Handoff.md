# iOS + multiplatform work: handoff

Branch `feat/ios-ancs`, 21 commits ahead of `main`. Last updated 2026-09-29.

Read this first if you are picking the work up. It says what exists, what is proven, what is
merely written, and what will bite you. Its companion `iOS_Multiplatform_Status.md` is the
per-phase record; this one is the orientation.

---

## 1. The shape of the thing, and why

OpenApex is an ESP32 motorcycle terminal plus a phone app. Android scrapes Google Maps
notifications, adds GNSS/IMU telemetry, and writes a fixed 210-byte packet to the terminal over a
custom GATT service.

**iOS breaks three assumptions at once**: no notification listener, no call observation, no
persistent background service. So the roles invert. On iOS the **terminal** is the GATT client and
reads ANCS (notifications) and CTS (time) off the phone directly; the app is reduced to the
telemetry half. It is not optional — nothing else can supply speed, position or battery.

That inversion is why `normalize.c` lives on the terminal (SPEC §2.4): **one C normalizer serves
both the Android packet path and the iOS/ANCS path.** Write nav text rules anywhere else and you
will have two of them, in two languages, drifting.

Consequences to design around, not to fix:

- ANCS carries **text only**: `AppIdentifier`, `Title`, `Subtitle`, `Message`, `Date`. No maneuver
  glyph, so a roundabout's exit direction — which on Android only the icon supplies — is unknown.
  No progress, no traffic segments, no telemetry.
- **Apple Maps posts no notifications** and cannot be a nav source. Google Maps posts under
  `CategoryIDLocation`. Waze is unknown.
- When iOS kills the app the terminal goes telemetry-blind while ANCS nav and the CTS clock keep
  working. Surface that gap to the rider; do not paper over it.

---

## 2. Where the code is

### Firmware (`firmware/main/`) — Phase A, done

| File | What it is |
|---|---|
| `apple_client.c/.h` | GATT-client role on the phone's own connection: discovery, Service Changed |
| `ancs_core.c/.h` | Notification Source parsing, Control Point requests, Data Source reassembly, UID table |
| `ancs_nav.c/.h` | ANCS notification → `raw_notif_t`, then into `normalize.c` **unchanged** |
| `cts_client.c/.h` | Current Time Service, feeding the same clock sink as `pipeline.c` |
| `packet_merge.c/.h` | Two-group merge: ANCS nav + app telemetry, per-group freshness |
| `ancs_bridge.c/.h` | Glue; `#if BOARD_HAS_ANCS` with no-op stubs, the `touch_driver.c` pattern |

Packet **v6** (`packet.h`) is the same 210 bytes with two new flag bits: `nav_fields_absent`
(0x02) and `source_is_ios` (0x04). The decoder accepts versions 5 **and** 6 — the first time the
strict-equality version check has needed a window.

### Shared Kotlin (`phone/shared/src/commonMain/`) — Phase B, done

```
protocol/   RawNotifPacket  CallPacket  SettingsTlv  RiderMesh  Fragmentation
ble/        TerminalLink  DeviceId  TerminalCharacteristic  ControlQueue  ControlOp
mesh/       RiderMeshSync  MeshTransport
store/      SecureStore  RiderMeshStore  RiderStoreCore
telemetry/  TelemetryAggregator
```

Targets are `androidTarget` + `jvm` only. **iOS targets are deliberately not declared yet** — they
need a Mac. `jvm` exists purely so `commonMain` is genuinely compiled and tested on Windows.

### Android (`phone/androidApp/`)

`RelayBleClient` implements `TerminalLink` and is now GATT calls plus the one-operation-in-flight
rule, nothing else. `RelayService` owns GNSS, sensors, the foreground service and feeds
`TelemetryAggregator`. `NavNotificationRelayService` and `CallRelay` stay Android-only in full;
their iOS counterparts are empty by design, because ANCS owns both.

---

## 3. How to verify anything

```powershell
# Firmware host suites (9). No hardware needed. This is the main gate.
powershell -NoProfile -File scripts/build-countdown.ps1

# Shared + Android
cd phone; .\gradlew.bat :shared:jvmTest :androidApp:assembleDebug     # 61 Kotlin tests

# Firmware build / flash
pio run -d firmware -e prototype_c3
pio run -d firmware -e prototype_c3 -t upload --upload-port COM30
```

**The cross-language check is the one to know about.** `firmware/test_host/packet_kotlin_fixture_test.c`
decodes packets the *Kotlin* packer actually produced, using the *firmware's* `packet.c`. One wire
format, two implementations, in two languages — and nothing compared them before; the Kotlin tests
only ever agreed with Kotlin. Regenerate the arrays with:

```
cd phone; .\gradlew.bat :shared:jvmTest --tests '*GoldenFixtureDump*' --rerun-tasks
```

and paste the printed arrays. Do that **only when the layout changes on purpose**. An unintended
diff there is precisely the failure the file exists to cause.

---

## 4. What is actually proven, and what is not

Verified on the C3 with an Android phone: ANCS protocol core, the mapping into `normalize.c`, the
GATT-client role, CTS, Service Changed rediscovery, solicitation advertising, packet v6, the merge,
`BOARD_HAS_ANCS` (built both ways: 64.9% flash on, 64.4% off), and the whole KMP restructure — full
mesh sync and gapless packet decode after every step.

**Not verified, and not claimed:**

- **The single-write fast path has never run.** `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`, so this PC
  cannot take the connection slot while the phone holds it. The length discrimination is sound by
  construction (a fragment is ≤20 bytes, a whole packet is 210) but no writer has sent one. First
  real test is the iPhone.
- **The 31.6 s dead link is still open.** A connected link with zero decoded packets for 31.6 s
  (885 s in one session), cleared only by a reconnect. Never reproduced on the current build. The
  `rc=6` fixed alongside it was this branch's own log noise, not that symptom's cause.
- **Dual-role heap headroom under sustained load is unmeasured.** Phone link + mesh Coded-PHY
  advert and scan + an active GATT-client session have not been run together under load. **The C3
  is the risk; the S3 has headroom.** `BOARD_HAS_ANCS` exists so this can be gated per board.
- **`tools/decode_ancs.py` has never been run against a real capture.** It exists; nothing has
  fed it.

---

## 5. Traps

These cost time. They are all still live.

- **`publish()` is driven only by fused location updates.** With no GNSS fix — i.e. indoors — the
  packet stream stops entirely and looks exactly like a regression. It is not a smoke test unless
  the phone can see sky. Use the terminal's `decoded packet` serial lines as the witness.
- **Opening COM30 at default settings reboots the ESP32** (DTR/RTS). Clear `dtr` and `rts` before
  `open()` or you will reset the thing you are measuring and then misread the result.
- **`adb install -r` returns before the package replace completes**, which then kills the app you
  just launched. Force-stop and relaunch *after* the replace, or you will read an empty log.
- **File encodings are not uniform and edits fail silently on the wrong one.** `ble_link.c` and
  `main.c` are **cp1252**; `apple_client.*`, `ancs_*` and `packet.h` are UTF-8; `docs/*.md` and
  `OpenApex_SPEC.md` are **CRLF**. Detect the line ending before patching.
- **`adb shell pm grant` fails on OxygenOS** — the shell is not given `GRANT_RUNTIME_PERMISSIONS`.
  Open `android.settings.APPLICATION_DETAILS_SETTINGS` and grant by hand.
- **The notification-listener grant is stored fully qualified.** The package rename would have
  silently stopped nav scraping with no error anywhere. If you rename again:
  `adb shell cmd notification allow_listener org.openapex.androidauto/org.openapex.app.NavNotificationRelayService`.
- **`applicationId` stays `org.openapex.androidauto`** even though the package is `org.openapex.app`.
  Changing it forces an uninstall, which costs the test phone its pairing, settings and rider
  identity. Relative class names in `am` commands expand against the applicationId — always fully
  qualify.
- **A fresh install crash-loops.** Pre-existing: `RelayService.onCreate` starts a `connectedDevice`
  foreground service before `BLUETOOTH_CONNECT` is granted. Survivable for a developer granting
  permissions out of band; wrong for anyone else. **Not fixed.**

---

## 6. Design decisions you should not relitigate without reading why

- **The ESP32 is the ANCS client**, not the app. iOS cannot relay notifications; this is forced.
- **No third-party KMP BLE library** (Kable, blue-falcon). iOS background reliability hinges on
  `CBCentralManager` state restoration and we need full control. See `Phone_App_Architecture.md §4`.
- **Randomness is injected into `RiderStoreCore`**, not taken from the platform. It avoids an
  expect/actual whose two actuals would be the same line, and it lets a test pin the bytes — the
  only way to assert anything about a function whose whole job is to be unpredictable.
- **`DeviceId` is opaque.** Android's value is the MAC already in SharedPreferences, so there was
  nothing to migrate. iOS gives `CBPeripheral.identifier`, which is per-install and differs between
  two phones looking at one terminal. Never parse it, compare it across platforms, or display it.
- **Sentinel discipline** (SPEC §3): *a missing field must never become a fabricated zero.* This is
  why the stale-fix test asserts the bytes equal a no-fix packet rather than merely differing.
- **Everything in the shared layer is callback-shaped, not suspending**, because the Android app
  has **no coroutines at all** — it is main-looper `Handler`s throughout. See §7.

---

## 7. What is left

### Phase C — iOS platform layer. Not started.

Written-not-compiled by design until a Mac exists. Write against the Phase B interfaces so that
"get a Mac" is a build-and-fix task, not a design task.

- `IosTerminalLink : TerminalLink` — `CBCentralManager` with `CBCentralManagerOptionRestoreIdentifierKey`,
  `willRestoreState`, `retrievePeripherals(withIdentifiers:)`. iOS has no explicit bond API:
  encryption is established when the terminal demands it, so handle first-write-fails-then-pairs.
  No `refresh()` equivalent — handle **Service Changed** instead.
- **Both pairing paths.** `ASDiscoverySession` (AccessorySetupKit, iOS 18) *and* plain
  CoreBluetooth relying on ANCS solicitation plus system pairing.
- `IosTelemetrySource` — `CLLocationManager` at `kCLLocationAccuracyBestForNavigation`,
  `allowsBackgroundLocationUpdates`, `CMDeviceMotion`, `UIDevice.batteryLevel`. Feeds
  `TelemetryAggregator` with `telemetryOnly = true`.
- `IosSecureStore : SecureStore` — Keychain. Two methods.
- `Info.plist` — `UIBackgroundModes: [bluetooth-central, location]`.
- `iosApp` — thin SwiftUI host wrapping `ComposeUIViewController`.

**The one question no amount of code answers: whether an AccessorySetupKit bond is system-level
enough for iOS to publish ANCS.** ASK pairs "without the usual OS pairing prompt" and its bonds are
app-scoped. Hence both paths — it makes this a five-minute test on day one instead of a rebuild.

### The coroutine conversion

Deliberately deferred and deliberately not smuggled into a module move. Turning the queue pump —
single-op-in-flight, control > call > nav priority — into a `Channel` is the biggest single refactor
left. `TerminalLink` and `MeshTransport` both change shape when it happens; that is expected, and
it is why they are small. Give it its own step with tests around it.

### Phase A leftovers

Settings TLV nav-source tag (auto / force-app / force-ANCS, wanted for day-one A/B) · `ancs_sim.c`
driving `sim_lvgl` · `replay.c` ANCS tier · the OTA service spec (`Phone_App_Architecture.md §6`
has a frame sketch, no UUID, no byte layout).

### Phase D — day one with the iPhone

Ordered highest-information first, because each step is minutes: **pair** (ASK, then plain
CoreBluetooth; confirm via the `apple` console command that ANCS and CTS were discovered — this is
the highest-risk moment in the project) → **log everything, filter nothing**
(`ancs_nav_set_permissive()` exists for this) → Google Maps on a short route, then Apple Maps
(expect silence, confirm it), then Waze → **tune text rules against the capture on the bench** →
longer drive for maneuver coverage.

Answer these, which the plan could not: does Google Maps *modify* one persistent notification per
turn or post new UIDs, and how often? Is distance in the text, in a format `parse_distance_metres()`
already handles? Does `ActionIDPositive` actually answer a call?

**The branch point:** if Google Maps posts only sparse, non-updating notifications, iOS
turn-by-turn via ANCS is not viable and the fallback is an in-app navigation engine as the source
of truth on both platforms. Phases A–C survive that — ANCS still carries calls, CTS still carries
time, and the KMP telemetry stack is what a nav engine plugs into.

---

## 8. Environment

- ESP32-C3 on **COM30**, BT MAC `90:70:69:c0:ea:de`. Clean firmware build ~12 min, incremental ~35 s.
- Test phone: OnePlus (OxygenOS), `adb` at `%LOCALAPPDATA%\Android\Sdk\platform-tools`.
- JDK 21, Gradle 8.13 via the wrapper in `phone/`, AGP 8.13.2, Kotlin 2.2.20, ESP-IDF 6.1.0.
- Minimum iOS 18.
