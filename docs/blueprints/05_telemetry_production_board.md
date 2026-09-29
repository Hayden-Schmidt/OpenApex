# 05 — Advanced telemetry and production reference board

**Status:** FUTURE  
**Last reconciled with code:** not yet  
**Reference docs:** [hardware](../reference/hardware.md)

| Phase | Name | Status | Evidence |
|---|---|---|---|
| 05.1 | Waveshare 1.75 AMOLED S3 profile and drivers | NOT STARTED |  |
| 05.2 | Terminal GNSS, IMU, RTC and power hardware | NOT STARTED |  |
| 05.3 | Telemetry comparison and diagnostics | NOT STARTED |  |
| 05.4 | Dock power and enclosure testing | NOT STARTED |  |

The S3 reference board is not in hand. Blueprint 04's relay phase (04.4) needs ESP-NOW
hardware from here.

## Scope

*Was SPEC §11 Phase 2.*

- Add the Waveshare 1.75 AMOLED S3 profile and CO5300/CST9217 drivers.
- Add LC76G UART support on the 1.75-GPS variant.
- Add QMI8658 motion wake, tilt, and optional COG smoothing.
- Add PCF85063 RTC and AXP2101 power controls where needed.
- Add optional phone/terminal telemetry comparison and diagnostics.
- Add dock power testing and weather/UV/vibration enclosure testing.

## Open decisions

*Was SPEC §13 item 6.*

6. Confirm dock pin count, wake/ID pin requirements, and external buck converter packaging.
