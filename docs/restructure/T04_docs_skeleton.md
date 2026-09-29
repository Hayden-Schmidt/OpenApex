# T04 — Docs skeleton; move the whole non-SPEC docs

Read `00_CONTEXT.md` first. This task moves **whole files** only. The SPEC is untouched here,
except for links to files this task moves.

## Create

- `docs/reference/`, `docs/blueprints/`, `docs/investigations/`
- `docs/README.md`: a **draft** index. T15 finalises it. Sections:
  - **Start here:** what OpenApex is (2 sentences), and where to go for setup
    (`reference/DEVELOPMENT_SETUP.md`), the phone app (`reference/Phone_App_Architecture.md`),
    the firmware (`firmware/README.md`), and designs (`../design/README.md`).
  - **Blueprints:** a table of the six blueprints from `00_CONTEXT.md`, with status. Files that do
    not exist yet are listed as "(pending Tnn)".
  - **Reference:** one line per file (list the ones that exist; the rest as pending).
  - **Investigations:** one line per file.
  - **Conventions:** status words and headers, copied from `00_CONTEXT.md` (the blueprint header,
    reference header and investigation header), so they outlive this pack.

## Move (`git mv`, whole files)

| From | To | Also |
|---|---|---|
| `docs/DEVELOPMENT_SETUP.md` | `docs/reference/DEVELOPMENT_SETUP.md` | Reference header |
| `docs/Phone_App_Architecture.md` | `docs/reference/Phone_App_Architecture.md` | Reference header (keep its own status line below it) |
| `docs/CDM_Cold_Wake_Investigation.md` | `docs/investigations/CDM_Cold_Wake_Investigation.md` | Investigation header; it is still *unresolved*, so write "open" instead of "frozen" |
| `docs/Capture_Ride_Plan.md` | `docs/investigations/2026-09-28_capture_ride_plan.md` | Investigation header |
| `docs/Android16_ProgressStyle_Notification_Extras.md` | `docs/investigations/2026-09-24_android16_progressstyle_extras.md` | Investigation header |
| `docs/Heading_Sensor_Fusion_Plan.md` | `docs/blueprints/02_heading_sensor_fusion.md` | Blueprint header (below) |

Keep the file names of the two reference docs, since they are linked from many places. Dates for
the investigations are their last-commit dates; check with `git log -1 --format=%ad --date=short`
and correct them if they differ.

**Blueprint 02 header.** Status COMPLETE. The doc already says "implemented
(`firmware/main/heading_fusion.c` + `heading_store.c`, packet v3)", so use that as the evidence.
Phases: if the doc has natural steps (for example "data collection" then "filter"), list them as
02.1, 02.2 … all DONE with evidence. Otherwise use one phase, 02.1. Last reconciled with code:
today, **only** if you checked those two files exist and are built (`firmware/main/CMakeLists.txt`);
otherwise "not yet".

## Fix references to moved files

`git grep -n` each old file name. Known references today:

- `Heading_Sensor_Fusion_Plan`: `firmware/main/CMakeLists.txt`, `heading_fusion.c/.h`,
  `heading_store.h`, `packet.h`, `phone/…/RelayRecorder.kt`, `RelayService.kt`, `RawNotifPacket.kt`
- `Phone_App_Architecture`: `README.md`, `DEVELOPMENT_SETUP.md`, `OpenApex_SPEC.md`,
  `iOS_Handoff.md`, `firmware/main/ble_link.c`
- `DEVELOPMENT_SETUP`: `README.md`, `design/DESIGN_NOTES.md`
- `CDM_Cold_Wake`: `Phone_App_Architecture.md`, `firmware/main/ble_link.c`,
  `phone/…/NavNotificationRelayService.kt`
- `Capture_Ride_Plan`: `OpenApex_SPEC.md`
- `Android16_ProgressStyle`: `phone/…/NavNotificationRelayService.kt`

In code, only comments change. Update relative Markdown links **inside** the moved docs, since
they are now one level deeper (`../` prefixes). Section references such as `§3` inside
`Phone_App_Architecture.md` refer to that doc itself; leave them.

## Not moved here

- `OpenApex_SPEC.md` — T05-T10 extract it; T14 deletes it.
- `iOS_Handoff.md`, `iOS_Multiplatform_Status.md` — T12.
- `RiderMesh_Phase2_Plan.md` — T13.

## Done when

- The three folders and the draft `docs/README.md` exist.
- `git grep -n -E "Heading_Sensor_Fusion_Plan|docs/DEVELOPMENT_SETUP|docs/Phone_App_Architecture|CDM_Cold_Wake_Investigation|Capture_Ride_Plan|Android16_ProgressStyle"`
  shows only new paths, or hits in this pack.
- Relative links inside the moved docs resolve. Check each `](…)` target exists on disk.
- `powershell -NoProfile -File scripts/build-countdown.ps1` passes (comments changed in
  `CMakeLists.txt`) and the phone build passes (comments in Kotlin).
- `00_CONTEXT.md` is updated and one commit is made.
