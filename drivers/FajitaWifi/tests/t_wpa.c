// SPDX-License-Identifier: BSD-2-Clause-Patent
// WPA2-PSK supplicant core host tests: SHA-1 / HMAC-SHA1 (FIPS 180, RFC 2202), AES (FIPS-197 C.1-C.3), AES key wrap
// (RFC 3394 4.1-4.6), the PSK (IEEE 802.11-2016 J.4), the PRF (J.3), and a full 4-way handshake against the frames of
// an independent reference (tools/wifi/gen-wpa-vectors.py -> wpa_vectors.h): M2 and M4 must match byte for byte.
// Every passphrase here is a published test vector; no real credentials.
#include "tcheck.h"
#include "../WifiWpa.h"
#include "wpa_vectors.h"
#include <string.h>

static int hex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; ++k) {
            const char c = h[k];
            v = v << 4 | (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10);
        }
        out[n++] = (uint8_t)v;
    }
    return (int)n;
}
static int eqhex(const uint8_t *b, size_t n, const char *h)
{
    uint8_t want[128];
    return hex(h, want, sizeof want) == (int)n && !memcmp(b, want, n);
}

static void t_wpa_sha1_hmac(void)
{
    uint8_t d[20];
    wwpa_sha1("abc", 3, d);
    CHECK(eqhex(d, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    wwpa_sha1("", 0, d);
    CHECK(eqhex(d, 20, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    wwpa_sha1(m, strlen(m), d);
    CHECK(eqhex(d, 20, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    wwpa_sha1_ctx c;   // a million 'a', in uneven pieces
    wwpa_sha1_init(&c);
    uint8_t a[997];
    memset(a, 'a', sizeof a);
    size_t left = 1000000;
    while (left) {const size_t k = left < sizeof a ? left : sizeof a; wwpa_sha1_update(&c, a, k); left -= k;}
    wwpa_sha1_final(&c, d);
    CHECK(eqhex(d, 20, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
    // RFC 2202 test cases 1, 2 and 6 (a key longer than the block).
    uint8_t key[80];
    memset(key, 0x0b, 20);
    const uint8_t *v[1]; size_t vl[1];
    v[0] = (const uint8_t *)"Hi There"; vl[0] = 8;
    wwpa_hmac_sha1(key, 20, 1, v, vl, d);
    CHECK(eqhex(d, 20, "b617318655057264e28bc0b6fb378c8ef146be00"));
    v[0] = (const uint8_t *)"what do ya want for nothing?"; vl[0] = 28;
    wwpa_hmac_sha1((const uint8_t *)"Jefe", 4, 1, v, vl, d);
    CHECK(eqhex(d, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    memset(key, 0xaa, 80);
    v[0] = (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First"; vl[0] = 54;
    wwpa_hmac_sha1(key, 80, 1, v, vl, d);
    CHECK(eqhex(d, 20, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
    // Split input == joined input.
    const uint8_t *v2[2] = {(const uint8_t *)"what do ya ", (const uint8_t *)"want for nothing?"};
    const size_t vl2[2] = {11, 17};
    wwpa_hmac_sha1((const uint8_t *)"Jefe", 4, 2, v2, vl2, d);
    CHECK(eqhex(d, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
}

static void t_wpa_aes(void)
{
    uint8_t key[32], pt[16], ct[16], back[16];
    hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    hex("00112233445566778899aabbccddeeff", pt, 16);
    static const struct {size_t kl; const char *ct;} fips[] = {
        {16, "69c4e0d86a7b0430d8cdb78070b4c55a"}, {24, "dda97ca4864cdfe06eaf70a0ec0d7191"}, {32, "8ea2b7ca516745bfeafc49904b496089"}};
    for (unsigned i = 0; i < 3; ++i) {
        wwpa_aes a;
        CHECK(wwpa_aes_init(&a, key, fips[i].kl) == 0);
        wwpa_aes_encrypt(&a, pt, ct);
        CHECK(eqhex(ct, 16, fips[i].ct));
        wwpa_aes_decrypt(&a, ct, back);
        CHECK(!memcmp(back, pt, 16));
    }
    wwpa_aes a;
    CHECK(wwpa_aes_init(&a, key, 20) == WWPA_ERR_INVAL);
}

static void t_wpa_keywrap(void)
{
    uint8_t kek[32], data[32], out[40], back[32];
    hex("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", kek, 32);
    hex("00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F", data, 32);
    static const struct {size_t kl, dl; const char *ct;} rfc[] = {   // RFC 3394 4.1 .. 4.6
        {16, 16, "1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5"},
        {24, 16, "96778B25AE6CA435F92B5B97C050AED2468AB8A17AD84E5D"},
        {32, 16, "64E8C3F9CE0F5BA263E9777905818A2A93C8191E7D6E8AE7"},
        {24, 24, "031D33264E15D33268F24EC260743EDCE1C6C7DDEE725A936BA814915C6762D2"},
        {32, 24, "A8F9BC1612C68B3FF6E6F4FBE30E71E4769C8B80A32CB8958CD5D17D6B254DA1"},
        {32, 32, "28C9F404C4B810F4CBCCB35CFB87F8263F5786E2D80ED326CBC7F0E71A99F43BFB988B9B7A02DD21"}};
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(wwpa_aes_wrap(kek, rfc[i].kl, data, rfc[i].dl, out) == 0);
        uint8_t want[40];
        CHECK(hex(rfc[i].ct, want, sizeof want) == (int)rfc[i].dl + 8 && !memcmp(out, want, rfc[i].dl + 8));
        CHECK(wwpa_aes_unwrap(kek, rfc[i].kl, out, rfc[i].dl + 8, back) == 0 && !memcmp(back, data, rfc[i].dl));
        out[3] ^= 1;   // a flipped bit fails the integrity check, and the output is wiped
        CHECK(wwpa_aes_unwrap(kek, rfc[i].kl, out, rfc[i].dl + 8, back) == WWPA_ERR_UNWRAP);
        uint8_t z[32] = {0};
        CHECK(!memcmp(back, z, rfc[i].dl));
    }
    CHECK(wwpa_aes_wrap(kek, 16, data, 12, out) == WWPA_ERR_INVAL);
    CHECK(wwpa_aes_wrap(kek, 16, data, 8, out) == WWPA_ERR_INVAL);
    CHECK(wwpa_aes_unwrap(kek, 16, out, 16, back) == WWPA_ERR_INVAL);
    // In place.
    uint8_t buf[24];
    memcpy(buf, data, 16);
    CHECK(wwpa_aes_wrap(kek, 16, buf, 16, buf) == 0 && eqhex(buf, 24, rfc[0].ct));
}

static void t_wpa_psk_prf(void)
{
    uint8_t pmk[32];
    // IEEE 802.11-2016 J.4.2, tests 1-3.
    CHECK(wwpa_psk("password", 8, (const uint8_t *)"IEEE", 4, pmk) == 0);
    CHECK(eqhex(pmk, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    CHECK(wwpa_psk("ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, pmk) == 0);
    CHECK(eqhex(pmk, 32, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));
    CHECK(!memcmp(pmk, V_PMK, 32));
    CHECK(wwpa_psk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32, (const uint8_t *)"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", 32, pmk) == 0);
    CHECK(eqhex(pmk, 32, "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62"));
    CHECK(wwpa_psk("short", 5, (const uint8_t *)"x", 1, pmk) == WWPA_ERR_INVAL);
    CHECK(wwpa_psk("0123456789012345678901234567890123456789012345678901234567890123", 64, (const uint8_t *)"x", 1, pmk) == WWPA_ERR_INVAL);
    CHECK(wwpa_psk("bad\x01pass", 9, (const uint8_t *)"x", 1, pmk) == WWPA_ERR_INVAL);
    CHECK(wwpa_psk("password", 8, (const uint8_t *)"x", 33, pmk) == WWPA_ERR_INVAL);
    // PRF-512 (J.3 test 1; tests 2-3 cross-checked with Python hmac).
    uint8_t key[80], out[64];
    memset(key, 0x0b, 20);
    wwpa_prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 64);
    CHECK(eqhex(out, 64, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"));
    wwpa_prf((const uint8_t *)"Jefe", 4, "prefix-2", (const uint8_t *)"what do ya want for nothing?", 28, out, 64);
    CHECK(eqhex(out, 64, "47c4908e30c947521ad20be9053450ecbea23d3aa604b77326d8b3825ff7475c06f51fb9c5313d1e9f90d897d134b72e090fc23150bc8414382043418678e700"));
    memset(key, 0xaa, 80);
    wwpa_prf(key, 80, "prefix-4", (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, out, 64);
    CHECK(eqhex(out, 64, "24a935455fc470dd2a9b5a2715c61be8ff9b5837de5b440f7d4f538c387731c0b5e8e3fb0154c18a3fbe459fff3b7c461aa0680b2a147258006cbf2462f84672"));
    uint8_t shortout[20];   // a truncated PRF is the prefix of the longer one
    memset(key, 0x0b, 20);
    wwpa_prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, shortout, 20);
    CHECK(eqhex(shortout, 20, "bcd4c650b30b9684951829e0d75f9d54b862175e"));
    // PTK: the reference's KCK/KEK/TK; the min/max ordering makes the derivation symmetric.
    wwpa_ptk p, q;
    CHECK(wwpa_ptk_derive(V_PMK, V_AA, V_SPA, V_ANONCE, V_SNONCE, 16, &p) == 0);
    CHECK(!memcmp(p.kck, V_KCK, 16) && !memcmp(p.kek, V_KEK, 16) && !memcmp(p.tk, V_TK, 16) && p.tk_len == 16);
    CHECK(wwpa_ptk_derive(V_PMK, V_SPA, V_AA, V_SNONCE, V_ANONCE, 16, &q) == 0 && !memcmp(&p, &q, sizeof p));
    CHECK(wwpa_ptk_derive(V_PMK, V_AA, V_SPA, V_ANONCE, V_SNONCE, 5, &q) == WWPA_ERR_INVAL);
}

static void t_wpa_frames(void)
{
    wwpa_key k;
    CHECK(wwpa_key_parse(V_M1, sizeof V_M1, &k) == 0);
    CHECK(k.eapol_version == 2 && k.info == (WWPA_KI_VER_HMAC_SHA1_AES | WWPA_KI_PAIRWISE | WWPA_KI_ACK) && k.key_len == 16);
    CHECK(k.replay == 1 && !memcmp(k.nonce, V_ANONCE, 32) && k.data_len == 0);
    CHECK(wwpa_key_parse(V_M3, sizeof V_M3, &k) == 0 && k.replay == 2 && k.data_len == sizeof V_M3 - 99);
    // Rebuilding a parsed frame gives the same bytes.
    uint8_t buf[256];
    CHECK(wwpa_key_build(&k, buf, sizeof buf) == (long)sizeof V_M3 && !memcmp(buf, V_M3, sizeof V_M3));
    CHECK(wwpa_key_build(&k, buf, 100) == WWPA_ERR_NOSPC);
    CHECK(wwpa_key_verify(V_KCK, V_M3, sizeof V_M3) == 0 && wwpa_key_verify(V_KCK, V_M2, sizeof V_M2) == 0);
    memcpy(buf, V_M3, sizeof V_M3);
    buf[120] ^= 0x40;
    CHECK(wwpa_key_verify(V_KCK, buf, sizeof V_M3) == WWPA_ERR_MIC);
    // Not EAPOL-Key / wrong descriptor / truncated / inconsistent lengths. Trailing padding past the body is ignored.
    memcpy(buf, V_M1, sizeof V_M1);
    buf[1] = 0;
    CHECK(wwpa_key_parse(buf, sizeof V_M1, &k) == WWPA_ERR_PROTO);
    buf[1] = 3; buf[4] = 254;
    CHECK(wwpa_key_parse(buf, sizeof V_M1, &k) == WWPA_ERR_PROTO);
    buf[4] = 2;
    CHECK(wwpa_key_parse(buf, sizeof V_M1 - 1, &k) == WWPA_ERR_INVAL);
    buf[98] = 1;   // key data length 1 with no data
    CHECK(wwpa_key_parse(buf, sizeof V_M1, &k) == WWPA_ERR_INVAL);
    buf[98] = 0;
    memset(buf + sizeof V_M1, 0, 20);
    CHECK(wwpa_key_parse(buf, sizeof V_M1 + 20, &k) == 0);
    // KDEs: the reference's M3 key data.
    uint8_t plain[128];
    CHECK(wwpa_key_parse(V_M3, sizeof V_M3, &k) == 0);
    CHECK(wwpa_aes_unwrap(V_KEK, 16, k.data, k.data_len, plain) == 0);
    wwpa_kdes kd;
    CHECK(wwpa_kde_parse(plain, k.data_len - 8u, &kd) == 0);
    CHECK(kd.rsne_len == sizeof V_RSNE && !memcmp(kd.rsne, V_RSNE, sizeof V_RSNE));
    CHECK(kd.gtk_len == 16 && !memcmp(kd.gtk, V_GTK, 16) && kd.gtk_idx == 1 && kd.gtk_tx == 0);
    const uint8_t bad[] = {0x30, 0x40, 0x01};   // an element running past the data
    CHECK(wwpa_kde_parse(bad, sizeof bad, &kd) == WWPA_ERR_PROTO);
}

static void t_wpa_handshake(void)
{
    wwpa_supp s;
    uint8_t out[256];
    size_t n = 0;
    wwpa_supp_init(&s, V_PMK, V_AA, V_SPA, V_RSNE, sizeof V_RSNE, V_RSNE, sizeof V_RSNE);
    // M3 before M1: refused.
    CHECK(wwpa_supp_rx(&s, V_M3, sizeof V_M3, V_SNONCE, out, sizeof out, &n) == WWPA_ERR_STATE && n == 0);
    // M1 -> M2, byte for byte the reference's.
    CHECK(wwpa_supp_rx(&s, V_M1, sizeof V_M1, V_SNONCE, out, sizeof out, &n) == WWPA_GOT_M1);
    CHECK(n == sizeof V_M2 && !memcmp(out, V_M2, sizeof V_M2));
    // The same M1 again (same replay counter): dropped.
    CHECK(wwpa_supp_rx(&s, V_M1, sizeof V_M1, V_SNONCE, out, sizeof out, &n) == WWPA_ERR_REPLAY && n == 0);
    // A tampered M3 (MIC), an M3 with another ANonce, and an M3 replayed with an old counter are dropped.
    uint8_t m3[sizeof V_M3];
    memcpy(m3, V_M3, sizeof m3);
    m3[sizeof m3 - 1] ^= 1;
    CHECK(wwpa_supp_rx(&s, m3, sizeof m3, NULL, out, sizeof out, &n) == WWPA_ERR_MIC);
    memcpy(m3, V_M3, sizeof m3);
    m3[17] ^= 1;
    wwpa_key_sign(V_KCK, m3, sizeof m3);
    CHECK(wwpa_supp_rx(&s, m3, sizeof m3, NULL, out, sizeof out, &n) == WWPA_ERR_PROTO);
    memcpy(m3, V_M3, sizeof m3);
    m3[16] = 1;   // replay counter 1 == M1's
    wwpa_key_sign(V_KCK, m3, sizeof m3);
    CHECK(wwpa_supp_rx(&s, m3, sizeof m3, NULL, out, sizeof out, &n) == WWPA_ERR_REPLAY);
    CHECK(s.state == WWPA_GOT_M1 && s.gtk_len == 0);
    // M3 -> M4 (the reference's bytes); PTK and GTK installed.
    CHECK(wwpa_supp_rx(&s, V_M3, sizeof V_M3, NULL, out, sizeof out, &n) == WWPA_DONE);
    CHECK(n == sizeof V_M4 && !memcmp(out, V_M4, sizeof V_M4));
    CHECK(!memcmp(s.ptk.tk, V_TK, 16) && !memcmp(s.ptk.kck, V_KCK, 16) && s.gtk_len == 16 && !memcmp(s.gtk, V_GTK, 16)
          && s.gtk_idx == 1);
    // The same M3 again: replay. A retransmitted M3 (counter 3): M4 again, keys not reinstalled.
    CHECK(wwpa_supp_rx(&s, V_M3, sizeof V_M3, NULL, out, sizeof out, &n) == WWPA_ERR_REPLAY);
    memcpy(m3, V_M3, sizeof m3);
    m3[16] = 3;
    wwpa_key_sign(V_KCK, m3, sizeof m3);
    CHECK(wwpa_supp_rx(&s, m3, sizeof m3, NULL, out, sizeof out, &n) == WWPA_M4_RESENT && n == sizeof V_M4 && out[16] == 3);
    CHECK(s.state == WWPA_DONE);
    wwpa_supp_clear(&s);
    CHECK(s.gtk_len == 0 && s.ptk.tk[0] == 0);
    // An RSNE in M3 that differs from the beacon's (a downgrade): refused before M4.
    uint8_t other[sizeof V_RSNE];
    memcpy(other, V_RSNE, sizeof other);
    other[sizeof other - 3] = 0x01;   // AKM 00-0f-ac:1 (802.1X) instead of PSK
    wwpa_supp_init(&s, V_PMK, V_AA, V_SPA, V_RSNE, sizeof V_RSNE, other, sizeof other);
    CHECK(wwpa_supp_rx(&s, V_M1, sizeof V_M1, V_SNONCE, out, sizeof out, &n) == WWPA_GOT_M1);
    CHECK(wwpa_supp_rx(&s, V_M3, sizeof V_M3, NULL, out, sizeof out, &n) == WWPA_ERR_PROTO && n == 0 && s.state == WWPA_GOT_M1);
    // A wrong PMK (wrong passphrase): M2 goes out, M3's MIC fails.
    uint8_t pmk[32];
    CHECK(wwpa_psk("not-the-passphrase", 18, (const uint8_t *)"ThisIsASSID", 11, pmk) == 0);
    wwpa_supp_init(&s, pmk, V_AA, V_SPA, V_RSNE, sizeof V_RSNE, V_RSNE, sizeof V_RSNE);
    CHECK(wwpa_supp_rx(&s, V_M1, sizeof V_M1, V_SNONCE, out, sizeof out, &n) == WWPA_GOT_M1);
    CHECK(wwpa_supp_rx(&s, V_M3, sizeof V_M3, NULL, out, sizeof out, &n) == WWPA_ERR_MIC);
    // Key descriptor version 1 (HMAC-MD5/RC4, TKIP) is not supported.
    uint8_t m1[sizeof V_M1];
    memcpy(m1, V_M1, sizeof m1);
    m1[6] = (uint8_t)((m1[6] & ~7) | 1);
    wwpa_supp_init(&s, V_PMK, V_AA, V_SPA, V_RSNE, sizeof V_RSNE, 0, 0);
    CHECK(wwpa_supp_rx(&s, m1, sizeof m1, V_SNONCE, out, sizeof out, &n) == WWPA_ERR_PROTO);
    // No room for M2.
    CHECK(wwpa_supp_rx(&s, V_M1, sizeof V_M1, V_SNONCE, out, 50, &n) == WWPA_ERR_NOSPC && s.state == WWPA_IDLE);
    wwpa_supp_clear(&s);
}

// WPA3-Personal (AKM SAE): key descriptor version 0, KDF-SHA-256 PTK, AES-CMAC MIC, an IGTK KDE in M3 (ignored: BIP is not
// offloaded); M2 and M4 byte for byte the Python reference's.
static void t_wpa_handshake_sae(void)
{
    wwpa_supp s;
    uint8_t out[256];
    size_t n = 0;
    wwpa_supp_init(&s, V_SAE_PMK, V_AA, V_SPA, V_SAE_RSNE, sizeof V_SAE_RSNE, V_SAE_RSNE, sizeof V_SAE_RSNE);
    // without the AKM set, the version-0 M1 is refused (version 2 expected)
    CHECK(wwpa_supp_rx(&s, V_SAE_M1, sizeof V_SAE_M1, V_SNONCE, out, sizeof out, &n) == WWPA_ERR_PROTO);
    wwpa_supp_set_akm(&s, WWPA_AKM_SAE);
    CHECK(wwpa_supp_rx(&s, V_SAE_M1, sizeof V_SAE_M1, V_SNONCE, out, sizeof out, &n) == WWPA_GOT_M1);
    CHECK(n == sizeof V_SAE_M2 && !memcmp(out, V_SAE_M2, sizeof V_SAE_M2));
    CHECK(wwpa_supp_rx(&s, V_SAE_M3, sizeof V_SAE_M3, NULL, out, sizeof out, &n) == WWPA_DONE);
    CHECK(n == sizeof V_SAE_M4 && !memcmp(out, V_SAE_M4, sizeof V_SAE_M4));
    CHECK(!memcmp(s.ptk.tk, V_SAE_TK, 16) && !memcmp(s.ptk.kck, V_SAE_KCK, 16) && s.gtk_len == 16 && !memcmp(s.gtk, V_GTK, 16));
    // a PSK-style (HMAC-SHA1) MIC on an SAE association is refused
    uint8_t m3[sizeof V_SAE_M3];
    memcpy(m3, V_SAE_M3, sizeof m3);
    wwpa_supp_init(&s, V_SAE_PMK, V_AA, V_SPA, V_SAE_RSNE, sizeof V_SAE_RSNE, V_SAE_RSNE, sizeof V_SAE_RSNE);
    wwpa_supp_set_akm(&s, WWPA_AKM_SAE);
    CHECK(wwpa_supp_rx(&s, V_SAE_M1, sizeof V_SAE_M1, V_SNONCE, out, sizeof out, &n) == WWPA_GOT_M1);
    wwpa_key_sign(V_SAE_KCK, m3, sizeof m3);
    CHECK(wwpa_supp_rx(&s, m3, sizeof m3, NULL, out, sizeof out, &n) == WWPA_ERR_MIC);
    wwpa_supp_clear(&s);
}

void t_wpa(void)
{
    RUN(t_wpa_sha1_hmac);
    RUN(t_wpa_aes);
    RUN(t_wpa_keywrap);
    RUN(t_wpa_psk_prf);
    RUN(t_wpa_frames);
    RUN(t_wpa_handshake);
    RUN(t_wpa_handshake_sae);
}
