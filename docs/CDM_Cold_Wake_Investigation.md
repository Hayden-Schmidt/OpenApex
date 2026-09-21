# CompanionDeviceManager Cold-Wake Investigation

**Status:** unresolved, root cause narrowed to an OEM/AOSP-level restriction outside this
project's control. Written up for independent/external investigation.

**Date range:** 2026-09 (see `git log` on the files below for exact commit timestamps).

## 1. Summary

OpenApex uses Android's `CompanionDeviceManager` (CDM) observer mode so a phone with the app
fully killed can be woken by the OS when the ESP32-C3 terminal starts advertising over BLE (e.g.
"bike powers on"), without the user manually opening the app. This works when the app process is
already warm, but **the cold-wake callback (`onDeviceAppeared` / `onDevicePresenceEvent`) does not
fire when the app process has been killed**, even though CDM's own presence-detection engine
correctly detects the terminal as "Nearby" at the same moment. Five candidate root causes internal
to this project were investigated and ruled out (§5). External research turned up a matching,
currently unresolved AOSP issue report and strong community consensus that this is an OEM-level
process-wake restriction, not an API-usage bug (§6). The project has since implemented a
`BOOT_COMPLETED`-triggered foreground service with its own BLE scan as a working fallback that
does not depend on the CDM callback (§7), but the underlying CDM cold-wake defect itself remains
unresolved and is the subject of this write-up.

## 2. Hardware / software under test

**Terminal (BLE peripheral / GATT server):**
- ESP32-C3 (generic dev board, `PROTOTYPE_C3_GC9A01` profile per `docs/OpenApex_SPEC.md` §6.1)
- Firmware: ESP-IDF 5.x, NimBLE BLE stack, built/flashed via PlatformIO
- Source: `firmware/main/ble_link.c`
- BLE MAC: `88:56:A6:29:76:8A` (stable/static across every reset tested; not an RPA-rotating
  address — no RPA code exists in the firmware)
- Advertising: legacy (non-extended) `ADV_IND`, primary payload = flags (3B) + 128-bit service UUID
  complete list (18B) + complete local name "OpenApex" (10B) = 31B (payload budget maximum).
  Advertising interval `itvl_min=32`/`itvl_max=64` (20-40ms, i.e. `BLE_GAP_CONN_MODE_UND` /
  `BLE_GAP_DISC_MODE_GEN`) — set explicitly after finding the interval had defaulted to NimBLE's
  ~1.28s (see §5).
- Service UUID: see `RelayBleClient.SERVICE_UUID` in the Android source (project-specific 128-bit
  UUID; omitted here as it carries no diagnostic value).
- Pairing: Just Works (`BLE_SM_IO_CAP_NO_IO`), Secure Connections (`sm_sc=1`), nav characteristic
  requires an encrypted link (`BLE_GATT_CHR_F_WRITE_ENC`). Bonds persist in NVS
  (`CONFIG_BT_NIMBLE_NVS_PERSIST=y`).

**Phone (BLE central / GATT client, CDM observer):**
- OnePlus 15, OxygenOS (Android 16 / API level 36 at test time)
- `targetSdkVersion=35` in the built APK (app not yet targeting 36; irrelevant to the API-36
  CDM-method-signature check performed in §5, which tests the *device's* OS, not the app's target)
- Package under test: `org.openapex.androidauto`
- Relevant components:
  - `MainActivity.kt` — builds the CDM `AssociationRequest`, calls
    `startObservingDevicePresence` (SDK-version-gated between the legacy
    `String`-address overload and the API-36 `ObservingDevicePresenceRequest` overload)
  - `OpenApexCompanionService.kt` — extends `CompanionDeviceService`; implements both
    `onDeviceAppeared(String)`/`onDeviceDisappeared(String)` (legacy) and
    `onDevicePresenceEvent(DevicePresenceEvent)` (API 36+), so behavior is captured on both
    callback paths
  - `RelayService.kt` — foreground service that owns the actual GATT client connection
  - `BootCompletedReceiver.kt` — added after this investigation as a fallback (see §7); not part
    of the original CDM path

## 3. Test fixture

**Automated:** `scripts/test-ble-link.sh <serial-port> <window-seconds>`
1. Resets the C3 over serial (RTS-toggle via a small Python helper using `pyserial`) so a fresh
   BLE advertisement session begins.
2. Force-stops the Android app (`adb shell am force-stop org.openapex.androidauto`) — this is the
   "cold process" precondition; CDM's promise is that it can wake the app from exactly this state.
3. Does **not** relaunch the app.
4. Captures the C3 serial log and a filtered `adb logcat` concurrently for the fixed window.
5. Prints a lean pass/fail summary (did `onDeviceAppeared`/`onDevicePresenceEvent` fire within the
   window, did a GATT connection get established).
6. Raw logs land in `logs/` (gitignored — never committed, never contain personal data beyond the
   device's own static BLE MAC, which is hardware, not personal, data).

**Manual/ad-hoc (used to cross-check and to simulate a `BOOT_COMPLETED` event without a real
reboot):**
```
adb shell pm list packages | grep openapex
adb shell dumpsys companiondevice                                  # full CDM state dump
adb shell cmd companiondevice list 0                                # active associations only
adb shell cmd companiondevice disassociate 0 <pkg> <mac>            # remove one stale association
adb shell cmd companiondevice notify-device-disappeared <ASSOC_ID>  # force presence state to false
adb shell cmd companiondevice notify-device-appeared <ASSOC_ID>     # simulate a presence event
adb shell am force-stop org.openapex.androidauto
adb shell am broadcast -n org.openapex.androidauto/.BootCompletedReceiver \
    -a android.intent.action.BOOT_COMPLETED                        # simulate boot without reboot
adb logcat -d -s OpenApexBootReceiver:I RelayService:I RelayService:W OpenApexCompanion:I
adb shell cmd appops get org.openapex.androidauto
adb shell dumpsys deviceidle
javap -classpath <SDK>/platforms/android-36/android.jar android.companion.CompanionDeviceManager
```
`javap` against the real `android-36/android.jar` was used throughout to get exact, verified API
signatures for the API-36 CDM classes (`ObservingDevicePresenceRequest`, `DevicePresenceEvent`,
`AssociationInfo`) rather than relying on documentation or guesswork.

## 4. Observed failure

With a clean single CDM association, app process fully killed (`am force-stop`), and the C3
freshly reset and advertising at a 20-40ms interval:
- `dumpsys companiondevice` shows the terminal listed under **"Nearby BLE Devices"** for the
  association — i.e. CDM's own presence-detection engine correctly and promptly detects the
  terminal.
- `onDeviceAppeared` (legacy) / `onDevicePresenceEvent` (API 36) is **not delivered** to the killed
  app within the test window (90s in the scripted test; also confirmed absent over longer manual
  waits).
- No new `org.openapex.androidauto` process is spawned by the OS as a result.

This isolates the failure to one specific step in the pipeline: CDM detects presence correctly,
but the subsequent wake-and-deliver-callback step to the killed app's `CompanionDeviceService`
does not happen (or is delayed far beyond any reasonable window for this use case).

## 5. Candidate root causes investigated and ruled out

1. **Service UUID placed in scan response instead of primary advertising payload.** Ruled out —
   `ble_gap_adv_set_fields()` is the only call setting advertising data in `ble_link.c`, and it
   sets the primary payload; there is no separate scan-response payload in use.
2. **ESP32 MAC address instability (rotating/RPA address).** Ruled out — the C3's MAC
   (`88:56:A6:29:76:8A`) was identical across every reset/boot tested; no RPA code exists in
   firmware (uses `ble_hs_util_ensure_addr` / `ble_hs_id_infer_auto` for a stable public/static
   address).
3. **`CompanionDeviceService` manifest/binding misconfiguration.** Ruled out — confirmed correct
   via direct manifest inspection and `dumpsys activity services` showing correct binding with the
   right permission (`android.permission.BIND_COMPANION_DEVICE_SERVICE`) and intent-filter
   (`android.companion.CompanionDeviceService`).
4. **OxygenOS/ColorOS OS-level background restriction** (Doze allowlist,
   `RUN_ANY_IN_BACKGROUND`, `START_FOREGROUND` app-op, the app's own "allow background activity"
   toggle). Ruled out — all confirmed allowed via `dumpsys deviceidle` / `cmd appops get`, the
   device was not in Doze during any test window, and the user confirmed "allow background
   activity" is on with no separate "auto-launch" toggle present in Settings on this device/ROM.
5. **Slow BLE advertising interval.** A real bug, and fixed, but **not** the cause of the cold-wake
   failure: `adv_params` in `start_advertising()` was zero-initialized, leaving NimBLE's default
   interval (~1.28s), which could plausibly cause Android's low-power background scanner to miss
   an advertisement for multiple scan cycles. Changed to `itvl_min=32`/`itvl_max=64` (20-40ms).
   Retested after this fix with a freshly cleaned single association: **identical failure**.
6. **Missing/absent BLE device name** (picker showed a raw MAC instead of a name). Added a
   complete local name ("OpenApex") to the advertising payload. Retested: **identical failure**.
   Expected, since CDM's `AssociationRequest` filter matches on service UUID, not device name —
   included here because it was a real (separately fixed) usability issue and to document it was
   explicitly checked, not overlooked.
7. **Stale/duplicate CDM associations.** Found and cleaned up 10 duplicate associations for the
   same MAC, accumulated from repeated test-cycle app relaunches during development
   (`adb shell cmd companiondevice disassociate`). Possible source of state-machine noise/races.
   Retested with a single clean association: **identical failure** — not the cause, but a real
   cleanup that should be considered standard test hygiene going forward (verify
   `cmd companiondevice list 0` shows exactly one association before any cold-wake test).
8. **API 36 (Android 16) deprecation of the presence-observation API.** The test device runs
   Android 16, which deprecates `startObservingDevicePresence(String)` /
   `onDeviceAppeared(String)` in favor of `startObservingDevicePresence(ObservingDevicePresenceRequest)`
   + `onDevicePresenceEvent(DevicePresenceEvent)`. Migrated both `MainActivity.kt` and
   `OpenApexCompanionService.kt` to use the new API on API 36+ via an `SDK_INT` check (old API
   path preserved for API < 36). Retested with a freshly cleaned single association:
   **identical failure** — `onDevicePresenceEvent` also never fires from a killed process, and
   `dumpsys companiondevice` still shows the terminal correctly detected as "Nearby." This rules
   out the deprecation/API-migration itself as the cause.

## 6. External research

A literature/community search (Google Issue Tracker, `dontkillmyapp.com`, and related developer
discussion) turned up:
- **Google Issue Tracker #432207962** — a currently open, unresolved report describing the same
  symptom: `onDeviceAppeared` / `onDevicePresenceEvent` not firing for a killed process despite CDM
  correctly detecting device presence.
- OnePlus/OxygenOS and ColorOS-based devices are widely and independently documented as among the
  most aggressive Android OEMs at killing background processes and **silently reverting**
  battery-optimization/autostart whitelisting the user has granted, with **no programmatic (ADB or
  public API) way to detect or correct this from application code**. This class of behavior is
  well known enough to have a dedicated community tracking site (dontkillmyapp.com) cataloguing
  OEM-specific restrictions and user-facing workarounds (which are manual, in-Settings toggles with
  no stable identifiers across OxygenOS versions).

We were not able to find a documented fix, workaround, or ETA from Google or OnePlus for this
specific defect.

## 7. Current mitigation (does not resolve the underlying defect)

Since cold-wake cannot be relied upon on this OEM, `RelayService` is now also started from a
`BOOT_COMPLETED` broadcast receiver (`BootCompletedReceiver.kt`) and, when started with no device
address (i.e. not CDM-triggered), performs its own BLE scan filtered on the terminal's service
UUID rather than waiting on a CDM callback. This was verified end-to-end on-device: a simulated
boot broadcast against a force-stopped app successfully starts the service, scans, finds the C3,
and establishes a GATT connection — all without the CDM callback ever firing. This gets the
practical "bike on -> phone reconnects" outcome working on this device, but it is a workaround for
the boot case specifically; a phone that is *already on* when the terminal starts advertising
(without an intervening reboot) still cannot be woken from a killed app state on this OEM.

One related, secondary defect surfaced during this fallback's own testing: promoting the
foreground service to the `location` FGS type immediately upon a BLE connect (needed for GNSS)
throws `SecurityException` on API 34+ when the service was started from a background context
(`Foreground service started from background can not have location/camera/microphone access`).
This is a distinct, well-documented Android platform restriction (unrelated to CDM) and is handled
by catching the exception and retrying GNSS promotion on the next BLE reconnect rather than
crashing.

## 8. Open questions for further investigation

1. Is there any known, even undocumented, Android/OEM API or ADB-settable flag that restores
   reliable CDM cold-wake delivery on OxygenOS/ColorOS, short of the user manually toggling
   OEM-specific battery/autostart settings per device model/ROM version?
2. Does the defect reproduce identically on stock/Pixel Android, or is it strictly an
   OxygenOS/ColorOS-induced regression? (Not yet tested on non-OEM-skinned hardware; this is a
   priority next step for scoping whether the mitigation in §7 is OEM-specific hardening or a
   universal necessity.)
3. Is Google Issue Tracker #432207962 (or a related report) actively triaged, and is there a
   public timeline?
4. Does `CompanionDeviceManager.getMyAssociations()` / `AssociationInfo` expose any per-association
   diagnostic state (last-observed timestamp, wake-attempt count, etc.) that could distinguish
   "OS never tried to wake the app" from "OS tried and was blocked/killed it again immediately,"
   which would materially change where the fix belongs (Android CDM internals vs. OEM process
   policy)?
