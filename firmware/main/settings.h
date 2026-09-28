#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Rider-facing terminal settings, pushed from the phone over the settings characteristic
// (docs/OpenApex_SPEC.md §5.6) and persisted in NVS by settings_store.c so they survive a boot with
// no phone in range. Pure C with no ESP-IDF dependency: host tests and firmware/sim_lvgl link it
// as-is.
//
// Wire and storage format is the same TLV: a leading schema byte, then [tag u8][len u8][value]
// records. A write may carry any subset of tags; absent tags keep their current value.

#define SETTINGS_SCHEMA_VERSION 1u

enum {
    SETTINGS_TAG_ACCENT = 0x01,     // len 3: R, G, B
    SETTINGS_TAG_AVATAR = 0x02,     // len 1: settings_avatar_t
    SETTINGS_TAG_ETA_FORMAT = 0x03, // len 1: pipeline_eta_format_t
    SETTINGS_TAG_CLOCK_12H = 0x04,  // len 1: 0 = 24h, 1 = 12h
    SETTINGS_TAG_UNITS = 0x05,      // len 1: settings_units_t
    SETTINGS_TAG_DIAL_OUTER = 0x06, // len 1: gui_dial_outer_t
    SETTINGS_TAG_FRIENDS_ON_COMPASS = 0x07, // len 1: 0 hidden, 1 shown (rider mesh, §5.7)
    SETTINGS_TAG_FRIEND_MARKER = 0x08,      // len 1: settings_friend_marker_t
    SETTINGS_TAG_MESH_ENABLED = 0x09,       // len 1: 0 radio silent, 1 on
};

// Avatar values: the default character, a built-in character (1..SETTINGS_AVATAR_PRESET_MAX,
// gui/avatars_data.h) or the rider's upload.
typedef enum {
    SETTINGS_AVATAR_DEFAULT = 0,       // SETTINGS_AVATAR_DEFAULT_PRESET
    SETTINGS_AVATAR_PRESET_MAX = 9,    // presets are 1..9
    SETTINGS_AVATAR_CUSTOM = 0xFF,     // uploaded art (avatar_store.c); falls back to the default if none
} settings_avatar_t;

// The character behind SETTINGS_AVATAR_DEFAULT: the dragon.
#define SETTINGS_AVATAR_DEFAULT_PRESET 8

typedef enum {
    SETTINGS_UNITS_METRIC = 0,
    SETTINGS_UNITS_IMPERIAL = 1,
} settings_units_t;

typedef enum {
    SETTINGS_FRIEND_MARKER_CHARACTER = 0, // the friend's 8x8 character in their accent
    SETTINGS_FRIEND_MARKER_DOT = 1,       // a dot in their accent
} settings_friend_marker_t;

typedef struct {
    uint32_t accent_rgb; // 0xRRGGBB; every accent use in the GUI and the avatar tint
    uint8_t avatar;      // settings_avatar_t
    uint8_t eta_format;  // pipeline_eta_format_t
    uint8_t clock_12h;
    uint8_t units;       // settings_units_t
    uint8_t dial_outer;  // gui_dial_outer_t -- the page's start-up ring; touch-hold still swaps it
    uint8_t friends_on_compass;
    uint8_t friend_marker; // settings_friend_marker_t
    uint8_t mesh_enabled;
} terminal_settings_t;

// Longest encoding settings_tlv_encode() produces: schema byte + every tag.
#define SETTINGS_TLV_MAX_LEN (1u + (2u + 3u) + 8u * (2u + 1u))

void settings_defaults(terminal_settings_t *out);

// Applies a TLV write on top of `*inout`. All-or-nothing: on a wrong schema byte, a truncated
// record, a known tag with the wrong length or an out-of-range value, `*inout` is left untouched
// and false is returned. Unknown tags are skipped so an older terminal accepts a newer phone.
bool settings_tlv_decode(const uint8_t *buf, size_t len, terminal_settings_t *inout);

// Full encoding of `s`; returns bytes written, or 0 if `cap` is too small.
size_t settings_tlv_encode(const terminal_settings_t *s, uint8_t *buf, size_t cap);

// Process-wide current settings. One writer at a time (boot load, then the NimBLE host task); any
// task may read. Fields are independent bytes/words, so a reader racing a write sees at worst a mix
// of old and new values for one frame -- and settings_generation() has moved, so it re-reads.
const terminal_settings_t *settings_get(void);
void settings_set(const terminal_settings_t *s);

// Bumped by every settings_set(). Consumers compare against the last value they applied.
uint32_t settings_generation(void);

#ifdef __cplusplus
}
#endif
