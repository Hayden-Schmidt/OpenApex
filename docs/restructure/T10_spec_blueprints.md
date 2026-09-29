# T10 — SPEC → blueprints 01, 05, 06; distribute §13

Read `00_CONTEXT.md` first. Use the **extraction procedure** in `T05_spec_system_nav.md` (read that
section only). After this task the SPEC should be nothing but headings and stubs.

**Do not reconcile status here.** Move the text verbatim; T11 checks it against the code. Every
blueprint gets **Last reconciled with code: not yet**.

## Blueprint 01 — `docs/blueprints/01_nav_terminal_core.md`

Title: "01 — Nav terminal core (C3 notification + GNSS display)". Status: `IN PROGRESS`
(provisional, and T11 sets it).

Sources, in this order:

| Source | Section there |
|---|---|
| §2 intro, §2.1 "Phase 1 does", §2.2 "explicitly does not do" | Scope |
| §11 "Current status (2026-09-21)" | Status history (keep the date in the heading; T11 writes a new current status above it) |
| §11 "Recommended next steps" | Next steps (as of 2026-09-21) |
| §11 "Phase 1: C3 notification/GNSS display POC" checklist + exit test | Checklist; Exit test |
| §12 Acceptance Criteria | Acceptance criteria |

Phase table: provisional. The SPEC calls this work "Phase 1" and never split it. Propose phases
from the checklist's natural groups, for example:

- 01.1 Firmware skeleton and BLE packet path
- 01.2 Android source adapter
- 01.3 Countdown
- 01.4 On-device GUI
- 01.5 Physical display and mechanical prototype

Mark every status as `(T11)`. T11 fills them. Rename "Phase 1" mentions inside the moved text only
where they mean this blueprint, as `blueprint 01` (for example "Phase 1 exit test" → "Exit test");
leave quoted history alone.

## Blueprint 05 — `docs/blueprints/05_telemetry_production_board.md`

Title: "05 — Advanced telemetry and production reference board". Status `FUTURE`. Source: §11
"Phase 2". Its bullets become phases 05.1… (one per bullet group), all `NOT STARTED`. Note in the
Scope section that the S3 reference board is not in hand. Blueprint 04's relay stage (04.4)
depends on S3/ESP-NOW hardware from here; cross-link it.

## Blueprint 06 — `docs/blueprints/06_road_context.md`

Title: "06 — Road context". Status `FUTURE`. Sources: §11 "Phase 3", and §5.4 "Future polyline
stream". Phases from the bullets, all `NOT STARTED`. Keep the "full offline maps remain out of
scope" paragraph as Non-goals.

## §13 Open Decisions

Go through each item. Note: there are two items numbered 8; keep them distinct as 8a and 8b in
the mapping.

- **RESOLVED items (struck through):** move them to the owning blueprint's "Resolved decisions"
  section verbatim.
- **Open items:** move each to the blueprint (or reference doc) it belongs to, under "Open
  decisions". Rough guide:
  - board / dock / packaging → 01 or 05
  - iOS ANCS → 03 (`blueprints/03_ios_multiplatform.md`; if T12 has not run, put it in 01 with a
    carry-over note to move it)
  - packet layout → `reference/ble_link.md` "Open decisions"
  - CDM cold wake (item 9) → 01, linking `investigations/CDM_Cold_Wake_Investigation.md`
- Code references §13 items 8, 9 (`§13.8`, `§13.9`) and "§13" generally. Fill in the "§13 item
  mapping" table in `00_CONTEXT.md` for every item, so T14 can rewrite those references.
- Open items carried over from T09 (listed in Carry-over notes as `OPEN ITEM`): assign each to a
  blueprint's "Open decisions" and mark the note `→ assigned to NN`.

## Done when

- The three blueprints exist with the header from `00_CONTEXT.md`.
- §2, §5.4, §11, §12 and §13 are stubs. Read the whole SPEC top to bottom: every section is a
  heading plus one stub line, or the preamble.
- The §13 mapping table is complete.
- The docs/README.md draft is updated.
- `00_CONTEXT.md` is updated and one commit is made. Docs only.
