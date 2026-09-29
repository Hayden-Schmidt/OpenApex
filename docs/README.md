# OpenApex docs

Three kinds of doc, in three folders:

- **`reference/`** — how it works *now*. Kept true to the code; if code and reference disagree,
  one of them is a bug.
- **`blueprints/`** — numbered plans. Each has a status block and numbered phases (`NN.1`, `NN.2`…).
- **`investigations/`** — dated logs and research. Frozen once closed.

New here? Read [reference/system_overview.md](reference/system_overview.md), then
[reference/DEVELOPMENT_SETUP.md](reference/DEVELOPMENT_SETUP.md). Designs are in
[../design/](../design/README.md).

## Blueprints

| # | Blueprint | Status | Phases done | Last reconciled |
|---|---|---|---|---|
| 01 | [Nav terminal core](blueprints/01_nav_terminal_core.md) | IN PROGRESS | 01.1-01.4 of 5 (01.5 partial: mechanical prototype) | 2026-09-29 |
| 02 | [Heading sensor fusion](blueprints/02_heading_sensor_fusion.md) | IN PROGRESS | 02.1-02.2 of 3 (tuning and road verification left) | 2026-09-29 |
| 03 | [iOS + Android multiplatform](blueprints/03_ios_multiplatform.md) | IN PROGRESS | 03.1-03.2 of 4 (03.3-03.4 need a Mac / iPhone) | not yet |
| 04 | [Rider mesh](blueprints/04_rider_mesh.md) | PLANNED | 0 of 4 (04.4 deferred) | not yet |
| 05 | [Telemetry and production board](blueprints/05_telemetry_production_board.md) | FUTURE | 0 of 4 | not yet |
| 06 | [Road context](blueprints/06_road_context.md) | FUTURE | 0 of 3 | not yet |

## Reference

| Doc | Covers |
|---|---|
| [system_overview](reference/system_overview.md) | Scope, system boundary, source adapters, where normalization lives, firmware runtime and concurrency |
| [nav_pipeline](reference/nav_pipeline.md) | Notification normalization, countdown |
| [ble_link](reference/ble_link.md) | Phone ↔ terminal BLE: Android path, identifiers, incoming calls |
| [ios_ancs](reference/ios_ancs.md) | The iOS / ANCS path |
| [settings_and_avatar](reference/settings_and_avatar.md) | Settings TLV, device info, avatar upload |
| [rider_mesh_v1](reference/rider_mesh_v1.md) | The rider mesh as built (wire v1); blueprint 04 plans v2 |
| [gui_and_display](reference/gui_and_display.md) | Display behaviour, GUI architecture (tiers, screens, themes) |
| [hardware](reference/hardware.md) | Board profiles, GNSS/sensors, power and mechanical |
| [decisions](reference/decisions.md) | Architecture decision records ADR-1…ADR-5 |
| [licensing_and_porting](reference/licensing_and_porting.md) | Licensing notes, porting references to the old MotoNav repo |
| [Phone_App_Architecture](reference/Phone_App_Architecture.md) | The KMP phone app |
| [DEVELOPMENT_SETUP](reference/DEVELOPMENT_SETUP.md) | Toolchains, builds, tests, VS Code tasks |

## Investigations

| Log | Status |
|---|---|
| [CDM cold-wake investigation](investigations/CDM_Cold_Wake_Investigation.md) | Open |
| [2026-09-24 Android 16 ProgressStyle extras](investigations/2026-09-24_android16_progressstyle_extras.md) | Closed |
| [2026-09-24 Ride capture findings](investigations/2026-09-24_ride_capture_findings.md) | Closed |
| [2026-09-24 Nav arrow join quantisation](investigations/2026-09-24_nav_arrow_join_quantisation.md) | Closed (has "What remains") |
| [2026-09-26 On-device session](investigations/2026-09-26_on_device_session.md) | Closed |
| [2026-09-28 Capture ride plan](investigations/2026-09-28_capture_ride_plan.md) | Plan for a capture ride |

## Conventions

**Blueprint header** (every blueprint starts with this):

```markdown
# NN — <Title>

**Status:** COMPLETE | IN PROGRESS | PLANNED | FUTURE
**Last reconciled with code:** YYYY-MM-DD (or "not yet")
**Reference docs:** links

| Phase | Name | Status | Evidence |
|---|---|---|---|
| NN.1 | … | DONE / PARTIAL / NOT STARTED / DEFERRED / BLOCKED (why) | file, test or commit |
```

A phase is DONE only with evidence. Update the table above when a blueprint's status changes. To
add a blueprint, take the next free number, copy the header, and add a row here.

**Reference header:** `**Kind:** Reference — describes the code as built.` plus where it came from
and when it was last verified against code. **Investigation header:** `**Kind:** Investigation log`
plus its date.

## Where did the SPEC go?

`docs/OpenApex_SPEC.md` was split on 2026-09-29. Old references map as follows. Each moved section
carries a `*Was SPEC §x.*` line under its heading, so you can search for it.

| SPEC § | Now |
|---|---|
| title block, §1, §2.3, §2.4, §8, §8.1 | [reference/system_overview.md](reference/system_overview.md) |
| §2, §2.1, §2.2 | [blueprints/01_nav_terminal_core.md](blueprints/01_nav_terminal_core.md) "Scope" |
| §3, §4 | [reference/nav_pipeline.md](reference/nav_pipeline.md) |
| §5 intro, §5.1, §5.3, §5.5 | [reference/ble_link.md](reference/ble_link.md) |
| §5.2 | [reference/ios_ancs.md](reference/ios_ancs.md) |
| §5.4 | [blueprints/06_road_context.md](blueprints/06_road_context.md) |
| §5.6 | [reference/settings_and_avatar.md](reference/settings_and_avatar.md) |
| §5.7 | [reference/rider_mesh_v1.md](reference/rider_mesh_v1.md) |
| §6, §7, §10 | [reference/hardware.md](reference/hardware.md) |
| §9, §16.6 | [reference/gui_and_display.md](reference/gui_and_display.md) |
| §11 Current status, next steps, Phase 1; §12 | [blueprints/01_nav_terminal_core.md](blueprints/01_nav_terminal_core.md) |
| §11 Phase 2 | [blueprints/05_telemetry_production_board.md](blueprints/05_telemetry_production_board.md) |
| §11 Phase 3 | [blueprints/06_road_context.md](blueprints/06_road_context.md) |
| §13 items 1, 2, 5, 7, 8 (both), 9 | [blueprints/01_nav_terminal_core.md](blueprints/01_nav_terminal_core.md) "Open / Resolved decisions" |
| §13 item 3 | [blueprints/03_ios_multiplatform.md](blueprints/03_ios_multiplatform.md) "Open decisions" |
| §13 item 4 | [reference/ble_link.md](reference/ble_link.md) "Open decisions" |
| §13 item 6 | [blueprints/05_telemetry_production_board.md](blueprints/05_telemetry_production_board.md) "Open decisions" |
| §14, §15 | [reference/licensing_and_porting.md](reference/licensing_and_porting.md) |
| §16.1-§16.5 | [reference/decisions.md](reference/decisions.md) ADR-1…ADR-5 |
| §17 | [investigations/2026-09-24_ride_capture_findings.md](investigations/2026-09-24_ride_capture_findings.md) |
| §18 | [investigations/2026-09-24_nav_arrow_join_quantisation.md](investigations/2026-09-24_nav_arrow_join_quantisation.md) |
| §19 | [investigations/2026-09-26_on_device_session.md](investigations/2026-09-26_on_device_session.md) |

Other moves the same day: `iOS_Handoff.md` + `iOS_Multiplatform_Status.md` → blueprint 03;
`Heading_Sensor_Fusion_Plan.md` → blueprint 02; `RiderMesh_Phase2_Plan.md` → blueprint 04;
`design reference/` → `../design/`; `Google Icons Archive/` + `Demo/` → `tools/maneuver_icons/`;
`simulator/` retired in favour of `firmware/sim_lvgl`.
