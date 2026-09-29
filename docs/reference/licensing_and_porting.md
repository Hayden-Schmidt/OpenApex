# Licensing and porting references

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §14, §15 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## Source and Licensing Notes

*Was SPEC §14.*

- OpenApex application and firmware code are intended to remain under the repository's chosen open
  source license.
- The terminal renders data received from supported smartphone navigation applications; it does not
  reproduce their maps or perform local map extraction in Phase 1.
- OpenStreetMap attribution is required for any OSM-backed phone-side service used later.
- Platform notification APIs, ANCS, and navigation-application terms must be reviewed before public
  distribution. The Phase 1 notification path is a technical proof of concept and must not assume
  that every application permits redistribution or persistent storage of its notification data.

## Existing Code Porting References

*Was SPEC §15.*

This section maps the Phase 1 work onto code preserved in the historical
[OpenApex repository](https://github.com/Hayden-Schmidt/OpenApex). File links are references for
implementation, not claims that the old code already satisfies this specification.

### BLE transport and packet precedent

*Was SPEC §15.1.*

- [BleLink.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ride/BleLink.kt) contains the existing Android
  GATT server, service/characteristic UUIDs, notification subscription handling, advertising, and
  packet publication lifecycle. Reuse its permission checks and lifecycle shape where the Android
  helper remains the GATT server.
- [RideStatePacket.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ride/RideStatePacket.kt) contains the
  existing versioned 12-byte little-endian packet packer, explicit unknown sentinels, clamping, and
  byte-layout documentation. Adapt the model for normalized notification fields and GNSS freshness;
  do not silently change the existing packet meaning.
- [RideStatePacketTest.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/test/java/com/motonav/app/ride/RideStatePacketTest.kt) is the
  pattern for pure JVM wire-format tests. Add fixtures for Google Maps, Apple Maps/ANCS, idle,
  arrived, stale GNSS, malformed packets, and countdown baselines.
- [RideSessionService.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ride/RideSessionService.kt) owns the
  existing long-lived foreground-service lifecycle and currently starts BLE, sensors, and packet
  publication. Reuse its lifecycle/wake-lock lessons, but do not copy its Ferrostar route ownership
  into the Phase 1 notification adapter.

### Phone GNSS and heading

*Was SPEC §15.2.*

- [RideSessionService.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ride/RideSessionService.kt) already
  requests fused location updates, converts location speed from m/s to km/h, and combines sensor
  values before BLE publication. This is the starting point for the phone GNSS passthrough path.
- [RideSensorsStateHolder.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/location/RideSensorsStateHolder.kt)
  is the existing small shared holder for nullable speed and heading. Extend the data contract with
  fix validity and freshness rather than fabricating zeros.
- [CompassTracker.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/location/CompassTracker.kt) contains
  phone rotation-vector heading, magnetic-declination correction, smoothing, and GPS-bearing fallback.
  Reuse the heading math for phone telemetry; the terminal QMI8658 is not an equivalent absolute
  compass.
- [CompassMath.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/location/CompassMath.kt) and its tests are
  the pure-math reference for wrap-safe angle smoothing.

### Notification scraping gap

*Was SPEC §15.3.*

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

### C3 terminal countdown and renderer

*Was SPEC §15.4.*

There is no ESP32 firmware in the repository yet. The following phone UI code is still useful for
the embedded renderer contract:

- [NavDial.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ui/dial/NavDial.kt) defines the existing idle,
  active, rerouting, arrived, speed, and stale-display concepts to port as behavior, not as Compose
  code.
- [DialLayout.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ui/dial/DialLayout.kt) stores fractional
  layout constants suitable for transcription into C/LVGL profile-independent constants.
- [NavDialConfig.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ui/dial/NavDialConfig.kt) shows the
  existing display-toggle model for speed and other elements.
- [RouteLine.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ui/dial/RouteLine.kt) and
  [RouteGeometry.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/nav/RouteGeometry.kt) are **not Phase 1
  dependencies**. They belong to the later vector road-context phase and must not pull route
  geometry into the C3 POC.

The new firmware should add a pure host-testable countdown module. Its inputs are normalized
notification distance, phone speed, packet age, maneuver sequence, and local monotonic time. Its
output is estimated distance plus freshness/state flags. Keep this math independent of LVGL and BLE.

### Offline maps and routing: existing code, explicitly later

*Was SPEC §15.5.*

Offline functionality exists in the Android app, but none of it belongs in Phase 1 or the C3:

- [OfflineMapTiles.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/map/OfflineMapTiles.kt) implements the
  phone-side MapLibre `pmtiles://file://` loading path for the route-selection map.
- [RouteSelectionMapScreen.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/map/RouteSelectionMapScreen.kt)
  is the phone-side route-selection map UI, not a terminal navigation surface.
- [OfflineTileDownloader.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ride/OfflineTileDownloader.kt)
  downloads the phone-side Valhalla tile tarball using a temporary file and atomic rename.
- [LocalRouteProvider.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ride/LocalRouteProvider.kt) is the
  phone-side local Valhalla bridge. It is not firmware code and must not be ported to the C3.
- [OfflineMapTilesTest.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/test/java/com/motonav/app/map/OfflineMapTilesTest.kt) is the
  reference for testing the phone-side PMTiles path.

These files are references for later smartphone offline work only. Phase 1 must not import their
dependencies, data formats, tile files, or route geometry into the embedded project.

### Navigation models and later migration

*Was SPEC §15.6.*

- [Maneuver.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/nav/Maneuver.kt) contains the current maneuver
  bucketing concept. Reuse the stable bucket meanings when mapping notification text, but do not
  rely on Kotlin enum ordinals as the long-term wire contract.
- [Geocoding.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/nav/Geocoding.kt) is phone-side destination
  search logic and is out of Phase 1.
- [Elevation.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/nav/Elevation.kt) is phone-side Valhalla
  elevation logic and is out of Phase 1.
- [RouterBackend.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/ride/RouterBackend.kt) and
  [SettingsStore.kt](https://github.com/Hayden-Schmidt/OpenApex/blob/main/app/src/main/java/com/motonav/app/settings/SettingsStore.kt) describe
  configurable phone routing providers. They are later phone-app references, not C3 requirements.

### Porting rule

*Was SPEC §15.7.*

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
