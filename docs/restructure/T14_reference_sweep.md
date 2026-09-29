# T14 — Rewrite every SPEC reference; delete the SPEC

Read `00_CONTEXT.md` first. Its SPEC mapping and §13 item mapping tables drive this task.

Before T01 there were about 81 `§` references in code and tooling, plus more in docs. The busiest:

| Ref | Count | Target |
|---|---|---|
| §5.7 | 23 | reference/rider_mesh_v1.md |
| §16.6 | 19 | reference/gui_and_display.md |
| §5.6 | 13 | reference/settings_and_avatar.md |
| §2.4 | 12 | reference/system_overview.md |
| §5.5 | 9 | reference/ble_link.md |
| §13 | 9 | per the §13 item mapping |
| §3 | 8 | reference/nav_pipeline.md |

## Steps

1. **Inventory.** List every hit:

   ```
   git grep -n -E "OpenApex_SPEC|SPEC ?§|§ ?[0-9]+(\.[0-9]+)?" -- ':!docs/restructure'
   ```

   Classify each hit:
   - (a) a SPEC reference → rewrite
   - (b) a reference to a section of some *other* doc, such as `Phone_App_Architecture.md §3` or
     a blueprint's own `§04.1.x` → leave, unless it points at a moved doc's old name
   - (c) prose using § for something else → leave
2. **Rewrite style.**
   - **Code comments:** `docs/reference/settings_and_avatar.md` (path from repo root, no
     anchors). Example: `// see docs/OpenApex_SPEC.md §5.6` →
     `// see docs/reference/settings_and_avatar.md`. When the old ref named a subsection that is a
     heading in the new doc, add it: `docs/reference/ble_link.md "Incoming calls"`. Never change
     code, only comment text. Keep line length within the file's existing style.
   - **Markdown:** relative links with heading anchors where useful,
     `[settings](../reference/settings_and_avatar.md)`.
   - `ADR 16.N` / `§16.N` → `docs/reference/decisions.md ADR-N` (16.6 → `gui_and_display.md "GUI
     architecture"`).
   - "Phase 1" meaning the SPEC roadmap → `blueprint 01`. "Phase A-D" meaning iOS → `03.1-03.4`.
     Grep for `Phase [1-3A-D]` and judge each hit; leave commit-history quotes.
3. **Remove the "(was SPEC §16.N)" and "Revised by SPEC §16.N" wording.**
   - "(was SPEC §16.N)" notes in `decisions.md`: keep them. They help history readers.
   - The "Revised by SPEC §16.N" lines added in T05-T08: rewrite them to point at `decisions.md
     ADR-N`.
4. **Delete `docs/OpenApex_SPEC.md`.** First confirm it contains only headings, stubs and nothing
   else (read it fully). Anything non-stub means an earlier task missed it: move it per the mapping
   and note it.
5. Also fix the known leftovers from Carry-over notes marked "for T14".

## Done when

- The inventory grep returns only class (b) and (c) hits. List them in the report so the owner can
  eyeball them.
- `git grep -n "OpenApex_SPEC"` is empty outside this pack.
- Code changed only in comments. Prove it with `git diff --stat` plus a skim, and run:
  - `powershell -NoProfile -File scripts/build-countdown.ps1`
  - `pio run -d firmware -e prototype_c3`
  - `pio run -d firmware/sim_lvgl`
  - `cd phone; .\gradlew.bat :shared:jvmTest :androidApp:assembleDebug`
- `00_CONTEXT.md` is updated and one commit is made.
