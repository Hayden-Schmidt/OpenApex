#include "mesh_proto.h"

#include <math.h>
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put_u32(uint8_t *p, uint32_t v) {
    put_u16(p, (uint16_t)v);
    put_u16(p + 2, (uint16_t)(v >> 16));
}
static uint32_t get_u32(const uint8_t *p) { return get_u16(p) | ((uint32_t)get_u16(p + 2) << 16); }
static void put_rgb(uint8_t *p, uint32_t rgb) {
    p[0] = (uint8_t)(rgb >> 16);
    p[1] = (uint8_t)(rgb >> 8);
    p[2] = (uint8_t)rgb;
}
static uint32_t get_rgb(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

// Name as length + bytes; the decoded copy is always terminated. An embedded NUL would let two
// names that differ after it compare equal on the terminal and not on the phone, so it's malformed.
static bool get_name(const uint8_t *p, uint8_t n, char out[MESH_NAME_MAX + 1]) {
    if (n > MESH_NAME_MAX || memchr(p, 0, n) != NULL) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}
static uint8_t name_len(const char *name) {
    const size_t n = strnlen(name, MESH_NAME_MAX);
    return (uint8_t)n;
}

static void put_header(const mesh_header_t *h, uint8_t *buf) {
    buf[0] = 'O';
    buf[1] = 'A';
    buf[2] = MESH_PROTO_VERSION;
    buf[3] = h->type;
    memcpy(&buf[4], h->sender, MESH_RIDER_ID_LEN);
    put_u16(&buf[12], h->seq);
}

bool mesh_decode_header(const uint8_t *buf, size_t len, mesh_header_t *out) {
    if (len < MESH_HEADER_LEN || buf[0] != 'O' || buf[1] != 'A' || buf[2] != MESH_PROTO_VERSION) return false;
    out->type = buf[3];
    memcpy(out->sender, &buf[4], MESH_RIDER_ID_LEN);
    out->seq = get_u16(&buf[12]);
    return true;
}

// Look block, shared by HELLO (clear) and LOOK (encrypted): avatar | accent 3 | name_len | name |
// has_custom | art 80 (if has_custom).
#define LOOK_MAX_LEN (6u + MESH_NAME_MAX + MESH_ART_BYTES)

static size_t look_len(const mesh_look_t *look) {
    return 6u + name_len(look->name) + (look->has_custom ? MESH_ART_BYTES : 0u);
}

static void put_look(uint8_t *p, const mesh_look_t *look) {
    const uint8_t n = name_len(look->name);
    *p++ = look->avatar;
    put_rgb(p, look->accent_rgb);
    p += 3;
    *p++ = n;
    memcpy(p, look->name, n);
    p += n;
    *p++ = look->has_custom ? 1 : 0;
    if (look->has_custom) memcpy(p, look->art, MESH_ART_BYTES);
}

// `len` is exactly the bytes available; trailing bytes are malformed.
static bool get_look(const uint8_t *p, size_t len, mesh_look_t *out) {
    if (len < 6u) return false;
    const uint8_t n = p[4];
    if (len < 6u + n) return false;
    mesh_look_t l;
    memset(&l, 0, sizeof(l));
    l.avatar = p[0];
    l.accent_rgb = get_rgb(&p[1]);
    if (!get_name(&p[5], n, l.name)) return false;
    const uint8_t custom = p[5 + n];
    if (custom > 1) return false;
    l.has_custom = custom == 1;
    if (len != 6u + n + (l.has_custom ? MESH_ART_BYTES : 0u)) return false;
    if (l.has_custom) memcpy(l.art, &p[6 + n], MESH_ART_BYTES);
    *out = l;
    return true;
}

// HELLO body: caps | flags (bit0 look present) | [look block]
#define HELLO_FLAG_LOOK 0x01u

size_t mesh_encode_hello(const mesh_header_t *h, const mesh_look_t *look, uint8_t caps, uint8_t *buf,
                         size_t cap) {
    const size_t len = MESH_HEADER_LEN + 2u + (look ? look_len(look) : 0u);
    if (cap < len) return 0;
    put_header(h, buf);
    buf[MESH_HEADER_LEN] = caps;
    buf[MESH_HEADER_LEN + 1] = look ? HELLO_FLAG_LOOK : 0;
    if (look) put_look(&buf[MESH_HEADER_LEN + 2], look);
    return len;
}

bool mesh_decode_hello(const uint8_t *buf, size_t len, mesh_look_t *look, bool *has_look, uint8_t *caps) {
    if (len < MESH_HEADER_LEN + 2u) return false;
    const uint8_t flags = buf[MESH_HEADER_LEN + 1];
    if (flags & ~HELLO_FLAG_LOOK) return false;
    *has_look = (flags & HELLO_FLAG_LOOK) != 0;
    if (*has_look) {
        if (!get_look(&buf[MESH_HEADER_LEN + 2], len - MESH_HEADER_LEN - 2u, look)) return false;
    } else if (len != MESH_HEADER_LEN + 2u) {
        return false;
    }
    *caps = buf[MESH_HEADER_LEN];
    return true;
}

size_t mesh_encode_target(const mesh_header_t *h, const uint8_t target[MESH_RIDER_ID_LEN], uint8_t *buf,
                          size_t cap) {
    const size_t len = MESH_HEADER_LEN + MESH_RIDER_ID_LEN;
    if (cap < len) return 0;
    put_header(h, buf);
    memcpy(&buf[MESH_HEADER_LEN], target, MESH_RIDER_ID_LEN);
    return len;
}

bool mesh_decode_target(const uint8_t *buf, size_t len, uint8_t target[MESH_RIDER_ID_LEN]) {
    if (len < MESH_HEADER_LEN + MESH_RIDER_ID_LEN) return false;
    memcpy(target, &buf[MESH_HEADER_LEN], MESH_RIDER_ID_LEN);
    return true;
}

#define KEYS_BODY_LEN (MESH_RIDER_ID_LEN + MESH_RIDER_ID_LEN + MESH_KEY_LEN)

size_t mesh_encode_keys(const mesh_header_t *h, const uint8_t target[MESH_RIDER_ID_LEN],
                        const uint8_t rider_id[MESH_RIDER_ID_LEN], const uint8_t key[MESH_KEY_LEN], uint8_t *buf,
                        size_t cap) {
    const size_t len = MESH_HEADER_LEN + KEYS_BODY_LEN;
    if (cap < len) return 0;
    put_header(h, buf);
    memcpy(&buf[MESH_HEADER_LEN], target, MESH_RIDER_ID_LEN);
    memcpy(&buf[MESH_HEADER_LEN + MESH_RIDER_ID_LEN], rider_id, MESH_RIDER_ID_LEN);
    memcpy(&buf[MESH_HEADER_LEN + 2u * MESH_RIDER_ID_LEN], key, MESH_KEY_LEN);
    return len;
}

bool mesh_decode_keys(const uint8_t *buf, size_t len, uint8_t target[MESH_RIDER_ID_LEN],
                      uint8_t rider_id[MESH_RIDER_ID_LEN], uint8_t key[MESH_KEY_LEN]) {
    if (len < MESH_HEADER_LEN + KEYS_BODY_LEN) return false;
    memcpy(target, &buf[MESH_HEADER_LEN], MESH_RIDER_ID_LEN);
    memcpy(rider_id, &buf[MESH_HEADER_LEN + MESH_RIDER_ID_LEN], MESH_RIDER_ID_LEN);
    memcpy(key, &buf[MESH_HEADER_LEN + 2u * MESH_RIDER_ID_LEN], MESH_KEY_LEN);
    return true;
}

// POS body: session u32 | age_ms u16 | ttl u8 | ciphertext 13 | tag 8. The AAD is header + session,
// so neither can be swapped onto another rider's ciphertext; age_ms and ttl are outside it because
// a 04.4 relay edits them.
#define POS_PLAIN_LEN 13u
#define AAD_LEN (MESH_HEADER_LEN + 4u) // header + session
#define POS_FRAME_LEN (AAD_LEN + 3u + POS_PLAIN_LEN + MESH_CRYPTO_TAG_LEN)

// Sender pseudonym | low 3 bytes of session | seq: unique per frame while a boot's session holds.
static void frame_nonce(const uint8_t *frame, uint8_t nonce[MESH_CRYPTO_NONCE_LEN]) {
    memcpy(nonce, &frame[4], MESH_RIDER_ID_LEN);
    memcpy(&nonce[8], &frame[MESH_HEADER_LEN], 3);
    memcpy(&nonce[11], &frame[12], 2);
}

size_t mesh_encode_pos(const mesh_header_t *h, const uint8_t key[MESH_KEY_LEN], uint32_t session,
                       const mesh_pos_t *pos, uint16_t age_ms, uint8_t ttl, uint8_t *buf, size_t cap) {
    if (cap < POS_FRAME_LEN) return 0;
    put_header(h, buf);
    put_u32(&buf[MESH_HEADER_LEN], session);
    put_u16(&buf[AAD_LEN], age_ms);
    buf[AAD_LEN + 2] = ttl;
    uint8_t pt[POS_PLAIN_LEN];
    put_u32(&pt[0], (uint32_t)pos->lat_e7);
    put_u32(&pt[4], (uint32_t)pos->lon_e7);
    put_u16(&pt[8], pos->speed_kmh_x10);
    put_u16(&pt[10], pos->heading_deg_x10);
    pt[12] = pos->acc_m;
    uint8_t nonce[MESH_CRYPTO_NONCE_LEN];
    frame_nonce(buf, nonce);
    uint8_t *ct = &buf[AAD_LEN + 3u];
    mesh_ccm_encrypt(key, nonce, buf, AAD_LEN, pt, POS_PLAIN_LEN, ct, ct + POS_PLAIN_LEN);
    return POS_FRAME_LEN;
}

bool mesh_decode_pos(const uint8_t *buf, size_t len, const uint8_t key[MESH_KEY_LEN], mesh_pos_t *pos,
                     uint16_t *age_ms, uint8_t *ttl) {
    if (len < POS_FRAME_LEN) return false;
    uint8_t nonce[MESH_CRYPTO_NONCE_LEN];
    frame_nonce(buf, nonce);
    const uint8_t *ct = &buf[AAD_LEN + 3u];
    uint8_t pt[POS_PLAIN_LEN];
    if (!mesh_ccm_decrypt(key, nonce, buf, AAD_LEN, ct, POS_PLAIN_LEN, pt, ct + POS_PLAIN_LEN)) return false;
    pos->lat_e7 = (int32_t)get_u32(&pt[0]);
    pos->lon_e7 = (int32_t)get_u32(&pt[4]);
    pos->speed_kmh_x10 = get_u16(&pt[8]);
    pos->heading_deg_x10 = get_u16(&pt[10]);
    pos->acc_m = pt[12];
    *age_ms = get_u16(&buf[AAD_LEN]);
    *ttl = buf[AAD_LEN + 2];
    return true;
}

// LOOK body: session u32 | ciphertext (look block) | tag 8.
size_t mesh_encode_look(const mesh_header_t *h, const uint8_t key[MESH_KEY_LEN], uint32_t session,
                        const mesh_look_t *look, uint8_t *buf, size_t cap) {
    const size_t plain = look_len(look);
    const size_t len = AAD_LEN + plain + MESH_CRYPTO_TAG_LEN;
    if (cap < len) return 0;
    put_header(h, buf);
    put_u32(&buf[MESH_HEADER_LEN], session);
    uint8_t pt[LOOK_MAX_LEN];
    put_look(pt, look);
    uint8_t nonce[MESH_CRYPTO_NONCE_LEN];
    frame_nonce(buf, nonce);
    uint8_t *ct = &buf[AAD_LEN];
    mesh_ccm_encrypt(key, nonce, buf, AAD_LEN, pt, plain, ct, ct + plain);
    return len;
}

bool mesh_decode_look(const uint8_t *buf, size_t len, const uint8_t key[MESH_KEY_LEN], mesh_look_t *look) {
    if (len < AAD_LEN + 6u + MESH_CRYPTO_TAG_LEN) return false;
    const size_t plain = len - AAD_LEN - MESH_CRYPTO_TAG_LEN;
    if (plain > LOOK_MAX_LEN) return false;
    uint8_t nonce[MESH_CRYPTO_NONCE_LEN];
    frame_nonce(buf, nonce);
    uint8_t pt[LOOK_MAX_LEN];
    const uint8_t *ct = &buf[AAD_LEN];
    if (!mesh_ccm_decrypt(key, nonce, buf, AAD_LEN, ct, plain, pt, ct + plain)) return false;
    return get_look(pt, plain, look);
}

bool mesh_identity_decode(const uint8_t *buf, size_t len, mesh_identity_t *out) {
    const size_t fixed = 1u + MESH_RIDER_ID_LEN + MESH_KEY_LEN + 1u;
    if (len < fixed || buf[0] != MESH_IDENTITY_VERSION) return false;
    const uint8_t n = buf[fixed - 1];
    if (len != fixed + n) return false;
    mesh_identity_t id;
    memcpy(id.id, &buf[1], MESH_RIDER_ID_LEN);
    memcpy(id.key, &buf[1 + MESH_RIDER_ID_LEN], MESH_KEY_LEN);
    if (!get_name(&buf[fixed], n, id.name)) return false;
    *out = id;
    return true;
}

size_t mesh_identity_encode(const mesh_identity_t *id, uint8_t *buf, size_t cap) {
    const uint8_t n = name_len(id->name);
    const size_t len = 1u + MESH_RIDER_ID_LEN + MESH_KEY_LEN + 1u + n;
    if (cap < len) return 0;
    buf[0] = MESH_IDENTITY_VERSION;
    memcpy(&buf[1], id->id, MESH_RIDER_ID_LEN);
    memcpy(&buf[1 + MESH_RIDER_ID_LEN], id->key, MESH_KEY_LEN);
    buf[1 + MESH_RIDER_ID_LEN + MESH_KEY_LEN] = n;
    memcpy(&buf[2 + MESH_RIDER_ID_LEN + MESH_KEY_LEN], id->name, n);
    return len;
}

void mesh_friend_encode(const mesh_friend_t *f, uint8_t out[MESH_FRIEND_RECORD_LEN]) {
    memset(out, 0, MESH_FRIEND_RECORD_LEN);
    memcpy(&out[0], f->id, MESH_RIDER_ID_LEN);
    memcpy(&out[8], f->key, MESH_KEY_LEN);
    out[24] = f->look.avatar;
    put_rgb(&out[25], f->look.accent_rgb);
    out[28] = f->look.has_custom ? MESH_FRIEND_FLAG_CUSTOM : 0;
    out[29] = name_len(f->look.name);
    memcpy(&out[30], f->look.name, out[29]);
    if (f->look.has_custom) memcpy(&out[42], f->look.art, MESH_ART_BYTES);
}

bool mesh_friend_decode(const uint8_t in[MESH_FRIEND_RECORD_LEN], mesh_friend_t *out) {
    mesh_friend_t f;
    memset(&f, 0, sizeof(f));
    memcpy(f.id, &in[0], MESH_RIDER_ID_LEN);
    memcpy(f.key, &in[8], MESH_KEY_LEN);
    f.look.avatar = in[24];
    f.look.accent_rgb = get_rgb(&in[25]);
    f.look.has_custom = (in[28] & MESH_FRIEND_FLAG_CUSTOM) != 0;
    if (!get_name(&in[30], in[29], f.look.name)) return false;
    if (f.look.has_custom) memcpy(f.look.art, &in[42], MESH_ART_BYTES);
    *out = f;
    return true;
}

void mesh_offset(int32_t a_lat_e7, int32_t a_lon_e7, int32_t b_lat_e7, int32_t b_lon_e7, float *east_m,
                 float *north_m) {
    const double kEarthM = 6371000.0;
    const double kDeg = 3.14159265358979323846 / 180.0;
    const double lat_mid = ((double)a_lat_e7 + (double)b_lat_e7) * 0.5e-7 * kDeg;
    // Longitude difference wrapped into [-180, 180) so a pair straddling the antimeridian is near.
    double dlon = ((double)b_lon_e7 - (double)a_lon_e7) * 1e-7;
    if (dlon >= 180.0) dlon -= 360.0;
    if (dlon < -180.0) dlon += 360.0;
    *east_m = (float)(dlon * kDeg * cos(lat_mid) * kEarthM);
    *north_m = (float)(((double)b_lat_e7 - (double)a_lat_e7) * 1e-7 * kDeg * kEarthM);
}

void mesh_geo(int32_t a_lat_e7, int32_t a_lon_e7, int32_t b_lat_e7, int32_t b_lon_e7, float *bearing_deg,
              float *dist_m) {
    float east, north;
    mesh_offset(a_lat_e7, a_lon_e7, b_lat_e7, b_lon_e7, &east, &north);
    double b = atan2((double)east, (double)north) * 180.0 / 3.14159265358979323846;
    if (b < 0) b += 360.0;
    *bearing_deg = (float)b;
    *dist_m = hypotf(east, north);
}
