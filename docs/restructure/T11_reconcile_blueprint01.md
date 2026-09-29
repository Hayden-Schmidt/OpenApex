# T11 — Reconcile blueprint 01 status against the code

Read `00_CONTEXT.md` first, then `docs/blueprints/01_nav_terminal_core.md`.

This is the one task that **may change substance**, but only status and "current" claims, and
only with evidence. The status text in blueprint 01 dates from 2026-09-21. A lot has landed since:
the C++ LVGL GUI with many screens, heading fusion, the rider mesh v1, iOS Phase A/B, telemetry
packet v6 and more.

## Inputs

- `git log --oneline --date=short --format="%ad %h %s"` on `main` (and the restructure branch)
- `firmware/main/`, `firmware/gui/`, `firmware/test_host/`, `phone/`
- `docs/reference/*` (for what is built), `docs/blueprints/02`, `03` (so you don't double-count
  their work)
- "Carry-over notes" in `00_CONTEXT.md`: resolve every note addressed to T11 or about blueprint 01

## Steps

1. **Phases.** Settle blueprint 01's phase list. T10 proposed 01.1-01.5. Adjust it if the code
   shows a better split, for example GUI screens as their own phase. Keep it to the scope in
   §Scope. If substantial built work has no home in any blueprint (for example the whole GUI screen
   set), add a carry-over note for the owner. **Do not** create a new blueprint.
2. **Checklist.** For each item, find evidence:
   - DONE: file path(s) plus a test or commit
   - PARTIAL: what exists and what is missing
   - NOT STARTED
   - DEFERRED / out of scope: owner decisions, such as no non-touch board support at this stage
3. **Status block.** Write a new "## Current status (YYYY-MM-DD)" above the old dated one. Keep the
   old block unedited under "Status history"; it is history. Set the header's **Status**, the phase
   table, and **Last reconciled with code: today**.
4. **Acceptance criteria and exit test.** Mark each as met / not met / unverified-on-hardware,
   with evidence. "Touch enabled or disabled": the owner scoped non-touch out, so mark it
   `DEFERRED (owner: no non-touch support at this stage)`. Do not delete it.
5. **Next steps.** Replace the stale "Recommended next steps (2026-09-21)" list, moving the old one
   into Status history. The new list covers only what is genuinely next **for blueprint 01**, as a
   pointer list. Work belonging to blueprints 02-06 is referenced, not repeated.
6. **Reference docs spot-check (light).** For each carry-over note that flags a stale claim in a
   `reference/` doc about blueprint-01 functionality: if the code clearly contradicts the doc, fix
   the sentence and cite the file. If it is unclear, leave it and keep the note for the owner.
7. Update the blueprint 01 line in `docs/README.md` (status, plus which phases are done).

## Done when

- Every checklist item, phase, acceptance criterion and exit-test clause has a status and evidence.
- Blueprint 01's header says "Last reconciled with code: <today>".
- Carry-over notes for T11 are resolved or explicitly left for the owner.
- `00_CONTEXT.md` is updated and one commit is made. Docs only; no code changes.
