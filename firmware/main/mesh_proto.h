#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rider mesh wire formats (docs/reference/rider_mesh_v1.md): terminal-to-terminal frames over ESP-NOW,
// plus the two phone-facing values that feed them (rider identity, friend record). Pure C with no
// ESP-IDF dependency, like settings.c, so host tests and the simulator link it as-is. Multi-byte
// fields are little-endian except colours, which are RGB big-endian as in the settings TLV.
//
// Every frame starts with the same 14-byte header:
//   'O' 'A' | proto_ver | type | sender rider_id (8) | seq u16
// A receiver drops a wrong magic or version and ignores types it does not know, so a newer
// terminal's frames (nav share, types 0x10-0x1F) pass harmlessly through an older one.

#define MESH_PROTO_VERSION 1u
#define MESH_HEADER_LEN 14u
#define MESH_FRAME_MAX 250u // ESP-NOW v1 payload limit
#define MESH_RIDER_ID_LEN 8u
#define MESH_KEY_LEN MESH_CRYPTO_KEY_LEN
#define MESH_NAME_MAX 12u   // bytes of UTF-8, not characters
#define MESH_ART_BYTES 80u  // custom avatar: 16x16 then 8x8, 2 bpp (avatar_store.h)
#define MESH_ART8_OFFSET 64u
#define MESH_ART8_BYTES 16u

enum {
    MESH_TYPE_HELLO = 0x01,
    MESH_TYPE_FRIEND_ACCEPT = 0x02,
    MESH_TYPE_FRIEND_ACK = 0x03,
    MESH_TYPE_POS = 0x04,
    // 0x10-0x1F reserved for nav share (Phase 2): encrypted like POS, fragmented.
    MESH_TYPE_NAV_SHARE_FIRST = 0x10,
    MESH_TYPE_NAV_SHARE_LAST = 0x1F,
};

// HELLO caps bits.
#define MESH_CAP_NAV_SHARE 0x01u // reserved for Phase 2; never set yet

typedef struct {
    uint8_t type;
    uint8_t sender[MESH_RIDER_ID_LEN];
    uint16_t seq;
} mesh_header_t;

// How a rider appears to others: the same fields the settings TLV and avatar store hold.
typedef struct {
    uint8_t avatar;       // settings_avatar_t
    uint32_t accent_rgb;  // 0xRRGGBB
    char name[MESH_NAME_MAX + 1];
    bool has_custom;      // `art` is valid (avatar == custom and art was uploaded)
    uint8_t art[MESH_ART_BYTES];
} mesh_look_t;

typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    uint16_t speed_kmh_x10;   // 0xFFFF = unknown
    uint16_t heading_deg_x10; // 0xFFFF = unknown
    uint8_t acc_m;            // horizontal accuracy, 255 = 255 m or worse
} mesh_pos_t;

// -- frames ------------------------------------------------------------------------------------
// Encoders return bytes written, or 0 if `cap` is too small. Decoders take the whole frame
// (header included) and return false on anything malformed.

bool mesh_decode_header(const uint8_t *buf, size_t len, mesh_header_t *out);

size_t mesh_encode_hello(const mesh_header_t *h, const mesh_look_t *look, uint8_t caps, uint8_t *buf,
                         size_t cap);
bool mesh_decode_hello(const uint8_t *buf, size_t len, mesh_look_t *look, uint8_t *caps);

// FRIEND_ACCEPT: "I, the sender, accept `target`; here is my key". Broadcast, so it carries the
// target rather than relying on a MAC address.
size_t mesh_encode_accept(const mesh_header_t *h, const uint8_t target[MESH_RIDER_ID_LEN],
                          const uint8_t key[MESH_KEY_LEN], uint8_t *buf, size_t cap);
bool mesh_decode_accept(const uint8_t *buf, size_t len, uint8_t target[MESH_RIDER_ID_LEN],
                        uint8_t key[MESH_KEY_LEN]);

// FRIEND_ACK: "I received `target`'s accept" -- stops its retries. Not itself a friendship.
size_t mesh_encode_ack(const mesh_header_t *h, const uint8_t target[MESH_RIDER_ID_LEN], uint8_t *buf,
                       size_t cap);
bool mesh_decode_ack(const uint8_t *buf, size_t len, uint8_t target[MESH_RIDER_ID_LEN]);

// POS: encrypted under the sender's own key, which only its friends hold. `session` is a random
// per-boot value re-rolled when seq wraps; with the sender id and seq it makes the nonce unique.
size_t mesh_encode_pos(const mesh_header_t *h, const uint8_t key[MESH_KEY_LEN], uint32_t session,
                       const mesh_pos_t *pos, uint8_t *buf, size_t cap);
bool mesh_decode_pos(const uint8_t *buf, size_t len, const uint8_t key[MESH_KEY_LEN], mesh_pos_t *pos);

// -- identity (phone -> terminal, identity characteristic) ---------------------------------------
// [ver=1][rider_id 8][rider_key 16][name_len u8][name <= 12 bytes UTF-8]

#define MESH_IDENTITY_VERSION 1u
#define MESH_IDENTITY_MAX_LEN (1u + MESH_RIDER_ID_LEN + MESH_KEY_LEN + 1u + MESH_NAME_MAX)

typedef struct {
    uint8_t id[MESH_RIDER_ID_LEN];
    uint8_t key[MESH_KEY_LEN];
    char name[MESH_NAME_MAX + 1];
} mesh_identity_t;

bool mesh_identity_decode(const uint8_t *buf, size_t len, mesh_identity_t *out);
size_t mesh_identity_encode(const mesh_identity_t *id, uint8_t *buf, size_t cap);

// -- friend record (friends characteristic and NVS) ----------------------------------------------
// Fixed 122 bytes:
//   rider_id 8 | rider_key 16 | avatar u8 | accent RGB 3 | flags u8 | name_len u8 | name 12
//   (zero-padded) | art 80 (zeros unless flags bit0)

#define MESH_FRIEND_RECORD_LEN 122u
#define MESH_FRIEND_FLAG_CUSTOM 0x01u

typedef struct {
    uint8_t id[MESH_RIDER_ID_LEN];
    uint8_t key[MESH_KEY_LEN];
    mesh_look_t look;
} mesh_friend_t;

void mesh_friend_encode(const mesh_friend_t *f, uint8_t out[MESH_FRIEND_RECORD_LEN]);
bool mesh_friend_decode(const uint8_t in[MESH_FRIEND_RECORD_LEN], mesh_friend_t *out);

// -- geometry ------------------------------------------------------------------------------------

// Initial bearing (degrees, 0 = north, clockwise, [0, 360)) and distance (m) from `a` to `b`.
// Equirectangular: well under 1% error at the few-hundred-metre range the radio reaches.
void mesh_geo(int32_t a_lat_e7, int32_t a_lon_e7, int32_t b_lat_e7, int32_t b_lon_e7, float *bearing_deg,
              float *dist_m);

#ifdef __cplusplus
}
#endif
