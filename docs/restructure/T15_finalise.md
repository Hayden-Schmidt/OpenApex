# T15 — Final index, READMEs, link check, delete this pack

Read `00_CONTEXT.md` first. Last task.

## Steps

1. **`docs/README.md`: final.** Every file in `docs/reference/`, `docs/blueprints/` and
   `docs/investigations/` is listed, and nothing else.
   - Blueprint table: number, title, status, phases done (for example "03.1-03.2 of 4"), last
     reconciled date. Statuses must match each blueprint's header exactly. Read each header; don't
     copy from `00_CONTEXT.md`.
   - Keep the Conventions section (status words, the three header templates).
   - Add "How to add a blueprint": next free number, header template, add a row here.
2. **Root `README.md`.**
   - Point to `docs/README.md` as the doc entry point. Replace the `OpenApex_SPEC.md` links.
   - Fix stale lines logged in carry-over, for example:
     - "touch or non-touch variant": the owner scoped non-touch out
     - `gui_screens.c`: removed long ago
     - "SPEC §11 for full status" → blueprint index
   - Keep it short: what it is, layout, where to start.
3. **`docs/reference/DEVELOPMENT_SETUP.md`.**
   - Update "Repository layout" for the new tree: `design/`, `tools/maneuver_icons/`, docs
     folders, no `simulator/`.
   - Update the "Verified working (2026-09-21)" block. Only claim what you re-ran today (the four
     verification commands in `00_CONTEXT.md`). Keep other lines with their original date, marked
     "not re-verified".
4. **Carry-over notes.** Work through every remaining note:
   - resolve it, or
   - move it to the right blueprint's "Open decisions", or
   - list it for the owner in the final report.
   Nothing may be silently dropped.
5. **Link check.** For every `*.md` in the repo (excluding `firmware/managed_components`, `.pio`,
   `build`), check that every relative link target exists and every `#anchor` matches a heading.
   A short Python script in a temp folder outside the repo is fine; do not commit it. Fix broken links.
6. **Delete the pack.** `git rm -r docs/restructure/`. Before deleting, copy the SPEC mapping
   table and the §13 mapping into `docs/README.md` under "Where did the SPEC go?" It is the one
   thing from this pack worth keeping, for anyone following an old reference.
7. **Leftover grep (after deleting the pack):**

   ```
   git grep -n -E "OpenApex_SPEC|RiderMesh_Phase2_Plan|iOS_Handoff|iOS_Multiplatform_Status|Google Icons Archive|design reference|simulator/index|docs/restructure"
   ```

   The only allowed hits are the "Where did the SPEC go?" table in `docs/README.md` and the
   old → new table in `design/README.md`.
8. **Full verification:** all four commands from `00_CONTEXT.md` Rule 7.
9. **Suggest, don't edit:** propose to the owner a one-line addition to the project `CLAUDE.md`:
   "Docs start at docs/README.md; blueprints carry status." The owner decides.

## Done when

- The index is complete and statuses match the headers.
- Link check: zero broken links. Leftover grep: only the two allowed tables.
- All four verification commands pass (or the report explains a pre-existing toolchain issue).
- Carry-over is empty, with each note resolved, moved or reported.
- One final commit is made. The report to the owner includes: the blueprint status table, the
  owner-facing leftovers, the proposed CLAUDE.md line, and "ready to open a PR from
  `chore/restructure`" (do not open it unless asked).
