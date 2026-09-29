# 03 — iOS + Android multiplatform (KMP)

**Status:** IN PROGRESS  
**Last reconciled with code:** not yet (the source docs were updated 2026-09-29)  
**Reference docs:** [ios_ancs](../reference/ios_ancs.md), [Phone_App_Architecture](../reference/Phone_App_Architecture.md), [ble_link](../reference/ble_link.md)

| Phase | Name | Status | Evidence |
|---|---|---|---|
| 03.1 | Firmware (was Phase A) | DONE | "Firmware (`firmware/main/`)" and "What is actually proven" below |
| 03.2 | KMP restructure (was Phase B) | DONE | "Shared Kotlin" below; 61 Kotlin tests |
| 03.3 | iOS platform layer (was Phase C) | NOT STARTED | Needs a Mac |
| 03.4 | Day one with the iPhone (was Phase D) | NOT STARTED | Needs a Mac and an iPhone |

Part 1 is the handoff (was `docs/blueprints/03_ios_multiplatform.md`): read it first. Part 2 is the detailed
verification record by phase (was `docs/blueprints/03_ios_multiplatform.md`). They overlap; Part 2 is the
more precise record of what was verified and how.

## Part 1 — Handoff

Branch `feat/ios-ancs`, 21 commits ahead of `main`. Last updated 2026-09-29.

Read this first if you are picking the work up. It says what exists, what is proven, what is
merely written, and what will bite you. Its companion, Part 2 below, is the
per-phase record; this one is the orientation.

---

### 1. The shape of the thing, and why

OpenApex is an ESP32 motorcycle terminal plus a phone app. Android scrapes Google Maps
notifications, adds GNSS/IMU telemetry, and writes a fixed 210-byte packet to the terminal over a
custom GATT service.

**iOS breaks three assumptions at once**: no notification listener, no call observation, no
persistent background service. So the roles invert. On iOS the **terminal** is the GATT client and
reads ANCS (notifications) and CTS (time) off the phone directly; the app is reduced to the
telemetry half. It is not optional — nothing else can supply speed, position or battery.

That inversion is why `normalize.c` lives on the terminal ([system_overview](../reference/system_overview.md)): **one C normalizer serves
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

### 2. Where the code is

#### Firmware (`firmware/main/`) — 03.1 (was Phase A), done

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

#### Shared Kotlin (`phone/shared/src/commonMain/`) — 03.2 (was Phase B), done

```
protocol/   RawNotifPacket  CallPacket  SettingsTlv  RiderMesh  Fragmentation
ble/        TerminalLink  DeviceId  TerminalCharacteristic  ControlQueue  ControlOp
mesh/       RiderMeshSync  MeshTransport
store/      SecureStore  RiderMeshStore  RiderStoreCore
telemetry/  TelemetryAggregator
```

Targets are `androidTarget` + `jvm` only. **iOS targets are deliberately not declared yet** — they
need a Mac. `jvm` exists purely so `commonMain` is genuinely compiled and tested on Windows.

#### Android (`phone/androidApp/`)

`RelayBleClient` implements `TerminalLink` and is now GATT calls plus the one-operation-in-flight
rule, nothing else. `RelayService` owns GNSS, sensors, the foreground service and feeds
`TelemetryAggregator`. `NavNotificationRelayService` and `CallRelay` stay Android-only in full;
their iOS counterparts are empty by design, because ANCS owns both.

---

### 3. How to verify anything

```powershell
# Firmware host suites (9). No hardware needed. This is the main gate.
powershell -NoProfile -File scripts/run-host-tests.ps1

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

### 4. What is actually proven, and what is not

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

### 5. Traps

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
  older docs under `docs/` are **CRLF**. Detect the line ending before patching.
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

### 6. Design decisions you should not relitigate without reading why

- **The ESP32 is the ANCS client**, not the app. iOS cannot relay notifications; this is forced.
- **No third-party KMP BLE library** (Kable, blue-falcon). iOS background reliability hinges on
  `CBCentralManager` state restoration and we need full control. See `Phone_App_Architecture.md §4`.
- **Randomness is injected into `RiderStoreCore`**, not taken from the platform. It avoids an
  expect/actual whose two actuals would be the same line, and it lets a test pin the bytes — the
  only way to assert anything about a function whose whole job is to be unpredictable.
- **`DeviceId` is opaque.** Android's value is the MAC already in SharedPreferences, so there was
  nothing to migrate. iOS gives `CBPeripheral.identifier`, which is per-install and differs between
  two phones looking at one terminal. Never parse it, compare it across platforms, or display it.
- **Sentinel discipline** ([nav_pipeline](../reference/nav_pipeline.md)): *a missing field must never become a fabricated zero.* This is
  why the stale-fix test asserts the bytes equal a no-fix packet rather than merely differing.
- **Everything in the shared layer is callback-shaped, not suspending**, because the Android app
  has **no coroutines at all** — it is main-looper `Handler`s throughout. See §7.

---

### 7. What is left

#### 03.3 (was Phase C) — iOS platform layer. Not started.

Written-not-compiled by design until a Mac exists. Write against the 03.2 interfaces so that
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

#### The coroutine conversion

Deliberately deferred and deliberately not smuggled into a module move. Turning the queue pump —
single-op-in-flight, control > call > nav priority — into a `Channel` is the biggest single refactor
left. `TerminalLink` and `MeshTransport` both change shape when it happens; that is expected, and
it is why they are small. Give it its own step with tests around it.

#### 03.1 (was Phase A) leftovers

Settings TLV nav-source tag (auto / force-app / force-ANCS, wanted for day-one A/B) · `ancs_sim.c`
driving `sim_lvgl` · `replay.c` ANCS tier · the OTA service spec (`Phone_App_Architecture.md §6`
has a frame sketch, no UUID, no byte layout).

#### 03.4 (was Phase D) — day one with the iPhone

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
of truth on both platforms. 03.1–C survive that — ANCS still carries calls, CTS still carries
time, and the KMP telemetry stack is what a nav engine plugs into.

---

### 8. Environment

- ESP32-C3 on **COM30**, BT MAC `90:70:69:c0:ea:de`. Clean firmware build ~12 min, incremental ~35 s.
- Test phone: OnePlus (OxygenOS), `adb` at `%LOCALAPPDATA%\Android\Sdk\platform-tools`.
- JDK 21, Gradle 8.13 via the wrapper in `phone/`, AGP 8.13.2, Kotlin 2.2.20, ESP-IDF 6.1.0.
- Minimum iOS 18.

## Part 2 — Verification record by phase

Working branch: `feat/ios-ancs`. Last updated 2026-09-29.

Companion to the build-first plan. This file records **what is actually done and what has actually
been verified**, because the two are not the same thing and the difference is what matters when
the iPhone is in hand. Every "verified" line below says what hardware or harness proved it; where
nothing proved it, it says that instead.

### The shape of the thing

iOS cannot scrape notifications, observe calls, or keep a background service alive. So on iOS the
**terminal** reads ANCS and CTS off the phone as a GATT client, and the phone app is reduced to the
telemetry half. That split is why `normalize.c` lives on the terminal ([system_overview](../reference/system_overview.md)) — one C
normalizer serves the Android packet path and the iOS/ANCS path, rather than the same rules being
written twice in two languages and drifting.

Consequences to keep in view, not to fix:

- ANCS carries text only. No maneuver glyph, so a roundabout's exit direction — which on Android
  only the icon supplies — is unknown on iOS. No progress, no traffic segments, no telemetry.
- Apple Maps posts no notifications at all and cannot be a nav source. Google Maps posts under
  `CategoryIDLocation`.
- When iOS kills the app, the terminal goes telemetry-blind while ANCS nav and the CTS clock keep
  working. That gap is surfaced to the rider, not papered over.

### 03.1 (was Phase A) — firmware

Done and verified on the C3:

| Item | Verified by |
|---|---|
| ANCS protocol core (`ancs_core.c`), Data Source reassembly, UID lifecycle | `test_host/ancs_test.c` |
| ANCS → `raw_notif_t` mapping reusing `normalize.c` unchanged (`ancs_nav.c`) | `ancs_test.c`, `packet_normalize_test.c` |
| GATT-client role on the phone's own connection (`apple_client.c`), CTS (`cts_client.c`), Service Changed rediscovery | On device: Service Changed discovered and subscribed at handle 3 |
| ANCS service solicitation in the scan response, 31-byte advert split, pairing on connect, fast/slow advert intervals | On device, with an Android phone: link encrypted, no regression |
| Advertising no longer restarted while connected; a failed re-configure survivable | On device: no `rc=6`, packets unbroken at 1 Hz |
| Packet v6 (`nav_fields_absent`, `source_is_ios`), version window `[5,6]` | `test_host/packet_merge_test.c` |
| Nav/telemetry merge with per-half freshness (`packet_merge.c`) | `packet_merge_test.c`; on device, v5 passthrough unbroken at 1 Hz |
| Single-write fast path on the nav characteristic | **Compiles, undriven** — see below |
| `BOARD_HAS_ANCS`, DEVICE_INFO caps bits 1 and 2 | Built both ways: on 64.9% flash, off 64.4% (11.7 KB smaller) |
| Day-one capture: `DRIVE_LOG_KIND_ANCS` raw bytes both directions, `apple` console dump, `tools/decode_ancs.py` | Console dump seen on device; decoder not yet run against a real capture |

Host suites: 8, all passing via `scripts/run-host-tests.ps1`.

#### Not verified, and not claimed

- **The single-write fast path has never run.** `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`, so this PC
  cannot take the connection slot while the phone holds it. The code compiles and the length
  discrimination is sound by construction (a fragment write is ≤20 bytes, a whole packet is 210),
  but no writer has sent one. First real test is the iPhone, or a second central.
- **The 31.6 s dead link is still open.** Reported symptom: a connected link with zero decoded
  packets for 31.6 s (885 s in one user session), cleared only by a disconnect/reconnect. It did
  not reproduce on the current build. The `rc=6` that was fixed alongside it was this branch's own
  log noise, not that symptom's cause. Do not treat it as resolved.
- **Dual-role heap headroom under sustained load is unmeasured.** Phone link + mesh Coded-PHY
  advert and scan + an active GATT-client session have not been run together under load. The C3 is
  the risk; the S3 has headroom. `BOARD_HAS_ANCS` exists so this can be gated per board if needed.

#### Deliberately deferred

Settings TLV nav-source preference tag (auto / force-app / force-ANCS) · `ancs_sim.c` driving
`sim_lvgl` · `replay.c` ANCS tier · the OTA service spec. None of these block plugging in an
iPhone. `replay.c`'s ANCS tier is wanted the day *after* a capture, for tuning text rules against
it on the bench.

### 03.2 (was Phase B) — KMP restructure

**Done.** `android/` is now `phone/` with `:androidApp` and a multiplatform `:shared`; a Gradle
wrapper exists where there was none; the Car App shell is gone; packages are `org.openapex.protocol`
/ `org.openapex.app`. `:shared:jvmTest` is 61 tests, plus 9 firmware host suites.

What is in `commonMain`, and why each had to move before the one after it:

| | |
|---|---|
| `RawNotifPacket`, `CallPacket`, `SettingsTlv`, `RiderMesh` | the wire formats, already pure |
| `Fragmentation` / `FragmentQueue` | what to send next and in what order |
| `SecureStore` + `RiderStoreCore` | had to precede the mesh sync, which reached into `Context` |
| `RiderMeshSync` on `MeshTransport` | had to leave `RelayBleClient` before `TerminalLink` could |
| `TerminalCharacteristic` + `ControlQueue` | the last non-GATT thing in `RelayBleClient` |
| `TerminalLink` + `DeviceId` | the interface `iosMain` implements |
| `TelemetryAggregator` | was fields scattered across `RelayService` |
| `telemetryOnlyPacket()` (v6) | the iOS packet, written before iOS can compile |

`RelayBleClient` is now GATT calls and the one-operation-in-flight rule, nothing else.

**`DeviceId` needs no migration.** It is opaque, and Android's value is the MAC string already in
SharedPreferences under the same key. iOS supplies `CBPeripheral.identifier` instead -- per-install,
so it differs between two phones looking at one terminal. Nothing above the link may parse it.

**Verified on hardware throughout, not just at the end.** Each step was installed on the OnePlus
and run against the C3: a full mesh sync (`identity push ok=true`, `friends sync terminal=1
merged=1`, `friends push ok=true`, FRIENDS subscribed) through the new store, then the new sync,
then the new queue, then over a `DeviceId` connect; and the C3 decoding packets gaplessly, seq
40-52 at 1 Hz with GNSS, and on the final build with `speed_x10=65535 heading=65535` -- the absent
sentinels, correct indoors. **Not one byte changed on the wire**, which was the pass condition.

New: `packet_kotlin_fixture_test.c` decodes packets the Kotlin packer actually produced using the
firmware's own `packet.c`. One wire format has two implementations in two languages and nothing
compared them before; the Kotlin tests only ever agreed with Kotlin.

Two bugs found and fixed on the way, both pre-existing:

- `RelayBleClient.writeAll` locked its own `frames` parameter, which shadowed the `FragmentQueue`
  field every other caller locks, so `controlOps` was mutated outside its guard. The queue now has
  an explicit lock object.
- The notification-listener grant is stored **fully qualified**, so the package rename would have
  silently stopped nav scraping. Re-issued with `cmd notification allow_listener`.

Still open, and not from this work:

- **A fresh install crash-loops.** `RelayService.onCreate` starts a `connectedDevice` foreground
  service before `BLUETOOTH_CONNECT` is granted. Survivable for a developer; wrong for anyone else.
- **No coroutines anywhere** -- everything is still a main-looper `Handler`. Converting the queue
  pump to a `Channel` is the biggest single refactor left and deliberately was not smuggled into a
  module move. `TerminalLink` and `MeshTransport` change shape when it happens.
- `publish()` is driven *only* by fused location updates, so with no GNSS fix the packet stream
  stops entirely. Worth knowing: it is not a smoke test unless the phone can see sky.

### 03.3 (was Phase C) — iOS platform layer

Not started. Written-not-compiled by design: Kotlin/Native iOS targets and Xcode need a Mac, which
does not exist yet. Everything in `commonMain` is genuinely verified on Windows via the JVM target,
so only the platform layer is unproven.

The one question no amount of code answers: **whether an AccessorySetupKit bond is system-level
enough for iOS to publish ANCS.** ASK pairs "without the usual OS pairing prompt" and its bonds are
app-scoped. So both paths get written — ASK, and plain CoreBluetooth relying on ANCS solicitation
plus system pairing — making it a five-minute test on day one instead of a rebuild.

### 03.4 (was Phase D) — day one with the iPhone

Ordered highest-information first.

1. **Pair**, ASK first then plain CoreBluetooth. Confirm via the `apple` console command that ANCS
   and CTS were discovered. This is the highest-risk moment in the project.
2. **Log everything, filter nothing.** `ancs_nav_set_permissive()` exists for exactly this. Google
   Maps on a short route, then Apple Maps (expect silence — confirm it), then Waze (unknown).
3. **Answer what the plan could not.** Does Google Maps modify one persistent notification per turn
   or post new UIDs, and how often? Is distance in the text, in a format
   `parse_distance_metres()` already handles? Does `CategoryIDIncomingCall` behave as [ble_link](../reference/ble_link.md)
   assumes, and does `ActionIDPositive` actually answer the call?
4. **Tune text rules against the capture** on the bench: `tools/decode_ancs.py` → fixture →
   `replay.c`. Rules change; architecture does not.
5. **Longer drive** for maneuver coverage. Only 6 of 13 classes have ever been observed on Android;
   iOS starts from zero.

**The branch point:** if step 3 shows Google Maps posts only sparse, non-updating notifications,
iOS turn-by-turn via ANCS is not viable, and the fallback is an in-app navigation engine as the
source of truth on both platforms. 03.1–C survive that: ANCS still carries calls, CTS still
carries time, and the KMP telemetry stack is what a nav engine would plug into.

### Environment notes

- ESP32-C3 on **COM30**, BT MAC `90:70:69:c0:ea:de`.
- Build: `pio run -d firmware -e prototype_c3`; add `-t upload --upload-port COM30` to flash.
  A clean build is ~12 minutes, incremental ~35 s.
- Host tests: `powershell -NoProfile -File scripts/run-host-tests.ps1`.
- Encodings are not uniform and edits fail silently on the wrong one: `ble_link.c` and `main.c` are
  **cp1252**, `apple_client.*` and `ancs_*` are UTF-8, the docs under `docs/` are mixed (older ones **CRLF**, newer ones LF).
- The Android project has **no Gradle wrapper**; Gradle 8.13 is unpacked under
  `~/.gradle/wrapper/dists/` from Android Studio and can be invoked directly.

## Open decisions

*Was SPEC §13 item 3.*

3. Confirm the iOS ANCS subscription and notification-attribute flow on the intended iPhone.
