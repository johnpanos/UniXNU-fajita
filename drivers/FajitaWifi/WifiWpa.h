// SPDX-License-Identifier: BSD-2-Clause-Patent
// WPA2-PSK supplicant core (IEEE 802.11-2016 12.7): PBKDF2-HMAC-SHA1 PSK -> PMK, the PRF and the pairwise key
// hierarchy (PTK = KCK | KEK | TK), EAPOL-Key frames (parse and build, messages 1-4 of the 4-way handshake, key
// descriptor version 2: HMAC-SHA1-128 MIC + AES key wrap), and GTK unwrap (RFC 3394). Freestanding C (no libc beyond
// the compiler's headers), with its own small SHA-1, HMAC and AES. Host-tested against the Annex J and RFC 3394 vectors
// (tests/t_wpa.c). Nothing here logs: a caller must never log a passphrase, PMK, PTK or GTK.
#ifndef FAJITA_WIFI_WPA_H
#define FAJITA_WIFI_WPA_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {WWPA_OK = 0, WWPA_ERR_INVAL = -22, WWPA_ERR_PROTO = -71, WWPA_ERR_MIC = -74 /* EBADMSG */, WWPA_ERR_REPLAY = -114,
      WWPA_ERR_NOSPC = -28, WWPA_ERR_STATE = -95, WWPA_ERR_UNWRAP = -129 /* EKEYREJECTED */};

// ---- primitives ----
#define WWPA_SHA1_LEN 20u
typedef struct {uint32_t h[5]; uint64_t bytes; uint8_t buf[64]; unsigned fill;} wwpa_sha1_ctx;
void wwpa_sha1_init(wwpa_sha1_ctx *c);
void wwpa_sha1_update(wwpa_sha1_ctx *c, const void *data, size_t n);
void wwpa_sha1_final(wwpa_sha1_ctx *c, uint8_t out[WWPA_SHA1_LEN]);
void wwpa_sha1(const void *data, size_t n, uint8_t out[WWPA_SHA1_LEN]);
// HMAC-SHA1 over the concatenation of nv vectors.
void wwpa_hmac_sha1(const uint8_t *key, size_t key_len, unsigned nv, const uint8_t *const *v, const size_t *vlen,
                    uint8_t out[WWPA_SHA1_LEN]);

// AES-128/192/256 block encrypt/decrypt (FIPS-197).
typedef struct {uint32_t rk[60]; unsigned rounds;} wwpa_aes;
int wwpa_aes_init(wwpa_aes *a, const uint8_t *key, size_t key_len);   // 16, 24 or 32
void wwpa_aes_encrypt(const wwpa_aes *a, const uint8_t in[16], uint8_t out[16]);
void wwpa_aes_decrypt(const wwpa_aes *a, const uint8_t in[16], uint8_t out[16]);
// RFC 3394 key wrap: n (a multiple of 8, >= 16) bytes in, n + 8 out. Unwrap: n (>= 24) in, n - 8 out, or
// WWPA_ERR_UNWRAP when the integrity check value is not A6A6A6A6A6A6A6A6 (out is then zeroed).
int wwpa_aes_wrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t n, uint8_t *out);
int wwpa_aes_unwrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t n, uint8_t *out);

// ---- key hierarchy ----
#define WWPA_PMK_LEN 32u
#define WWPA_NONCE_LEN 32u
#define WWPA_KCK_LEN 16u
#define WWPA_KEK_LEN 16u
#define WWPA_MIC_LEN 16u
// PSK = PBKDF2(HMAC-SHA1, passphrase, ssid, 4096, 256) (12.7.1.3, J.4). The passphrase is 8..63 printable ASCII
// characters, the SSID 0..32 octets. A 64-hex-digit PSK is not a passphrase: decode it and use it as the PMK directly.
int wwpa_psk(const char *pass, size_t pass_len, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[WWPA_PMK_LEN]);
// PRF-n (12.7.1.2): HMAC-SHA1(K, label || 0 || data || i) for i = 0, 1, ... truncated to out_len bytes.
void wwpa_prf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t data_len, uint8_t *out,
              size_t out_len);
typedef struct {
    uint8_t kck[WWPA_KCK_LEN], kek[WWPA_KEK_LEN], tk[32];
    unsigned tk_len;   // 16 for CCMP-128
} wwpa_ptk;
// PTK = PRF(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(ANonce,SNonce)).
int wwpa_ptk_derive(const uint8_t pmk[WWPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                    const uint8_t anonce[WWPA_NONCE_LEN], const uint8_t snonce[WWPA_NONCE_LEN], unsigned tk_len, wwpa_ptk *ptk);

// ---- EAPOL-Key frames (802.1X EAPOL header + the RSN key descriptor, 12.7.2) ----
#define WWPA_EAPOL_HDR 4u
#define WWPA_KEY_HDR 95u                      // descriptor type .. key data length
#define WWPA_KEY_FRAME_MIN (WWPA_EAPOL_HDR + WWPA_KEY_HDR)   // 99: the MIC is at 81, key data at 99
#define WWPA_KEY_MIC_OFFSET 81u
#define WWPA_EAPOL_TYPE_KEY 3u
#define WWPA_DESC_RSN 2u
// Key Information bits
#define WWPA_KI_VER_MASK 0x0007u
#define WWPA_KI_VER_HMAC_SHA1_AES 2u
#define WWPA_KI_PAIRWISE 0x0008u
#define WWPA_KI_INSTALL 0x0040u
#define WWPA_KI_ACK 0x0080u
#define WWPA_KI_MIC 0x0100u
#define WWPA_KI_SECURE 0x0200u
#define WWPA_KI_ERROR 0x0400u
#define WWPA_KI_REQUEST 0x0800u
#define WWPA_KI_ENC_DATA 0x1000u

typedef struct {
    uint8_t eapol_version;
    uint16_t info, key_len;
    uint64_t replay;
    uint8_t nonce[WWPA_NONCE_LEN], iv[16], rsc[8], mic[WWPA_MIC_LEN];
    uint16_t data_len;
    const uint8_t *data;   // points into the frame
} wwpa_key;
// Parse an EAPOL frame (from the EAPOL header; trailing bytes past the EAPOL body are ignored). 0, or WWPA_ERR_PROTO
// for a frame that is not an RSN EAPOL-Key, WWPA_ERR_INVAL for a truncated or inconsistent one.
int wwpa_key_parse(const uint8_t *frame, size_t len, wwpa_key *k);
// Build an EAPOL-Key frame from k (k->data, k->data_len as the key data; the MIC field is written as k->mic). Returns
// the frame length or WWPA_ERR_NOSPC.
long wwpa_key_build(const wwpa_key *k, uint8_t *out, size_t cap);
// HMAC-SHA1-128 MIC of a whole EAPOL frame with its MIC field taken as zero (the frame is not modified).
int wwpa_key_mic(const uint8_t kck[WWPA_KCK_LEN], const uint8_t *frame, size_t len, uint8_t mic[WWPA_MIC_LEN]);
// Write the MIC into a built frame.
int wwpa_key_sign(const uint8_t kck[WWPA_KCK_LEN], uint8_t *frame, size_t len);
// Constant-time MIC check of a received frame.
int wwpa_key_verify(const uint8_t kck[WWPA_KCK_LEN], const uint8_t *frame, size_t len);

// The GTK KDE (00-0F-AC:1) in decrypted key data; also returns the RSN element (0x30) if present. 0 or WWPA_ERR_PROTO.
typedef struct {
    const uint8_t *rsne; unsigned rsne_len;   // the whole element, id and length included
    uint8_t gtk[32]; unsigned gtk_len, gtk_idx, gtk_tx;
} wwpa_kdes;
int wwpa_kde_parse(const uint8_t *data, size_t len, wwpa_kdes *out);

// ---- the supplicant side of the 4-way handshake ----
// wwpa_supp_rx results (and s->state): M1 answered; M3 answered and the keys are new (install them); an M3
// retransmission after WWPA_DONE answered with M4 again, keys unchanged (never reinstall them: KRACK, CVE-2017-13077).
enum {WWPA_IDLE = 0, WWPA_GOT_M1, WWPA_DONE, WWPA_M4_RESENT};
typedef struct {
    uint8_t pmk[WWPA_PMK_LEN], aa[6], spa[6], anonce[WWPA_NONCE_LEN], snonce[WWPA_NONCE_LEN];
    const uint8_t *own_rsne; unsigned own_rsne_len;   // our RSN element (sent in M2, the one in the association request)
    const uint8_t *ap_rsne; unsigned ap_rsne_len;     // the AP's from the beacon/probe response; M3's must match (0 = skip)
    wwpa_ptk ptk, tptk;
    uint64_t replay;
    int replay_valid, state;
    uint8_t gtk[32];
    unsigned gtk_len, gtk_idx;
    uint8_t eapol_version;
} wwpa_supp;
void wwpa_supp_init(wwpa_supp *s, const uint8_t pmk[WWPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                    const uint8_t *own_rsne, unsigned own_rsne_len, const uint8_t *ap_rsne, unsigned ap_rsne_len);
// Feed one EAPOL frame from the AP. snonce: a fresh random nonce, used when the frame is M1. On M1 writes M2 to out,
// on M3 writes M4 (the PTK and GTK are then ready: s->ptk, s->gtk) and returns WWPA_DONE; returns WWPA_GOT_M1 after
// M1. *out_len is 0 when nothing is to be sent. A frame that fails a check (replay counter, MIC, ANonce, RSNE, key
// data) is dropped with a negative error and leaves the state unchanged.
int wwpa_supp_rx(wwpa_supp *s, const uint8_t *frame, size_t len, const uint8_t snonce[WWPA_NONCE_LEN], uint8_t *out,
                 size_t cap, size_t *out_len);
// Wipe every key in s.
void wwpa_supp_clear(wwpa_supp *s);

#ifdef __cplusplus
}
#endif
#endif
