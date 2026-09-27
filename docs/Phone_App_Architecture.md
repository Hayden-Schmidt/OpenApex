# OpenApex Phone App Architecture

**Status:** Active. **Scope:** the Android + iOS companion app. **Stack:** Kotlin Multiplatform (KMP) + Compose Multiplatform (CMP).

## 1. Role
The phone is a **dumb relay** plus the control panel for the terminal (SPEC §2.4). It never parses navigation text.

| Job | Android | iOS |
|---|---|---|
| Nav data | NotificationListener, raw relay over custom GATT | none: terminal reads ANCS directly |
| Time / battery | via packet | none: terminal reads CTS / BAS directly |
| GNSS + motion | Fused location, rotation vector, accel/gyro | CoreLocation, CoreMotion |
| Devices, settings push, firmware update, status | shared | shared |

## 2. Why KMP + CMP
- The hard parts run in the background and are OS-specific (BLE background, location, notification listener, CDM, CoreBluetooth restoration). KMP calls native APIs directly, with no bridge layer.
- The UI, protocol, settings and firmware-update logic are written once, so there's no Kotlin/Swift drift. This is the same reasoning as the single C normaliser in SPEC §2.4.
- Compose draws every pixel itself, so a custom animated design system looks the same on both platforms.

**Rule:** protocol, state and UI go in shared code. Anything that must keep running in the background is written natively per platform, behind a shared interface.

## 3. Module layout
```
phone/
  shared/            KMP library
    commonMain/
      protocol/      packet v4 codec, settings TLV, firmware-update frames (byte-exact with firmware)
      ble/           TerminalLink interface, connection state machine
      telemetry/     TelemetrySource interface, sample model
      device/        known devices, reconnect state
      settings/      terminal config model + local persistence
      ota/           firmware-update state machine (chunk, CRC, resume, verify)
      ui/            all screens, navigation, design system
    androidMain/     Android BLE, location/sensors, NotificationListener relay, foreground service, CDM, boot
    iosMain/         CoreBluetooth (with state restoration), CoreLocation, CoreMotion
    commonTest/      protocol + state machine tests, golden packet fixtures
  androidApp/        Activity + manifest only
  iosApp/            Xcode project, thin SwiftUI host for ComposeUIViewController, Info.plist background modes
```
Migration: `android/` becomes `phone/androidApp` plus `androidMain`. Delete the `OpenApexCarAppService` stub and the `androidx.car.app` dependency. Rename the package `org.openapex.androidauto` to `org.openapex.app`.

## 4. Platform boundaries (shared interfaces)
```kotlin
interface TerminalLink {            // one per platform, native BLE stack
    val state: StateFlow<LinkState>
    suspend fun connect(device: DeviceId)
    suspend fun write(char: TerminalChar, bytes: ByteArray, withResponse: Boolean)
    fun notifications(char: TerminalChar): Flow<ByteArray>
}
interface TelemetrySource { val samples: Flow<TelemetrySample> }
interface NavRelaySource  { val raw: Flow<RawNavFields> }   // Android only; iOS = empty
```
- BLE is native on each platform, not a third-party KMP BLE library. iOS background reliability depends on CoreBluetooth state restoration, which we need full control over.
- DI: constructor injection plus a small manual container per platform. Add a DI library only if the manual container gets painful.

## 5. State architecture
- Unidirectional flow: `Repository (StateFlow) -> ViewModel (UiState) -> Composable`. Events go back up as function calls.
- One `AppState` source of truth: link, device list, telemetry health, firmware-update progress, terminal settings.
- Use AndroidX `lifecycle-viewmodel` (multiplatform), `kotlinx.coroutines`, `kotlinx.serialization`.
- Navigation: Compose Multiplatform navigation.
- Persistence: DataStore (multiplatform) for app prefs and known devices.

## 6. BLE contract additions (spec before code)
Add these to SPEC §5 before any UI work that depends on them:
1. **Settings characteristic:** versioned TLV. Read the full config, write a partial config, terminal acks.
2. **Firmware-update service:** custom and framework-agnostic. Frames: begin(size, hash, version), chunk(offset, data), commit, abort; status notifications. Resumable. The terminal verifies the image before swapping.
3. **Device info:** firmware version, board profile, gfx tier, capability bits (so the UI hides features the board lacks).
Each gets golden byte fixtures shared by commonTest and the firmware native tests.

## 7. UI and design system
- **Custom design system, not stock Material.** It lives in `ui/theme`: colour tokens (high-contrast, matching the terminal), type scale, shapes, spacing, and **motion tokens** (durations, spring specs) that every animation uses.
- Dark first.
- Screens (v1):
  1. **Home / status:** link state, live telemetry health, terminal preview.
  2. **Devices:** scan, pair/bond, forget, rename.
  3. **Terminal settings:** grouped, pushed to the ESP32, shows sync state.
  4. **Firmware update:** pick/download image, progress ring, resume.
  5. **App settings / permissions:** guided setup (notification access, background location, battery exemptions per OEM).
  6. **Diagnostics:** packet log, recorder control.
- **Animations:** `AnimatedContent` for state changes, shared-element transitions for device card to detail, Canvas for the progress ring and the live round-display preview.
- **Round-display preview:** a Canvas component rendering the terminal state from the same `AppState`. It's a mirror of the terminal, not a second renderer of truth.
- Native embeds (SwiftUI/UIKit) only where the iOS feel clearly matters. None are planned for v1.

## 8. Platform specifics
**Android:** foreground service (`connectedDevice|location`), CDM observer + boot receiver + reconnect (never rely on CDM alone; see [CDM_Cold_Wake_Investigation.md](CDM_Cold_Wake_Investigation.md)), NotificationListener, relay-only.
**iOS:** `bluetooth-central` + `location` background modes; `CBCentralManager` restore identifier. Bond with the terminal so its ANCS/CTS/BAS client works over the same link. Validate early on a real iPhone.

## 9. Testing
- `commonTest`: codecs are byte-exact against golden fixtures; firmware-update and link state machines use fake links.
- Compose UI previews + screenshot tests for the design system.
- Hardware loop: phone to S3 sim / C3, via the existing `scripts/test-ble-link.sh` pattern.

## 10. Roadmap
1. Specify the settings, firmware-update and device-info contracts in SPEC §5.
2. Restructure to `phone/` KMP layout with no behaviour change; delete the Auto stub; rename the package.
3. Move the protocol and state into commonMain along with their tests.
4. Build the design system, then the screens.
5. Add iosApp once a Mac is available; the first milestone is a background BLE + location soak test.

