#include "mesh_crypto.h"

#include <string.h>

static const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }

void mesh_aes128_encrypt(const uint8_t key[MESH_CRYPTO_KEY_LEN], const uint8_t in[16], uint8_t out[16]) {
    uint8_t rk[176];
    memcpy(rk, key, 16);
    uint8_t rcon = 0x01;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            const uint8_t t0 = t[0];
            t[0] = (uint8_t)(kSbox[t[1]] ^ rcon);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[t0];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }

    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int round = 1; round <= 10; round++) {
        uint8_t t[16];
        // SubBytes + ShiftRows: state is column-major, row r rotates left by r.
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) t[c * 4 + r] = kSbox[s[((c + r) % 4) * 4 + r]];
        }
        if (round != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = &t[c * 4];
                const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                const uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ rk[round * 16 + i]);
    }
    memcpy(out, s, 16);
}

// CBC-MAC over B0 | encoded AAD | payload, each zero-padded to the block (RFC 3610 §2.2).
static void ccm_mac(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t len, uint8_t mac[16]) {
    uint8_t b[16];
    b[0] = (uint8_t)((aad_len ? 0x40 : 0) | (((MESH_CRYPTO_TAG_LEN - 2) / 2) << 3) | (2 - 1));
    memcpy(&b[1], nonce, MESH_CRYPTO_NONCE_LEN);
    b[14] = (uint8_t)(len >> 8);
    b[15] = (uint8_t)len;
    mesh_aes128_encrypt(key, b, mac);

    if (aad_len) {
        // First AAD block leads with the 2-byte length.
        size_t used = 0;
        size_t pos = 2;
        memset(b, 0, 16);
        b[0] = (uint8_t)(aad_len >> 8);
        b[1] = (uint8_t)aad_len;
        while (used < aad_len) {
            while (pos < 16 && used < aad_len) b[pos++] = aad[used++];
            for (int i = 0; i < 16; i++) mac[i] ^= b[i];
            mesh_aes128_encrypt(key, mac, mac);
            memset(b, 0, 16);
            pos = 0;
        }
    }
    for (size_t off = 0; off < len; off += 16) {
        const size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; i++) mac[i] ^= pt[off + i];
        mesh_aes128_encrypt(key, mac, mac);
    }
}

// CTR keystream: A_i = flags(L-1) | nonce | counter i. A_0 masks the tag, A_1.. the payload.
static void ccm_ctr(const uint8_t *key, const uint8_t *nonce, uint16_t counter, uint8_t out[16]) {
    uint8_t a[16];
    a[0] = 2 - 1;
    memcpy(&a[1], nonce, MESH_CRYPTO_NONCE_LEN);
    a[14] = (uint8_t)(counter >> 8);
    a[15] = (uint8_t)counter;
    mesh_aes128_encrypt(key, a, out);
}

static void ccm_crypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t ks[16];
    for (size_t off = 0; off < len; off += 16) {
        ccm_ctr(key, nonce, (uint16_t)(off / 16 + 1), ks);
        const size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    }
}

void mesh_ccm_encrypt(const uint8_t key[MESH_CRYPTO_KEY_LEN], const uint8_t nonce[MESH_CRYPTO_NONCE_LEN],
                      const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t len, uint8_t *ct,
                      uint8_t tag[MESH_CRYPTO_TAG_LEN]) {
    uint8_t mac[16], s0[16];
    ccm_mac(key, nonce, aad, aad_len, pt, len, mac);
    ccm_crypt(key, nonce, pt, len, ct);
    ccm_ctr(key, nonce, 0, s0);
    for (size_t i = 0; i < MESH_CRYPTO_TAG_LEN; i++) tag[i] = (uint8_t)(mac[i] ^ s0[i]);
}

bool mesh_ccm_decrypt(const uint8_t key[MESH_CRYPTO_KEY_LEN], const uint8_t nonce[MESH_CRYPTO_NONCE_LEN],
                      const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t len, uint8_t *pt,
                      const uint8_t tag[MESH_CRYPTO_TAG_LEN]) {
    uint8_t mac[16], s0[16];
    ccm_crypt(key, nonce, ct, len, pt);
    ccm_mac(key, nonce, aad, aad_len, pt, len, mac);
    ccm_ctr(key, nonce, 0, s0);
    uint8_t diff = 0;
    for (size_t i = 0; i < MESH_CRYPTO_TAG_LEN; i++) diff |= (uint8_t)(tag[i] ^ mac[i] ^ s0[i]);
    if (diff != 0) {
        memset(pt, 0, len);
        return false;
    }
    return true;
}

void mesh_tag_key(const uint8_t rider_key[MESH_CRYPTO_KEY_LEN], uint8_t tag_key[MESH_CRYPTO_KEY_LEN]) {
    static const uint8_t kLabel[16] = {'O', 'A', '-', 'T', 'A', 'G', 'K', 'E', 'Y'};
    mesh_aes128_encrypt(rider_key, kLabel, tag_key);
}

void mesh_pseudonym_make(const uint8_t tag_key[MESH_CRYPTO_KEY_LEN], const uint8_t prand[4],
                         uint8_t out[MESH_PSEUDONYM_LEN]) {
    uint8_t block[16] = {0};
    memcpy(block, prand, 4);
    uint8_t enc[16];
    mesh_aes128_encrypt(tag_key, block, enc);
    memcpy(out, prand, 4);
    memcpy(out + 4, enc, 4);
}

bool mesh_pseudonym_match(const uint8_t tag_key[MESH_CRYPTO_KEY_LEN], const uint8_t pseudonym[MESH_PSEUDONYM_LEN]) {
    uint8_t want[MESH_PSEUDONYM_LEN];
    mesh_pseudonym_make(tag_key, pseudonym, want);
    return memcmp(want + 4, pseudonym + 4, 4) == 0;
}
