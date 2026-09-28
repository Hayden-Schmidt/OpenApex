#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../main/mesh_core.h"
#include "../main/mesh_crypto.h"
#include "../main/mesh_proto.h"

// gcc -std=c11 -Ifirmware/main -o mesh_test firmware/test_host/mesh_test.c firmware/main/mesh_core.c
//     firmware/main/mesh_proto.c firmware/main/mesh_crypto.c -lm

static void test_aes_fips197(void) {
    uint8_t k[16], p[16], o[16];
    for (int i = 0; i < 16; i++) {
        k[i] = (uint8_t)i;
        p[i] = (uint8_t)(i * 0x11);
    }
    static const uint8_t want[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                     0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    mesh_aes128_encrypt(k, p, o);
    assert(memcmp(o, want, 16) == 0);
}

// RFC 3610 packet vector #1.
static void test_ccm_rfc3610(void) {
    uint8_t k[16], a[8], p[23], c[23], t[8], back[23];
    for (int i = 0; i < 16; i++) k[i] = (uint8_t)(0xC0 + i);
    for (int i = 0; i < 8; i++) a[i] = (uint8_t)i;
    for (int i = 0; i < 23; i++) p[i] = (uint8_t)(8 + i);
    static const uint8_t n[13] = {0, 0, 0, 3, 2, 1, 0, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    static const uint8_t want_c[23] = {0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2, 0xF0, 0x66, 0xD0, 0xC2,
                                       0xC0, 0xF9, 0x89, 0x80, 0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3, 0x84};
    static const uint8_t want_t[8] = {0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0};
    mesh_ccm_encrypt(k, n, a, 8, p, 23, c, t);
    assert(memcmp(c, want_c, 23) == 0 && memcmp(t, want_t, 8) == 0);
    assert(mesh_ccm_decrypt(k, n, a, 8, c, 23, back, t) && memcmp(back, p, 23) == 0);
    c[0] ^= 1;
    assert(!mesh_ccm_decrypt(k, n, a, 8, c, 23, back, t));
}

static mesh_look_t look_of(uint8_t avatar, uint32_t rgb, bool custom) {
    mesh_look_t l;
    memset(&l, 0, sizeof(l));
    l.avatar = avatar;
    l.accent_rgb = rgb;
    l.has_custom = custom;
    for (int i = 0; custom && i < (int)MESH_ART_BYTES; i++) l.art[i] = (uint8_t)i;
    return l;
}

static void test_hello_round_trip(void) {
    mesh_header_t h = {MESH_TYPE_HELLO, {1, 2, 3, 4, 5, 6, 7, 8}, 0x1234};
    for (int custom = 0; custom < 2; custom++) {
        mesh_look_t l = look_of(0xFF, 0x00FFE8, custom);
        strcpy(l.name, "Hayden");
        uint8_t buf[MESH_FRAME_MAX];
        const size_t n = mesh_encode_hello(&h, &l, 0, buf, sizeof(buf));
        assert(n == MESH_HEADER_LEN + 7 + 6 + (custom ? MESH_ART_BYTES : 0));
        mesh_header_t h2;
        mesh_look_t l2;
        uint8_t caps;
        assert(mesh_decode_header(buf, n, &h2) && h2.type == h.type && h2.seq == h.seq);
        assert(memcmp(h2.sender, h.sender, 8) == 0);
        assert(mesh_decode_hello(buf, n, &l2, &caps) && caps == 0);
        assert(l2.avatar == l.avatar && l2.accent_rgb == l.accent_rgb && strcmp(l2.name, "Hayden") == 0);
        assert(l2.has_custom == l.has_custom && (!custom || memcmp(l2.art, l.art, MESH_ART_BYTES) == 0));
        assert(!mesh_decode_hello(buf, n - 1, &l2, &caps)); // truncated
    }
    uint8_t bad[MESH_HEADER_LEN] = {'O', 'B', 1};
    mesh_header_t h3;
    assert(!mesh_decode_header(bad, sizeof(bad), &h3));
}

static void test_pos_auth(void) {
    uint8_t key[16] = {9}, other[16] = {8};
    mesh_header_t h = {MESH_TYPE_POS, {1, 1, 1, 1, 1, 1, 1, 1}, 7};
    mesh_pos_t p = {-338688000, 1512093000, 523, 1800, 5}, q;
    uint8_t buf[MESH_FRAME_MAX];
    const size_t n = mesh_encode_pos(&h, key, 0xABCDEF, &p, buf, sizeof(buf));
    assert(n > 0);
    assert(mesh_decode_pos(buf, n, key, &q) && q.lat_e7 == p.lat_e7 && q.lon_e7 == p.lon_e7);
    assert(q.speed_kmh_x10 == 523 && q.heading_deg_x10 == 1800 && q.acc_m == 5);
    assert(!mesh_decode_pos(buf, n, other, &q)); // stranger
    buf[12] ^= 1;                                // tampered seq (AAD)
    assert(!mesh_decode_pos(buf, n, key, &q));
}

static void test_identity_and_record(void) {
    mesh_identity_t id = {{1, 2, 3, 4, 5, 6, 7, 8}, {0xAA}, "Sam"};
    uint8_t buf[MESH_IDENTITY_MAX_LEN];
    const size_t n = mesh_identity_encode(&id, buf, sizeof(buf));
    assert(n == 29);
    mesh_identity_t back;
    assert(mesh_identity_decode(buf, n, &back) && strcmp(back.name, "Sam") == 0);
    assert(!mesh_identity_decode(buf, n - 1, &back));
    buf[0] = 2;
    assert(!mesh_identity_decode(buf, n, &back));

    mesh_friend_t f;
    memset(&f, 0, sizeof(f));
    f.id[0] = 5;
    f.key[15] = 6;
    f.look = look_of(3, 0x123456, true);
    strcpy(f.look.name, "Twelve_chars");
    uint8_t rec[MESH_FRIEND_RECORD_LEN];
    mesh_friend_encode(&f, rec);
    mesh_friend_t g;
    assert(mesh_friend_decode(rec, &g));
    assert(memcmp(&g.id, &f.id, 8) == 0 && g.key[15] == 6 && strcmp(g.look.name, "Twelve_chars") == 0);
    assert(g.look.has_custom && memcmp(g.look.art, f.look.art, MESH_ART_BYTES) == 0);
    rec[29] = 13; // name too long
    assert(!mesh_friend_decode(rec, &g));
}

static void test_geo(void) {
    float b, d;
    // 0.001 deg of latitude north ~ 111.2 m.
    mesh_geo(0, 0, 10000, 0, &b, &d);
    assert(fabsf(b - 0.0f) < 0.01f && fabsf(d - 111.19f) < 0.5f);
    // Due east at 60 deg latitude: half the equatorial distance.
    mesh_geo(600000000, 0, 600000000, 20000, &b, &d);
    assert(fabsf(b - 90.0f) < 0.1f && fabsf(d - 111.19f) < 0.5f);
    mesh_geo(0, 0, -10000, -10000, &b, &d);
    assert(fabsf(b - 225.0f) < 0.1f);
    // Across the antimeridian is near, not the long way round.
    mesh_geo(0, 1799990000, 0, -1799990000, &b, &d);
    assert(d < 300.0f && fabsf(b - 90.0f) < 0.1f);
}

// -- two terminals on one air ---------------------------------------------------------------------

typedef struct {
    uint8_t frames[32][MESH_FRAME_MAX];
    size_t lens[32];
    size_t count;
} air_t;

static void air_send(const uint8_t *f, size_t len, void *ctx) {
    air_t *a = ctx;
    assert(a->count < 32);
    memcpy(a->frames[a->count], f, len);
    a->lens[a->count++] = len;
}

// Delivers everything `from` sent to `to` (and to no one if `to` is NULL).
static void deliver(air_t *from, mesh_core_t *to, air_t *to_air, uint32_t now) {
    air_t pending = *from;
    from->count = 0;
    for (size_t i = 0; to && i < pending.count; i++) mesh_core_rx(to, pending.frames[i], pending.lens[i], now, air_send, to_air);
}

static void setup(mesh_core_t *c, uint8_t idbyte, const char *name, uint32_t rgb) {
    mesh_core_init(c, idbyte * 1000u);
    mesh_identity_t id;
    memset(&id, 0, sizeof(id));
    memset(id.id, idbyte, 8);
    memset(id.key, idbyte + 0x40, 16);
    strcpy(id.name, name);
    mesh_core_set_identity(c, &id);
    mesh_look_t l = look_of(idbyte, rgb, false);
    mesh_core_set_look(c, &l);
}

static void exchange(mesh_core_t *a, air_t *aa, mesh_core_t *b, air_t *ba, uint32_t now) {
    deliver(aa, b, ba, now);
    deliver(ba, a, aa, now);
    deliver(aa, b, ba, now);
    deliver(ba, a, aa, now);
}

static void test_friending_and_position(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 0xFF0000);
    setup(&b, 2, "Ben", 0x0000FF);
    uint32_t now = 1000;
    mesh_core_tick(&a, now, true, air_send, &aa);
    mesh_core_tick(&b, now, true, air_send, &ba);
    exchange(&a, &aa, &b, &ba, now);
    now += 100;
    mesh_core_tick(&a, now, true, air_send, &aa);
    mesh_core_tick(&b, now, true, air_send, &ba);

    uint8_t pid[8];
    mesh_look_t pl;
    assert(mesh_core_prompt(&a, pid, &pl) == MESH_PROMPT_OFFER && pid[0] == 2 && strcmp(pl.name, "Ben") == 0);
    assert(pl.accent_rgb == 0x0000FF);
    assert(mesh_core_prompt(&b, pid, &pl) == MESH_PROMPT_OFFER && pid[0] == 1);

    // Ann holds first: waiting, not yet friends.
    uint8_t ben[8], ann[8];
    memset(ben, 2, 8);
    memset(ann, 1, 8);
    mesh_core_accept(&a, ben, now, air_send, &aa);
    exchange(&a, &aa, &b, &ba, now);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_WAITING);
    assert(mesh_core_friend_find(&b, ann) == NULL);

    // Ben holds: both sides complete.
    mesh_core_accept(&b, ann, now, air_send, &ba);
    exchange(&a, &aa, &b, &ba, now);
    assert(mesh_core_friend_find(&a, ben) && mesh_core_friend_find(&b, ann));
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_DONE);
    assert(mesh_core_prompt(&b, NULL, NULL) == MESH_PROMPT_DONE);
    mesh_friend_event_t ev;
    assert(mesh_core_take_friend_event(&a, &ev) && ev.kind == MESH_FRIEND_EVENT_ADDED && ev.record.id[0] == 2);
    assert(memcmp(ev.record.key, b.self.key, 16) == 0);
    assert(!mesh_core_take_friend_event(&a, &ev));

    now += MESH_PROMPT_DONE_MS + 10;
    mesh_core_tick(&a, now, true, air_send, &aa);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE);

    // Positions: Ben 111 m due north of Ann.
    mesh_pos_t pa = {0, 0, 0, 0, 3}, pb = {10000, 0, 0, 0, 3};
    mesh_core_set_own_pos(&a, &pa, now);
    mesh_core_set_own_pos(&b, &pb, now);
    aa.count = ba.count = 0;
    mesh_core_tick(&a, now, true, air_send, &aa);
    mesh_core_tick(&b, now, true, air_send, &ba);
    exchange(&a, &aa, &b, &ba, now);
    mesh_friend_fix_t fix[4];
    assert(mesh_core_friends_in_range(&a, now, fix, 4) == 1);
    assert(fix[0].f->id[0] == 2 && fabsf(fix[0].bearing_deg) < 0.1f && fabsf(fix[0].dist_m - 111.2f) < 1.0f);
    assert(mesh_core_friends_in_range(&b, now, fix, 4) == 1 && fabsf(fix[0].bearing_deg - 180.0f) < 0.1f);

    // Out of range: gone after the timeout.
    now += MESH_PEER_TIMEOUT_MS + 1;
    mesh_core_set_own_pos(&a, &pa, now);
    mesh_core_tick(&a, now, true, air_send, &aa);
    assert(mesh_core_friends_in_range(&a, now, fix, 4) == 0);
}

static void test_stranger_cannot_read_position(void) {
    static mesh_core_t a, c;
    air_t aa = {0}, ca = {0};
    setup(&a, 1, "Ann", 1);
    setup(&c, 3, "Cat", 3);
    // Ann has a friend (not Cat) near, so she shares.
    mesh_friend_t f;
    memset(&f, 0, sizeof(f));
    memset(f.id, 9, 8);
    mesh_core_friend_upsert(&a, &f);
    mesh_core_rx(&a, NULL, 0, 0, air_send, &aa); // no-op on empty
    uint8_t hello[MESH_FRAME_MAX];
    mesh_header_t h = {MESH_TYPE_HELLO, {9, 9, 9, 9, 9, 9, 9, 9}, 1};
    mesh_look_t l = look_of(1, 1, false);
    const size_t n = mesh_encode_hello(&h, &l, 0, hello, sizeof(hello));
    mesh_core_rx(&a, hello, n, 1000, air_send, &aa);
    mesh_pos_t p = {1, 2, 0, 0, 1};
    mesh_core_set_own_pos(&a, &p, 1000);
    mesh_core_set_own_pos(&c, &p, 1000);
    mesh_core_tick(&a, 1000, true, air_send, &aa);
    deliver(&aa, &c, &ca, 1000);
    mesh_friend_fix_t fix[2];
    assert(mesh_core_friends_in_range(&c, 1000, fix, 2) == 0);
}

static void test_prompt_gate_and_timeout(void) {
    assert(mesh_core_prompt_gate(false, 0, 1000));
    assert(mesh_core_prompt_gate(true, 51, 1000));
    assert(mesh_core_prompt_gate(true, 10, 199));
    assert(!mesh_core_prompt_gate(true, 50, 200));
    assert(!mesh_core_prompt_gate(true, 10, 0xFFFF)); // unknown speed near a turn: hold back

    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    uint32_t now = 1000;
    mesh_core_tick(&b, now, true, air_send, &ba);
    deliver(&ba, &a, &aa, now);
    mesh_core_tick(&a, now, false, air_send, &aa);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE); // gated: not offered yet
    mesh_core_tick(&a, now + 100, true, air_send, &aa);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_OFFER);

    // Hidden time does not count toward the timeout.
    for (now = 1100; now < 1100 + 2 * MESH_PROMPT_TIMEOUT_MS; now += 500) {
        mesh_core_tick(&b, now, true, air_send, &ba);
        deliver(&ba, &a, &aa, now);
        mesh_core_tick(&a, now, false, air_send, &aa);
        aa.count = 0;
    }
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE); // hidden, still pending
    assert(a.prompt == MESH_PROMPT_OFFER);
    for (uint32_t t = 0; t <= MESH_PROMPT_TIMEOUT_MS; t += 500, now += 500) {
        mesh_core_tick(&b, now, true, air_send, &ba);
        deliver(&ba, &a, &aa, now);
        mesh_core_tick(&a, now, true, air_send, &aa);
        aa.count = 0;
    }
    assert(a.prompt == MESH_PROMPT_NONE);
    // Suppressed for the session.
    mesh_core_tick(&a, now + 500, true, air_send, &aa);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE);
}

static void test_refriend_after_lost_list(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    // Ann still has Ben; Ben lost his list.
    mesh_friend_t f;
    memset(&f, 0, sizeof(f));
    memset(f.id, 2, 8);
    memcpy(f.key, b.self.key, 16);
    mesh_core_friend_upsert(&a, &f);
    uint32_t now = 1000;
    mesh_core_tick(&a, now, true, air_send, &aa);
    mesh_core_tick(&b, now, true, air_send, &ba);
    exchange(&a, &aa, &b, &ba, now);
    mesh_core_tick(&a, now + 10, true, air_send, &aa);
    mesh_core_tick(&b, now + 10, true, air_send, &ba);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE);
    assert(mesh_core_prompt(&b, NULL, NULL) == MESH_PROMPT_OFFER);
    uint8_t ann[8];
    memset(ann, 1, 8);
    mesh_core_accept(&b, ann, now + 10, air_send, &ba);
    for (int i = 0; i < 4; i++) exchange(&a, &aa, &b, &ba, now + 10);
    assert(mesh_core_friend_find(&b, ann) != NULL);
    assert(aa.count == 0 && ba.count == 0); // no ping-pong
}

int main(void) {
    test_aes_fips197();
    test_ccm_rfc3610();
    test_hello_round_trip();
    test_pos_auth();
    test_identity_and_record();
    test_geo();
    test_friending_and_position();
    test_stranger_cannot_read_position();
    test_prompt_gate_and_timeout();
    test_refriend_after_lost_list();
    printf("mesh_test: all passed\n");
    return 0;
}
