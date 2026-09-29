# Hardware

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §6, §7, §10 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## Hardware Profiles

*Was SPEC §6.*

Firmware uses compile-time profiles. Board-specific pins and drivers must not leak into the
navigation model or packet decoder. Every profile declares its SoC, display, touch, GNSS, IMU, RTC,
PMIC, buttons, memory, and power capabilities.

### Phase 1 prototype: `PROTOTYPE_C3_GC9A01` generic C3 round display

*Was SPEC §6.1.*

Target listing: AliExpress item `1005006194839720`.

- ESP32-C3, single-core RISC-V target. Seller descriptions calling it dual-core are incorrect.
- 1.28-inch round 240x240 GC9A01 IPS LCD.
- Touch and non-touch product variants. The delivered board is the touch variant (CST816 at I2C
  0x15, confirmed working; `firmware/main/touch_driver.c`).
- No onboard GNSS or IMU assumed.
- Phone GNSS passthrough is therefore the only Phase 1 telemetry source.

Buyer-reported touch-variant pins are provisional until checked against the delivered board:

| Function | Reported pin |
|---|---:|
| GC9A01 SCLK | GPIO6 |
| GC9A01 MOSI | GPIO7 |
| GC9A01 DC | GPIO2 |
| GC9A01 CS | GPIO10 |
| GC9A01 reset | board-dependent / not confirmed |
| LCD backlight | GPIO3 |
| CST816/CST816D SDA | GPIO4 |
| CST816/CST816D SCL | GPIO5 |
| CST816/CST816D interrupt | GPIO0 |
| CST816/CST816D reset | GPIO1 |

The touch driver must compile out for the non-touch variant. GPIO maps must remain configurable.
Brownout behavior, reset behavior, backlight control, and actual display orientation are Phase 1
bring-up checks.

### Single-core expansion targets

*Was SPEC §6.2.*

The firmware architecture must also support other single-core ESP32 families, including ESP32-C6,
through separate profiles such as `GENERIC_C6_GC9A01`. C3/C6 support is not allowed to assume dual-
core scheduling or S3-only peripherals. Unsupported peripherals must compile out cleanly.

### Reference production target: Waveshare 1.75 AMOLED

*Was SPEC §6.3.*

Target board: `ESP32-S3-Touch-AMOLED-1.75` and `ESP32-S3-Touch-AMOLED-1.75-G` GPS variant.

- ESP32-S3R8, Xtensa LX7 dual-core, up to 240 MHz.
- 8 MB stacked PSRAM and 16 MB external NOR flash.
- 1.75-inch circular AMOLED, 466x466, approximately 700 cd/m2.
- CO5300 display driver over QSPI.
- CST9217 capacitive touch over I2C.
- QMI8658 6-axis IMU over I2C.
- PCF85063 RTC with backup power path.
- AXP2101 power-management IC.
- TF/microSD slot for later logging or data features.
- GPS variant includes onboard Quectel LC76G and IPEX1 antenna connection.

The GPS variant is not required for Phase 1. Its GNSS driver belongs to the later telemetry phase.

## Optional GNSS and Sensor Hardware

*Was SPEC §7.*

### Terminal GNSS

*Was SPEC §7.1.*

Some profiles may enable `GNSS_LC76G_UART` or `GNSS_EXTERNAL_NMEA_UART`. The LC76G supports
GPS, GLONASS, Galileo, and BeiDou reception. Terminal GNSS is supplemental telemetry and later
verification input; it is not the Phase 1 navigation authority.

### Heading

*Was SPEC §7.2.*

QMI8658 is a 6-axis accelerometer/gyroscope and is not an absolute compass. Absolute heading is
provided by phone magnetometer data or GNSS course-over-ground while moving above approximately
3 km/h. Below that threshold, heading is unknown or held with an explicit stale indicator.

### Dormant audio and peripherals

*Was SPEC §7.3.*

The 1.75 board contains ES7210/ES8311-class audio hardware, microphones, echo-cancellation
circuitry, and speaker connections. Audio is permanently disabled in firmware configuration. It is
not initialized, scheduled, rendered, or used for navigation.

## Power and Mechanical Design

*Was SPEC §10.*

```text
switched motorcycle 12 V -> 2 A fuse -> external waterproof 12-to-5 V buck
                         -> weather-sealed handlebar pogo dock -> terminal
```

- No buck converter or voltage transformation occurs inside the sealed terminal.
- Power is coupled to switched ignition; key-off removes dock power.
- Use a 2-pin or 4-pin recessed spring-loaded pogo interface. Mechanical bayonet or twist-lock
  retention carries wind and vibration loads; pogo contacts carry electrical current only.
- Use a dummy cap or low-side MOSFET arrangement to prevent wet exposed contacts shorting.
- The terminal has no internal LiPo requirement. The 3.7 V battery header on some development
  boards is not part of the motorcycle terminal assembly.
- Immediate power loss must be safe. Active navigation uses RAM state and does not require a
  shutdown sequence or filesystem transaction.
- ASA or polycarbonate is preferred for the shell. A metal bezel requires an RF-transparent window
  over the ESP32 antenna region.
- A 10 mm sun hood may reduce overhead glare but must not block the display, antenna, touch surface,
  or dock retention.
