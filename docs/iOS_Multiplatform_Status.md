# iOS + Android Multiplatform: current state

Working branch: `feat/ios-ancs`. Last updated 2026-09-29.

Companion to the build-first plan. This file records **what is actually done and what has actually
been verified**, because the two are not the same thing and the difference is what matters when
the iPhone is in hand. Every "verified" line below says what hardware or harness proved it; where
nothing proved it, it says that instead.

## The shape of the thing

iOS cannot scrape notifications, observe calls, or keep a background service alive. So on iOS the
**terminal** reads ANCS and CTS off the phone as a GATT client, and the phone app is reduced to the
telemetry half. That split is why `normalize.c` lives on the terminal (SPEC §2.4) — one C
normalizer serves the Android packet path and the iOS/ANCS path, rather than the same rules being
written twice in two languages and drifting.

Consequences to keep in view, not to fix:

- ANCS carries text only. No maneuver glyph, so a roundabout's exit direction — which on Android
  only the icon supplies — is unknown on iOS. No progress, no traffic segments, no telemetry.
- Apple Maps posts no notifications at all and cannot be a nav source. Google Maps posts under
  `CategoryIDLocation`.
- When iOS kills the app, the terminal goes telemetry-blind while ANCS nav and the CTS clock keep
  working. That gap is surfaced to the rider, not papered over.

## Phase A — firmware

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

Host suites: 8, all passing via `scripts/build-countdown.ps1`.

### Not verified, and not claimed

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

### Deliberately deferred

Settings TLV nav-source preference tag (auto / force-app / force-ANCS) · `ancs_sim.c` driving
`sim_lvgl` · `replay.c` ANCS tier · the OTA service spec. None of these block plugging in an
iPhone. `replay.c`'s ANCS tier is wanted the day *after* a capture, for tuning text rules against
it on the bench.

## Phase B — KMP restructure

In progress. Done: a Gradle wrapper (there was none), `android/` renamed to `phone/` with
`:androidApp` and a multiplatform `:shared`, the Car App shell deleted, packages renamed to
`org.openapex.protocol` and `org.openapex.app`, and the four wire-format files plus
fragmentation and the send queue moved to `commonMain`. `:shared:jvmTest` is 34 tests.

**Verified on hardware after the move** (2026-09-29): reinstalled on the OnePlus, 13 packets
written at 1 Hz as 13 fragments each, and the C3 decoded 26 consecutive packets (seq 150-175)
with no gaps and no warnings. The restructure does not change a byte on the wire.

Two things that only a reinstall could show:

- The notification-listener grant is stored **fully qualified**, so the package rename would have
  silently stopped nav scraping even on an in-place update -- the listener would point at a class
  that no longer exists, with no error anywhere. Re-issued with `cmd notification allow_listener`.
- **Pre-existing, not from this work:** on a fresh install the app crash-loops.
  `RelayService.onCreate` starts a `connectedDevice` foreground service before
  `BLUETOOTH_CONNECT` is granted and Android throws. Survivable for a developer who grants
  permissions out of band; wrong for anyone else.

Also done: the rider store behind `SecureStore` + `RiderStoreCore`, and the rider-mesh sync state
machine as `RiderMeshSync` on `MeshTransport` + `RiderMeshStore`. `:shared:jvmTest` is now 45
tests. Both verified on the OnePlus, in place: a full sync -- `identity push ok=true`,
`friends sync: terminal=1 merged=1`, `friends push ok=true`, FRIENDS subscribed -- so the list was
read off the terminal, merged, stored in the new format and pushed back. The rider identity
survived the storage change, which is the one value that cannot be regenerated without orphaning
every friendship.

Fixed along the way: `RelayBleClient.writeAll` locked its own `frames` parameter instead of the
`FragmentQueue` field of the same name, leaving `controlOps` mutated outside the lock that guards
it everywhere else. Pre-existing. The queue now has an explicit lock object.

Remaining: the control-op queue and a characteristic enum, the `TerminalLink` interface with
`DeviceId` replacing the saved MAC, `TelemetryAggregator` out of `RelayService`, and v6 plus
`telemetryOnly()`.

**Open question at the `DeviceId` step:** whether replacing the stored MAC needs a migration path
or whether a re-pair is acceptable.

The Android app today: single Gradle module, ~3.3k LOC, **no coroutines anywhere** (everything is
a main-looper `Handler`), no Compose, no DI. Four files are already pure Kotlin and move verbatim:
`RawNotifPacket.kt`, `CallPacket.kt`, `SettingsTlv.kt`, `RiderMesh.kt`.

**The pass condition for the whole phase: not one byte changes on the wire.** The Android capture
fixtures must replay end-to-end identically before and after.

The hazard is the coroutine conversion. Turning the BLE queue pump — single-op-in-flight, with
control > call > nav priority — into a `Channel` is the single biggest refactor in the plan and
should be its own step with tests around it, not smuggled into the module move.

## Phase C — iOS platform layer

Not started. Written-not-compiled by design: Kotlin/Native iOS targets and Xcode need a Mac, which
does not exist yet. Everything in `commonMain` is genuinely verified on Windows via the JVM target,
so only the platform layer is unproven.

The one question no amount of code answers: **whether an AccessorySetupKit bond is system-level
enough for iOS to publish ANCS.** ASK pairs "without the usual OS pairing prompt" and its bonds are
app-scoped. So both paths get written — ASK, and plain CoreBluetooth relying on ANCS solicitation
plus system pairing — making it a five-minute test on day one instead of a rebuild.

## Phase D — day one with the iPhone

Ordered highest-information first.

1. **Pair**, ASK first then plain CoreBluetooth. Confirm via the `apple` console command that ANCS
   and CTS were discovered. This is the highest-risk moment in the project.
2. **Log everything, filter nothing.** `ancs_nav_set_permissive()` exists for exactly this. Google
   Maps on a short route, then Apple Maps (expect silence — confirm it), then Waze (unknown).
3. **Answer what the plan could not.** Does Google Maps modify one persistent notification per turn
   or post new UIDs, and how often? Is distance in the text, in a format
   `parse_distance_metres()` already handles? Does `CategoryIDIncomingCall` behave as SPEC §5.5
   assumes, and does `ActionIDPositive` actually answer the call?
4. **Tune text rules against the capture** on the bench: `tools/decode_ancs.py` → fixture →
   `replay.c`. Rules change; architecture does not.
5. **Longer drive** for maneuver coverage. Only 6 of 13 classes have ever been observed on Android;
   iOS starts from zero.

**The branch point:** if step 3 shows Google Maps posts only sparse, non-updating notifications,
iOS turn-by-turn via ANCS is not viable, and the fallback is an in-app navigation engine as the
source of truth on both platforms. Phases A–C survive that: ANCS still carries calls, CTS still
carries time, and the KMP telemetry stack is what a nav engine would plug into.

## Environment notes

- ESP32-C3 on **COM30**, BT MAC `90:70:69:c0:ea:de`.
- Build: `pio run -d firmware -e prototype_c3`; add `-t upload --upload-port COM30` to flash.
  A clean build is ~12 minutes, incremental ~35 s.
- Host tests: `powershell -NoProfile -File scripts/build-countdown.ps1`.
- Encodings are not uniform and edits fail silently on the wrong one: `ble_link.c` and `main.c` are
  **cp1252**, `apple_client.*` and `ancs_*` are UTF-8, `docs/OpenApex_SPEC.md` is **CRLF**.
- The Android project has **no Gradle wrapper**; Gradle 8.13 is unpacked under
  `~/.gradle/wrapper/dists/` from Android Studio and can be invoked directly.
