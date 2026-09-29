# 06 — Road context

**Status:** FUTURE  
**Last reconciled with code:** not yet  
**Reference docs:** [ble_link](../reference/ble_link.md)

| Phase | Name | Status | Evidence |
|---|---|---|---|
| 06.1 | Stream downsampled route coordinates from the phone | NOT STARTED |  |
| 06.2 | Heading-relative road-context line on the terminal | NOT STARTED |  |
| 06.3 | Versioned polyline chunks | NOT STARTED |  |

## Scope and non-goals

*Was SPEC §11 Phase 3.*

Only after Phase 1 display reliability and Phase 2 hardware support are complete:

- stream downsampled vector route coordinates from the smartphone;
- draw a heading-relative road-context line on the terminal;
- negotiate/version polyline chunks separately from the core notification packet.

Full offline maps remain out of scope for the terminal. Offline routing and map selection, if used,
remain smartphone-side features.

## Future polyline stream

*Was SPEC §5.4.*

Route context is Phase 2 or later. A future smartphone companion may downsample a 500 m to 1 km
route-ahead buffer into local `(delta_x, delta_y)` points and stream them in BLE chunks every 1-2
seconds. This is explicitly excluded from Phase 1.
