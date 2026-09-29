# T12 — Blueprint 03: merge the two iOS docs

Read `00_CONTEXT.md` first. It can run any time after T04.

## Inputs

- `docs/iOS_Handoff.md`: "Read this first if you are picking the work up". Sections: shape and
  why, where the code is, how to verify, what is proven, traps, design decisions not to
  relitigate, what is left, environment.
- `docs/iOS_Multiplatform_Status.md`: "what is actually done and what has actually been verified",
  organised by Phase A-D.

The two overlap heavily. Both are "last updated 2026-09-29".

## Target: `docs/blueprints/03_ios_multiplatform.md`

Title: "03 — iOS + Android multiplatform (KMP)". Blueprint header from `00_CONTEXT.md`.

| Phase | Was | Status (from the docs; verify the evidence exists) |
|---|---|---|
| 03.1 | Phase A — firmware (ANCS client etc.) | DONE, per docs |
| 03.2 | Phase B — KMP restructure | DONE, per docs |
| 03.3 | Phase C — iOS platform layer (written-not-compiled until a Mac exists) | NOT STARTED |
| 03.4 | Phase D — day one with the iPhone | NOT STARTED / BLOCKED (no Mac / iPhone) |

Structure, keeping the handoff's role as the entry point:

1. **Start here:** the handoff's opening, what exists and what is proven, in its own words.
2. **Phases:** the table above, then one section per phase. Merge the handoff's and the status
   doc's text for that phase. Where they say the same thing, keep the more specific wording. Where
   they conflict, keep both and add a carry-over note. Include "Phase A leftovers" under 03.1 as a
   "Remaining" list.
3. **How to verify:** from the handoff, verbatim.
4. **Traps and design decisions not to relitigate:** from the handoff, verbatim.
5. **Environment:** merge both docs' environment sections.

Rename "Phase A/B/C/D" to "03.1-03.4" throughout. The first time each appears, add "(was Phase
X)", because commit messages say "Phase B".

## Also

- `git rm` both old docs after the merge. Keep nothing verbatim-duplicated.
- References to the old files: `git grep -n -E "iOS_Handoff|iOS_Multiplatform_Status"` (known:
  `DEVELOPMENT_SETUP.md`, `Phone_App_Architecture.md`, each other). Update them to the blueprint.
- Section refs like "iOS_Handoff §4" → the new heading.
- If T10 parked the iOS ANCS open decision from SPEC §13 in blueprint 01 with a carry-over note,
  move it here.
- Update the docs/README.md draft.

## Done when

- One blueprint exists and the two old docs are gone. Everything from both is present, or was a
  pure duplicate. Check section by section and list any dropped paragraph in the report with the
  reason.
- The grep above shows only the blueprint path.
- `00_CONTEXT.md` is updated and one commit is made. Docs only.
