#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// AES-128-CCM (RFC 3610) for rider-mesh position frames (docs/reference/rider_mesh_v1.md): 13-byte
// nonce, 8-byte tag. A portable software AES rather than mbedtls so the host tests exercise the
// exact code the terminal runs; at one frame a second per friend, speed is irrelevant.

#define MESH_CRYPTO_KEY_LEN 16u
#define MESH_CRYPTO_NONCE_LEN 13u
#define MESH_CRYPTO_TAG_LEN 8u

// One AES-128 block encryption (exposed for the FIPS-197 known-answer test).
void mesh_aes128_encrypt(const uint8_t key[MESH_CRYPTO_KEY_LEN], const uint8_t in[16], uint8_t out[16]);

// Encrypts `len` bytes of `pt` into `ct` (may alias) and writes the tag. `aad` is authenticated
// but not encrypted. `len` and `aad_len` must each be under 0xFF00.
void mesh_ccm_encrypt(const uint8_t key[MESH_CRYPTO_KEY_LEN], const uint8_t nonce[MESH_CRYPTO_NONCE_LEN],
                      const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t len, uint8_t *ct,
                      uint8_t tag[MESH_CRYPTO_TAG_LEN]);

// Decrypts and verifies. On a tag mismatch returns false and zeroes `pt`, so a forged frame never
// leaks a plausible position to a caller that forgot to check.
bool mesh_ccm_decrypt(const uint8_t key[MESH_CRYPTO_KEY_LEN], const uint8_t nonce[MESH_CRYPTO_NONCE_LEN],
                      const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t len, uint8_t *pt,
                      const uint8_t tag[MESH_CRYPTO_TAG_LEN]);


// -- pseudonyms (docs/blueprints/04_rider_mesh.md 04.1.3) ------------------------------------------
// A rider appears on air as prand (4) | hash (4), hash = AES(tag_key, prand | zero pad)[0..3], like a
// BLE resolvable private address. Only a holder of the rider's key can tell whose it is.

#define MESH_PSEUDONYM_LEN 8u

// tag_key = AES(rider_key, "OA-TAGKEY" | zero pad): the key that drives CCM is never also used as a
// raw block cipher on attacker-chosen input.
void mesh_tag_key(const uint8_t rider_key[MESH_CRYPTO_KEY_LEN], uint8_t tag_key[MESH_CRYPTO_KEY_LEN]);
void mesh_pseudonym_make(const uint8_t tag_key[MESH_CRYPTO_KEY_LEN], const uint8_t prand[4],
                         uint8_t out[MESH_PSEUDONYM_LEN]);
// True when `pseudonym` was made with `tag_key`. A false match on a stranger is about 2^-32 per key.
bool mesh_pseudonym_match(const uint8_t tag_key[MESH_CRYPTO_KEY_LEN], const uint8_t pseudonym[MESH_PSEUDONYM_LEN]);

#ifdef __cplusplus
}
#endif
