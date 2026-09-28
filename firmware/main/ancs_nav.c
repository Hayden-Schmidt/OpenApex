#include "ancs_nav.h"

#include "normalize.h"

#include <string.h>

// Bundle identifiers, lowercase. Apple Maps is listed deliberately: it is reported not to post
// ANCS notifications at all (it is too system-integrated to raise a UNNotification), so having it
// classified means a capture can state that as an observation rather than a silence we guessed at.
static const struct {
    const char *app_id;
    ancs_nav_source_t source;
} kApps[] = {
    {"com.google.maps", ANCS_NAV_SOURCE_GOOGLE_MAPS},
    {"com.waze.iphone", ANCS_NAV_SOURCE_WAZE},
    {"com.apple.maps", ANCS_NAV_SOURCE_APPLE_MAPS},
};

// Permissive until a real iPhone has been recorded. See ancs_nav.h.
static bool s_permissive = true;

void ancs_nav_set_permissive(bool permissive) {
    s_permissive = permissive;
}

bool ancs_nav_permissive(void) {
    return s_permissive;
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ieq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (lower(*a) != lower(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

ancs_nav_source_t ancs_nav_classify_app(const char *app_id) {
    if (app_id == NULL || app_id[0] == '\0') {
        return ANCS_NAV_SOURCE_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof(kApps) / sizeof(kApps[0]); i++) {
        if (ieq(app_id, kApps[i].app_id)) {
            return kApps[i].source;
        }
    }
    return ANCS_NAV_SOURCE_UNKNOWN;
}

bool ancs_nav_is_navigation(const ancs_notification_t *n) {
    if (n == NULL) {
        return false;
    }
    // An incoming call is never navigation, whoever posted it -- that path is call_state's
    // (SPEC §5.5) and must not be allowed to write the nav page.
    if (n->category == ANCS_CATEGORY_INCOMING_CALL || n->category == ANCS_CATEGORY_MISSED_CALL) {
        return false;
    }
    const ancs_nav_source_t src = ancs_nav_classify_app(n->app_id);
    if (src == ANCS_NAV_SOURCE_GOOGLE_MAPS || src == ANCS_NAV_SOURCE_WAZE) {
        return true;
    }
    if (src == ANCS_NAV_SOURCE_APPLE_MAPS) {
        // Recognised, and recognised as not a working source. Capture it, never display it.
        return s_permissive;
    }
    // Unknown app. Google Maps posts turn-by-turn under CategoryIDLocation on iOS, so that
    // category is the strongest hint we have for an app we have not seen before.
    if (n->category == ANCS_CATEGORY_LOCATION) {
        return true;
    }
    return s_permissive;
}

// Does this text line state a distance?
//
// Not "does it start with a number": an ETA line does too, and "5 min" parsed as FIVE METRES is
// exactly the class of bug that made "1.1 km" count down from one metre on the 2026-09-23
// capture. A distance line has a number followed by a real unit token, and the unit has to end on
// a word boundary or "min" matches "mi".
//
// Metric only, deliberately. normalize_parse_distance_metres() understands m and km; accepting a
// unit it would then misread is worse than declining the line and letting normalize_packet() fall
// back to scanning the title. Imperial waits for a capture that actually shows it.
static bool line_states_distance(const char *s) {
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    // Find the end of the leading number, mirroring the parser's own lenience about a decimal
    // point or comma so "1.1 km" and "1,1 km" both reach the unit check.
    const char *p = s;
    while (*p == ' ' || *p == '	') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    while ((*p >= '0' && *p <= '9') || *p == '.' || *p == ',') {
        p++;
    }
    // Maps separates value and unit with a no-break space (U+00A0, UTF-8 0xC2 0xA0).
    while (*p == ' ' || *p == '	' || (unsigned char)*p == 0xC2 || (unsigned char)*p == 0xA0) {
        p++;
    }

    static const char *kUnits[] = {"km", "m", "kilometres", "kilometers", "metres", "meters"};
    for (size_t i = 0; i < sizeof(kUnits) / sizeof(kUnits[0]); i++) {
        const size_t n = strlen(kUnits[i]);
        size_t j = 0;
        while (j < n && p[j] != '\0' && lower(p[j]) == kUnits[i][j]) {
            j++;
        }
        if (j != n) {
            continue;
        }
        // Word boundary, or "m" swallows the "m" of "min" and "metres" of a longer word.
        const char c = p[n];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            continue;
        }
        return true;
    }
    return false;
}

// Truncation here is intended and routine -- ANCS attributes are longer than the packet fields
// they land in -- so this copies an explicit length rather than using strncpy, whose truncation
// warning would be correct and useless.
static void copy_bounded(char *dst, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool ancs_nav_to_raw(const ancs_notification_t *n, uint32_t sequence, raw_notif_t *out) {
    if (n == NULL || out == NULL) {
        return false;
    }
    if (n->title[0] == '\0' && n->subtitle[0] == '\0' && n->message[0] == '\0') {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->sequence = sequence;

    // Everything ANCS cannot carry stays at its sentinel. A missing field must never become a
    // fabricated zero (SPEC §3) -- on this path that is most of the packet, and the telemetry
    // half arrives separately from the iOS companion app.
    out->gnss_fix_valid = false;
    out->speed_kmh_x10 = RAW_U16_UNKNOWN;
    out->heading_deg = RAW_U16_UNKNOWN;
    out->battery_percent = RAW_U8_UNKNOWN;
    out->progress = -1;
    out->progress_max = -1;
    out->icon_rotation_deg = RAW_I16_UNKNOWN;
    out->bearing_accuracy_deg_x10 = RAW_U16_UNKNOWN;
    out->yaw_deg = RAW_U16_UNKNOWN;
    out->yaw_rate_dps_x10 = RAW_I16_UNKNOWN;
    out->epoch_s = RAW_U32_UNKNOWN;
    out->tz_offset_min = RAW_I16_UNKNOWN;
    out->segment_count = 0;
    out->lat_e7 = RAW_I32_UNKNOWN;
    out->lon_e7 = RAW_I32_UNKNOWN;
    out->h_accuracy_m = RAW_U16_UNKNOWN;
    for (int i = 0; i < 3; i++) {
        out->accel_mg[i] = RAW_I16_UNKNOWN;
        out->gyro_mdps[i] = RAW_I16_UNKNOWN;
    }

    // Title is the maneuver line: derive_maneuver() and extract_street() both read it.
    copy_bounded(out->title_str, sizeof(out->title_str), n->title);

    // ANCS has no numeric distance field, so work out which of the two remaining text lines holds
    // one and give that to distance_str; the other becomes the ETA line. Which line Google Maps
    // uses for what on iOS is exactly what the first capture answers, so decide it from the text
    // rather than hardcoding a slot that may turn out to be the wrong one.
    //
    // If neither parses, distance_str is left empty and normalize_packet() falls back to scanning
    // the title, which is the behaviour the Android path already relies on.
    const bool sub_has_distance = line_states_distance(n->subtitle);
    const bool msg_has_distance = line_states_distance(n->message);

    if (sub_has_distance) {
        copy_bounded(out->distance_str, sizeof(out->distance_str), n->subtitle);
        copy_bounded(out->eta_str, sizeof(out->eta_str), n->message);
    } else if (msg_has_distance) {
        copy_bounded(out->distance_str, sizeof(out->distance_str), n->message);
        copy_bounded(out->eta_str, sizeof(out->eta_str), n->subtitle);
    } else {
        // Neither is a distance. Prefer the subtitle as the ETA line: on Android that role is
        // android.subText, and the shapes have matched everywhere else so far.
        copy_bounded(out->eta_str, sizeof(out->eta_str),
                     n->subtitle[0] != '\0' ? n->subtitle : n->message);
    }
    return true;
}

static bool digits(const char *s, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

bool ancs_nav_parse_date(const char *date, int *year, int *month, int *day, int *hour, int *minute,
                         int *second) {
    if (date == NULL || strlen(date) < 15) {
        return false;
    }
    if (date[8] != 'T') {
        return false;
    }
    int y, mo, d, h, mi, s;
    if (!digits(date, 4, &y) || !digits(date + 4, 2, &mo) || !digits(date + 6, 2, &d) ||
        !digits(date + 9, 2, &h) || !digits(date + 11, 2, &mi) || !digits(date + 13, 2, &s)) {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
        return false;
    }
    if (year) *year = y;
    if (month) *month = mo;
    if (day) *day = d;
    if (hour) *hour = h;
    if (minute) *minute = mi;
    if (second) *second = s;
    return true;
}
