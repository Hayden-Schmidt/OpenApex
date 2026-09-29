# T05 — SPEC → reference: system overview, nav pipeline, display

Read `00_CONTEXT.md` first, especially Rules 3 and 4 (verbatim moves, stubs) and the SPEC
mapping.

## Extraction procedure (T05-T09 all use this)

1. Find each source section in `docs/OpenApex_SPEC.md` **by heading**. Line numbers shift as
   earlier tasks leave stubs.
2. Create the target file with the right header from `00_CONTEXT.md`. Paste the section text
   verbatim, adjusting heading levels so the new file has one `#` title and `##` sections.
3. Allowed edits only: "MotoNav" → "OpenApex"; bare `§N` → `SPEC §N`; heading levels; header.
4. **Superseded check.** SPEC §16 says its ADRs "supersede" earlier sections where they revise
   them. For each section you move, search §16 (wherever it currently lives: the SPEC, or
   `reference/decisions.md` after T08) for text revising it. Example: §16.5 flips the BLE roles,
   which §5.1 may predate. If found, add one line under that section's heading:
   `> Revised by SPEC §16.N — see there before relying on this section.` Do not merge the texts.
5. Replace the moved text in the SPEC with the stub line (Rule 4), keeping the SPEC heading.
6. Anything stale you notice (claims contradicting the code or other docs): **do not fix**. Add it
   to "Carry-over notes" with file and heading.

## This task

| Source | Target | Section heading there |
|---|---|---|
| SPEC title block (the lines under `# MotoNav Technical Specification`: status, scope, first hardware, reference hardware) | `docs/reference/system_overview.md` | "Scope" (at the top, under the header) |
| §1 System Boundary | `docs/reference/system_overview.md` | System boundary |
| §2.3 Platform source adapters | same | Platform source adapters |
| §2.4 Normalization ownership | same | Normalization ownership |
| §8 Firmware Runtime + §8.1 Concurrency model | same | Firmware runtime; Concurrency model |
| §3 Notification Normalization | `docs/reference/nav_pipeline.md` | Notification normalization |
| §4 C3 Countdown Processing | same | Countdown processing |
| §9 Display Behavior | `docs/reference/gui_and_display.md` | Display behavior |

`gui_and_display.md` gets a second section in T08 (GUI architecture, from §16.6). For now, add an
empty `## GUI architecture` heading with the line "Pending T08."

§2 intro, §2.1 and §2.2 stay in the SPEC for T10 (they are blueprint 01 scope). §2.4 is referenced
12 times in code, so keep its heading text recognisable.

## Done when

- The three files exist with headers, and every row above is moved with a stub left in the SPEC.
- Nothing was reworded beyond the allowed edits. Spot-check with `git diff -M --stat` and read the
  diff of the SPEC: only deletions plus stub lines.
- The docs/README.md draft lists the new reference files.
- `00_CONTEXT.md` is updated (Progress, carry-over; mapping only if you deviated) and one commit is
  made. No builds are needed (docs only).
