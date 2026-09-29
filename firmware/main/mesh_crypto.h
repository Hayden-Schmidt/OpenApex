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

#ifdef __cplusplus
}
#endif
