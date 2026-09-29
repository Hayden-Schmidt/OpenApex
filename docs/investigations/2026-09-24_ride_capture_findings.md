# 2026-09-24 — Ride capture: findings and fixes

**Kind:** Investigation log — frozen; do not update except to add a closing note.  
**Moved from:** OpenApex_SPEC.md §17 (restructure, 2026-09-29)

## 2026-09-24 ride capture: findings and fixes

*Was SPEC §17.*

The second full capture (7 phone sessions, 8,176 terminal records) confirmed the 2026-09-23 glyph
work and exposed two transport faults that had been silently destroying most of the data.

**Confirmed correct.** Replaying all 478 decoded packets through the current normalizer reproduces
what the terminal displayed exactly — 0 mismatches, where the pre-fix normalizer differed on 150.
Maneuvers resolved: TURN_RIGHT 154, ROUNDABOUT_STRAIGHT 148, TURN_LEFT 91, ROUNDABOUT_LEFT 22,
STRAIGHT 17, ARRIVED 8, UNKNOWN 7 (6 empty titles, 1 "Rerouting..."). The duplicate-GNSS-callback
fix holds: zero duplicate fix timestamps, one callback instance per session.

**Fault 1 — no GNSS in 4 of 7 sessions.** `ACCESS_BACKGROUND_LOCATION` was declared but not
granted, so any background-started `RelayService` could not promote itself to the `location` FGS
type; the promotion threw, was retried every 30 s, and was denied every time. The ride out
(09:22–09:26, foreground start) had 226 samples and 93% bearing coverage; everything after the
09:26 service restart had none. The terminal log corroborates exactly — heading present on 334/351
packets in the good session, unknown on 109/109 in the next.

*Fixed by:* requesting the permission as an explicit second stage in `MainActivity`, deep-linking
to Settings when the system dialog is spent, distinguishing the permanent case from the transient
one in `tryStartGnss()`, and posting a high-priority "No speed or compass" notification instead of
degrading silently. Riding past a dead compass with no indication is the failure being fixed, as
much as the permission itself.

**Fault 2 — 3 of 7 sessions delivered nothing at all.** The terminal logged 6,690 `DECODE_FAILED`
against 8 `CONNECTED`, every failure with `detail=20` — the default-ATT-MTU payload (23 − 3). The
phone sent all 622 packets at 146 bytes; `WRITE_TYPE_NO_RESPONSE` truncated them to 20 and reported
success. The display sat on its boot screen for three entire sessions while the phone logged
`queued=true` throughout. One stale connection also burned 6,565 ring slots — 80% of the 1 MB log
partition — on nothing but repeated identical failures.

*Fixed by:* tracking the negotiated MTU on the phone and refusing to write a packet the link cannot
carry whole; handling `BLE_GAP_EVENT_MTU` in firmware so the negotiated value is logged rather than
inferred; a distinct `TRUNCATED` event; rate-limiting repeated failures so a dead link cannot eat
the ring; and marking the view stale via `ble_link_is_faulted()` so the dial greys instead of
holding a frame forever. A dedicated fault screen naming the cause is follow-up work — `gui_app`
distinguishes only idle from dial today.

**Glyph tables now match with tolerance.** Angles are measurements of rendered bitmaps, so
exact-match would answer UNKNOWN for a glyph that merely shifted a degree between Maps releases.
Matching is within ±3°, guarded by `test_glyph_tables_are_separable()`, which fails if any two
entries in a table come within 2x the tolerance. Note 133 (arrived) and 135 (roundabout, 1st exit)
are only 2° apart but live in different families, selected by title text before either is consulted.

**Coverage is still the main risk.** Only 6 of 13 maneuver classes have ever been observed;
`SLIGHT_RIGHT`, `SHARP_LEFT`, `U_TURN` and `ROUNDABOUT_RIGHT` have no glyph entry at all and survive
only on English text matching. No capture has ever contained a km-scale distance. See
[the capture ride plan](2026-09-28_capture_ride_plan.md).
