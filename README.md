# OpenApex

A phone-assisted motorcycle navigation terminal for high-visibility round ESP32 displays.

The phone reads Google Maps and Apple Maps navigation notifications and relays them, with phone
GNSS telemetry, over BLE to the terminal. The terminal normalizes the maneuver, counts the distance
down between notification updates, and renders a high-contrast round dial. Terminals also talk to
each other over a rider-to-rider mesh (friends on the compass).

No local maps, tiles, route calculation, rerouting, map matching or terminal-side navigation.

## Start here

- **[docs/README.md](docs/README.md)** — the docs index: reference docs (how it works now),
  numbered blueprints with their status, and investigation logs.
- [docs/reference/DEVELOPMENT_SETUP.md](docs/reference/DEVELOPMENT_SETUP.md) — Android SDK, Gradle,
  ESP-IDF, host tests and VS Code tasks.
- [design/README.md](design/README.md) — screen designs and art.

## Layout

| Path | What |
|---|---|
| `firmware/` | ESP-IDF terminal firmware (`main/`), the C++ LVGL GUI (`gui/`), host tests (`test_host/`), the SDL GUI simulator (`sim_lvgl/`) |
| `phone/` | Kotlin Multiplatform companion app: `shared/` (wire formats, BLE queue, mesh sync) and `androidApp/` |
| `docs/` | Reference, blueprints, investigations — see `docs/README.md` |
| `design/` | Figma exports, notes, avatar art, status-icon SVGs |
| `tools/` | Icon pipelines (`maneuver_icons/`, `icons/`) and drive/ANCS log decoders |
| `scripts/` | Setup helpers, `run-host-tests.ps1`, the BLE link test |

## Quick checks

```powershell
powershell -NoProfile -File scripts/run-host-tests.ps1            # firmware host suites
pio run -d firmware -e prototype_c3                               # C3 firmware
pio run -d firmware/sim_lvgl                                      # GUI in an SDL window
cd phone; .\gradlew.bat :shared:jvmTest :androidApp:assembleDebug  # phone
```

## Hardware

- Current: generic ESP32-C3 with a 240x240 GC9A01 round touch display (CST816).
- Production reference (not yet in hand): Waveshare ESP32-S3R8 1.75-inch AMOLED with CO5300,
  CST9217, QMI8658, PCF85063, AXP2101 and optional LC76G GPS.

## Repository boundary

The previous MotoNav repository remains the historical reference for reusable Android BLE, sensor,
packet-test, dial-layout, and later offline-map ideas. Its Ferrostar/Valhalla and offline-map
implementation is intentionally not included here.

## License

MIT. See [LICENSE](LICENSE).
