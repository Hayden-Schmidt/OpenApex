#include "packet.h"

#include <string.h>

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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
    return true;
}
