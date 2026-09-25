#include "pipeline.h"

#include <stdio.h>
#include <string.h>

// Dev-only drive logger. This file stays host-testable: every drive_log_* call compiles to a
// static inline no-op unless OPENAPEX_DRIVE_LOG is defined, which the host tests never define.
#include "drive_log.h"
#include "heading_fusion.h"
#include "normalize.h"
#include "odometer.h"
#include "packet.h"

// Identity of the maneuver currently being counted down, used only to tell the countdown when to
// discard its smoothed speed and start fresh.
//
// This deliberately does NOT use the packet sequence number. That increments on every packet the
// phone sends (several per second), so passing it made countdown_accept() see a "new maneuver"
// every single time and reset the speed filter on each one -- the 0.7/0.3 smoothing never ran.
// A maneuver is the same maneuver while the glyph and the street name hold steady, regardless of
// how many packets describe it.
static nav_icon_t s_last_icon = NAV_ICON_UNKNOWN;
static char s_last_street[NAV_STREET_LEN];
static uint32_t s_maneuver_id;
static bool s_have_maneuver;

// Wall clock, from the phone. The C3 has no RTC, so time is the last synced local epoch carried
// forward on the monotonic clock -- which keeps the clock running across a dropped link.
static bool s_have_time;
static int64_t s_local_epoch_s_at_sync;
static uint32_t s_sync_ms;

static pipeline_eta_format_t s_eta_format = PIPELINE_ETA_FORMAT_DEFAULT;
// The arrival time as normalize.c left it; out->eta is rebuilt from it every tick so the time left
// keeps counting down between packets.
static char s_eta_arrival[NAV_ETA_LEN];

void pipeline_set_eta_format(pipeline_eta_format_t format) { s_eta_format = format; }

static uint32_t maneuver_identity(const nav_model_t *model) {
    bool changed = !s_have_maneuver || model->icon_type != s_last_icon ||
                   strncmp(model->street_name, s_last_street, sizeof(s_last_street)) != 0;
    if (changed) {
        s_maneuver_id++;
        s_last_icon = model->icon_type;
        strncpy(s_last_street, model->street_name, sizeof(s_last_street) - 1);
        s_last_street[sizeof(s_last_street) - 1] = '\0';
        s_have_maneuver = true;
    }
    return s_maneuver_id;
}

void pipeline_reset(void) {
    s_last_icon = NAV_ICON_UNKNOWN;
    s_last_street[0] = '\0';
    s_maneuver_id = 0;
    s_have_maneuver = false;
    countdown_reset();
    heading_fusion_reset();
    odometer_reset();
    s_have_time = false;
    s_eta_arrival[0] = '\0';
}

static void accept_time(const raw_notif_t *raw, uint32_t now_ms) {
    // epoch 0 is a zero-filled field, not 1970: never show a fabricated time.
    if (raw->epoch_s == RAW_U32_UNKNOWN || raw->epoch_s == 0U || raw->tz_offset_min == (int16_t)RAW_I16_UNKNOWN) {
        return;
    }
    s_local_epoch_s_at_sync = (int64_t)raw->epoch_s + (int64_t)raw->tz_offset_min * 60;
    s_sync_ms = now_ms;
    s_have_time = true;
}

static void format_clock(uint32_t now_ms, char *out, size_t out_len) {
    if (!s_have_time) {
        out[0] = '\0';
        return;
    }
    int64_t local_s = s_local_epoch_s_at_sync + (int64_t)((now_ms - s_sync_ms) / 1000U);
    int64_t minute_of_day = ((local_s / 60) % 1440 + 1440) % 1440;
    snprintf(out, out_len, "%02d:%02d", (int)(minute_of_day / 60), (int)(minute_of_day % 60));
}

// Local minute of day, or -1 when the phone clock has never synced.
static int local_minute_of_day(uint32_t now_ms) {
    if (!s_have_time) return -1;
    int64_t local_s = s_local_epoch_s_at_sync + (int64_t)((now_ms - s_sync_ms) / 1000U);
    return (int)(((local_s / 60) % 1440 + 1440) % 1440);
}

// Minute of day of the first "H:MM" in `eta` (with an optional am/pm after it), or -1.
static int parse_arrival_minute(const char *eta) {
    for (const char *p = eta; *p != '\0'; p++) {
        int h, m, n = 0;
        if (*p < '0' || *p > '9' || sscanf(p, "%d:%2d%n", &h, &m, &n) != 2) continue;
        if (h > 23 || m > 59) return -1;
        const char *suffix = p + n;
        while (*suffix == ' ') suffix++;
        if ((suffix[0] == 'p' || suffix[0] == 'P') && h < 12) h += 12;
        if ((suffix[0] == 'a' || suffix[0] == 'A') && h == 12) h = 0;
        return h * 60 + m;
    }
    return -1;
}

static void format_eta(uint32_t now_ms, char *out, size_t out_len) {
    const int arrive = parse_arrival_minute(s_eta_arrival);
    if (arrive < 0) {  // no clock time in it: pass through whatever Maps said
        snprintf(out, out_len, "%s", s_eta_arrival);
        return;
    }
    const int now = local_minute_of_day(now_ms);
    if (s_eta_format != PIPELINE_ETA_TIME_LEFT || now < 0) {
        snprintf(out, out_len, "ETA %02d:%02d", arrive / 60, arrive % 60);
        return;
    }
    // An arrival past midnight wraps forward; one only just behind the clock (Maps not yet caught
    // up) reads as 0 rather than as nearly a day.
    int left = (arrive - now + 1440) % 1440;
    if (left > 720) left = 0;
    if (left < 60) {
        snprintf(out, out_len, "%d min", left);
    } else {
        snprintf(out, out_len, "%d h %d min", left / 60, left % 60);
    }
}

// Builds heading_fusion's input from the raw packet fields it needs directly -- gps course
// accuracy, phone yaw and yaw rate never survive into nav_model_t, which only carries the
// telemetry fields the display shows verbatim (see normalize.c). Sentinel-checked the same way
// packet_decode leaves them: RAW_U16_UNKNOWN/RAW_I16_UNKNOWN mean "no reading", never zero.
static void build_heading_input(const raw_notif_t *raw, uint32_t now_ms, heading_input_t *out) {
    memset(out, 0, sizeof(*out));
    out->timestamp_ms = now_ms;

    out->gps_course_valid = raw->heading_deg != RAW_U16_UNKNOWN;
    out->gps_course_deg = out->gps_course_valid ? (float)raw->heading_deg : 0.0f;

    out->gps_accuracy_valid = raw->bearing_accuracy_deg_x10 != RAW_U16_UNKNOWN;
    out->gps_accuracy_deg = out->gps_accuracy_valid ? (float)raw->bearing_accuracy_deg_x10 / 10.0f : 0.0f;

    out->speed_valid = raw->speed_kmh_x10 != RAW_U16_UNKNOWN;
    out->speed_kmh = out->speed_valid ? (float)raw->speed_kmh_x10 / 10.0f : 0.0f;

    out->yaw_valid = raw->yaw_deg != RAW_U16_UNKNOWN;
    out->yaw_deg = out->yaw_valid ? (float)raw->yaw_deg : 0.0f;

    out->yaw_rate_valid = raw->yaw_rate_dps_x10 != (int16_t)RAW_I16_UNKNOWN;
    out->yaw_rate_dps = out->yaw_rate_valid ? (float)raw->yaw_rate_dps_x10 / 10.0f : 0.0f;
}

void view_state_apply_packet(const raw_notif_t *raw, uint32_t now_ms, terminal_view_state_t *out) {
    nav_model_t model;
    normalize_packet(raw, &model);
    drive_log_model(&model);

    // Distance to the NEXT MANEUVER only. remaining_meters is deliberately not used as a fallback:
    // it is progressMax - progress, the distance left in the whole trip, and substituting it made
    // the display jump from "10 m" to thousands of metres on the final update before a turn --
    // exactly when the rider needs it most. The two quantities only coincide on the last leg of a
    // journey with one maneuver left.
    int32_t distance_meters = model.distance_meters;
    bool distance_valid = distance_meters >= 0;

    countdown_input_t input;
    memset(&input, 0, sizeof(input));
    input.distance_valid = distance_valid;
    input.distance_meters = distance_valid ? (uint32_t)distance_meters : 0U;
    input.timestamp_ms = now_ms;
    input.speed_kmh = (model.speed_kmh_x10 != NAV_U16_UNKNOWN) ? (float)model.speed_kmh_x10 / 10.0f : 0.0f;
    input.speed_valid = model.speed_kmh_x10 != NAV_U16_UNKNOWN;
    input.maneuver_sequence = maneuver_identity(&model);
    // The phone keeps sending telemetry-only packets (no title, distance, progress or glyph) while
    // Maps is not routing. Those must not seed a countdown baseline -- one would route every later
    // packet to VIEW_ACTIVE and pull the nav page up with no route -- and a route that ended must
    // not leave its anchor behind for the next one.
    const bool has_nav = raw->title_str[0] != '\0' || distance_valid || raw->progress >= 0 ||
                         raw->icon_rotation_deg != RAW_I16_UNKNOWN;
    if (has_nav) {
        countdown_accept(&input);
    } else {
        countdown_reset();
    }

    heading_input_t heading_input;
    build_heading_input(raw, now_ms, &heading_input);
    heading_fusion_accept(&heading_input);

    odometer_accept(raw->gnss_fix_valid, input.speed_valid, input.speed_kmh, now_ms);
    accept_time(raw, now_ms);

    out->icon_type = model.icon_type;
    out->speed_kmh_x10 = model.speed_kmh_x10;
    out->battery_percent = model.battery_percent;
    out->sequence = model.sequence;
    strncpy(out->street_name, model.street_name, sizeof(out->street_name) - 1);
    out->street_name[sizeof(out->street_name) - 1] = '\0';
    strncpy(s_eta_arrival, model.eta, sizeof(s_eta_arrival) - 1);
    s_eta_arrival[sizeof(s_eta_arrival) - 1] = '\0';
    memcpy(out->traffic, model.traffic, sizeof(out->traffic));
    out->traffic_count = model.traffic_count;
    out->trip_progress_permille = model.trip_progress_permille != NAV_U16_UNKNOWN ? model.trip_progress_permille : 0U;

    if (model.icon_type == NAV_ICON_ARRIVED) {
        out->state = VIEW_ARRIVED;
        out->distance_meters = 0;
    } else if (!distance_valid && !countdown_has_baseline()) {
        // Never had a distance for this maneuver — nothing to count down from.
        out->state = VIEW_IDLE;
        out->distance_meters = 0;
    } else {
        // Includes the no-distance-but-we-have-an-anchor case: Maps drops the distance on the
        // final update before the turn, and blanking the screen at the moment of the maneuver is
        // the worst possible behaviour. Keep counting down from the last known distance.
        out->state = VIEW_ACTIVE;
    }

    view_state_tick(now_ms, out);
    drive_log_view(out);
}

void view_state_tick(uint32_t now_ms, terminal_view_state_t *out) {
    // Compass output is independent of nav state -- the terminal is a speed/heading display first
    // and a nav display second (see RelayService.publish()), so this runs even in VIEW_IDLE, ahead
    // of the countdown-only early return below.
    heading_output_t heading = heading_fusion_estimate(now_ms);
    out->heading_deg = heading.valid ? (uint16_t)(((uint32_t)(heading.heading_deg + 0.5f)) % 360U) : NAV_U16_UNKNOWN;
    out->heading_confidence = heading.confidence;
    out->heading_frozen = heading.frozen;

    format_clock(now_ms, out->clock, sizeof(out->clock));
    format_eta(now_ms, out->eta, sizeof(out->eta));
    out->odometer_meters = odometer_meters();

    if (out->state != VIEW_ACTIVE && out->state != VIEW_STALE) {
        return;
    }
    countdown_output_t co = countdown_estimate(now_ms);
    out->distance_meters = co.distance_meters;
    out->stale = co.stale;
    if (co.stale && out->state == VIEW_ACTIVE) {
        out->state = VIEW_STALE;
    }
    // Called at 10Hz from countdown_task; drive_log_view() filters to actual changes.
    drive_log_view(out);
}
