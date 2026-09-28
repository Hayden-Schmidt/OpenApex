#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/ancs_core.h"
#include "../main/ancs_nav.h"
#include "../main/cts_client.h"
#include "../main/nav_model.h"
#include "../main/normalize.h"

// ---------------------------------------------------------------------------------------------
// Harness: a fake iPhone. Captures Control Point writes, and can answer them by emitting a Data
// Source response split at any boundary the test chooses -- which is the point, because iOS
// fragments that response arbitrarily, including mid-length-field.
// ---------------------------------------------------------------------------------------------

#define CP_MAX 16
static uint8_t g_cp[CP_MAX][ANCS_CP_BUF_LEN];
static size_t g_cp_len[CP_MAX];
static int g_cp_count;

static ancs_notification_t g_last;
static bool g_last_removed;
static int g_notif_count;

static int g_raw_count[3];

static void reset_capture(void) {
    g_cp_count = 0;
    g_notif_count = 0;
    g_last_removed = false;
    memset(&g_last, 0, sizeof(g_last));
    memset(g_raw_count, 0, sizeof(g_raw_count));
}

static void on_cp(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    assert(g_cp_count < CP_MAX);
    assert(len <= ANCS_CP_BUF_LEN);
    memcpy(g_cp[g_cp_count], data, len);
    g_cp_len[g_cp_count] = len;
    g_cp_count++;
}

static void on_notif(const ancs_notification_t *n, bool removed, void *ctx) {
    (void)ctx;
    g_last = *n;
    g_last_removed = removed;
    g_notif_count++;
}

static void on_raw(ancs_raw_kind_t kind, const uint8_t *data, size_t len, void *ctx) {
    (void)data;
    (void)len;
    (void)ctx;
    assert((int)kind >= 0 && (int)kind < 3);
    g_raw_count[(int)kind]++;
}

static void ns_event(ancs_core_t *c, uint8_t event_id, uint8_t flags, uint8_t category,
                     uint32_t uid) {
    uint8_t ev[ANCS_NOTIFICATION_SOURCE_LEN];
    ev[0] = event_id;
    ev[1] = flags;
    ev[2] = category;
    ev[3] = 1; // CategoryCount
    ev[4] = (uint8_t)(uid & 0xFFu);
    ev[5] = (uint8_t)((uid >> 8) & 0xFFu);
    ev[6] = (uint8_t)((uid >> 16) & 0xFFu);
    ev[7] = (uint8_t)((uid >> 24) & 0xFFu);
    assert(ancs_core_on_notification_source(c, ev, sizeof(ev)));
}

// Builds a Get Notification Attributes response body for the five attributes we always request.
static size_t build_response(uint8_t *out, uint32_t uid, const char *app, const char *title,
                             const char *subtitle, const char *message, const char *date) {
    const char *vals[5] = {app, title, subtitle, message, date};
    const uint8_t ids[5] = {ANCS_ATTR_APP_IDENTIFIER, ANCS_ATTR_TITLE, ANCS_ATTR_SUBTITLE,
                            ANCS_ATTR_MESSAGE, ANCS_ATTR_DATE};
    size_t o = 0;
    out[o++] = ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES;
    out[o++] = (uint8_t)(uid & 0xFFu);
    out[o++] = (uint8_t)((uid >> 8) & 0xFFu);
    out[o++] = (uint8_t)((uid >> 16) & 0xFFu);
    out[o++] = (uint8_t)((uid >> 24) & 0xFFu);
    for (int i = 0; i < 5; i++) {
        const size_t n = strlen(vals[i]);
        out[o++] = ids[i];
        out[o++] = (uint8_t)(n & 0xFFu);
        out[o++] = (uint8_t)(n >> 8);
        memcpy(&out[o], vals[i], n);
        o += n;
    }
    return o;
}

// Feeds a response to the core in fixed-size chunks, the way a real ATT MTU would carve it up.
static void feed_chunked(ancs_core_t *c, const uint8_t *buf, size_t len, size_t chunk) {
    for (size_t o = 0; o < len; o += chunk) {
        size_t n = len - o;
        if (n > chunk) {
            n = chunk;
        }
        assert(ancs_core_on_data_source(c, &buf[o], n));
    }
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------------------------

static void test_control_point_encoding(void) {
    uint8_t buf[ANCS_CP_BUF_LEN];

    const size_t n = ancs_encode_get_attributes(buf, sizeof(buf), 0x11223344u);
    // [cmd][uid u32][appid][title+u16][subtitle+u16][message+u16][date]
    assert(n == 1 + 4 + 1 + 3 + 3 + 3 + 1);
    assert(buf[0] == ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES);
    assert(rd_u32(&buf[1]) == 0x11223344u);
    assert(buf[5] == ANCS_ATTR_APP_IDENTIFIER);
    assert(buf[6] == ANCS_ATTR_TITLE);
    assert((buf[7] | (buf[8] << 8)) == ANCS_TITLE_LEN - 1);
    assert(buf[9] == ANCS_ATTR_SUBTITLE);
    assert(buf[12] == ANCS_ATTR_MESSAGE);
    assert(buf[15] == ANCS_ATTR_DATE);

    const size_t a = ancs_encode_perform_action(buf, sizeof(buf), 7u, ANCS_ACTION_POSITIVE);
    assert(a == 6);
    assert(buf[0] == ANCS_CMD_PERFORM_NOTIFICATION_ACTION && rd_u32(&buf[1]) == 7u && buf[5] == 0);

    // A buffer too small must write nothing rather than a truncated request.
    assert(ancs_encode_get_attributes(buf, 4, 1) == 0);
    assert(ancs_encode_perform_action(buf, 5, 1, ANCS_ACTION_POSITIVE) == 0);
}

// The reassembly is the fiddly part of ANCS: the same response must parse identically however iOS
// splits it, including one byte at a time, which lands splits inside every length field.
static void test_data_source_reassembly_at_every_split(void) {
    uint8_t resp[ANCS_DS_BUF_LEN];
    const size_t len = build_response(resp, 42u, "com.google.Maps", "Turn right onto Greville Rd",
                                      "220 m", "12 min", "20260928T101500");

    for (size_t chunk = 1; chunk <= len; chunk++) {
        ancs_core_t c;
        reset_capture();
        ancs_core_init(&c, on_cp, on_notif, NULL);
        ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 42u);
        assert(g_cp_count == 1); // the fetch went out immediately

        feed_chunked(&c, resp, len, chunk);

        assert(g_notif_count == 1);
        assert(!g_last_removed);
        assert(g_last.uid == 42u);
        assert(g_last.category == ANCS_CATEGORY_LOCATION);
        assert(strcmp(g_last.app_id, "com.google.Maps") == 0);
        assert(strcmp(g_last.title, "Turn right onto Greville Rd") == 0);
        assert(strcmp(g_last.subtitle, "220 m") == 0);
        assert(strcmp(g_last.message, "12 min") == 0);
        assert(strcmp(g_last.date, "20260928T101500") == 0);
        assert(c.stat_attrs_complete == 1);
    }
}

// Turn-by-turn arrives as MODIFIED on one long-lived notification, so a modify must re-fetch.
static void test_modified_refetches(void) {
    ancs_core_t c;
    uint8_t resp[ANCS_DS_BUF_LEN];
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);

    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 9u);
    size_t len = build_response(resp, 9u, "com.google.Maps", "In 400 m, turn left", "", "", "");
    feed_chunked(&c, resp, len, 20);
    assert(g_notif_count == 1 && strcmp(g_last.title, "In 400 m, turn left") == 0);

    ns_event(&c, ANCS_EVENT_MODIFIED, 0, ANCS_CATEGORY_LOCATION, 9u);
    assert(g_cp_count == 2); // a second fetch, not a silent reuse of the stale text
    len = build_response(resp, 9u, "com.google.Maps", "Turn left onto Shakespeare Rd", "", "", "");
    feed_chunked(&c, resp, len, 20);
    assert(g_notif_count == 2 && strcmp(g_last.title, "Turn left onto Shakespeare Rd") == 0);
}

static void test_removed(void) {
    ancs_core_t c;
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);

    ns_event(&c, ANCS_EVENT_ADDED, ANCS_EVENT_FLAG_POSITIVE_ACTION, ANCS_CATEGORY_INCOMING_CALL, 5u);
    assert(ancs_core_category_of(&c, 5u) == ANCS_CATEGORY_INCOMING_CALL);

    ns_event(&c, ANCS_EVENT_REMOVED, 0, ANCS_CATEGORY_OTHER, 5u);
    assert(g_notif_count == 1 && g_last_removed);
    // The category comes from what we tracked, not from the (unreliable) removal event.
    assert(g_last.category == ANCS_CATEGORY_INCOMING_CALL);
    assert(ancs_core_category_of(&c, 5u) == -1);
}

// A notification withdrawn mid-fetch must not complete: the page it would drive is already gone.
static void test_removed_during_fetch(void) {
    ancs_core_t c;
    uint8_t resp[ANCS_DS_BUF_LEN];
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);

    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 3u);
    const size_t len = build_response(resp, 3u, "com.google.Maps", "Turn right", "", "", "");
    assert(ancs_core_on_data_source(&c, resp, 8)); // partial
    ns_event(&c, ANCS_EVENT_REMOVED, 0, ANCS_CATEGORY_LOCATION, 3u);
    const int after_removal = g_notif_count;
    // The tail of the abandoned response must be discarded, not folded into the next one.
    assert(ancs_core_on_data_source(&c, &resp[8], len - 8));
    assert(g_notif_count == after_removal);
}

static void test_malformed_and_overflow(void) {
    ancs_core_t c;
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);
    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 1u);

    // Wrong command id in the response header.
    uint8_t bad[8] = {0x7F, 1, 0, 0, 0, 0, 0, 0};
    assert(!ancs_core_on_data_source(&c, bad, sizeof(bad)));
    assert(c.stat_ds_malformed == 1);
    assert(g_notif_count == 0);

    // An attribute length that can never fit the reassembly buffer is a desync, not slow arrival:
    // it must fail immediately rather than stall the stream waiting for bytes that never come.
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);
    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 1u);
    uint8_t huge[8] = {ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES, 1, 0, 0, 0, ANCS_ATTR_TITLE, 0xFF, 0xFF};
    assert(!ancs_core_on_data_source(&c, huge, sizeof(huge)));
    assert(c.stat_ds_malformed == 1);

    // Too-short Notification Source events are rejected outright.
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);
    uint8_t shortev[4] = {0, 0, 0, 0};
    assert(!ancs_core_on_notification_source(&c, shortev, sizeof(shortev)));
    assert(g_notif_count == 0 && g_cp_count == 0);
}

// The raw tap must see every byte, in both directions, before any parsing -- a stream we fail to
// understand on day one is still recorded verbatim.
static void test_raw_tap(void) {
    ancs_core_t c;
    uint8_t resp[ANCS_DS_BUF_LEN];
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);
    ancs_core_set_raw_tap(&c, on_raw);

    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 1u);
    const size_t len = build_response(resp, 1u, "x", "y", "", "", "");
    feed_chunked(&c, resp, len, 7);

    assert(g_raw_count[ANCS_RAW_NOTIFICATION_SOURCE] == 1);
    assert(g_raw_count[ANCS_RAW_CONTROL_POINT] == 1);
    assert(g_raw_count[ANCS_RAW_DATA_SOURCE] == (int)((len + 6) / 7));

    // Even a malformed stream is tapped.
    uint8_t bad[3] = {0xEE, 0, 0};
    (void)ancs_core_on_data_source(&c, bad, sizeof(bad));
    assert(g_raw_count[ANCS_RAW_DATA_SOURCE] == (int)((len + 6) / 7) + 1);
}

// ANCS answers one Get Notification Attributes at a time, so a second notification arriving
// mid-fetch must queue rather than race a second write onto the Control Point.
static void test_requests_are_serialised(void) {
    ancs_core_t c;
    uint8_t resp[ANCS_DS_BUF_LEN];
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);

    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 1u);
    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_SOCIAL, 2u);
    assert(g_cp_count == 1); // only the first went out

    size_t len = build_response(resp, 1u, "com.google.Maps", "Turn right", "", "", "");
    feed_chunked(&c, resp, len, 32);
    assert(g_notif_count == 1);
    assert(g_cp_count == 2); // completing the first released the second
    assert(rd_u32(&g_cp[1][1]) == 2u);

    len = build_response(resp, 2u, "com.apple.MobileSMS", "Jane", "", "on my way", "");
    feed_chunked(&c, resp, len, 32);
    assert(g_notif_count == 2 && g_last.uid == 2u);
    assert(g_last.category == ANCS_CATEGORY_SOCIAL);
}

static void test_reset_clears_state(void) {
    ancs_core_t c;
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);
    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 77u);
    assert(ancs_core_category_of(&c, 77u) == ANCS_CATEGORY_LOCATION);
    // UIDs are per-connection: after a drop, nothing may be carried into the next link.
    ancs_core_reset(&c);
    assert(ancs_core_category_of(&c, 77u) == -1);
    assert(!c.request_active);
}

static void test_perform_action(void) {
    ancs_core_t c;
    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);
    ns_event(&c, ANCS_EVENT_ADDED, ANCS_EVENT_FLAG_POSITIVE_ACTION, ANCS_CATEGORY_INCOMING_CALL, 8u);
    const int before = g_cp_count;
    assert(ancs_core_perform_action(&c, 8u, ANCS_ACTION_POSITIVE));
    assert(g_cp_count == before + 1);
    const uint8_t *cp = g_cp[g_cp_count - 1];
    assert(cp[0] == ANCS_CMD_PERFORM_NOTIFICATION_ACTION && rd_u32(&cp[1]) == 8u && cp[5] == 0);
}

// ---------------------------------------------------------------------------------------------
// ancs_nav: the mapping onto the shape normalize.c already understands.
// ---------------------------------------------------------------------------------------------

static void test_app_classification(void) {
    assert(ancs_nav_classify_app("com.google.Maps") == ANCS_NAV_SOURCE_GOOGLE_MAPS);
    assert(ancs_nav_classify_app("COM.GOOGLE.MAPS") == ANCS_NAV_SOURCE_GOOGLE_MAPS);
    assert(ancs_nav_classify_app("com.waze.iphone") == ANCS_NAV_SOURCE_WAZE);
    assert(ancs_nav_classify_app("com.apple.Maps") == ANCS_NAV_SOURCE_APPLE_MAPS);
    assert(ancs_nav_classify_app("com.apple.MobileSMS") == ANCS_NAV_SOURCE_UNKNOWN);
    assert(ancs_nav_classify_app("") == ANCS_NAV_SOURCE_UNKNOWN);
    assert(ancs_nav_classify_app(NULL) == ANCS_NAV_SOURCE_UNKNOWN);
    // A prefix must not match: com.google.Map is not Maps.
    assert(ancs_nav_classify_app("com.google.Map") == ANCS_NAV_SOURCE_UNKNOWN);
}

static void test_navigation_gating(void) {
    ancs_notification_t n;
    memset(&n, 0, sizeof(n));

    ancs_nav_set_permissive(false);
    strcpy(n.app_id, "com.google.Maps");
    n.category = ANCS_CATEGORY_LOCATION;
    assert(ancs_nav_is_navigation(&n));

    // An incoming call is call_state's, whoever posted it -- it must never write the nav page.
    n.category = ANCS_CATEGORY_INCOMING_CALL;
    assert(!ancs_nav_is_navigation(&n));

    // Strict mode rejects an unknown app outside the Location category...
    strcpy(n.app_id, "com.apple.MobileSMS");
    n.category = ANCS_CATEGORY_SOCIAL;
    assert(!ancs_nav_is_navigation(&n));
    // ...but Location is the strongest hint we have for an app we have not seen before.
    n.category = ANCS_CATEGORY_LOCATION;
    assert(ancs_nav_is_navigation(&n));

    // Capture mode lets everything through so a first real-iPhone recording is not pre-filtered.
    ancs_nav_set_permissive(true);
    n.category = ANCS_CATEGORY_SOCIAL;
    assert(ancs_nav_is_navigation(&n));
    assert(ancs_nav_permissive());
    ancs_nav_set_permissive(false);
    assert(!ancs_nav_permissive());
    ancs_nav_set_permissive(true);
}

static void test_ancs_to_raw_fields(void) {
    ancs_notification_t n;
    raw_notif_t raw;
    memset(&n, 0, sizeof(n));
    strcpy(n.app_id, "com.google.Maps");
    strcpy(n.title, "Turn right onto Greville Rd");
    strcpy(n.subtitle, "220 m");
    strcpy(n.message, "Arrive 12:11");

    assert(ancs_nav_to_raw(&n, 17u, &raw));
    assert(raw.sequence == 17u);
    assert(strcmp(raw.title_str, "Turn right onto Greville Rd") == 0);
    // The subtitle parsed as a distance, so it is the distance line and the message is the ETA.
    assert(strcmp(raw.distance_str, "220 m") == 0);
    assert(strcmp(raw.eta_str, "Arrive 12:11") == 0);

    // Everything ANCS cannot carry stays at its sentinel. A missing field is never a zero.
    assert(!raw.gnss_fix_valid);
    assert(raw.speed_kmh_x10 == RAW_U16_UNKNOWN);
    assert(raw.heading_deg == RAW_U16_UNKNOWN);
    assert(raw.battery_percent == RAW_U8_UNKNOWN);
    assert(raw.progress == -1 && raw.progress_max == -1);
    assert(raw.icon_rotation_deg == RAW_I16_UNKNOWN);
    assert(raw.yaw_deg == RAW_U16_UNKNOWN);
    assert(raw.epoch_s == RAW_U32_UNKNOWN && raw.tz_offset_min == RAW_I16_UNKNOWN);
    assert(raw.segment_count == 0);
    assert(raw.lat_e7 == RAW_I32_UNKNOWN && raw.lon_e7 == RAW_I32_UNKNOWN);
    assert(raw.accel_mg[0] == RAW_I16_UNKNOWN && raw.gyro_mdps[2] == RAW_I16_UNKNOWN);

    // Which line holds the distance is decided from the text, because which line iOS uses is
    // exactly what the first capture answers. Swapped, it still lands in the right slot.
    memset(&n, 0, sizeof(n));
    strcpy(n.title, "Turn left");
    strcpy(n.subtitle, "5 min");
    strcpy(n.message, "1.1 km");
    assert(ancs_nav_to_raw(&n, 1u, &raw));
    assert(strcmp(raw.distance_str, "1.1 km") == 0);
    assert(strcmp(raw.eta_str, "5 min") == 0);

    // Neither line is a distance: the subtitle becomes the ETA and normalize falls back to the
    // title for the distance.
    memset(&n, 0, sizeof(n));
    strcpy(n.title, "In 300 m, turn right onto Onewa Rd");
    strcpy(n.subtitle, "Arrive 09:02");
    assert(ancs_nav_to_raw(&n, 1u, &raw));
    assert(raw.distance_str[0] == '\0');
    assert(strcmp(raw.eta_str, "Arrive 09:02") == 0);

    // A time line is not a distance line. "5 min" read as five metres is the same class of bug
    // as "1.1 km" counting down from one metre -- a leading number is not enough, the unit has to
    // be there, and "m" must not match the "m" of "min".
    static const char *kNotDistances[] = {"5 min", "12 min", "Arrive 12:11", "3 mins", "8 minutes"};
    for (size_t i = 0; i < sizeof(kNotDistances) / sizeof(kNotDistances[0]); i++) {
        memset(&n, 0, sizeof(n));
        strcpy(n.title, "Turn left");
        strcpy(n.subtitle, kNotDistances[i]);
        strcpy(n.message, "300 m");
        assert(ancs_nav_to_raw(&n, 1u, &raw));
        assert(strcmp(raw.distance_str, "300 m") == 0);
        assert(strcmp(raw.eta_str, kNotDistances[i]) == 0);
    }

    // The units that are real distances, including the no-break space Maps uses.
    static const char *kDistances[] = {"220 m", "1.1 km", "1,1 km", "50m", "2 KM", "900 metres",
                                       "300\xc2\xa0" "m"};
    for (size_t i = 0; i < sizeof(kDistances) / sizeof(kDistances[0]); i++) {
        memset(&n, 0, sizeof(n));
        strcpy(n.title, "Turn left");
        strcpy(n.subtitle, kDistances[i]);
        strcpy(n.message, "12 min");
        assert(ancs_nav_to_raw(&n, 1u, &raw));
        assert(strcmp(raw.distance_str, kDistances[i]) == 0);
        assert(strcmp(raw.eta_str, "12 min") == 0);
    }

    // An empty notification carries nothing a nav page could use.
    memset(&n, 0, sizeof(n));
    assert(!ancs_nav_to_raw(&n, 1u, &raw));
}

// The point of SPEC §2.4: the same normalizer, the same answer, from either platform's source.
static void test_ancs_and_android_normalize_identically(void) {
    static const struct {
        const char *title;
        const char *distance;
        nav_icon_t icon;
        int32_t metres;
    } kCases[] = {
        {"Turn right onto Greville Rd", "220 m", NAV_ICON_TURN_RIGHT, 220},
        {"Turn left onto Shakespeare Rd", "80 m", NAV_ICON_TURN_LEFT, 80},
        {"Keep right onto SH 1", "1.1 km", NAV_ICON_SLIGHT_RIGHT, 1100},
        {"Make a U-turn", "50 m", NAV_ICON_U_TURN, 50},
    };

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        // The Android shape: a clean numeric distance field alongside the title.
        raw_notif_t android;
        memset(&android, 0, sizeof(android));
        android.speed_kmh_x10 = RAW_U16_UNKNOWN;
        android.heading_deg = RAW_U16_UNKNOWN;
        android.battery_percent = RAW_U8_UNKNOWN;
        android.progress = -1;
        android.progress_max = -1;
        android.icon_rotation_deg = RAW_I16_UNKNOWN;
        strcpy(android.title_str, kCases[i].title);
        strcpy(android.distance_str, kCases[i].distance);

        // The ANCS shape: the same facts, arriving as notification text.
        ancs_notification_t n;
        raw_notif_t ios;
        memset(&n, 0, sizeof(n));
        strcpy(n.app_id, "com.google.Maps");
        strcpy(n.title, kCases[i].title);
        strcpy(n.subtitle, kCases[i].distance);
        assert(ancs_nav_to_raw(&n, 0u, &ios));

        nav_model_t a, b;
        normalize_packet(&android, &a);
        normalize_packet(&ios, &b);

        assert(a.icon_type == kCases[i].icon);
        assert(a.distance_meters == kCases[i].metres);
        assert(b.icon_type == a.icon_type);
        assert(b.distance_meters == a.distance_meters);
        assert(strcmp(b.street_name, a.street_name) == 0);
    }
}

// The one real regression from losing the glyph: text never states a roundabout exit direction,
// and on iOS there is no icon angle to fall back on. It must degrade to ROUNDABOUT_STRAIGHT --
// never to a confident wrong turn arrow.
static void test_roundabout_degrades_safely_without_a_glyph(void) {
    ancs_notification_t n;
    raw_notif_t raw;
    nav_model_t m;
    memset(&n, 0, sizeof(n));
    strcpy(n.app_id, "com.google.Maps");
    strcpy(n.title, "At the roundabout, take the 1st exit");
    strcpy(n.subtitle, "150 m");
    assert(ancs_nav_to_raw(&n, 0u, &raw));
    normalize_packet(&raw, &m);
    assert(m.icon_type == NAV_ICON_ROUNDABOUT_STRAIGHT);
    assert(m.icon_type != NAV_ICON_TURN_LEFT && m.icon_type != NAV_ICON_TURN_RIGHT);
    assert(m.distance_meters == 150);
}

static void test_date_parsing(void) {
    int y, mo, d, h, mi, s;
    assert(ancs_nav_parse_date("20260928T101502", &y, &mo, &d, &h, &mi, &s));
    assert(y == 2026 && mo == 9 && d == 28 && h == 10 && mi == 15 && s == 2);
    assert(!ancs_nav_parse_date("20260928101502", &y, &mo, &d, &h, &mi, &s)); // missing T
    assert(!ancs_nav_parse_date("2026092", &y, &mo, &d, &h, &mi, &s));        // too short
    assert(!ancs_nav_parse_date("20261328T101502", &y, &mo, &d, &h, &mi, &s)); // month 13
    assert(!ancs_nav_parse_date("2026092xT101502", &y, &mo, &d, &h, &mi, &s)); // non-digit
    assert(!ancs_nav_parse_date("", &y, &mo, &d, &h, &mi, &s));
    assert(!ancs_nav_parse_date(NULL, &y, &mo, &d, &h, &mi, &s));
}

// Long values must truncate into our buffers rather than overrun them, and still terminate.
static void test_oversized_attributes(void) {
    ancs_core_t c;
    uint8_t resp[ANCS_DS_BUF_LEN];
    char longtitle[256];
    memset(longtitle, 'A', sizeof(longtitle) - 1);
    longtitle[sizeof(longtitle) - 1] = '\0';

    reset_capture();
    ancs_core_init(&c, on_cp, on_notif, NULL);
    ns_event(&c, ANCS_EVENT_ADDED, 0, ANCS_CATEGORY_LOCATION, 1u);
    const size_t len = build_response(resp, 1u, "com.google.Maps", longtitle, "", "", "");
    feed_chunked(&c, resp, len, 23);
    assert(g_notif_count == 1);
    assert(strlen(g_last.title) == ANCS_TITLE_LEN - 1);
    assert(g_last.title[ANCS_TITLE_LEN - 1] == '\0');
}

// ---------------------------------------------------------------------------------------------
// Current Time Service: the iOS clock, which has to work with no app running.
// ---------------------------------------------------------------------------------------------

static void make_cts(uint8_t *b, int year, int mon, int day, int h, int mi, int s) {
    b[0] = (uint8_t)(year & 0xFF);
    b[1] = (uint8_t)(year >> 8);
    b[2] = (uint8_t)mon;
    b[3] = (uint8_t)day;
    b[4] = (uint8_t)h;
    b[5] = (uint8_t)mi;
    b[6] = (uint8_t)s;
    b[7] = 1; // day of week
    b[8] = 0; // fractions256
    b[9] = 0; // adjust reason
}

static void test_cts(void) {
    // Anchors computed independently of the implementation.
    assert(cts_days_from_civil(1970, 1, 1) == 0);
    assert(cts_days_from_civil(1970, 1, 2) == 1);
    assert(cts_days_from_civil(2000, 3, 1) == 11017);
    assert(cts_days_from_civil(2026, 9, 28) == 20724);
    // Leap-year handling: 2024 has a 29 February, 2100 does not.
    assert(cts_days_from_civil(2024, 3, 1) - cts_days_from_civil(2024, 2, 28) == 2);
    assert(cts_days_from_civil(2100, 3, 1) - cts_days_from_civil(2100, 2, 28) == 1);

    uint8_t b[CTS_CURRENT_TIME_LEN];
    uint32_t epoch = 0;

    make_cts(b, 1970, 1, 1, 0, 0, 0);
    // Year 1970 is below the sanity floor -- a phone reporting it is not telling us the time.
    assert(!cts_parse_current_time(b, sizeof(b), &epoch));

    make_cts(b, 2026, 9, 28, 10, 15, 2);
    assert(cts_parse_current_time(b, sizeof(b), &epoch));
    assert(epoch == (uint32_t)20724 * 86400u + 10u * 3600u + 15u * 60u + 2u);
    // The terminal only ever shows HH:MM, so that is what has to survive the round trip.
    assert((epoch % 86400u) / 3600u == 10u && (epoch % 3600u) / 60u == 15u);

    // A short read is not a time.
    assert(!cts_parse_current_time(b, CTS_CURRENT_TIME_LEN - 1, &epoch));
    assert(!cts_parse_current_time(NULL, sizeof(b), &epoch));

    // All-zero is CTS "unknown", not midnight on 1 January year zero.
    memset(b, 0, sizeof(b));
    assert(!cts_parse_current_time(b, sizeof(b), &epoch));

    // Out-of-range fields are rejected rather than normalised into a plausible wrong time.
    make_cts(b, 2026, 13, 1, 0, 0, 0);
    assert(!cts_parse_current_time(b, sizeof(b), &epoch));
    make_cts(b, 2026, 1, 32, 0, 0, 0);
    assert(!cts_parse_current_time(b, sizeof(b), &epoch));
    make_cts(b, 2026, 1, 1, 24, 0, 0);
    assert(!cts_parse_current_time(b, sizeof(b), &epoch));
    make_cts(b, 2026, 1, 1, 0, 60, 0);
    assert(!cts_parse_current_time(b, sizeof(b), &epoch));
}

int main(void) {
    test_control_point_encoding();
    test_data_source_reassembly_at_every_split();
    test_modified_refetches();
    test_removed();
    test_removed_during_fetch();
    test_malformed_and_overflow();
    test_raw_tap();
    test_requests_are_serialised();
    test_reset_clears_state();
    test_perform_action();

    test_app_classification();
    test_navigation_gating();
    test_ancs_to_raw_fields();
    test_ancs_and_android_normalize_identically();
    test_roundabout_degrades_safely_without_a_glyph();
    test_date_parsing();
    test_oversized_attributes();
    test_cts();

    puts("ancs tests passed");
    return 0;
}
