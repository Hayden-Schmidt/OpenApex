# Heading Sensor Fusion — Design Plan

Status: **planned, not implemented.** Raw-input logging landed first (see "Data collection"),
so the filter can be built and tuned against recorded rides rather than by guessing and re-driving.

## The faults this fixes

Observed on the road with the current code, which relays `Location.getBearing()` straight through:

1. **Compass snaps to north at a standstill.** GPS course-over-ground is undefined at zero speed;
   the bearing goes invalid and reads as 0.
2. **Compass follows the rider, not the bike.** Twisting in the seat rotates the phone in the
   jacket, and any phone-orientation source rotates with it.
3. **No smoothing at all.** Heading arrives at ~1 Hz and is displayed as a step, so even a correct
   value looks wrong.

## Why a naive speed-weighted blend does not work

The obvious fix — weight GPS against phone orientation by speed — fails because the two sources do
not measure the same thing in the same frame:

- **GPS course-over-ground** is the *bike's* direction of travel.
- **Phone yaw** (`TYPE_ROTATION_VECTOR`) is the *phone's* orientation, which sits at an arbitrary
  and slowly-changing rotation from the bike's forward axis.

Averaging 30° of GPS course with 200° of phone yaw yields a confident 115°, pointing nowhere. The
two must be brought into a common frame before any blending is meaningful.

## Design

### 1. Learn the mount offset continuously

```
offset = circular_mean(gps_course − phone_yaw)
```

Accumulated **only** while the evidence is trustworthy:

- speed above ~15 km/h (GPS course noise scales roughly as 1/speed),
- GPS accuracy within threshold,
- yaw rate low (riding straight, not mid-corner).

Time constant 30–60 s. `phone_yaw + offset` is then an estimate of bike heading in the same frame
as GPS course. Self-learning every ride, with no calibration ritual, and it re-converges when the
phone moves to a different pocket.

This is **not** jacket-specific. A handlebar-mounted device has the same unknown fixed offset
between its forward axis and the bike's, set by how the mount happens to sit. Same estimator,
tighter convergence.

### 2. Measure mount trustworthiness rather than assuming it

The offset estimator also yields its own **circular variance**, which is a direct measure of how
stable the mount is: a phone in a tank bag converges tight; a phone on a flapping jacket stays
wide. High variance discounts phone-derived heading and leans harder on GPS plus freeze-at-stop.

The jacket problem becomes a quantity the system observes about the actual setup, rather than a
constant baked in from an assumption.

### 3. Speed-scaled confidence, applied to filter gain

GPS course noise scales roughly as 1/speed, so speed sets *how much to trust GPS*, not *how much
of each frame to mix*:

| Speed | Behaviour |
|---|---|
| > ~15 km/h | GPS course is truth. Heavy weight, tight circular low-pass. Offset estimator updates. |
| ~3–15 km/h | Blend GPS with gyro-propagated heading via the learned offset, weight shifting with speed. |
| < ~3 km/h | **Freeze.** Hold last good heading, propagate only slow gyro yaw. Never snap to north. |

Freezing at a stop is the honest answer: a stopped bike's heading genuinely is not changing, so the
last known-good value is *more* correct than any live reading. This is the direct fix for fault 1.

### 4. Non-negotiables

- **All smoothing must be circular.** Filter the unit vector, not the angle. Linear averaging of
  degrees sweeps the compass 358° the wrong way on every crossing of north.
- **Slew-rate limit at the display**, interpolating at the existing 10 Hz tick. Heading arriving at
  1 Hz always looks like stepping otherwise. Cap at roughly 90°/s.
- **Never fabricate.** No heading is a sentinel, not a zero — matching `normalize.c` and the packet
  format. The output carries a confidence value so the display can dim or hide rather than lie.

### 5. Use the rotation vector, not the raw magnetometer

`TYPE_ROTATION_VECTOR` is already gyro/accel/mag fused by Android and is tilt-compensated, which is
the genuinely hard part on a leaning motorcycle.

## Where it runs: on the terminal

The fusion belongs in firmware, as a pure C module beside `normalize.c` and `countdown.c`.

**Why not the phone**, despite the phone having richer sensors:

- The **ESP32-S3 AMOLED** hardware has an onboard IMU. Its heading needs the same fusion, and the
  same mount-offset learning (see §1). Building on the phone means rewriting it later.
- With the fusion on the terminal and a pluggable yaw source, the S3 migration is a change of
  *input*, not an algorithm change: "phone rotation vector, relayed" becomes "onboard IMU".
- The C3 prototype has no IMU, so today's yaw must come over the link regardless. The module never
  needs to know which it is.

### No magnetometer required

GPS course supplies the absolute north reference; the IMU only covers the gaps between fixes and at
low speed. A 6-axis accel+gyro IMU is sufficient — and preferable. A magnetometer on handlebars sits
inside a steel frame, beside ignition coils and switched DC current, and is the least trustworthy
sensor in that environment. **If the S3 board carries a magnetometer, ignore it for heading.**

### Compute cost

Heading is stored as a **unit vector `(cos θ, sin θ)`**, never as degrees:

- circular low-pass → two multiply-adds, no trig
- circular mean for offset → accumulate two components, no trig
- slew limit → one dot product, one rotation
- `atan2` → **once per display update**, not per filter step

Worst case is the C3: RV32IMC has no hardware FPU, so floats are software-emulated. At 10 Hz this is
on the order of 100 float ops per tick — roughly 100k cycles/s against 160 MHz, under 0.1% CPU.
State is ~100 bytes. The S3's LX7 has a single-precision FPU, where it is free.

If it ever became tight, the same vector formulation drops to Q15 fixed point cleanly. Not expected.

## Interface sketch

`firmware/main/heading_fusion.c` — pure C, no platform dependencies, host-testable with plain gcc
so it can be tuned against recorded drives.

```c
typedef struct {
    float  gps_course_deg;   bool gps_course_valid;
    float  gps_accuracy_m;
    float  speed_kmh;        bool speed_valid;
    float  yaw_deg;          bool yaw_valid;       // phone now, onboard IMU later
    float  yaw_rate_dps;     bool yaw_rate_valid;
    uint32_t timestamp_ms;
} heading_input_t;

typedef struct {
    float heading_deg;       // bike frame
    float confidence;        // 0..1; display dims or hides below threshold
    bool  frozen;            // held from last good fix rather than live
} heading_output_t;
```

Validity flags on every field, same sentinel discipline as the rest of the pipeline.

## Packet impact (v3)

The packet currently carries a single fused `heading_deg`. The terminal needs the **raw** inputs
instead:

- GPS course + validity
- GPS accuracy
- phone yaw (rotation vector) + validity
- yaw rate + validity

That is a `RAW_NOTIF_VERSION` bump to 3, changing both sides together. Motion samples
(`accel_mg`/`gyro_mdps`) are already present in the packet but currently unused.

## Data collection (done)

Implemented ahead of the filter so the filter can be developed offline:

- `RelayRecorder.telemetry(...)` extended with GPS bearing, accuracy, speed, and bearing/speed
  validity flags recorded separately rather than pre-fused.
- `TYPE_ROTATION_VECTOR` registered in `RelayService`, with yaw/pitch/roll logged per sample.
- Gyro yaw rate and accelerometer samples logged alongside.

This is dev-only, gated on `BuildConfig.DEBUG` like the rest of `RelayRecorder`.

### What the first real capture (2026-09-23) showed was missing

The Bunnings ride produced 1635 telemetry samples and 7569 orientation samples, and two gaps in
the logging blocked using them. Both are fixed now, but **no ride captured before 2026-09-24 has
the fields**, so the filter has to be tuned against a fresh capture:

- **No position was logged at all.** Without lat/lon the learned mount offset can only be checked
  against the same GPS course it is derived from — there is no independent track to reconstruct a
  true direction of travel from. `telemetry()` now records `latDeg`, `lonDeg`, `altitudeM` and
  `fixElapsedRealtimeNanos` (the fix's own monotonic clock; wall-clock is not interpolatable).
- **Every fix was logged twice**, ~7 ms apart with identical bearing/speed/accuracy, one copy of
  each pair carrying a yaw frozen at exactly `249.10715` for the whole ride (817 of 1635 samples) —
  an orphaned second `RelayService` instance with a dead sensor listener, sharing the singleton
  recorder. `startGnss()` now calls `stopGnss()` before registering, and each sample is stamped
  with an `instanceId` so a recurrence is visible in the log rather than inferred from it.

Anything reading a pre-2026-09-24 capture must de-duplicate on `(bearingDeg, speedKmh, accuracyM)`
and discard samples whose `yawDeg` is 249.10715.

### What it showed was good

- `headingDeg` as relayed equals `Location.bearing` exactly — no corruption on the send path.
- `bearingAccuracyDeg` is mostly under 0.5° at road speed, which makes it a usable gate for the
  offset estimator (see the second open question below).

With a recorded ride, the filter can be written, tuned and regression-tested against the actual
jacket, bike and phone placement without riding again — the same payoff `replay.c` gives for
maneuver parsing.

## Open questions

- Exact IMU part on the S3 AMOLED board: 6-axis or 9-axis, and its sample rate.
- Whether GPS accuracy is a good enough proxy for course reliability, or whether a separate
  course-accuracy estimate (`Location.getBearingAccuracyDegrees()`, API 26+) should gate the
  offset estimator instead. Both are now logged, so this is answerable from data.
- Behaviour on a roundabout at low speed, where heading changes fast *and* GPS course is noisy —
  the one case where freeze-at-stop and true rotation are hardest to tell apart.
