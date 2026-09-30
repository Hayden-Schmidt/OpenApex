# OpenApex ESP-IDF firmware

Phase 1 target: `PROTOTYPE_C3_GC9A01`.

Layout:

- `main/` -- ESP-IDF firmware. Pure-C, host-tested core (`packet`, `packet_merge`, `normalize`,
  `pipeline`, `countdown`, `heading_fusion`, `ancs_core`, `settings`, `call_state`) plus the platform
  glue around it: the NimBLE peripheral (`ble_link.c`), the iOS ANCS/CTS client (`apple_client.c`,
  `ancs_bridge.c`), NVS stores, and the GC9A01 display and CST816 touch drivers. The rider mesh
  (`mesh*.c`) is in development.
- `gui/` -- the C++ LVGL GUI, reached only through `gui_app.hpp` and `terminal_view_state_t`.
- `sim_lvgl/` -- the desktop simulator for visual work.
- `test_host/` -- host tests, run by `scripts/run-host-tests.ps1`.

Tasks, queues and the one-writer rule are described in
[system_overview](../docs/reference/system_overview.md) and at the top of `main/main.c`.

Build and flash with PlatformIO from the repo root:

```powershell
pio run -d firmware -e prototype_c3 -t upload
```

or from an ESP-IDF-exported terminal:

```powershell
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## Drive logging (development only)

Lets a drive happen with no laptop attached: the terminal writes what it decoded and displayed to a
flash partition, the phone writes what it read and sent to a JSONL file, and the two are joined
afterwards on the packet `seq`.

**Not in the default build.** `default_envs = prototype_c3` has no logger; the code compiles to
no-op inlines without `-DOPENAPEX_DRIVE_LOG=1`. Only the separate `prototype_c3_devlog` env turns it
on. The Android half is gated the same way, on `BuildConfig.DEBUG`.

Flash the logging build:

```powershell
pio run -d firmware -e prototype_c3_devlog -t upload
```

After the drive, pull both sides and decode:

```powershell
# Terminal: 1 MB `logs` partition at 0x210000 (see partitions.csv)
esptool read_flash 0x210000 0x100000 esp_log.bin

# Phone
adb pull /sdcard/Android/data/org.openapex.androidauto/files/drivelogs/

python tools/decode_drive.py esp_log.bin --phone drive-<timestamp>.jsonl
```

Replay a capture through the current parser to see whether a change regressed a real drive:

```powershell
gcc -std=c11 -o replay firmware/test_host/replay.c firmware/main/normalize.c `
    firmware/main/packet.c firmware/main/countdown.c firmware/main/pipeline.c `
    firmware/main/heading_fusion.c -lm
./replay esp_log.bin
```

Mismatches are printed where the current code disagrees with the maneuver the terminal actually
displayed on the road.

Captures contain the rider's route, addresses and timestamps. Scrub before committing one as a
fixture.

## UI settings

The phone configures the accent colour, avatar, ETA format, clock, units and default dial ring over
BLE ([settings_and_avatar](../docs/reference/settings_and_avatar.md)). Settings and a custom avatar are saved to NVS, so both apply
from the splash on every boot with no phone connected.

The nine preset avatars are the artist's pixel art in `design/characters`, transcribed
with their palettes into `gui/avatars_data.h` ([settings_and_avatar](../docs/reference/settings_and_avatar.md) covers how themed colours follow the accent).

Host test for the settings codec:

```powershell
gcc -std=c11 -Ifirmware/main -o settings_test firmware/test_host/settings_test.c firmware/main/settings.c
./settings_test
```

Preview settings in the simulator using a TLV in hex. This shows the orange owl preset in imperial
units; for a custom avatar pass `--settings 010201ff --avatar <160 hex digits>`:

```powershell
program.exe --settings 010103ff8000020102050101
```
