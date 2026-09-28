#include "settings.h"

#include "pipeline.h"

// Current settings start at the defaults so a build that never loads NVS (host tests, the
// simulator) behaves exactly as before settings existed.
static const terminal_settings_t kDefaults = {
    .accent_rgb = 0x00FFE8,
    .avatar = SETTINGS_AVATAR_DEFAULT,
    .eta_format = PIPELINE_ETA_FORMAT_DEFAULT,
    .clock_12h = 0,
    .units = SETTINGS_UNITS_METRIC,
    .dial_outer = 0, // GUI_DIAL_OUTER_COMPASS
    .friends_on_compass = 1,
    .friend_marker = SETTINGS_FRIEND_MARKER_CHARACTER,
    .mesh_enabled = 1,
};
static terminal_settings_t s_current;
static bool s_current_init;
static volatile uint32_t s_generation;

void settings_defaults(terminal_settings_t *out) { *out = kDefaults; }

// Highest valid value of each one-byte tag, indexed by tag; 0 marks a tag that is not one byte.
static const uint8_t kByteTagMax[] = {
    [SETTINGS_TAG_AVATAR] = SETTINGS_AVATAR_CUSTOM, // plus the gap check below
    [SETTINGS_TAG_ETA_FORMAT] = PIPELINE_ETA_TIME_LEFT,
    [SETTINGS_TAG_CLOCK_12H] = 1,
    [SETTINGS_TAG_UNITS] = SETTINGS_UNITS_IMPERIAL,
    [SETTINGS_TAG_DIAL_OUTER] = 1,
    [SETTINGS_TAG_FRIENDS_ON_COMPASS] = 1,
    [SETTINGS_TAG_FRIEND_MARKER] = SETTINGS_FRIEND_MARKER_DOT,
    [SETTINGS_TAG_MESH_ENABLED] = 1,
};

bool settings_tlv_decode(const uint8_t *buf, size_t len, terminal_settings_t *inout) {
    if (len < 1 || buf[0] != SETTINGS_SCHEMA_VERSION) return false;
    terminal_settings_t s = *inout;
    size_t i = 1;
    while (i < len) {
        if (len - i < 2) return false;
        const uint8_t tag = buf[i];
        const uint8_t n = buf[i + 1];
        const uint8_t *v = &buf[i + 2];
        if (len - i - 2 < n) return false;
        i += 2u + n;

        if (tag == SETTINGS_TAG_ACCENT) {
            if (n != 3) return false;
            s.accent_rgb = ((uint32_t)v[0] << 16) | ((uint32_t)v[1] << 8) | v[2];
            continue;
        }
        if (tag >= sizeof(kByteTagMax) || kByteTagMax[tag] == 0) continue; // unknown: skip
        if (n != 1 || v[0] > kByteTagMax[tag]) return false;
        if (tag == SETTINGS_TAG_AVATAR && v[0] > SETTINGS_AVATAR_PRESET_MAX &&
            v[0] != SETTINGS_AVATAR_CUSTOM) {
            return false;
        }
        switch (tag) {
        case SETTINGS_TAG_AVATAR: s.avatar = v[0]; break;
        case SETTINGS_TAG_ETA_FORMAT: s.eta_format = v[0]; break;
        case SETTINGS_TAG_CLOCK_12H: s.clock_12h = v[0]; break;
        case SETTINGS_TAG_UNITS: s.units = v[0]; break;
        case SETTINGS_TAG_DIAL_OUTER: s.dial_outer = v[0]; break;
        case SETTINGS_TAG_FRIENDS_ON_COMPASS: s.friends_on_compass = v[0]; break;
        case SETTINGS_TAG_FRIEND_MARKER: s.friend_marker = v[0]; break;
        case SETTINGS_TAG_MESH_ENABLED: s.mesh_enabled = v[0]; break;
        }
    }
    *inout = s;
    return true;
}

size_t settings_tlv_encode(const terminal_settings_t *s, uint8_t *buf, size_t cap) {
    if (cap < SETTINGS_TLV_MAX_LEN) return 0;
    size_t i = 0;
    buf[i++] = SETTINGS_SCHEMA_VERSION;
    buf[i++] = SETTINGS_TAG_ACCENT;
    buf[i++] = 3;
    buf[i++] = (uint8_t)(s->accent_rgb >> 16);
    buf[i++] = (uint8_t)(s->accent_rgb >> 8);
    buf[i++] = (uint8_t)s->accent_rgb;
    const uint8_t bytes[][2] = {
        {SETTINGS_TAG_AVATAR, s->avatar},       {SETTINGS_TAG_ETA_FORMAT, s->eta_format},
        {SETTINGS_TAG_CLOCK_12H, s->clock_12h}, {SETTINGS_TAG_UNITS, s->units},
        {SETTINGS_TAG_DIAL_OUTER, s->dial_outer},
        {SETTINGS_TAG_FRIENDS_ON_COMPASS, s->friends_on_compass},
        {SETTINGS_TAG_FRIEND_MARKER, s->friend_marker},
        {SETTINGS_TAG_MESH_ENABLED, s->mesh_enabled},
    };
    for (size_t k = 0; k < sizeof(bytes) / sizeof(bytes[0]); k++) {
        buf[i++] = bytes[k][0];
        buf[i++] = 1;
        buf[i++] = bytes[k][1];
    }
    return i;
}

const terminal_settings_t *settings_get(void) {
    // Lazily seeded rather than statically initialised: C cannot copy kDefaults at load time.
    // First call is on the boot path (settings_store_load or gui_app_init), before any other task.
    if (!s_current_init) {
        s_current = kDefaults;
        s_current_init = true;
    }
    return &s_current;
}

void settings_set(const terminal_settings_t *s) {
    s_current = *s;
    s_current_init = true;
    s_generation++;
}

uint32_t settings_generation(void) { return s_generation; }
