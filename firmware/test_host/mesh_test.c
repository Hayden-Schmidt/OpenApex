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
    (void)custom; // no custom art on the mesh in the POC (blueprint D1)
    mesh_look_t l;
    memset(&l, 0, sizeof(l));
    l.avatar = avatar;
    l.accent_rgb = rgb;
    return l;
}

// -- pseudonyms ----------------------------------------------------------------------------------

static void test_pseudonym(void) {
    uint8_t key[16], other[16], tag[16], tag2[16], ps[8], want[16];
    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)(i + 1);
        other[i] = (uint8_t)(i + 2);
    }
    mesh_tag_key(key, tag);
    mesh_tag_key(other, tag2);
    // tag_key = AES(rider_key, "OA-TAGKEY" | zero pad), and is not the rider key itself.
    uint8_t label[16] = {'O', 'A', '-', 'T', 'A', 'G', 'K', 'E', 'Y'};
    mesh_aes128_encrypt(key, label, want);
    assert(memcmp(tag, want, 16) == 0 && memcmp(tag, key, 16) != 0);

    const uint8_t prand[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    mesh_pseudonym_make(tag, prand, ps);
    assert(memcmp(ps, prand, 4) == 0);
    assert(mesh_pseudonym_match(tag, ps));
    assert(!mesh_pseudonym_match(tag2, ps)); // someone else's key does not resolve it
    ps[7] ^= 1;
    assert(!mesh_pseudonym_match(tag, ps)); // tampered hash
    ps[7] ^= 1;
    const uint8_t prand2[4] = {1, 2, 3, 4};
    uint8_t ps2[8];
    mesh_pseudonym_make(tag, prand2, ps2);
    assert(memcmp(ps, ps2, 8) != 0 && mesh_pseudonym_match(tag, ps2));
}

// -- frames --------------------------------------------------------------------------------------

static void test_hello_round_trip(void) {
    mesh_header_t h = {MESH_TYPE_HELLO, {1, 2, 3, 4, 5, 6, 7, 8}, 0x1234};
    {
        mesh_look_t l = look_of(0xFF, 0x00FFE8, false);
        strcpy(l.name, "Hayden");
        uint8_t buf[MESH_FRAME_MAX];
        const size_t n = mesh_encode_hello(&h, &l, 0, buf, sizeof(buf));
        assert(n == MESH_HEADER_LEN + 2 + 6 + 6);
        assert(buf[n - 1] == 0); // has_custom is always 0
        mesh_header_t h2;
        mesh_look_t l2;
        bool has;
        uint8_t caps;
        assert(mesh_decode_header(buf, n, &h2) && h2.type == h.type && h2.seq == h.seq);
        assert(memcmp(h2.sender, h.sender, 8) == 0);
        assert(mesh_decode_hello(buf, n, &l2, &has, &caps) && has && caps == 0);
        assert(l2.avatar == l.avatar && l2.accent_rgb == l.accent_rgb && strcmp(l2.name, "Hayden") == 0);
        assert(!mesh_decode_hello(buf, n - 1, &l2, &has, &caps)); // truncated
        // A later sender that includes art still decodes: the art is skipped.
        buf[n - 1] = 1;
        memset(&buf[n], 0x5A, MESH_ART_BYTES);
        assert(mesh_decode_hello(buf, n + MESH_ART_BYTES, &l2, &has, &caps) && has && strcmp(l2.name, "Hayden") == 0);
        assert(!mesh_decode_hello(buf, n + MESH_ART_BYTES - 1, &l2, &has, &caps));
    }
    // Not discoverable: header, caps and flags only.
    uint8_t buf[MESH_FRAME_MAX];
    mesh_look_t l2;
    bool has = true;
    uint8_t caps;
    const size_t n = mesh_encode_hello(&h, NULL, 0, buf, sizeof(buf));
    assert(n == MESH_HEADER_LEN + 2);
    assert(mesh_decode_hello(buf, n, &l2, &has, &caps) && !has);
    assert(!mesh_decode_hello(buf, n + 0, &l2, &has, &caps) == 0);
    buf[MESH_HEADER_LEN + 1] = 0x80; // unknown flag
    assert(!mesh_decode_hello(buf, n, &l2, &has, &caps));

    uint8_t bad[MESH_HEADER_LEN] = {'O', 'B', MESH_PROTO_VERSION};
    mesh_header_t h3;
    assert(!mesh_decode_header(bad, sizeof(bad), &h3));
    uint8_t v1[MESH_HEADER_LEN] = {'O', 'A', 1}; // v1 frames are dropped by version
    assert(!mesh_decode_header(v1, sizeof(v1), &h3));
}

static void test_target_and_keys_frames(void) {
    mesh_header_t h = {MESH_TYPE_FRIEND_REQUEST, {9, 9, 9, 9, 9, 9, 9, 9}, 3};
    const uint8_t target[8] = {1, 2, 3, 4, 5, 6, 7, 8}, id[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    uint8_t key[16], buf[MESH_FRAME_MAX], t2[8], id2[8], key2[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0x20 + i);
    size_t n = mesh_encode_target(&h, target, buf, sizeof(buf));
    assert(n == MESH_HEADER_LEN + 8 && mesh_decode_target(buf, n, t2) && memcmp(t2, target, 8) == 0);
    assert(!mesh_decode_target(buf, n - 1, t2));
    n = mesh_encode_keys(&h, target, id, key, buf, sizeof(buf));
    assert(n == MESH_HEADER_LEN + 32 && mesh_decode_keys(buf, n, t2, id2, key2));
    assert(memcmp(t2, target, 8) == 0 && memcmp(id2, id, 8) == 0 && memcmp(key2, key, 16) == 0);
    assert(!mesh_decode_keys(buf, n - 1, t2, id2, key2));
    assert(mesh_encode_keys(&h, target, id, key, buf, n - 1) == 0);
}

static void test_pos_auth(void) {
    uint8_t key[16] = {9}, other[16] = {8};
    mesh_header_t h = {MESH_TYPE_POS, {1, 1, 1, 1, 1, 1, 1, 1}, 7};
    mesh_pos_t p = {-338688000, 1512093000, 523, 1800, 5}, q;
    uint8_t buf[MESH_FRAME_MAX];
    uint16_t age;
    uint8_t ttl;
    const size_t n = mesh_encode_pos(&h, key, 0xABCDEF, &p, 1234, 0, buf, sizeof(buf));
    assert(n == MESH_HEADER_LEN + 4 + 3 + 13 + 8);
    assert(mesh_decode_pos(buf, n, key, &q, &age, &ttl) && q.lat_e7 == p.lat_e7 && q.lon_e7 == p.lon_e7);
    assert(q.speed_kmh_x10 == 523 && q.heading_deg_x10 == 1800 && q.acc_m == 5 && age == 1234 && ttl == 0);
    assert(!mesh_decode_pos(buf, n, other, &q, &age, &ttl)); // stranger
    // age_ms and ttl sit outside the AAD so a relay can edit them without the key.
    buf[MESH_HEADER_LEN + 4] ^= 0x10;
    buf[MESH_HEADER_LEN + 6] = 2;
    assert(mesh_decode_pos(buf, n, key, &q, &age, &ttl) && age != 1234 && ttl == 2);
    buf[12] ^= 1; // tampered seq (AAD)
    assert(!mesh_decode_pos(buf, n, key, &q, &age, &ttl));
}

static void test_look_frame(void) {
    uint8_t key[16] = {3}, other[16] = {4}, buf[MESH_FRAME_MAX];
    mesh_header_t h = {MESH_TYPE_LOOK, {2, 2, 2, 2, 2, 2, 2, 2}, 11};
    {
        mesh_look_t l = look_of(5, 0xABCDEF, false), r;
        strcpy(l.name, "Ann");
        const size_t n = mesh_encode_look(&h, key, 77, &l, buf, sizeof(buf));
        assert(n == MESH_HEADER_LEN + 4 + 6 + 3 + 8);
        assert(mesh_decode_look(buf, n, key, &r) && r.avatar == 5 && r.accent_rgb == 0xABCDEF);
        assert(strcmp(r.name, "Ann") == 0);
        assert(!mesh_decode_look(buf, n, other, &r)); // only friends can read it
        assert(!mesh_decode_look(buf, n - 1, key, &r));
        buf[MESH_HEADER_LEN + 4] ^= 1; // ciphertext bit
        assert(!mesh_decode_look(buf, n, key, &r));
    }
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
    f.look = look_of(3, 0x123456, false);
    strcpy(f.look.name, "Twelve_chars");
    uint8_t rec[MESH_FRIEND_RECORD_LEN];
    mesh_friend_encode(&f, rec);
    assert(rec[28] == 0 && rec[42] == 0 && rec[121] == 0); // flags and art reserved, zero
    mesh_friend_t g;
    assert(mesh_friend_decode(rec, &g));
    assert(memcmp(&g.id, &f.id, 8) == 0 && g.key[15] == 6 && strcmp(g.look.name, "Twelve_chars") == 0);
    rec[28] = MESH_FRIEND_FLAG_CUSTOM; // a phone record with art: accepted, art ignored
    memset(&rec[42], 0x77, MESH_ART_BYTES);
    assert(mesh_friend_decode(rec, &g) && g.look.avatar == 3);
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
    float e, n;
    mesh_offset(0, 0, 10000, 10000, &e, &n);
    assert(fabsf(e - 111.19f) < 0.5f && fabsf(n - 111.19f) < 0.5f);
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
    static air_t pending;
    pending = *from;
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

static void tick_both(mesh_core_t *a, air_t *aa, mesh_core_t *b, air_t *ba, uint32_t now) {
    mesh_core_tick(a, now, true, air_send, aa);
    mesh_core_tick(b, now, true, air_send, ba);
    exchange(a, aa, b, ba, now);
}

// Ticks both terminals every 100 ms for `ms`, delivering everything, and advances `*now`. Replies
// are deferred to the tick (MESH_REPLY_DELAY_MS), so a handshake needs time, not just exchanges.
static void settle(mesh_core_t *a, air_t *aa, mesh_core_t *b, air_t *ba, uint32_t *now, uint32_t ms) {
    for (uint32_t end = *now + ms; *now < end;) {
        *now += 100;
        tick_both(a, aa, b, ba, *now);
    }
}

static bool handshake_idle(const mesh_core_t *c) {
    for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
        if (c->peers[i].used && (c->peers[i].hs != MESH_HS_NONE || c->peers[i].ack_due)) return false;
    }
    return true;
}

// Ann requests Ben from the menu and Ben consents; both end up friends. Advances `*now_io`.
static void befriend_by_menu(mesh_core_t *a, air_t *aa, mesh_core_t *b, air_t *ba, uint32_t *now_io) {
    const uint32_t now = *now_io;
    mesh_core_set_menu_open(a, true, now);
    mesh_core_set_menu_open(b, true, now);
    tick_both(a, aa, b, ba, now);
    assert(mesh_core_request(a, b->pseud, now, air_send, aa));
    exchange(a, aa, b, ba, now);
    uint8_t pid[8];
    mesh_look_t pl;
    assert(mesh_core_prompt(b, pid, &pl) == MESH_PROMPT_OFFER && memcmp(pid, a->pseud, 8) == 0);
    assert(strcmp(pl.name, a->self.name) == 0);
    assert(mesh_core_prompt(a, pid, &pl) == MESH_PROMPT_WAITING && memcmp(pid, b->pseud, 8) == 0);
    assert(mesh_core_friend_find(b, a->self.id) == NULL); // not yet
    mesh_core_accept(b, a->pseud, now, air_send, ba);
    exchange(a, aa, b, ba, now);
    settle(a, aa, b, ba, now_io, 1500);
}

static void test_no_automatic_prompt(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    // Neither menu is open: they hear only pseudonyms, no look, and nothing prompts.
    for (uint32_t now = 1000; now < 30000; now += 500) tick_both(&a, &aa, &b, &ba, now);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE && mesh_core_prompt(&b, NULL, NULL) == MESH_PROMPT_NONE);
    const mesh_peer_t *np[4];
    assert(mesh_core_nearby(&a, 30000, np, 4) == 0); // no public look to list
    // The wire carries no name: the HELLO is header + caps + flags.
    mesh_core_tick(&a, 40000, true, air_send, &aa);
    assert(aa.count == 1 && aa.lens[0] == MESH_HEADER_LEN + 2);
    aa.count = 0;
    // A pseudonym is not the permanent id, and it is stable across a session.
    assert(memcmp(a.pseud, a.self.id, 8) != 0 && memcmp(a.pseud, b.pseud, 8) != 0);
}

static void test_menu_discovery_and_grace(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 0x0000FF);
    mesh_core_set_menu_open(&b, true, 1000);
    tick_both(&a, &aa, &b, &ba, 1000);
    const mesh_peer_t *np[4];
    assert(mesh_core_nearby(&a, 1000, np, 4) == 1 && strcmp(np[0]->look.name, "Ben") == 0);
    assert(np[0]->look.accent_rgb == 0x0000FF && memcmp(np[0]->id, b.pseud, 8) == 0);
    // Menu closes: still public for the grace period, then private again.
    mesh_core_set_menu_open(&b, false, 3000);
    tick_both(&a, &aa, &b, &ba, 5000);
    assert(mesh_core_nearby(&a, 5000, np, 4) == 1);
    uint32_t now = 3000 + MESH_DISCOVERABLE_GRACE_MS + 10;
    tick_both(&a, &aa, &b, &ba, now - 2000); // keep the peer alive
    tick_both(&a, &aa, &b, &ba, now);
    assert(mesh_core_nearby(&a, now, np, 4) == 0);
    // Always-discoverable overrides the menu.
    mesh_core_set_always_discoverable(&b, true);
    tick_both(&a, &aa, &b, &ba, now + 2000);
    assert(mesh_core_nearby(&a, now + 2000, np, 4) == 1);
}

static void test_friending_from_menu(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 0xFF0000);
    setup(&b, 2, "Ben", 0x0000FF);
    uint32_t now = 1000;
    befriend_by_menu(&a, &aa, &b, &ba, &now);
    assert(mesh_core_friend_find(&a, b.self.id) && mesh_core_friend_find(&b, a.self.id));
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_DONE);
    assert(mesh_core_prompt(&b, NULL, NULL) == MESH_PROMPT_DONE);
    mesh_friend_event_t ev;
    assert(mesh_core_take_friend_event(&a, &ev) && ev.kind == MESH_FRIEND_EVENT_ADDED);
    assert(memcmp(ev.record.id, b.self.id, 8) == 0 && memcmp(ev.record.key, b.self.key, 16) == 0);
    assert(strcmp(ev.record.look.name, "Ben") == 0); // the public look seen in the menu
    assert(!mesh_core_take_friend_event(&a, &ev));
    // The handshake settles: no retries left running, no stranded state.
    assert(handshake_idle(&a) && handshake_idle(&b));

    now += MESH_PROMPT_DONE_MS + 10;
    mesh_core_tick(&a, now, true, air_send, &aa);
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE);

    // They no longer list each other as strangers.
    const mesh_peer_t *np[4];
    assert(mesh_core_nearby(&a, now, np, 4) == 0);

    // Friends resolve each other's pseudonym on air: POS decrypts.
    mesh_pos_t pa = {0, 0, 0, 0, 3}, pb = {10000, 0, 0, 0, 3};
    for (int i = 0; i < 3; i++, now += 1000) {
        mesh_core_set_own_pos(&a, &pa, now);
        mesh_core_set_own_pos(&b, &pb, now);
        tick_both(&a, &aa, &b, &ba, now);
    }
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

static void test_request_lapses_and_retries(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    mesh_core_set_menu_open(&a, true, 1000);
    mesh_core_set_menu_open(&b, true, 1000);
    tick_both(&a, &aa, &b, &ba, 1000);
    assert(mesh_core_request(&a, b.pseud, 1000, air_send, &aa));
    assert(!mesh_core_request(&a, b.pseud, 1000, air_send, &aa)); // slot is busy
    // The first REQUEST is lost on air; the retry gets through.
    aa.count = 0;
    mesh_core_tick(&a, 1000 + MESH_REQUEST_RETRY_MS + MESH_RETRY_JITTER_MS, true, air_send, &aa); // retry is jittered
    assert(aa.count >= 1);
    deliver(&aa, &b, &ba, 3000);
    assert(mesh_core_prompt(&b, NULL, NULL) == MESH_PROMPT_OFFER);

    // Ben never answers: both sides lapse silently, nobody is a friend, and no auto re-offer.
    uint32_t now = 3000;
    for (; now < 3000 + 2 * MESH_PROMPT_TIMEOUT_MS; now += 500) {
        tick_both(&a, &aa, &b, &ba, now);
        aa.count = ba.count = 0;
    }
    assert(mesh_core_prompt(&a, NULL, NULL) == MESH_PROMPT_NONE);
    assert(mesh_core_prompt(&b, NULL, NULL) == MESH_PROMPT_NONE);
    assert(a.friend_count == 0 && b.friend_count == 0);
    for (size_t i = 0; i < MESH_MAX_PEERS; i++) assert(!a.peers[i].used || a.peers[i].hs == MESH_HS_NONE);
    // Ann can ask again.
    assert(mesh_core_request(&a, b.pseud, now, air_send, &aa));
}

static void test_mutual_request(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    mesh_core_set_menu_open(&a, true, 1000);
    mesh_core_set_menu_open(&b, true, 1000);
    tick_both(&a, &aa, &b, &ba, 1000);
    // Both pick each other at the same moment: each request is the other's consent.
    assert(mesh_core_request(&a, b.pseud, 1000, air_send, &aa));
    assert(mesh_core_request(&b, a.pseud, 1000, air_send, &ba));
    uint32_t now = 1000;
    exchange(&a, &aa, &b, &ba, now);
    settle(&a, &aa, &b, &ba, &now, 3000);
    assert(mesh_core_friend_find(&a, b.self.id) && mesh_core_friend_find(&b, a.self.id));
    assert(a.friend_count == 1 && b.friend_count == 1);
    assert(handshake_idle(&a) && handshake_idle(&b));
}

static void test_request_ignored_when_busy_or_unknown(void) {
    static mesh_core_t a, b, c;
    air_t aa = {0}, ba = {0}, ca = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    setup(&c, 3, "Cat", 3);
    // A request from a rider we never heard a HELLO from is dropped.
    mesh_core_set_menu_open(&b, true, 1000);
    assert(!mesh_core_request(&b, a.pseud, 1000, air_send, &ba)); // unknown peer
    // One slot: a second incoming request waits for the first to resolve.
    mesh_core_set_menu_open(&a, true, 1000);
    mesh_core_set_menu_open(&c, true, 1000);
    mesh_core_tick(&a, 1000, true, air_send, &aa);
    mesh_core_tick(&b, 1000, true, air_send, &ba);
    mesh_core_tick(&c, 1000, true, air_send, &ca);
    static air_t b_hello;
    b_hello = ba;
    deliver(&aa, &b, &ba, 1000);
    deliver(&ca, &b, &ba, 1000);
    deliver(&ba, &a, &aa, 1000);
    ba = b_hello;
    deliver(&ba, &c, &ca, 1000);
    aa.count = ba.count = ca.count = 0;
    assert(mesh_core_request(&a, b.pseud, 1000, air_send, &aa));
    assert(mesh_core_request(&c, b.pseud, 1000, air_send, &ca));
    deliver(&aa, &b, &ba, 1000);
    deliver(&ca, &b, &ba, 1000);
    uint8_t pid[8];
    assert(mesh_core_prompt(&b, pid, NULL) == MESH_PROMPT_OFFER && memcmp(pid, a.pseud, 8) == 0);
}

// The C3 hears little while its own advert is on air (FRAME_WINDOW_MS, 300 ms). Model that: a
// terminal that sent anything in the last 300 ms receives nothing. A reply sent the instant its
// trigger arrived always landed in that window, and with both sides retrying on the same 1 s
// period every retry did too, which left one side a friend and the other not.
typedef struct {
    air_t air;
    uint32_t busy_until;
} radio_t;

static void radio_tick(mesh_core_t *c, radio_t *r, uint32_t now) {
    const size_t before = r->air.count;
    mesh_core_tick(c, now, true, air_send, &r->air);
    if (r->air.count != before) r->busy_until = now + 300;
}

static void radio_deliver(radio_t *from, mesh_core_t *to, radio_t *to_r, uint32_t now) {
    static air_t pending;
    pending = from->air;
    from->air.count = 0;
    if ((int32_t)(now - to_r->busy_until) < 0) return; // the receiver is advertising: all lost
    const size_t before = to_r->air.count;
    for (size_t i = 0; i < pending.count; i++) mesh_core_rx(to, pending.frames[i], pending.lens[i], now, air_send, &to_r->air);
    if (to_r->air.count != before) to_r->busy_until = now + 300;
}

static void test_handshake_survives_own_advert_window(void) {
    static mesh_core_t a, b;
    static radio_t ra, rb;
    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    mesh_core_set_menu_open(&a, true, 1000);
    mesh_core_set_menu_open(&b, true, 1000);
    uint32_t now = 1000;
    for (; now < 5000; now += 50) { // B powered on 500 ms after A, so their HELLOs interleave
        radio_tick(&a, &ra, now);
        if (now >= 1500) radio_tick(&b, &rb, now);
        radio_deliver(&ra, &b, &rb, now + 10);
        radio_deliver(&rb, &a, &ra, now + 20);
    }
    assert(mesh_core_request(&a, b.pseud, now, air_send, &ra.air));
    ra.busy_until = now + 300;
    bool accepted = false;
    for (uint32_t end = now + 20000; now < end; now += 50) {
        radio_tick(&a, &ra, now);
        radio_tick(&b, &rb, now);
        radio_deliver(&ra, &b, &rb, now + 10);
        radio_deliver(&rb, &a, &ra, now + 20);
        if (!accepted && b.prompt == MESH_PROMPT_OFFER) {
            mesh_core_accept(&b, a.pseud, now, air_send, &rb.air);
            rb.busy_until = now + 300;
            accepted = true;
        }
    }
    assert(accepted);
    assert(mesh_core_friend_find(&a, b.self.id) != NULL);
    assert(mesh_core_friend_find(&b, a.self.id) != NULL); // failed before replies were deferred
    assert(handshake_idle(&a) && handshake_idle(&b));
}

static void test_gate_holds_prompt_and_timeout(void) {
    assert(mesh_core_prompt_gate(false, 0, 1000));
    assert(mesh_core_prompt_gate(true, 51, 1000));
    assert(mesh_core_prompt_gate(true, 10, 199));
    assert(!mesh_core_prompt_gate(true, 50, 200));
    assert(!mesh_core_prompt_gate(true, 10, 0xFFFF)); // unknown speed near a turn: hold back

    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    mesh_core_set_menu_open(&a, true, 1000);
    mesh_core_set_menu_open(&b, true, 1000);
    tick_both(&a, &aa, &b, &ba, 1000);
    assert(mesh_core_request(&a, b.pseud, 1000, air_send, &aa));
    deliver(&aa, &b, &ba, 1000);
    // Ben's prompt is raised but the gate hides it, and hidden time does not count toward the lapse.
    uint32_t now = 1100;
    for (; now < 1100 + 2 * MESH_PROMPT_TIMEOUT_MS; now += 500) {
        mesh_core_tick(&a, now, true, air_send, &aa);
        mesh_core_tick(&b, now, false, air_send, &ba);
        deliver(&aa, &b, &ba, now);
        deliver(&ba, &a, &aa, now);
    }
    assert(mesh_core_prompt(&b, NULL, NULL) == MESH_PROMPT_NONE); // hidden
    assert(b.prompt == MESH_PROMPT_OFFER);                        // but still pending
    for (uint32_t t = 0; t <= MESH_PROMPT_TIMEOUT_MS; t += 500, now += 500) {
        mesh_core_tick(&a, now, true, air_send, &aa);
        mesh_core_tick(&b, now, true, air_send, &ba);
        deliver(&aa, &b, &ba, now);
        deliver(&ba, &a, &aa, now);
    }
    assert(b.prompt == MESH_PROMPT_NONE);
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
    mesh_core_set_menu_open(&b, true, now);
    tick_both(&a, &aa, &b, &ba, now);
    // Ben asks Ann from his menu. Ann already consented once: she answers without a prompt.
    assert(mesh_core_request(&b, a.pseud, now, air_send, &ba));
    exchange(&a, &aa, &b, &ba, now);
    settle(&a, &aa, &b, &ba, &now, 3000);
    assert(mesh_core_prompt(&a, NULL, NULL) != MESH_PROMPT_OFFER);
    assert(mesh_core_friend_find(&b, a.self.id) != NULL);
    assert(handshake_idle(&a) && handshake_idle(&b)); // no ping-pong
    assert(a.friend_count == 1 && b.friend_count == 1);
}

static void test_stranger_cannot_read_position(void) {
    static mesh_core_t a, b, c;
    air_t aa = {0}, ba = {0}, ca = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    setup(&c, 3, "Cat", 3);
    mesh_core_set_menu_open(&a, true, 1000);
    mesh_core_set_menu_open(&b, true, 1000);
    tick_both(&a, &aa, &b, &ba, 1000);
    uint32_t t0 = 1000;
    befriend_by_menu(&a, &aa, &b, &ba, &t0);
    mesh_pos_t p = {1, 2, 0, 0, 1};
    for (uint32_t now = t0 + 1000; now < t0 + 4000; now += 1000) {
        mesh_core_set_own_pos(&a, &p, now);
        mesh_core_set_own_pos(&c, &p, now);
        mesh_core_tick(&a, now, true, air_send, &aa);
        mesh_core_tick(&b, now, true, air_send, &ba);
        deliver(&ba, &a, &aa, now);
        deliver(&aa, &c, &ca, now); // Cat overhears everything Ann sends
        mesh_core_tick(&c, now, true, air_send, &ca);
    }
    mesh_friend_fix_t fix[2];
    assert(mesh_core_friends_in_range(&c, t0 + 4000, fix, 2) == 0);
    for (size_t i = 0; i < MESH_MAX_PEERS; i++) assert(!c.peers[i].used || c.peers[i].friend_idx == MESH_NO_FRIEND);
}

static void test_look_on_rediscovery_and_change(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 0xFF0000);
    setup(&b, 2, "Ben", 0x0000FF);
    uint32_t now = 1000;
    befriend_by_menu(&a, &aa, &b, &ba, &now);
    mesh_friend_event_t ev;
    while (mesh_core_take_friend_event(&a, &ev)) {}
    mesh_core_set_menu_open(&a, false, now);
    mesh_core_set_menu_open(&b, false, now);

    // Ben changes his look while in range: pushed to Ann, encrypted.
    mesh_look_t nl = look_of(9, 0x00FF00, true);
    now += 5000;
    tick_both(&a, &aa, &b, &ba, now);
    mesh_core_set_look(&b, &nl);
    for (int i = 0; i < 6; i++, now += MESH_LOOK_PUSH_MS) tick_both(&a, &aa, &b, &ba, now);
    const mesh_friend_t *fa = mesh_core_friend_find(&a, b.self.id);
    assert(fa && fa->look.avatar == 9 && fa->look.accent_rgb == 0x00FF00);
    assert(strcmp(fa->look.name, "Ben") == 0);
    assert(mesh_core_take_friend_event(&a, &ev) && ev.kind == MESH_FRIEND_EVENT_UPDATED);
    assert(a.look_push_left == 0 && b.look_push_left == 0); // bounded: no repeating traffic

    // Ben changes again while Ann is out of range; she gets it when she comes back.
    nl = look_of(4, 0x123456, false);
    now += 5000;
    mesh_core_set_look(&b, &nl);
    for (int i = 0; i < 6; i++, now += 3000) {
        mesh_core_tick(&b, now, true, air_send, &ba);
        ba.count = 0; // Ann hears nothing
    }
    mesh_core_tick(&a, now, true, air_send, &aa); // her table times Ben out
    aa.count = 0;
    assert(fa->look.avatar == 9);
    for (int i = 0; i < 4; i++, now += MESH_LOOK_PUSH_MS) tick_both(&a, &aa, &b, &ba, now);
    assert(mesh_core_friend_find(&a, b.self.id)->look.avatar == 4);
    assert(mesh_core_friend_find(&a, b.self.id)->look.accent_rgb == 0x123456);

    // A stranger's LOOK, or one that fails to decrypt, changes nothing.
    static mesh_core_t c;
    air_t ca = {0};
    setup(&c, 3, "Cat", 3);
    mesh_core_set_look(&c, &nl);
    mesh_core_tick(&c, now, true, air_send, &ca);
    deliver(&ca, &a, &aa, now);
    assert(a.friend_count == 1);
}

// -- relative positioning ------------------------------------------------------------------------

#define E7_PER_M 89.9322 // 1e7 degrees-units per metre at the equator, R = 6371 km

static void put_fix(mesh_core_t *c, double north_m, double east_m, uint32_t now) {
    mesh_pos_t p = {(int32_t)(north_m * E7_PER_M), (int32_t)(east_m * E7_PER_M), 0xFFFF, 0xFFFF, 3};
    mesh_core_set_own_pos(c, &p, now);
}

static void make_friends(mesh_core_t *a, air_t *aa, mesh_core_t *b, air_t *ba, uint32_t now) {
    mesh_friend_t f;
    memset(&f, 0, sizeof(f));
    memcpy(f.id, b->self.id, 8);
    memcpy(f.key, b->self.key, 16);
    mesh_core_friend_upsert(a, &f);
    memcpy(f.id, a->self.id, 8);
    memcpy(f.key, a->self.key, 16);
    mesh_core_friend_upsert(b, &f);
    tick_both(a, aa, b, ba, now);
}

// Two riders side by side at 100 km/h. Compared against our *latest* fix, a friend whose fix is one
// second old would read ~28 m behind; aligned in time they are exactly abeam.
static void test_bearing_aligned_in_time(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    make_friends(&a, &aa, &b, &ba, 500);
    const double v = 100.0 / 3.6;
    // Ann's fixes arrive at 1 s, 2 s and 3 s; Ben, 20 m to her east, took his fix at 2 s and sends at 3 s.
    put_fix(&a, v * 1, 0, 1000);
    put_fix(&a, v * 2, 0, 2000);
    put_fix(&b, v * 2, 20, 2000);
    put_fix(&a, v * 3, 0, 3000);
    mesh_core_tick(&b, 3000, true, air_send, &ba); // Ben's fix is 1000 ms old when sent
    deliver(&ba, &a, &aa, 3000);
    mesh_friend_fix_t fix[2];
    assert(mesh_core_friends_in_range(&a, 3000, fix, 2) == 1);
    assert(fabsf(fix[0].bearing_deg - 90.0f) < 1.0f && fabsf(fix[0].dist_m - 20.0f) < 0.5f);

    // Without a fresh own fix nothing is compared: no offset, no marker.
    static mesh_core_t c, d;
    air_t ca = {0}, da = {0};
    setup(&c, 3, "Cat", 3);
    setup(&d, 4, "Dan", 4);
    make_friends(&c, &ca, &d, &da, 500);
    put_fix(&d, 0, 20, 2000);
    mesh_core_tick(&d, 2000, true, air_send, &da);
    deliver(&da, &c, &ca, 2000);
    assert(mesh_core_friends_in_range(&c, 2000, fix, 2) == 0);
}

static float bearing_dev(float b, float want) {
    float d = fabsf(b - want);
    return d > 180.0f ? 360.0f - d : d;
}

// GPS jitter of a few metres at close range swings the raw bearing wildly; the smoothed vector holds.
static void test_offset_smoothing(void) {
    static mesh_core_t a, b;
    air_t aa = {0}, ba = {0};
    setup(&a, 1, "Ann", 1);
    setup(&b, 2, "Ben", 2);
    make_friends(&a, &aa, &b, &ba, 500);
    float raw_worst = 0, smooth_worst = 0;
    mesh_friend_fix_t fix[2];
    for (int i = 1; i <= 30; i++) {
        const uint32_t now = 1000u * (uint32_t)i;
        const double jitter = (i % 2) ? 4.0 : -4.0; // Ben's fix wanders 4 m either side of 10 m north
        put_fix(&a, 0, 0, now);
        put_fix(&b, 10, jitter, now);
        for (uint32_t t = now; t < now + 1000; t += 100) { // POS is jittered, so tick finer than 1 s
            mesh_core_tick(&a, t, true, air_send, &aa);
            mesh_core_tick(&b, t, true, air_send, &ba);
            deliver(&aa, &b, &ba, t);
            deliver(&ba, &a, &aa, t);
        }
        if (i < 8) continue; // let the filter settle
        assert(mesh_core_friends_in_range(&a, now + 900, fix, 2) == 1);
        smooth_worst = fmaxf(smooth_worst, bearing_dev(fix[0].bearing_deg, 0.0f));
        raw_worst = fmaxf(raw_worst, (float)(atan2(4.0, 10.0) * 180.0 / 3.14159265358979));
    }
    assert(raw_worst > 20.0f);
    assert(smooth_worst < raw_worst * 0.6f);
    assert(fabsf(fix[0].dist_m - 10.0f) < 1.5f);

    // An offset older than MESH_OFFSET_MAX_AGE_MS is not drawn, even though the peer is still in range.
    const uint32_t later = 31000 + MESH_OFFSET_MAX_AGE_MS + 200; // last tick was at 30,900
    put_fix(&a, 0, 0, later);
    mesh_core_tick(&a, later, true, air_send, &aa);
    assert(a.peers[0].used || a.peers[1].used);
    assert(mesh_core_friends_in_range(&a, later, fix, 2) == 0);
}

int main(void) {
    test_aes_fips197();
    test_ccm_rfc3610();
    test_pseudonym();
    test_hello_round_trip();
    test_target_and_keys_frames();
    test_pos_auth();
    test_look_frame();
    test_identity_and_record();
    test_geo();
    test_no_automatic_prompt();
    test_menu_discovery_and_grace();
    test_friending_from_menu();
    test_request_lapses_and_retries();
    test_mutual_request();
    test_request_ignored_when_busy_or_unknown();
    test_gate_holds_prompt_and_timeout();
    test_refriend_after_lost_list();
    test_handshake_survives_own_advert_window();
    test_stranger_cannot_read_position();
    test_look_on_rediscovery_and_change();
    test_bearing_aligned_in_time();
    test_offset_smoothing();
    printf("mesh_test: all passed\n");
    return 0;
}
