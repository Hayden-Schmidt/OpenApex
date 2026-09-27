#include "avatar_store.h"

#include "board_profile.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "host/ble_att.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "avatar_store";

#define AVATAR_PARTITION_SUBTYPE 0x9B
#define AVATAR_MAGIC 0x5641414FU // "OAAV" little-endian
#define AVATAR_DATA_OFFSET 16U
#define AVATAR_SECTOR 4096U

enum {
    AVATAR_OP_BEGIN = 0x01,  // [op][w u16][h u16][crc32 u32]
    AVATAR_OP_DATA = 0x02,   // [op][offset u16][bytes...]
    AVATAR_OP_COMMIT = 0x03, // [op]
    AVATAR_OP_CLEAR = 0x04,  // [op]
};

typedef struct {
    uint32_t magic;
    uint16_t w;
    uint16_t h;
    uint32_t size;
    uint32_t crc32;
} avatar_header_t;
_Static_assert(sizeof(avatar_header_t) == AVATAR_DATA_OFFSET, "header must fill the data offset");

// Upload state; touched only from the NimBLE host task.
static bool s_uploading;
static avatar_header_t s_pending;
static volatile bool s_changed;

static const esp_partition_t *partition(void) {
    static const esp_partition_t *part;
    if (part == NULL) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, AVATAR_PARTITION_SUBTYPE, "avatar");
    }
    return part;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Standard CRC-32 (zlib / java.util.zip.CRC32) of the stored mask, read back from flash in chunks
// so verification needs no mask-sized buffer.
static uint32_t flash_crc(const esp_partition_t *part, uint32_t size) {
    uint8_t chunk[64];
    uint32_t crc = 0;
    for (uint32_t off = 0; off < size; off += sizeof(chunk)) {
        const uint32_t n = size - off < sizeof(chunk) ? size - off : sizeof(chunk);
        if (esp_partition_read(part, AVATAR_DATA_OFFSET + off, chunk, n) != ESP_OK) return ~crc;
        crc = esp_rom_crc32_le(crc, chunk, n);
    }
    return crc;
}

uint8_t *avatar_store_read(uint16_t *w, uint16_t *h) {
    const esp_partition_t *part = partition();
    if (part == NULL) return NULL;
    avatar_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK || hdr.magic != AVATAR_MAGIC ||
        hdr.w != BOARD_AVATAR_PX || hdr.h != BOARD_AVATAR_PX ||
        hdr.size != (uint32_t)hdr.w * hdr.h) {
        return NULL;
    }
    // Plain malloc: on the S3 CONFIG_SPIRAM_USE_MALLOC already sends an allocation this size to
    // PSRAM; on the C3 it is 4.9 KB of internal heap.
    uint8_t *buf = malloc(hdr.size);
    if (buf == NULL) return NULL;
    if (esp_partition_read(part, AVATAR_DATA_OFFSET, buf, hdr.size) != ESP_OK ||
        esp_rom_crc32_le(0, buf, hdr.size) != hdr.crc32) {
        ESP_LOGW(TAG, "stored avatar failed its CRC; using the logo");
        free(buf);
        return NULL;
    }
    *w = hdr.w;
    *h = hdr.h;
    return buf;
}

bool avatar_store_take_changed(void) {
    if (!s_changed) return false;
    s_changed = false;
    return true;
}

int avatar_store_ble_write(const uint8_t *buf, uint16_t len) {
    const esp_partition_t *part = partition();
    if (part == NULL) return BLE_ATT_ERR_UNLIKELY;
    if (len < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    switch (buf[0]) {
    case AVATAR_OP_BEGIN: {
        if (len != 9) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        const uint16_t w = rd16(&buf[1]);
        const uint16_t h = rd16(&buf[3]);
        // One size per board: the phone reads it from device info and rasterises to fit, so the
        // terminal never scales.
        if (w != BOARD_AVATAR_PX || h != BOARD_AVATAR_PX) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        const uint32_t size = (uint32_t)w * h;
        const uint32_t span = (AVATAR_DATA_OFFSET + size + AVATAR_SECTOR - 1) & ~(AVATAR_SECTOR - 1);
        if (span > part->size) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        // Erasing also invalidates the old header, so an abandoned upload leaves the logo, never a
        // mix of two avatars.
        if (esp_partition_erase_range(part, 0, span) != ESP_OK) return BLE_ATT_ERR_UNLIKELY;
        s_pending = (avatar_header_t){AVATAR_MAGIC, w, h, size, rd32(&buf[5])};
        s_uploading = true;
        s_changed = true;
        return 0;
    }
    case AVATAR_OP_DATA: {
        if (!s_uploading) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        if (len < 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        const uint32_t off = rd16(&buf[1]);
        const uint32_t n = len - 3U;
        if (off + n > s_pending.size) return BLE_ATT_ERR_INVALID_OFFSET;
        return esp_partition_write(part, AVATAR_DATA_OFFSET + off, &buf[3], n) == ESP_OK
                   ? 0
                   : BLE_ATT_ERR_UNLIKELY;
    }
    case AVATAR_OP_COMMIT:
        if (!s_uploading) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        s_uploading = false;
        if (flash_crc(part, s_pending.size) != s_pending.crc32) {
            ESP_LOGW(TAG, "avatar upload CRC mismatch; discarded");
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        if (esp_partition_write(part, 0, &s_pending, sizeof(s_pending)) != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        ESP_LOGI(TAG, "avatar stored (%ux%u)", s_pending.w, s_pending.h);
        s_changed = true;
        return 0;
    case AVATAR_OP_CLEAR:
        if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        s_uploading = false;
        if (esp_partition_erase_range(part, 0, AVATAR_SECTOR) != ESP_OK) return BLE_ATT_ERR_UNLIKELY;
        s_changed = true;
        return 0;
    default:
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
}
