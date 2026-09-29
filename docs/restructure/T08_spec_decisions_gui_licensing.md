# T08 — SPEC → reference: decisions (ADRs), GUI architecture, licensing and porting

Read `00_CONTEXT.md` first. Use the **extraction procedure** in `T05_spec_system_nav.md` (read that
section only).

## This task

| Source | Target | Section heading there |
|---|---|---|
| §16 intro + §16.1-§16.5 | `docs/reference/decisions.md` | `## ADR-1 — Normalization on the terminal` … `## ADR-5 — BLE role flip …` (same order, titles kept) |
| §16.6 GUI architecture | `docs/reference/gui_and_display.md` | GUI architecture (fill the heading T05 left empty) |
| §14 Source and Licensing Notes | `docs/reference/licensing_and_porting.md` | Source and licensing |
| §15 Existing Code Porting References (§15.1-§15.7) | same | Porting references |

Notes:

- **ADR numbering.** Under each ADR title add `*(was SPEC §16.N)*`, so references keep working
  during the sweep.
- In `decisions.md`, keep the §16 intro sentence ("authoritative; where they revise an earlier
  section, they supersede it"). Add one line after it: "ADR-6 (GUI architecture) lives in
  [gui_and_display.md](gui_and_display.md#gui-architecture) because it is the living description
  of the GUI."
- **Superseded check, reversed.** For each ADR, check that the reference section it revises (now
  in `reference/*.md` after T05-T07) carries the "Revised by" line. Add any that are missing.
- §16.6 is referenced 19 times in `firmware/gui/`, so keep its internal order and any
  sub-numbering.
- §15 ("porting references") points at MotoDev, the archived predecessor repo. The project rule
  (`CLAUDE.md`) is that MotoDev is a snippet reference only. Move it verbatim; don't comment on it.
- `licensing_and_porting.md`: the rider-mesh plan will add Monocypher (CC0/BSD-2) here when it is
  vendored. Do not add it now.

## Done when

- Files exist and are filled; every row is moved, with stubs in the SPEC.
- Every ADR that revises a reference section is cross-linked both ways.
- The docs/README.md draft is updated.
- `00_CONTEXT.md` is updated and one commit is made. Docs only.
