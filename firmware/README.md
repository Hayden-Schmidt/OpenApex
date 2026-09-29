# OpenApex ESP-IDF firmware

Phase 1 target: `PROTOTYPE_C3_GC9A01`.

This skeleton includes:

- ESP-IDF project structure targeting `esp32c3`.
- FreeRTOS task placeholders for BLE, countdown processing, and GUI.
- Pure C countdown processor with a host test.
- No local maps, routing, GNSS dependency, or S3-only hardware assumptions.

Run from an ESP-IDF-exported terminal:

```powershell
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

The GC9A01 driver, BLE packet decoder, and board pin profile are intentionally next work after the
prototype board is physically verified.

Until then, use `firmware/sim_lvgl` for visual work and `scripts/build-countdown.ps1` for
countdown/runtime host tests.

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
BLE (docs/OpenApex_SPEC.md §5.6). Settings and a custom avatar are saved to NVS, so both apply
from the splash on every boot with no phone connected.

The nine preset avatars are the artist's pixel art in `design reference/Characters`, transcribed
with their palettes into `gui/avatars_data.h` (spec §5.6 covers how themed colours follow the accent).

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
