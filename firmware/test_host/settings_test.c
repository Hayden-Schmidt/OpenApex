#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../main/pipeline.h"
#include "../main/settings.h"

// gcc -std=c11 -o settings_test firmware/test_host/settings_test.c firmware/main/settings.c

// Golden fixture shared with docs/OpenApex_SPEC.md §5.5 and Android's SettingsTlvTest: the full
// encoding of the factory defaults.
static const uint8_t kDefaultsTlv[] = {
    0x01,                         // schema
    0x01, 0x03, 0x00, 0xC2, 0xFF, // accent #00C2FF
    0x02, 0x01, 0x00,             // avatar: logo
    0x03, 0x01, 0x01,             // ETA: time left
    0x04, 0x01, 0x00,             // clock: 24h
    0x05, 0x01, 0x00,             // units: metric
    0x06, 0x01, 0x00,             // dial outer: compass
};

// Field-wise: struct padding is not guaranteed to survive a copy, so memcmp would be flaky.
static int settings_equal(const terminal_settings_t *a, const terminal_settings_t *b) {
    return a->accent_rgb == b->accent_rgb && a->avatar == b->avatar &&
           a->eta_format == b->eta_format && a->clock_12h == b->clock_12h &&
           a->units == b->units && a->dial_outer == b->dial_outer;
}

static void test_defaults_golden(void) {
    terminal_settings_t s;
    settings_defaults(&s);
    uint8_t buf[SETTINGS_TLV_MAX_LEN];
    size_t n = settings_tlv_encode(&s, buf, sizeof(buf));
    assert(n == sizeof(kDefaultsTlv));
    assert(memcmp(buf, kDefaultsTlv, n) == 0);
    assert(settings_tlv_encode(&s, buf, sizeof(buf) - 1) == 0);
}

static void test_round_trip(void) {
    terminal_settings_t a = {0x123456, SETTINGS_AVATAR_CUSTOM, PIPELINE_ETA_ARRIVAL, 1,
                             SETTINGS_UNITS_IMPERIAL, 1};
    uint8_t buf[SETTINGS_TLV_MAX_LEN];
    size_t n = settings_tlv_encode(&a, buf, sizeof(buf));
    terminal_settings_t b;
    settings_defaults(&b);
    assert(settings_tlv_decode(buf, n, &b));
    assert(settings_equal(&a, &b));
}

static void test_partial_write_keeps_other_fields(void) {
    terminal_settings_t s;
    settings_defaults(&s);
    const uint8_t w[] = {0x01, 0x05, 0x01, 0x01};
    assert(settings_tlv_decode(w, sizeof(w), &s));
    assert(s.units == SETTINGS_UNITS_IMPERIAL);
    assert(s.accent_rgb == 0x00C2FF);
}

static void test_unknown_tag_skipped(void) {
    terminal_settings_t s;
    settings_defaults(&s);
    const uint8_t w[] = {0x01, 0x7F, 0x02, 0xAA, 0xBB, 0x04, 0x01, 0x01};
    assert(settings_tlv_decode(w, sizeof(w), &s));
    assert(s.clock_12h == 1);
}

static void test_rejects_leave_settings_untouched(void) {
    terminal_settings_t s, before;
    settings_defaults(&s);
    before = s;
    const uint8_t bad_schema[] = {0x02, 0x04, 0x01, 0x01};
    const uint8_t truncated[] = {0x01, 0x04, 0x01};
    const uint8_t bad_len[] = {0x01, 0x01, 0x02, 0xFF, 0xFF};
    const uint8_t out_of_range[] = {0x01, 0x04, 0x01, 0x01, 0x05, 0x01, 0x09};
    const uint8_t avatar_gap[] = {0x01, 0x02, 0x01, 0x0B}; // past the presets, not custom
    assert(!settings_tlv_decode(bad_schema, sizeof(bad_schema), &s));
    assert(!settings_tlv_decode(truncated, sizeof(truncated), &s));
    assert(!settings_tlv_decode(bad_len, sizeof(bad_len), &s));
    assert(!settings_tlv_decode(out_of_range, sizeof(out_of_range), &s));
    assert(!settings_tlv_decode(avatar_gap, sizeof(avatar_gap), &s));
    assert(!settings_tlv_decode(bad_schema, 0, &s));
    assert(settings_equal(&s, &before));
}

static void test_avatar_values(void) {
    terminal_settings_t s;
    settings_defaults(&s);
    const uint8_t preset[] = {0x01, 0x02, 0x01, SETTINGS_AVATAR_PRESET_MAX};
    const uint8_t custom[] = {0x01, 0x02, 0x01, SETTINGS_AVATAR_CUSTOM};
    assert(settings_tlv_decode(preset, sizeof(preset), &s) && s.avatar == SETTINGS_AVATAR_PRESET_MAX);
    assert(settings_tlv_decode(custom, sizeof(custom), &s) && s.avatar == SETTINGS_AVATAR_CUSTOM);
}

static void test_generation_bumps(void) {
    uint32_t g = settings_generation();
    terminal_settings_t s = *settings_get();
    s.accent_rgb = 0xFF0000;
    settings_set(&s);
    assert(settings_generation() == g + 1);
    assert(settings_get()->accent_rgb == 0xFF0000);
}

int main(void) {
    test_defaults_golden();
    test_round_trip();
    test_partial_write_keeps_other_fields();
    test_unknown_tag_skipped();
    test_rejects_leave_settings_untouched();
    test_avatar_values();
    test_generation_bumps();
    printf("settings_test: all passed\n");
    return 0;
}
