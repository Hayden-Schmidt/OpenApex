#include "packet_merge.h"

#include <string.h>

// The held packet. Starts fully sentinelled, so a terminal that has heard only ANCS reports an
// unknown speed rather than zero.
static raw_notif_t s_held;
static bool s_init;
static uint32_t s_nav_ms;
static uint32_t s_telemetry_ms;
static bool s_nav_held;
static bool s_telemetry_held;

static void clear_nav(raw_notif_t *p) {
    p->distance_str[0] = '\0';
    p->eta_str[0] = '\0';
    p->title_str[0] = '\0';
    p->progress = -1;
    p->progress_max = -1;
    p->icon_rotation_deg = RAW_I16_UNKNOWN;
    p->segment_count = 0;
    memset(p->segments, 0, sizeof(p->segments));
}

static void clear_telemetry(raw_notif_t *p) {
    p->gnss_fix_valid = false;
    p->speed_kmh_x10 = RAW_U16_UNKNOWN;
    p->heading_deg = RAW_U16_UNKNOWN;
    p->battery_percent = RAW_U8_UNKNOWN;
    p->bearing_accuracy_deg_x10 = RAW_U16_UNKNOWN;
    p->yaw_deg = RAW_U16_UNKNOWN;
    p->yaw_rate_dps_x10 = RAW_I16_UNKNOWN;
    p->epoch_s = RAW_U32_UNKNOWN;
    p->tz_offset_min = RAW_I16_UNKNOWN;
    p->lat_e7 = RAW_I32_UNKNOWN;
    p->lon_e7 = RAW_I32_UNKNOWN;
    p->h_accuracy_m = RAW_U16_UNKNOWN;
    for (int i = 0; i < 3; i++) {
        p->accel_mg[i] = RAW_I16_UNKNOWN;
        p->gyro_mdps[i] = RAW_I16_UNKNOWN;
    }
}

static void copy_nav(raw_notif_t *dst, const raw_notif_t *src) {
    memcpy(dst->distance_str, src->distance_str, sizeof(dst->distance_str));
    memcpy(dst->eta_str, src->eta_str, sizeof(dst->eta_str));
    memcpy(dst->title_str, src->title_str, sizeof(dst->title_str));
    dst->progress = src->progress;
    dst->progress_max = src->progress_max;
    dst->icon_rotation_deg = src->icon_rotation_deg;
    dst->segment_count = src->segment_count;
    memcpy(dst->segments, src->segments, sizeof(dst->segments));
}

static void copy_telemetry(raw_notif_t *dst, const raw_notif_t *src) {
    dst->gnss_fix_valid = src->gnss_fix_valid;
    dst->speed_kmh_x10 = src->speed_kmh_x10;
    dst->heading_deg = src->heading_deg;
    dst->battery_percent = src->battery_percent;
    dst->bearing_accuracy_deg_x10 = src->bearing_accuracy_deg_x10;
    dst->yaw_deg = src->yaw_deg;
    dst->yaw_rate_dps_x10 = src->yaw_rate_dps_x10;
    dst->epoch_s = src->epoch_s;
    dst->tz_offset_min = src->tz_offset_min;
    dst->lat_e7 = src->lat_e7;
    dst->lon_e7 = src->lon_e7;
    dst->h_accuracy_m = src->h_accuracy_m;
    memcpy(dst->accel_mg, src->accel_mg, sizeof(dst->accel_mg));
    memcpy(dst->gyro_mdps, src->gyro_mdps, sizeof(dst->gyro_mdps));
}

void packet_merge_reset(void) {
    memset(&s_held, 0, sizeof(s_held));
    clear_nav(&s_held);
    clear_telemetry(&s_held);
    s_init = true;
    s_nav_held = false;
    s_telemetry_held = false;
}

static bool fresh(bool held, uint32_t stamp, uint32_t hold, uint32_t now_ms) {
    return held && (uint32_t)(now_ms - stamp) <= hold;
}

bool packet_merge_group_fresh(raw_group_t group, uint32_t now_ms) {
    if (group == RAW_GROUP_NAV) {
        return fresh(s_nav_held, s_nav_ms, PACKET_MERGE_NAV_HOLD_MS, now_ms);
    }
    return fresh(s_telemetry_held, s_telemetry_ms, PACKET_MERGE_TELEMETRY_HOLD_MS, now_ms);
}

bool packet_merge_accept(const raw_notif_t *in, uint32_t now_ms, raw_notif_t *out) {
    if (in == NULL || out == NULL || (in->source_groups & (RAW_GROUP_NAV | RAW_GROUP_TELEMETRY)) == 0) {
        return false;
    }
    if (!s_init) {
        packet_merge_reset();
    }

    if ((in->source_groups & RAW_GROUP_NAV) != 0) {
        copy_nav(&s_held, in);
        s_nav_ms = now_ms;
        s_nav_held = true;
    } else if (!packet_merge_group_fresh(RAW_GROUP_NAV, now_ms)) {
        clear_nav(&s_held);
        s_nav_held = false;
    }

    if ((in->source_groups & RAW_GROUP_TELEMETRY) != 0) {
        copy_telemetry(&s_held, in);
        s_telemetry_ms = now_ms;
        s_telemetry_held = true;
    } else if (!packet_merge_group_fresh(RAW_GROUP_TELEMETRY, now_ms)) {
        clear_telemetry(&s_held);
        s_telemetry_held = false;
    }

    // The sequence is the producer's, not the group's: whoever wrote last is the reason there is
    // a new view to build, and countdown's re-baselining wants it monotonic per producer. Both
    // producers count up on their own, so take the packet's rather than inventing a merged one.
    s_held.sequence = in->sequence;
    s_held.source_groups = in->source_groups;
    s_held.source_is_ios = in->source_is_ios;

    *out = s_held;
    return true;
}
