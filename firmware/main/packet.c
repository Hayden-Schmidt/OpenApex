#include "packet.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t read_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void copy_str(char *dst, size_t dst_len, const uint8_t *src) {
    // Copy at most dst_len - 1 bytes and force a terminator. Source strings from the relay are
    // already truncated+terminated; this is the defensive terminal-side bound.
    size_t n = 0;
    while (n < dst_len - 1 && src[n] != 0) {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = '\0';
}

bool packet_decode(const uint8_t *data, size_t len, raw_notif_t *out) {
    if (data == NULL || out == NULL || len < RAW_NOTIF_PACKET_SIZE) {
        return false;
    }
    if (data[0] != RAW_NOTIF_VERSION) {
        return false;
    }

    out->gnss_fix_valid = (data[1] & 0x01) != 0;
    out->sequence = read_u32(&data[2]);
    out->speed_kmh_x10 = read_u16(&data[6]);
    out->heading_deg = read_u16(&data[8]);
    out->battery_percent = data[10];

    uint32_t progress = read_u32(&data[11]);
    out->progress = (progress == RAW_U32_UNKNOWN) ? -1 : (int32_t)progress;

    uint32_t progress_max = read_u32(&data[15]);
    out->progress_max = (progress_max == RAW_U32_UNKNOWN) ? -1 : (int32_t)progress_max;

    copy_str(out->distance_str, RAW_DIST_STR_LEN, &data[19]);
    copy_str(out->eta_str, RAW_ETA_STR_LEN, &data[35]);
    copy_str(out->title_str, RAW_TITLE_STR_LEN, &data[67]);

    out->accel_mg[0] = read_i16(&data[131]);
    out->accel_mg[1] = read_i16(&data[133]);
    out->accel_mg[2] = read_i16(&data[135]);
    out->gyro_mdps[0] = read_i16(&data[137]);
    out->gyro_mdps[1] = read_i16(&data[139]);
    out->gyro_mdps[2] = read_i16(&data[141]);
    out->icon_rotation_deg = read_i16(&data[143]);

    out->bearing_accuracy_deg_x10 = read_u16(&data[145]);
    out->yaw_deg = read_u16(&data[147]);
    out->yaw_rate_dps_x10 = read_i16(&data[149]);

    out->epoch_s = read_u32(&data[152]);
    out->tz_offset_min = read_i16(&data[156]);
    // A count past the cap is a malformed packet, not a reason to read past the segment table.
    out->segment_count = data[158] <= RAW_MAX_SEGMENTS ? data[158] : 0;
    for (size_t i = 0; i < RAW_MAX_SEGMENTS; i++) {
        const uint8_t *s = &data[159 + i * 5];
        out->segments[i].end_permille = read_u16(s);
        out->segments[i].r = s[2];
        out->segments[i].g = s[3];
        out->segments[i].b = s[4];
    }
    return true;
}
