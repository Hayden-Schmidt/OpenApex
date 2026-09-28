#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ancs_core.h"
#include "packet.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maps an ANCS notification onto the raw packet shape the terminal already normalizes
// (docs/OpenApex_SPEC.md §2.4). This is the whole reason normalize.c lives in C on the terminal:
// on iOS the notification text arrives here with no phone app in the loop, so routing it through
// the same normalize_packet() the Android relay uses is what stops the maneuver/street/distance
// rules from being written twice and drifting.
//
// What ANCS can and cannot carry, stated once so nothing downstream invents a value:
//   have  - AppIdentifier, Title, Subtitle, Message, Date. Text only.
//   none  - icon_rotation_deg (the glyph second witness), progress/progress_max, traffic
//           segments, speed, heading, GNSS fix, battery, epoch.
// Every field in the second list is left at its "unknown" sentinel, and the telemetry half comes
// from the iOS companion app over the normal nav characteristic (packet v6, nav fields absent).

typedef enum {
    ANCS_NAV_SOURCE_UNKNOWN = 0,
    ANCS_NAV_SOURCE_GOOGLE_MAPS,
    ANCS_NAV_SOURCE_WAZE,
    ANCS_NAV_SOURCE_APPLE_MAPS, // known not to post ANCS notifications; kept so a capture says so
} ancs_nav_source_t;

// Which app an identifier belongs to, or UNKNOWN. Matching is exact and case-insensitive.
ancs_nav_source_t ancs_nav_classify_app(const char *app_id);

// True when this notification should drive the nav page: a known navigation app, in a category
// that carries turn-by-turn. Independent of the capture filter -- see ancs_nav_set_permissive().
bool ancs_nav_is_navigation(const ancs_notification_t *n);

// Capture mode. When permissive (the default until a real iPhone has been recorded), an app we do
// not recognise is still offered to the nav mapper so a capture shows what it would have produced.
// A hardcoded allow-list on first contact with a real iPhone means a wasted drive.
void ancs_nav_set_permissive(bool permissive);
bool ancs_nav_permissive(void);

// Fills `out` from an ANCS notification, ready for normalize_packet(). `sequence` is the caller's
// own monotonic counter -- ANCS has no sequence of its own. Returns false if the notification
// carries nothing a nav page could use, in which case `out` is untouched.
bool ancs_nav_to_raw(const ancs_notification_t *n, uint32_t sequence, raw_notif_t *out);

// Parses an ANCS Date ("yyyyMMdd'T'HHmmSS", phone-local time, no zone) into its parts. Returns
// false on anything that is not that exact shape. The terminal takes its clock from the Current
// Time Service, not from here; this exists so a capture can be ordered and aged.
bool ancs_nav_parse_date(const char *date, int *year, int *month, int *day, int *hour, int *minute,
                         int *second);

#ifdef __cplusplus
}
#endif
