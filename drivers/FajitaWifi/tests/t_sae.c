// SPDX-License-Identifier: BSD-2-Clause-Patent
// WifiSae: SHA-256 / HMAC (FIPS 180-4, RFC 4231), AES-CMAC (RFC 4493), P-256 scalar multiplication (values from an
// independent affine implementation), the SAE test vector of IEEE 802.11-2020 Annex J.10 (as in hostap's
// common_module_tests.c: commit, KCK, PMK, PMKID), a full exchange between two instances, and the commit validation.
#include "tcheck.h"
#include "../WifiSae.h"
#include <string.h>

static void hex(uint8_t *out, const char *h) {for (size_t i = 0; h[2 * i]; ++i) {unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v;}}
static uint32_t g_rng_state = 1;
static void rng(void *c, uint8_t *o, uint32_t n)
{
    (void)c;
    for (uint32_t i = 0; i < n; ++i) {g_rng_state = g_rng_state * 1103515245u + 12345u; o[i] = (uint8_t)(g_rng_state >> 16);}
}

static void t_sae_hashes(void)
{
    uint8_t d[32], want[32];
    wsae_sha256("abc", 3, d);
    hex(want, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(!memcmp(d, want, 32));
    static const char m448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    wsae_sha256(m448, strlen(m448), d);
    hex(want, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(!memcmp(d, want, 32));
    uint8_t key[20];
    memset(key, 0x0b, 20);
    const uint8_t *v[1] = {(const uint8_t *)"Hi There"};
    const size_t vl[1] = {8};
    wsae_hmac_sha256(key, 20, 1, v, vl, d);
    hex(want, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(!memcmp(d, want, 32));
    // RFC 4493
    uint8_t k[16], m[64], mac[16], w[16];
    hex(k, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(m, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    const uint8_t *mv[2] = {m, m + 7};
    size_t ml[2] = {0, 0};
    wsae_aes_cmac(k, 1, mv, ml, mac);
    hex(w, "bb1d6929e95937287fa37d129b756746");
    CHECK(!memcmp(mac, w, 16));
    ml[0] = 16;
    wsae_aes_cmac(k, 1, mv, ml, mac);
    hex(w, "070a16b46b4d4144f79bdd9dd04a287c");
    CHECK(!memcmp(mac, w, 16));
    ml[0] = 7; ml[1] = 33;   // 40 bytes, split
    wsae_aes_cmac(k, 2, mv, ml, mac);
    hex(w, "dfa66747de9ae63030ca32611497c827");
    CHECK(!memcmp(mac, w, 16));
    ml[0] = 64;
    wsae_aes_cmac(k, 1, mv, ml, mac);
    hex(w, "51f0bebf7e3b9d92fc49741779363cfe");
    CHECK(!memcmp(mac, w, 16));
}

static void t_sae_p256(void)
{
    uint8_t gx[32], gy[32], k[32], x[32], y[32], wx[32], wy[32];
    hex(gx, "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
    hex(gy, "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    hex(k, "0123456789abcdeffedcba98765432100f1e2d3c4b5a69788796a5b4c3d2e1f0");
    CHECK(wsae_p256_mul(gx, gy, k, x, y) == 0);
    hex(wx, "bab47346ed163f061ac6d2191c3deea7bd5070ca32f929656837d6ba05d64b27");
    hex(wy, "c14cf0f1f6041d29ee085d4a6a2b9df4665a45cdb50ec7dbf89f6130b49c8aed");
    CHECK(!memcmp(x, wx, 32) && !memcmp(y, wy, 32));
    hex(k, "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550");   // n - 1: -G
    CHECK(wsae_p256_mul(gx, gy, k, x, y) == 0);
    hex(wy, "b01cbd1c01e58065711814b583f061e9d431cca994cea1313449bf97c840ae0a");
    CHECK(!memcmp(x, gx, 32) && !memcmp(y, wy, 32));
    hex(k, "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");   // n: infinity
    CHECK(wsae_p256_mul(gx, gy, k, x, y) != 0);
    gy[31] ^= 1;   // not on the curve
    CHECK(wsae_p256_mul(gx, gy, k, x, y) != 0);
}

// IEEE 802.11-2020 Annex J.10 (hostap src/common/common_module_tests.c sae_tests)
static void t_sae_vector(void)
{
    const uint8_t a1[6] = {0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87}, a2[6] = {0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c};
    uint8_t rnd[32], mask[32], commit[98], peer[98], kck[32], pmk[32], pmkid[16];
    hex(rnd, "992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94");
    hex(mask, "9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322");
    hex(commit, "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c65d5ad9e00829707aa36ba8b859738fc961d08243505f47c035376d7ac4bc8"
                "d7b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325dbe1");
    hex(peer, "1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b5223e71b9bb048d3873f20556953a96c91536fd8ee6ca9b4a68a148b056a909b"
              "e03e83ae208f60f8ef5537858074db06687032399862999b511e0a1552a5fea317c2");
    hex(kck, "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a");
    hex(pmk, "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59");
    hex(pmkid, "8747a600eea3f9f22475df58ca1e5498");
    static const char pw[] = "mekmitasdigoat";
    wsae s;
    CHECK(wsae_init(&s, a1, a2, (const uint8_t *)pw, strlen(pw), rng, NULL) == 0);
    CHECK(s.pwe_iterations >= WSAE_MIN_ITER);   // the loop runs at least 40 times whatever iteration finds the point
    CHECK(wsae_set_commit(&s, rnd, mask) == 0);
    uint8_t out[200];
    CHECK(wsae_write_commit(&s, NULL, 0, out, sizeof out) == 98 && !memcmp(out, commit, 98));
    CHECK(wsae_process_commit(&s, peer, sizeof peer) == 0);
    CHECK(!memcmp(s.kck, kck, 32) && !memcmp(s.pmk, pmk, 32) && !memcmp(s.pmkid, pmkid, 16));
    // the PWE does not depend on the blinding randomness
    wsae s2;
    g_rng_state = 777;
    CHECK(wsae_init(&s2, a1, a2, (const uint8_t *)pw, strlen(pw), rng, NULL) == 0 && wsae_set_commit(&s2, rnd, mask) == 0);
    CHECK(wsae_write_commit(&s2, NULL, 0, out, sizeof out) == 98 && !memcmp(out, commit, 98));
    // the roles swapped (addresses in the other order) give the same PWE
    wsae s3;
    CHECK(wsae_init(&s3, a2, a1, (const uint8_t *)pw, strlen(pw), rng, NULL) == 0 && !memcmp(s3.pwe.x, s.pwe.x, 32));
    // an anti-clogging token sits between the group and the scalar
    const uint8_t tok[5] = {1, 2, 3, 4, 5};
    CHECK(wsae_write_commit(&s, tok, 5, out, sizeof out) == 103 && out[0] == 19 && !memcmp(out + 2, tok, 5) && !memcmp(out + 7, commit + 2, 96));
}

static void t_sae_exchange(void)
{
    const uint8_t sta[6] = {0x02, 0xfa, 0x6a, 0x17, 0x4e, 0x03}, ap[6] = {0xfa, 0xd0, 0x0e, 0xa8, 0x9f, 0x89};
    static const char pw[] = "correct horse battery";
    wsae a, b;
    g_rng_state = 4242;
    CHECK(wsae_init(&a, sta, ap, (const uint8_t *)pw, strlen(pw), rng, NULL) == 0);
    CHECK(wsae_init(&b, ap, sta, (const uint8_t *)pw, strlen(pw), rng, NULL) == 0);
    uint8_t ca[128], cb[128], fa[64], fb[64];
    const long la = wsae_write_commit(&a, NULL, 0, ca, sizeof ca), lb = wsae_write_commit(&b, NULL, 0, cb, sizeof cb);
    CHECK(la == 98 && lb == 98);
    CHECK(wsae_process_commit(&a, cb, (size_t)lb) == 0 && wsae_process_commit(&b, ca, (size_t)la) == 0);
    CHECK(!memcmp(a.pmk, b.pmk, 32) && !memcmp(a.kck, b.kck, 32) && !memcmp(a.pmkid, b.pmkid, 16));
    CHECK(wsae_write_confirm(&a, fa, sizeof fa) == 34 && wsae_write_confirm(&b, fb, sizeof fb) == 34);
    CHECK(fa[0] == 1 && fa[1] == 0);
    CHECK(wsae_check_confirm(&a, fb, 34) == 0 && wsae_check_confirm(&b, fa, 34) == 0);
    fb[10] ^= 1;
    CHECK(wsae_check_confirm(&a, fb, 34) == WSAE_ERR_CONFIRM);
    // a wrong password: the commits process, the confirm does not verify
    wsae c;
    CHECK(wsae_init(&c, ap, sta, (const uint8_t *)"wrong password", 14, rng, NULL) == 0);
    uint8_t cc[128], fc[64];
    const long lc = wsae_write_commit(&c, NULL, 0, cc, sizeof cc);
    wsae a2;
    CHECK(wsae_init(&a2, sta, ap, (const uint8_t *)pw, strlen(pw), rng, NULL) == 0);
    uint8_t ca2[128];
    const long la2 = wsae_write_commit(&a2, NULL, 0, ca2, sizeof ca2);
    CHECK(wsae_process_commit(&a2, cc, (size_t)lc) == 0 && wsae_process_commit(&c, ca2, (size_t)la2) == 0);
    CHECK(wsae_write_confirm(&c, fc, sizeof fc) == 34 && wsae_check_confirm(&a2, fc, 34) == WSAE_ERR_CONFIRM);
}

static void t_sae_validation(void)
{
    const uint8_t sta[6] = {2, 1, 1, 1, 1, 1}, ap[6] = {4, 2, 2, 2, 2, 2};
    wsae a, b;
    g_rng_state = 99;
    CHECK(wsae_init(&a, sta, ap, (const uint8_t *)"password", 8, rng, NULL) == 0);
    CHECK(wsae_init(&b, ap, sta, (const uint8_t *)"password", 8, rng, NULL) == 0);
    uint8_t cb[128], bad[128];
    CHECK(wsae_write_commit(&b, NULL, 0, cb, sizeof cb) == 98);
    memcpy(bad, cb, 98); bad[0] = 20;
    CHECK(wsae_process_commit(&a, bad, 98) == WSAE_ERR_GROUP);
    CHECK(wsae_process_commit(&a, cb, 97) == WSAE_ERR_PEER);
    memcpy(bad, cb, 98); memset(bad + 2, 0, 32);
    CHECK(wsae_process_commit(&a, bad, 98) == WSAE_ERR_PEER);   // scalar 0
    bad[33] = 1;
    CHECK(wsae_process_commit(&a, bad, 98) == WSAE_ERR_PEER);   // scalar 1
    hex(bad + 2, "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    CHECK(wsae_process_commit(&a, bad, 98) == WSAE_ERR_PEER);   // scalar r
    memcpy(bad, cb, 98); bad[97] ^= 1;
    CHECK(wsae_process_commit(&a, bad, 98) == WSAE_ERR_PEER);   // element off the curve
    memcpy(bad, cb, 98); memset(bad + 34, 0xff, 32);
    CHECK(wsae_process_commit(&a, bad, 98) == WSAE_ERR_PEER);   // x >= p
    uint8_t ca[128];
    CHECK(wsae_write_commit(&a, NULL, 0, ca, sizeof ca) == 98);
    CHECK(wsae_process_commit(&a, ca, 98) == WSAE_ERR_PEER);    // our own commit reflected
    CHECK(wsae_process_commit(&a, cb, 98) == 0 && a.have_keys);
    uint8_t buf[8];
    CHECK(wsae_write_confirm(&a, buf, sizeof buf) == WSAE_ERR_INVAL);   // too small
    wsae_clear(&a);
    CHECK(!a.have_keys && a.pmk[0] == 0);
}

void t_sae(void)
{
    RUN(t_sae_hashes);
    RUN(t_sae_p256);
    RUN(t_sae_vector);
    RUN(t_sae_exchange);
    RUN(t_sae_validation);
}
