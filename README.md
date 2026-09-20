# OpenApex

A phone-assisted motorcycle navigation terminal for high-visibility round ESP32 displays.

## Phase 1

OpenApex Phase 1 is a notification and phone-GNSS passthrough proof of concept targeting the
`PROTOTYPE_C3_GC9A01` 1.28-inch round display.

The phone reads supported Google Maps and Apple Maps navigation notifications, normalizes the
maneuver and distance data, and sends it with phone GNSS telemetry over BLE. The C3 parses the
packet, performs local distance countdown processing between notification updates, and renders a
high-contrast dial.

Phase 1 does not include local maps, map tiles, route calculation, rerouting, map matching,
polylines, offline routing, or terminal-side GNSS navigation.

See [docs/OpenApex_SPEC.md](docs/OpenApex_SPEC.md) for the complete architecture, hardware
profiles, BLE contract, countdown model, and development roadmap.

See [docs/DEVELOPMENT_SETUP.md](docs/DEVELOPMENT_SETUP.md) for Android SDK, Gradle, ESP-IDF,
host-test, and VS Code task setup.

## Hardware-free development

You can prototype before the C3 arrives:

- Open `simulator/index.html` directly for the interactive round-screen simulator.
- Run the VS Code task `Simulator: open round display`.
- Run the PlatformIO native environment with `pio run -e native` once the PlatformIO extension has
    installed/activated its CLI.

The native target and browser simulator use the same countdown implementation from
`firmware/main/countdown.c`. The browser is the visual target; PlatformIO native is the code/test
target. The C3 environment is ready for the physical board once its pin profile and display driver
are added.

## Hardware path

- Phase 1: generic ESP32-C3, 240x240 GC9A01 round display, touch or non-touch variant.
- Expansion: ESP32-C6 and other single-core profiles.
- Production reference: Waveshare ESP32-S3R8 1.75-inch AMOLED with CO5300, CST9217, QMI8658,
  PCF85063, AXP2101, and optional LC76G GPS.

## Repository boundary

This is the clean Phase 1 repository. The previous MotoNav repository remains the historical
reference for reusable Android BLE, sensor, packet-test, dial-layout, and later offline-map ideas.
Its Ferrostar/Valhalla and offline-map implementation is intentionally not included here.

## License

MIT. See [LICENSE](LICENSE).
