# T07 — SPEC → reference: rider mesh v1, hardware

Read `00_CONTEXT.md` first. Use the **extraction procedure** in `T05_spec_system_nav.md` (read that
section only).

## This task

| Source | Target | Section heading there |
|---|---|---|
| §5.7 Rider mesh | `docs/reference/rider_mesh_v1.md` | whole doc |
| §6 Hardware Profiles (intro, §6.1, §6.2, §6.3) | `docs/reference/hardware.md` | Hardware profiles |
| §7 Optional GNSS and Sensor Hardware (§7.1-§7.3) | same | GNSS and sensors |
| §10 Power and Mechanical Design | same | Power and mechanical |

Notes:

- **`rider_mesh_v1.md` is the mesh as built today** (wire v1, friending, POS). Blueprint 04 plans
  its replacement.
  - Under the header add: `> Planned successor: [blueprint 04](../blueprints/04_rider_mesh.md)
    (wire v2). This doc stays accurate to the code until 04.1 ships.` If T13 has not run yet, the
    link target is still `docs/RiderMesh_Phase2_Plan.md`; link that and leave a carry-over note
    for T14 to fix.
  - §5.7 is the most-referenced section in code (23 references), so keep its internal order.
- **Stale-looking hardware status** ("Phase 1 prototype", "first hardware"): §6.1's title says
  Phase 1. Move it verbatim; T14 rewrites "Phase 1" references to blueprint 01. Log anything
  contradicting the current board setup, such as the non-touch variant, which the owner has
  scoped out.

## Done when

- Both files exist with headers; every row is moved, with stubs in the SPEC.
- Only allowed edits (plus the one successor line) were made.
- The docs/README.md draft is updated.
- `00_CONTEXT.md` is updated and one commit is made. Docs only.
