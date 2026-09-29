# T06 — SPEC → reference: BLE link, iOS ANCS, settings and avatar

Read `00_CONTEXT.md` first. Use the **extraction procedure** in `T05_spec_system_nav.md` (read that
section only; the rest of T05 is already done).

## This task

| Source | Target | Section heading there |
|---|---|---|
| §5 intro (the lines between `## 5. BLE Architecture` and `### 5.1`) | `docs/reference/ble_link.md` | top of doc, as the overview |
| §5.1 Android path | same | Android path |
| §5.3 Project BLE identifiers | same | BLE identifiers |
| §5.5 Incoming calls | same | Incoming calls |
| §5.2 iOS/ANCS path | `docs/reference/ios_ancs.md` | whole doc |
| §5.6 Terminal settings, device info and avatar | `docs/reference/settings_and_avatar.md` | whole doc |

§5.4 (future polyline stream) stays in the SPEC for T10; it goes to blueprint 06. §5.7 is T07.

Notes:

- **Superseded check (important here).** §16.5 "BLE role flip" revises the Android path.
  §5.1 may describe either the old or the new roles. Add the "Revised by" line if it describes the
  old ones.
- §5.6 is referenced 13 times in code and firmware/README. Keep its content order intact, since
  code comments point at its tag tables.
- **Leave rider-mesh plan content alone.** `docs/RiderMesh_Phase2_Plan.md` (or
  `blueprints/04_rider_mesh.md`) plans new settings tags 0x0A-0x0C. The reference describes
  what is built.

## Done when

- The three files exist with headers; every row is moved, with stubs in the SPEC.
- Only allowed edits were made; the SPEC diff shows deletions plus stubs.
- The docs/README.md draft is updated.
- `00_CONTEXT.md` is updated and one commit is made. Docs only.
