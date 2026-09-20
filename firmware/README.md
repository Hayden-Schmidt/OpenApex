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
