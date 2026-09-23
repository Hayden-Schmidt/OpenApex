# Android 16 `Notification.ProgressStyle` extras — Google Maps nav notifications

Captured via `NavNotificationRelayService`'s diagnostic logging against real Google Maps
turn-by-turn notifications on a OnePlus (Android 16, `com.google.android.apps.maps`) using
`android.template = android.app.Notification$ProgressStyle`.

## Already wired up

| Extra | Type | Use |
|---|---|---|
| `android.title` | SpannableString | Maneuver text, feeds `derive_maneuver()` keyword matching |
| `android.shortCriticalText` | String | Distance-to-maneuver text (e.g. `"220"`) |
| `android.subText` | String | ETA text (e.g. `"Arrive 12:11"`) |
| `android.progress` / `android.progressMax` | Integer | Progress counters (e.g. `1/3731`) |
| `android.largeIcon` | Icon | Maneuver arrow bitmap — feeds PCA / farthest-point angle extraction |

## Present but not yet used

- **`android.progress` / `android.progressMax`** — appear to be meters-traveled / total-route-meters
  for the whole trip, not just the current maneuver. A precise, monotonic distance signal
  independent of text parsing or PCA. Could potentially replace/cross-check the
  debounce/countdown logic in `countdown.c`.

- **`android.progressSegments`** — `ArrayList<Bundle>`, one Bundle per segment (~124 bytes each,
  count grew from 1 to 2 across two consecutive packets as more of the route loaded). This is
  Android 16's `ProgressStyle.Segment` list.
  **Confirmed (per user, from visually correlating with Maps UI): these mark route-wide delay
  regions along the entire route (e.g. traffic-slowdown coloring on the progress bar), not
  turn-by-turn maneuver boundaries.** Not useful for per-maneuver distance/classification —
  ruled out as a replacement for text-based distance parsing.

- **`android.progressTrackerIcon`** — `Icon(TYPE_RESOURCE, pkg=com.google.android.apps.maps,
  id=0x7f0805b9)`, same id in every sample captured. Looks like the generic "current position"
  puck drawn on the progress bar itself, not a per-maneuver icon. Not useful for maneuver
  classification.

- **`android.requestPromotedOngoing`** — `true` in all samples; confirms this is Android 16's
  "promoted ongoing" persistent nav notification. Could be used as a cheap liveness/validity
  check to distinguish real nav notifications from stray Maps notifications without relying on
  title-text heuristics.

## Confirmed dead ends

- `android.progressPoints` — always empty (`[]`).
- `android.infoText`, `android.text`, `android.remoteInputHistory` — always `null`.
- `in_conference_scene_mode`, `in_full_screen_mode`, `android.showChronometer`,
  `android.showWhen`, `android.styledByProgress` — UI chrome flags, no nav-relevant value.
- `android.largeIcon` / `smallIcon` — both always `TYPE_BITMAP` with no resource name on every
  sample (plain turns and roundabouts alike). The "match by resource name" idea for exact
  maneuver identification is dead; PCA / farthest-point extraction on the bitmap mask is the
  only available path for icon-derived direction data.

## Sample raw capture (roundabout notification)

```
extra[android.appInfo] (ApplicationInfo) = ApplicationInfo{... com.google.android.apps.maps}
extra[android.largeIcon] (Icon) = Icon(typ=BITMAP size=168x168 tint=0xffffffff)
extra[android.progress] (Integer) = 1
extra[android.progressMax] (Integer) = 3731
extra[android.progressPoints] (ArrayList) = []
extra[android.progressSegments] (ArrayList) = [Bundle[mParcelledData.dataSize=124], Bundle[mParcelledData.dataSize=124]]
extra[android.progressTrackerIcon] (Icon) = Icon(typ=RESOURCE pkg=com.google.android.apps.maps id=0x7f0805b9)
extra[android.requestPromotedOngoing] (Boolean) = true
extra[android.shortCriticalText] (String) = 220
extra[android.subText] (String) = 12:11
extra[android.template] (String) = android.app.Notification$ProgressStyle
extra[android.title] (SpannableString) = 220 m · At the roundabout, take the 1st exit onto Apollo Dr
icon[largeIcon] type=BITMAP resId=null resName=null
icon[smallIcon] type=BITMAP resId=null resName=null
relay nav: title=220 m · At the roundabout, take the 1st exit onto Apollo Dr dist=220 m progress=7/3736 iconRotationDeg=87
```

## Related fix from this investigation

The roundabout icon's whole-mask PCA principal axis is dominated by the near-circular ring shape
and is unreliable (measured `263°` on a real right-hand exit, misclassified as LEFT). Fixed in
`NavNotificationRelayService.extractIconRotationDeg()` by using the mask pixel farthest from the
centroid (the exit tick's tip) for roundabout icons specifically, instead of PCA — same real
exit then measured `87°`, correctly bucketed as RIGHT.

## Open question: can we identify the maneuver without parsing the bitmap?

The earlier capture closed the door on `android.largeIcon` (always `TYPE_BITMAP`, no resource id),
but it only logged `largeIcon` and `smallIcon`. Two leads were never tested:

1. **`android.progressTrackerIcon`** — the only `TYPE_RESOURCE` icon in the whole notification
   (`id=0x7f0805b9`, `com.google.android.apps.maps`). Whether that id *varies by maneuver* is
   unknown; a single roundabout notification was captured, so there is nothing to compare against.
   If a left turn, a right turn and a roundabout carry different ids, a `resId -> maneuver` table
   replaces bitmap parsing outright. If the id is constant across a drive, it is a static
   "navigation" badge and this lead is dead.
2. **Bitmap fingerprinting** — even with no id, if Maps draws a small fixed set of glyphs rather
   than rotating one arrow, hashing the glyph identifies the maneuver exactly, with no geometry.

`NavNotificationRelayService.logManeuverIdentity()` now emits one de-duplicated `IDENT` line per
distinct rendering to answer both:

```
IDENT title="220 m · At roundabout, take 1st exit onto Apollo Dr" tracker=RESOURCE:0x7f0805b9:<name> small=type1 large=type1 size=168x168 mask=0x... shape=3-8-14-19-21-17-11-7
```

- `mask` is rotation-**sensitive** (alpha mask resampled to a fixed 16x16 grid).
- `shape` is rotation-**insensitive** (radial histogram about the centroid).

### How to read a drive's capture

| Observation | Conclusion |
|---|---|
| `tracker` resId differs across maneuver types | **Best case.** Build a resId→maneuver table; drop bitmap parsing for everything except possibly roundabout exit index. |
| `tracker` constant, `shape` takes a small set of distinct values matching maneuver types | Fingerprint→maneuver table; also drops geometry. |
| `tracker` constant, `shape` constant while `mask` varies | Maps rotates one arrow glyph. Geometry is the only source and the current PCA/farthest-point path stays. |

`logProgressStyleDiagnostics()` is now genuinely one-shot per process and recurses into nested
Bundles, so `android.progressSegments` / `android.progressPoints` are unparcelled and dumped
rather than printed as `Bundle[mParcelledData.dataSize=124]`. It also logs *every* Icon reachable
from the notification (all extras keys, icons nested in the ProgressStyle Parcelables, and action
icons) — not just large/small.

Capture with `adb logcat -s OpenApexRelay` over a drive that includes at least one left turn, one
right turn, one roundabout and one straight/continue.

## Unresolved: arrow handedness vs roundabout handedness

An on-road run reported every left/right inverted, so `derive_maneuver_from_angle()` (PCA arrow
path) was mirrored in `firmware/main/normalize.c`. `derive_roundabout_direction()` was **not**
mirrored: it uses a different extractor (farthest-point exit tick, not PCA) and the one real
measurement on record — a right-hand exit at `87°` — agrees with the unmirrored mapping. If the
next drive shows roundabouts inverted too, mirror that function as well.
