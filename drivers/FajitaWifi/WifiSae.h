// SPDX-License-Identifier: BSD-2-Clause-Patent
// WPA3-Personal for the WCN3990 station: SAE (IEEE 802.11-2020 12.4, ECC group 19 = NIST P-256, hunting-and-pecking) and the
// crypto the SAE AKM (00-0f-ac:8) needs in the 4-way handshake: SHA-256, HMAC-SHA-256, the 802.11 KDF (12.7.1.7.2) and
// AES-128-CMAC. Freestanding.
// The design follows hostap's sae.c / dragonfly.c: a fixed minimum of 40 hunting-and-pecking iterations that all run whatever
// iteration finds the point, the password replaced by a random stub after it is found, constant-time selection of x and of
// the seed's parity, a blinded quadratic-residue test (v * r^2 * qr|qnr with random r, qr, qnr), and validation of the peer's
// commit (1 < scalar < r, element coordinates < p and on the curve, result not the point at infinity, no reflection).
// Field arithmetic is Montgomery multiplication on 8 x 32-bit limbs. Host-tested against IEEE 802.11-2020 Annex J.10 (via
// hostap's common_module_tests.c) and a full SAE exchange between two instances (tests/t_sae.c).
#ifndef FAJITA_WIFI_SAE_H
#define FAJITA_WIFI_SAE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define WSAE_GROUP 19u
#define WSAE_PRIME_LEN 32u
#define WSAE_COMMIT_LEN (2u + 32u + 64u)   // group, scalar, element
#define WSAE_CONFIRM_LEN (2u + 32u)        // send-confirm, confirm
#define WSAE_MIN_ITER 40u
#define WSAE_MAX_ITER 200u
#define WSAE_TOKEN_MAX 128u
enum {WSAE_OK = 0, WSAE_ERR_INVAL = -22, WSAE_ERR_GROUP = -77 /* unsupported group: status 77 */, WSAE_ERR_PEER = -71,
      WSAE_ERR_CONFIRM = -74, WSAE_ERR_NOPWE = -2, WSAE_ERR_RNG = -5};

// ---- hashes ----
#define WSAE_SHA256_LEN 32u
typedef struct {uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; unsigned fill;} wsae_sha256_ctx;
void wsae_sha256_init(wsae_sha256_ctx *c);
void wsae_sha256_update(wsae_sha256_ctx *c, const void *data, size_t n);
void wsae_sha256_final(wsae_sha256_ctx *c, uint8_t out[WSAE_SHA256_LEN]);
void wsae_sha256(const void *data, size_t n, uint8_t out[WSAE_SHA256_LEN]);
void wsae_hmac_sha256(const uint8_t *key, size_t key_len, unsigned nv, const uint8_t *const *v, const size_t *vlen,
                      uint8_t out[WSAE_SHA256_LEN]);
// KDF-SHA-256-<bits>(key, label, context): HMAC(key, i LE16 || label || context || bits LE16), i = 1, 2, ...
void wsae_kdf_sha256(const uint8_t *key, size_t key_len, const char *label, const uint8_t *ctx, size_t ctx_len, uint8_t *out,
                     size_t bits);
// AES-128-CMAC (RFC 4493) over the concatenation of nv pieces
void wsae_aes_cmac(const uint8_t key[16], unsigned nv, const uint8_t *const *v, const size_t *vlen, uint8_t mac[16]);

// ---- SAE ----
typedef void (*wsae_rng_fn)(void *ctx, uint8_t *out, uint32_t n);
typedef struct {uint32_t x[8], y[8], z[8];} wsae_point;   // Jacobian, Montgomery form
typedef struct {
    wsae_rng_fn rng; void *rng_ctx;
    uint8_t own[6], peer[6];
    wsae_point pwe;
    uint32_t rand[8];                      // private (plain, not Montgomery)
    uint8_t own_scalar[32], own_element[64];
    uint8_t peer_scalar[32], peer_element[64];
    uint8_t kck[32], pmk[32], pmkid[16];
    uint16_t send_confirm, peer_send_confirm;
    int have_pwe, have_commit, have_keys;
    unsigned pwe_iterations;               // diagnostics: the loop always runs at least WSAE_MIN_ITER
} wsae;

// Derive the PWE (hunting and pecking) and a fresh commit (random rand and mask). own/peer: the two MAC addresses.
int wsae_init(wsae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *pw, size_t pw_len, wsae_rng_fn rng, void *rng_ctx);
// The commit from given rand and mask (big-endian, < r; test vectors), after the PWE is derived.
int wsae_set_commit(wsae *s, const uint8_t rand_be[32], const uint8_t mask_be[32]);
// The SAE Commit body after the auth header: group || [token] || scalar || element. Returns its length or < 0.
long wsae_write_commit(const wsae *s, const uint8_t *token, size_t token_len, uint8_t *out, size_t cap);
// The peer's Commit body: validates it and derives k, KCK, PMK and PMKID. WSAE_ERR_GROUP for another group.
int wsae_process_commit(wsae *s, const uint8_t *body, size_t len);
// Our Confirm body (send-confirm LE16 || confirm); the send-confirm counter goes up after each.
long wsae_write_confirm(wsae *s, uint8_t *out, size_t cap);
// The peer's Confirm body: WSAE_OK when its confirm verifies.
int wsae_check_confirm(wsae *s, const uint8_t *body, size_t len);
void wsae_clear(wsae *s);

// ---- P-256 helpers (exposed for the tests) ----
// k * (x, y) for affine big-endian coordinates; 0, or < 0 when the input is not on the curve or the result is infinity.
int wsae_p256_mul(const uint8_t x[32], const uint8_t y[32], const uint8_t k[32], uint8_t ox[32], uint8_t oy[32]);

#ifdef __cplusplus
}
#endif
#endif
