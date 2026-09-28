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

What exists today (Phase B complete; see docs/iOS_Handoff.md):

```
phone/
  shared/                          KMP library, androidTarget + jvm
    commonMain/
      protocol/    RawNotifPacket (v5 + v6), CallPacket, SettingsTlv, RiderMesh, Fragmentation
      ble/         TerminalLink, DeviceId, TerminalCharacteristic, ControlQueue
      mesh/        RiderMeshSync, MeshTransport
      store/       SecureStore, RiderMeshStore, RiderStoreCore
      telemetry/   TelemetryAggregator
    commonTest/    61 tests, incl. golden fixtures cross-checked against the firmware's C decoder
  androidApp/      RelayBleClient (TerminalLink impl), RelayService, nav/call relays, CDM, boot
```

Not yet present, and deliberately so: `iosMain/` and `iosApp/` need a Mac, and the iOS Kotlin
targets are not declared until then. `jvm` exists so `commonMain` is genuinely compiled and tested
on Windows rather than only inspected. `ota/` and `ui/` are unwritten.

Migration (done): `android/` became `phone/`, the `OpenApexCarAppService` stub and the
`androidx.car.app` dependency are gone, and the Kotlin package is now `org.openapex.app` for the
Android host and `org.openapex.protocol` for the shared wire formats. The **applicationId stays
`org.openapex.androidauto`** on purpose: it is the installed app's identity, and changing it
forces an uninstall that costs the test phone its pairing, settings and rider identity.

## 4. Platform boundaries (shared interfaces)

As built. Callback-shaped rather than suspending, because the Android app has no coroutines
anywhere -- it is main-looper `Handler`s throughout. These shapes change when the queue pump is
converted to a `Channel`, which is its own step; they are small so that change stays cheap.

```kotlin
@JvmInline value class DeviceId(val value: String)   // MAC on Android, CBPeripheral UUID on iOS

interface TerminalLink {                             // RelayBleClient / IosTerminalLink
    fun connect(id: DeviceId, background: Boolean = false)
    fun disconnect()
    fun write(packet: ByteArray)                     // nav, fragmented, no response
    fun writeCall(packet: ByteArray)
    fun readSettings(done: (TerminalSettings?) -> Unit)
    fun writeSettings(settings: TerminalSettings, done: (Boolean) -> Unit = {})
    fun readDeviceInfo(done: (DeviceInfo?) -> Unit)
    fun readAvatar(done: (AvatarArt?) -> Unit)
    fun uploadAvatar(art: AvatarArt, done: (Boolean) -> Unit = {})
    fun clearAvatar(done: (Boolean) -> Unit = {})
    fun pushFriends()
}

interface MeshTransport {                            // what RiderMeshSync needs, and no more
    fun writeAll(char: TerminalCharacteristic, frames: List<ByteArray>, done: (Boolean) -> Unit)
    fun read(char: TerminalCharacteristic, done: (ByteArray?) -> Unit)
    fun subscribe(char: TerminalCharacteristic)
    fun log(message: String)
}

interface SecureStore { fun get(key: String): ByteArray?; fun put(key: String, value: ByteArray?) }
```

Connection state is not on `TerminalLink`: a link can drop without anyone having asked it for
anything, so it is observed through `RelayStateHolder` instead.

Still to write, for iOS: `TelemetrySource` (Android feeds `TelemetryAggregator` directly today) and
`NavRelaySource` (Android only; iOS is empty by design, ANCS owns it).

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
- **Rider mesh UI** (SPEC §5.7; deferred from the mesh build, protocol and sync already done in `RiderMesh.kt` / `RiderStore.kt` / `RelayBleClient.kt`). Goes in **Terminal settings** plus a new **Friends** screen:
  - **Rider name** field: short/first name, at most 12 UTF-8 bytes (cut on a character boundary, `RiderMesh.nameBytes`). Saved with `RiderStore.setName`; the terminal gets it on the next connect (the identity push).
  - **Toggles** (settings TLV, `TerminalSettings`): friends on compass (0x07, default on), friend marker character/dot (0x08, default character), mesh enabled (0x09, default on, labelled as the privacy kill switch: off means the terminal is silent to other riders). Written with `RelayBleClient.writeSettings`.
  - **Friends list:** each friend's character in their accent (preset or custom 8x8 art), name, and remove. Remove calls `RiderStore.removeFriend` then `RelayBleClient.pushFriends()`; unfriending is phone-only, and the other rider keeps us until they remove us. Cap 16; show the count.
  - The list refreshes when the terminal notifies a new friendship (friends made on the terminal appear without user action).
  - No friend-adding UI on the phone: friending happens only on the terminal's hold ring.
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

