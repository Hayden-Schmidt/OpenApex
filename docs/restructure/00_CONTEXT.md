# Docs and repo restructure — shared context

Every restructure session reads this file first, then **one** task file (`T01`…`T15`). Nothing else
is needed to start. The task file says which other files to open.

To start a session, paste:

> Read `docs/restructure/00_CONTEXT.md`, then do `docs/restructure/TNN_<name>.md`. Follow both
> exactly.

## Why

OpenApex is a phone-to-ESP32 motorcycle navigation terminal: a Kotlin Multiplatform phone app
(`phone/`) relays navigation notifications and telemetry over BLE to an ESP32-C3 round-display
terminal (`firmware/`), which also runs a rider-to-rider mesh. The docs grew by accretion:

- `docs/OpenApex_SPEC.md` (about 1,370 lines, still titled "MotoNav") mixes living reference,
  plans, a roadmap whose status block is dated 2026-09-21, and dated investigation logs (§17-19).
  Its section order is 14, 15, 16 after 13.
- "Phase" means different things in the SPEC roadmap (Phase 1-3), the iOS work (Phase A-D) and the
  rider-mesh plan (Stages 1-4).
- Some repo folders are misleading. `Google Icons Archive/` is live source, `Demo/` is the
  reference maneuver renderer, and `simulator/` is superseded by `firmware/sim_lvgl`.

The goal: a new reader, human or Claude, can find what is built, what is planned and what is done,
without reading everything.

## Target layout

```
docs/
  README.md                  index: every doc, one line each, and every blueprint's status
  reference/                 how it works NOW. Kept true to the code.
  blueprints/                numbered plans, each with phases NN.1, NN.2… and a status block
  investigations/            dated logs and research. Frozen once closed
design/                      was "design reference/" (T03)
tools/maneuver_icons/        was "Google Icons Archive/Icon JSON/" + "Demo/" (T02)
```

Removed: `simulator/`, the root `platformio.ini`, the root Gradle wrapper, and the root
`maneuver_icon_debug*.png` (T01). `docs/OpenApex_SPEC.md` is removed in T14.

## Blueprints

| # | File | Phases | Expected status |
|---|---|---|---|
| 01 | `blueprints/01_nav_terminal_core.md` | from SPEC §2 and §11 Phase 1 | reconciled in T11 |
| 02 | `blueprints/02_heading_sensor_fusion.md` | as written | COMPLETE |
| 03 | `blueprints/03_ios_multiplatform.md` | iOS Phase A-D become 03.1-03.4 | IN PROGRESS (03.1, 03.2 done) |
| 04 | `blueprints/04_rider_mesh.md` | Stages 1-4 become 04.1-04.4 | PLANNED |
| 05 | `blueprints/05_telemetry_production_board.md` | from SPEC §11 Phase 2 | FUTURE |
| 06 | `blueprints/06_road_context.md` | from SPEC §11 Phase 3 and §5.4 | FUTURE |

### Blueprint header (every blueprint starts with this)

```markdown
# NN — <Title>

**Status:** COMPLETE | IN PROGRESS | PLANNED | FUTURE
**Last reconciled with code:** YYYY-MM-DD (or "not yet")
**Reference docs:** links to the docs/reference/ files this blueprint builds on

| Phase | Name | Status | Evidence |
|---|---|---|---|
| NN.1 | … | DONE / PARTIAL / NOT STARTED / DEFERRED / BLOCKED (why) | commit, file or test that proves it |
```

- Status words are exactly those, in capitals.
- A phase is DONE only with evidence (a file, a test or a commit).
- IN PROGRESS must say which phases are done in the index line.

### Reference doc header

```markdown
# <Title>

**Kind:** Reference — describes the code as built.
**Moved from:** OpenApex_SPEC.md §x, §y (restructure, YYYY-MM-DD)
**Last verified against code:** not yet
```

### Investigation header

```markdown
# <YYYY-MM-DD> — <Title>

**Kind:** Investigation log — frozen; do not update except to add a closing note.
**Moved from:** OpenApex_SPEC.md §x (restructure, YYYY-MM-DD)
```

## SPEC mapping (old section → new home)

T14 uses this table to rewrite every `SPEC §x` reference in code and docs. Extraction tasks must
keep it accurate. If you place something elsewhere, edit this table in the same commit.

| SPEC § | New home | Heading there |
|---|---|---|
| §1 System Boundary | `reference/system_overview.md` | System boundary |
| §2 intro, §2.1, §2.2 | `blueprints/01_nav_terminal_core.md` | Scope: does / does not |
| §2.3, §2.4 | `reference/system_overview.md` | Platform source adapters; Normalization ownership |
| §3 | `reference/nav_pipeline.md` | Notification normalization |
| §4 | `reference/nav_pipeline.md` | Countdown processing |
| §5 intro, §5.1, §5.3, §5.5 | `reference/ble_link.md` | Android path; BLE identifiers; Incoming calls |
| §5.2 | `reference/ios_ancs.md` | (whole doc) |
| §5.4 | `blueprints/06_road_context.md` | Future polyline stream |
| §5.6 | `reference/settings_and_avatar.md` | (whole doc) |
| §5.7 | `reference/rider_mesh_v1.md` | (whole doc; the mesh as built today) |
| §6, §7, §10 | `reference/hardware.md` | Hardware profiles; GNSS and sensors; Power and mechanical |
| §8, §8.1 | `reference/system_overview.md` | Firmware runtime; Concurrency model |
| §9 | `reference/gui_and_display.md` | Display behavior |
| §11 Current status, Recommended next steps, Phase 1 | `blueprints/01_nav_terminal_core.md` | Status; Phases |
| §11 Phase 2 | `blueprints/05_telemetry_production_board.md` | |
| §11 Phase 3 | `blueprints/06_road_context.md` | |
| §12 | `blueprints/01_nav_terminal_core.md` | Acceptance criteria |
| §13 | per item: see "§13 item mapping" below (filled in by T10) | |
| §14, §15 | `reference/licensing_and_porting.md` | |
| §16.1-§16.5 | `reference/decisions.md` | ADR-1 … ADR-5 (same order) |
| §16.6 | `reference/gui_and_display.md` | GUI architecture (was ADR 16.6) |
| §17 | `investigations/2026-09-24_ride_capture_findings.md` | |
| §18 | `investigations/2026-09-24_nav_arrow_join_quantisation.md` | |
| §19 | `investigations/2026-09-26_on_device_session.md` | |

### §13 item mapping (T10 fills this in)

| §13 item | New home |
|---|---|
| | |

## Rules for every session

1. **One task per session.** Do only what the task file says. If you find more work, add it to
   "Carry-over notes" below; do not do it.
2. **Branch.** Work on `chore/restructure` (see "Owner steps"). Make one commit per task:
   `Restructure TNN: <what>`. Do not push unless the task says so.
3. **Move text verbatim.** When text moves between docs, use `git mv` where a whole file moves.
   Otherwise cut from the old place and paste into the new one. Allowed edits while moving:
   - heading levels
   - "MotoNav" → "OpenApex"
   - bare `§N` → `SPEC §N`, so T14 can find every reference
   - the header block above
   Do **not** rewrite substance or fix stale claims (except in T11 and T13). Log them in
   "Carry-over notes".
4. **Leave a stub where SPEC text left.** Replace a moved SPEC section with one line:
   `Moved to [docs/reference/x.md](reference/x.md) (restructure TNN).` Keep the SPEC heading. The
   SPEC stays navigable until T14 deletes it.
5. **Code is touched only for paths and comments.** Tasks that move tooling (T01-T03) change paths
   in scripts and configs. Otherwise only comments change. No logic changes.
6. **Line endings.** Keep each file's existing endings. New files use LF.
7. **Verify before committing,** using the task's "Done when". Docs-only tasks need no builds.
   Tasks that touch code or tools run the relevant commands:

   ```powershell
   powershell -NoProfile -File scripts/build-countdown.ps1          # firmware host suites
   pio run -d firmware -e prototype_c3                              # C3 firmware build
   pio run -d firmware/sim_lvgl                                     # LVGL simulator build
   cd phone; .\gradlew.bat :shared:jvmTest :androidApp:assembleDebug  # phone
   ```

   If a build fails for a reason unrelated to the task (toolchain missing, etc.), say so in the
   report; do not "fix" it.
8. **Finish by updating this file:** tick the task in "Progress", add carry-over notes, and edit the
   mapping if you changed it. Commit that with the task. End with a short report: what moved, what
   was verified, and any carry-over notes.

## Owner steps (before T01)

- [ ] Decide the base. Recommended: merge `feat/ios-ancs` into `main` first, then branch
      `chore/restructure` from `main`. The restructure moves many files, and doing it while
      `feat/ios-ancs` is unmerged guarantees conflicts.
- [ ] No other feature branch should be active while the restructure runs.

## Progress

| Task | What | Depends on | Done |
|---|---|---|---|
| T01 | Repo cleanup: stray files, root Gradle wrapper, retire `simulator/` | owner steps | [ ] |
| T02 | Move maneuver icon pipeline + reference renderer to `tools/maneuver_icons/` | T01 | [ ] |
| T03 | Reorganise `design reference/` → `design/` | T02 | [ ] |
| T04 | Docs skeleton; move whole non-SPEC docs | T03 | [ ] |
| T05 | SPEC → reference: system overview, nav pipeline, display | T04 | [ ] |
| T06 | SPEC → reference: BLE link, iOS ANCS, settings and avatar | T05 | [ ] |
| T07 | SPEC → reference: rider mesh v1, hardware | T06 | [ ] |
| T08 | SPEC → reference: decisions (ADRs), GUI architecture, licensing and porting | T07 | [ ] |
| T09 | SPEC → investigations: §17-19 | T08 | [ ] |
| T10 | SPEC → blueprints 01, 05, 06; distribute §13 | T09 | [ ] |
| T11 | Reconcile blueprint 01 status against the code | T10 | [ ] |
| T12 | Blueprint 03: merge the two iOS docs | T04 | [ ] |
| T13 | Blueprint 04: rider mesh plan | T04 | [ ] |
| T14 | Rewrite every SPEC reference; delete the SPEC | T10-T13 | [ ] |
| T15 | Final index, READMEs, link check, delete this pack | T14 | [ ] |

T12 and T13 may run any time after T04. Everything else runs in order.

## Carry-over notes

Sessions append here: stale claims found, things out of scope, surprises. T11 and T15 work through
this list.

Seeded when the pack was written (2026-09-29):

- `README.md` "Hardware path" says "touch or non-touch variant". The owner has scoped non-touch
  out. → T15
- `README.md` "Hardware-free development" names `firmware/gui/gui_screens.c`, which was removed
  when the GUI was rebuilt in C++ (SPEC §11 "Recommended next steps" item 2). → T15
- SPEC §11 "Current status (2026-09-21)" and the Phase 1 checklist predate the GUI, mesh, heading
  fusion and iOS work. → T11
- SPEC §12 "builds with touch enabled or disabled": non-touch is scoped out. → T11 (mark
  DEFERRED, don't delete)
- `docs/DEVELOPMENT_SETUP.md` "Verified working (2026-09-21)" figures (RAM 3.3 %, Flash 14.3 %)
  are stale; the rider-mesh plan quotes C3 flash at 64.9 %. → T15
- SPEC §13 has two items numbered 8. → T10 (8a/8b)
