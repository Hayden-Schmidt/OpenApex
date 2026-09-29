# Navigation pipeline

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §3, §4 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## Notification Normalization

*Was SPEC §3.*

Notification text is inconsistent across applications and locales. Source adapters must normalize
it before transmission where practical; the C3 parser must remain small and deterministic.

The normalized model is:

```c
// Values are stable protocol values, not platform enum ordinals.
typedef enum {
    NAV_ICON_STRAIGHT = 0,
    NAV_ICON_TURN_LEFT,
    NAV_ICON_TURN_RIGHT,
    NAV_ICON_SLIGHT_LEFT,
    NAV_ICON_SLIGHT_RIGHT,
    NAV_ICON_SHARP_LEFT,
    NAV_ICON_SHARP_RIGHT,
    NAV_ICON_ROUNDABOUT,
    NAV_ICON_U_TURN,
    NAV_ICON_ARRIVED,
    NAV_ICON_UNKNOWN
} nav_icon_t;

typedef struct {
    nav_icon_t icon_type;
    char distance_str[16];
    char street_name[64];
    uint32_t distance_meters;
    uint16_t current_speed_kmh;
    uint16_t heading_degrees;
    uint8_t gnss_fix_valid;
    uint8_t phone_battery_percent;
    uint32_t notification_age_ms;
    uint32_t sequence;
} nav_payload_t;
```

Unknown numeric values use explicit sentinels. A missing field must never become a fabricated zero.
The source application and source platform should be included in the packet or diagnostics data.

The initial POC may use compact binary payloads. JSON is acceptable between a phone helper and an
intermediate service, but the terminal link should use a bounded binary representation for reliable
BLE notifications and simple C3 parsing.

## C3 Countdown Processing

*Was SPEC §4.*

Notification distances update discretely. The terminal must continue showing a slowly decreasing
value between notification updates instead of freezing the last displayed number.

On every valid navigation update, the C3 stores:

- notification distance in metres;
- local monotonic timestamp;
- latest phone GNSS speed;
- speed validity and freshness;
- maneuver sequence number.

While the maneuver sequence is unchanged, it estimates:

```text
estimated_distance_m = max(
    0,
    notification_distance_m - integrated_speed_mps * elapsed_seconds
)
```

Requirements:

- Convert phone speed from km/h to m/s before integration.
- Smooth speed changes to avoid visible jumps.
- Apply a maximum elapsed-time hold window; do not count down forever on stale data.
- Clamp the estimate at zero.
- Replace the baseline when a new notification distance arrives.
- Reset the baseline when the maneuver or source navigation state changes.
- Mark the estimate stale when BLE, notification, or GNSS freshness expires.
- Never use a missing speed as zero motion without exposing the stale/unknown state.
- Do not claim this is dead reckoning or route navigation; it is display interpolation only.
- Interpolation is optional and **off by default** (`COUNTDOWN_INTERPOLATE_DEFAULT` in
  `countdown.h`, runtime `countdown_set_interpolation()`). When off, the displayed distance is the
  latest notification distance, still marked stale on expiry.

The phone remains authoritative for the next maneuver and notification distance. The C3 only makes
the visible countdown smoother between authoritative updates.
