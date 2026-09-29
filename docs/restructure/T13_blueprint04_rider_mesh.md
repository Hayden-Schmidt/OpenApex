# T13 — Blueprint 04: rider mesh plan

Read `00_CONTEXT.md` first. It can run any time after T04.

## Input

`docs/RiderMesh_Phase2_Plan.md` (revision 2, 2026-09-29). All of its open questions are answered
(see its Decisions log). It is the agreed design and is **not** to be redesigned here.

## Target: `docs/blueprints/04_rider_mesh.md` (`git mv`, then edit)

1. **Header.** Blueprint header from `00_CONTEXT.md`. Status `PLANNED`. Reference docs:
   `reference/rider_mesh_v1.md` (the mesh as built), `reference/settings_and_avatar.md`.

   | Phase | Was | Status |
   |---|---|---|
   | 04.1 | Stage 1 — private friending | NOT STARTED |
   | 04.2 | Stage 2 — group rides (1-hop) | NOT STARTED |
   | 04.3 | Stage 3 — leader nav share (1-hop) | NOT STARTED |
   | 04.4 | Stage 4 — multi-hop relay | DEFERRED (needs ESP-NOW hardware, blueprint 05) |

2. **Rename throughout:** "Stage N" → "04.N". Section numbers `§1.x`, `§2.x` … inside the doc become
   `§04.1.x` or similar. Pick one scheme and apply it to every internal reference, then grep for
   leftovers. Remove the note explaining why it says "Stages" rather than "phases".
3. **"Before any of this starts":** item 1 (restructure lands first) is now done. Replace it with
   "Restructure: done (T01-T15)". Keep items 2 (screenless C3 test partner) and 3 (C3 heap
   measurement).
4. **Remove "Queued: docs and repo restructure"** at the end (this pack replaces it).
5. **References:** `docs/OpenApex_SPEC.md §5.7` → `reference/rider_mesh_v1.md`; `SPEC §5.6` →
   `reference/settings_and_avatar.md`; `SPEC §14` → `reference/licensing_and_porting.md`. Check
   the mapping in `00_CONTEXT.md` for any other `SPEC §`.
6. **Design links:** the U-table's "Design link" column and the "Designs live under …" sentence →
   `design/screens/07-rider-mesh/` (created by T03), with the old friend prompt at
   `design/screens/06-add-friend/`.
7. **Deferred hardening H1** stays as is. It is referenced from 04.2 and 04.3.
8. `reference/rider_mesh_v1.md`: if T07 linked `docs/RiderMesh_Phase2_Plan.md` as the successor,
   point it at the blueprint.
9. Update the docs/README.md draft (04: PLANNED, 0 of 4 phases done).

## Done when

- `git grep -n "RiderMesh_Phase2_Plan"` is empty outside this pack.
- `grep -n -E "Stage [1-4]|stage [1-4]" docs/blueprints/04_rider_mesh.md` is empty, unless inside
  "(was Stage N)" notes.
- Internal `§` references in the blueprint all resolve to an existing heading.
- `00_CONTEXT.md` is updated and one commit is made. Docs only.
