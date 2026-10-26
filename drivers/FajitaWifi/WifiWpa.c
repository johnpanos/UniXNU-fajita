// SPDX-License-Identifier: BSD-2-Clause-Patent
// WPA2-PSK supplicant core: SHA-1/HMAC, AES + key wrap, PBKDF2 PSK, PRF/PTK, EAPOL-Key, the 4-way handshake. See WifiWpa.h.
#include "WifiWpa.h"
#include "WifiSae.h"

static void cpy(uint8_t *d, const uint8_t *s, size_t n) {for (size_t i = 0; i < n; ++i) d[i] = s[i];}
static void wipe(void *p, size_t n) {volatile uint8_t *b = (volatile uint8_t *)p; for (size_t i = 0; i < n; ++i) b[i] = 0;}
static int cmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
static uint16_t be16(const uint8_t *p) {return (uint16_t)(p[0] << 8 | p[1]);}
static void put_be16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;}
static uint64_t be64(const uint8_t *p) {uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v = v << 8 | p[i]; return v;}
static void put_be64(uint8_t *p, uint64_t v) {for (unsigned i = 0; i < 8; ++i) p[7 - i] = (uint8_t)(v >> (8 * i));}

// ---- SHA-1 (FIPS 180-4) ----
static uint32_t rol(uint32_t x, unsigned n) {return x << n | x >> (32 - n);}
static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {f = (b & c) | (~b & d); k = 0x5a827999u;}
        else if (i < 40) {f = b ^ c ^ d; k = 0x6ed9eba1u;}
        else if (i < 60) {f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu;}
        else {f = b ^ c ^ d; k = 0xca62c1d6u;}
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    wipe(w, sizeof w);
}

void wwpa_sha1_init(wwpa_sha1_ctx *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xefcdab89u; c->h[2] = 0x98badcfeu; c->h[3] = 0x10325476u; c->h[4] = 0xc3d2e1f0u;
    c->bytes = 0;
    c->fill = 0;
}

void wwpa_sha1_update(wwpa_sha1_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->bytes += n;
    while (n) {
        const size_t k = 64 - c->fill < n ? 64 - c->fill : n;
        cpy(c->buf + c->fill, p, k);
        c->fill += (unsigned)k; p += k; n -= k;
        if (c->fill == 64) {sha1_block(c->h, c->buf); c->fill = 0;}
    }
}

void wwpa_sha1_final(wwpa_sha1_ctx *c, uint8_t out[WWPA_SHA1_LEN])
{
    const uint64_t bits = c->bytes * 8;
    uint8_t pad[72] = {0x80};
    const unsigned padn = c->fill < 56 ? 56 - c->fill : 120 - c->fill;
    put_be64(pad + padn, bits);
    wwpa_sha1_update(c, pad, padn + 8);
    for (unsigned i = 0; i < 5; ++i) {out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
                                      out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];}
    wipe(c, sizeof *c);
}

void wwpa_sha1(const void *data, size_t n, uint8_t out[WWPA_SHA1_LEN])
{
    wwpa_sha1_ctx c;
    wwpa_sha1_init(&c);
    wwpa_sha1_update(&c, data, n);
    wwpa_sha1_final(&c, out);
}

void wwpa_hmac_sha1(const uint8_t *key, size_t key_len, unsigned nv, const uint8_t *const *v, const size_t *vlen,
                    uint8_t out[WWPA_SHA1_LEN])
{
    uint8_t k[64] = {0}, pad[64], inner[WWPA_SHA1_LEN];
    if (key_len > 64) wwpa_sha1(key, key_len, k);
    else cpy(k, key, key_len);
    wwpa_sha1_ctx c;
    for (unsigned i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    wwpa_sha1_init(&c);
    wwpa_sha1_update(&c, pad, 64);
    for (unsigned i = 0; i < nv; ++i) wwpa_sha1_update(&c, v[i], vlen[i]);
    wwpa_sha1_final(&c, inner);
    for (unsigned i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    wwpa_sha1_init(&c);
    wwpa_sha1_update(&c, pad, 64);
    wwpa_sha1_update(&c, inner, sizeof inner);
    wwpa_sha1_final(&c, out);
    wipe(k, sizeof k); wipe(pad, sizeof pad); wipe(inner, sizeof inner);
}

// ---- AES (FIPS-197) ----
static const uint8_t SBOX[256] = {
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
static const uint8_t INV_SBOX[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};


static uint8_t xt(uint8_t x) {return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1b : 0));}
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {if (b & 1) r ^= a; a = xt(a); b >>= 1;}
    return r;
}
static uint32_t sub_word(uint32_t w)
{
    return (uint32_t)SBOX[w >> 24] << 24 | (uint32_t)SBOX[(w >> 16) & 0xff] << 16 | (uint32_t)SBOX[(w >> 8) & 0xff] << 8 | SBOX[w & 0xff];
}

int wwpa_aes_init(wwpa_aes *a, const uint8_t *key, size_t key_len)
{
    if (key_len != 16 && key_len != 24 && key_len != 32) return WWPA_ERR_INVAL;
    const unsigned nk = (unsigned)key_len / 4;
    a->rounds = nk + 6;
    const unsigned total = 4 * (a->rounds + 1);
    for (unsigned i = 0; i < nk; ++i)
        a->rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    uint8_t rcon = 1;
    for (unsigned i = nk; i < total; ++i) {
        uint32_t t = a->rk[i - 1];
        if (i % nk == 0) {t = sub_word(t << 8 | t >> 24) ^ (uint32_t)rcon << 24; rcon = xt(rcon);}
        else if (nk > 6 && i % nk == 4) t = sub_word(t);
        a->rk[i] = a->rk[i - nk] ^ t;
    }
    return WWPA_OK;
}

static void add_rk(uint8_t s[16], const uint32_t *rk)
{
    for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r) s[4 * c + r] ^= (uint8_t)(rk[c] >> (24 - 8 * r));
}

void wwpa_aes_encrypt(const wwpa_aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    cpy(s, in, 16);
    add_rk(s, a->rk);
    for (unsigned round = 1; round <= a->rounds; ++round) {
        for (unsigned i = 0; i < 16; ++i) s[i] = SBOX[s[i]];
        for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r) t[4 * c + r] = s[4 * ((c + r) & 3) + r];   // ShiftRows
        if (round != a->rounds) {
            for (unsigned c = 0; c < 4; ++c) {
                uint8_t *col = t + 4 * c;
                const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                col[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                col[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                col[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        }
        cpy(s, t, 16);
        add_rk(s, a->rk + 4 * round);
    }
    cpy(out, s, 16);
    wipe(s, 16); wipe(t, 16);
}

void wwpa_aes_decrypt(const wwpa_aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    cpy(s, in, 16);
    add_rk(s, a->rk + 4 * a->rounds);
    for (unsigned round = a->rounds; round >= 1; --round) {
        for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r) t[4 * ((c + r) & 3) + r] = s[4 * c + r];   // InvShiftRows
        for (unsigned i = 0; i < 16; ++i) s[i] = INV_SBOX[t[i]];
        add_rk(s, a->rk + 4 * (round - 1));
        if (round != 1) {
            for (unsigned c = 0; c < 4; ++c) {
                uint8_t *col = s + 4 * c;
                const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                col[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                col[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                col[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }
    cpy(out, s, 16);
    wipe(s, 16); wipe(t, 16);
}

// ---- RFC 3394 key wrap ----
int wwpa_aes_wrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t n, uint8_t *out)
{
    if (n < 16 || n % 8 || !in || !out) return WWPA_ERR_INVAL;
    wwpa_aes a;
    if (wwpa_aes_init(&a, kek, kek_len)) return WWPA_ERR_INVAL;
    const size_t blocks = n / 8;
    uint8_t A[8], b[16];
    for (unsigned i = 0; i < 8; ++i) A[i] = 0xa6;
    for (size_t i = n; i-- > 0;) out[8 + i] = in[i];   // in and out may overlap at the same start
    for (unsigned j = 0; j < 6; ++j) {
        for (size_t i = 1; i <= blocks; ++i) {
            cpy(b, A, 8); cpy(b + 8, out + 8 * i, 8);
            wwpa_aes_encrypt(&a, b, b);
            const uint64_t t = (uint64_t)blocks * j + i;
            for (unsigned k = 0; k < 8; ++k) A[k] = b[k] ^ (uint8_t)(t >> (56 - 8 * k));
            cpy(out + 8 * i, b + 8, 8);
        }
    }
    cpy(out, A, 8);
    wipe(&a, sizeof a); wipe(b, sizeof b);
    return WWPA_OK;
}

int wwpa_aes_unwrap(const uint8_t *kek, size_t kek_len, const uint8_t *in, size_t n, uint8_t *out)
{
    if (n < 24 || n % 8 || !in || !out) return WWPA_ERR_INVAL;
    wwpa_aes a;
    if (wwpa_aes_init(&a, kek, kek_len)) return WWPA_ERR_INVAL;
    const size_t blocks = n / 8 - 1;
    uint8_t A[8], b[16];
    cpy(A, in, 8);
    for (size_t i = 0; i < n - 8; ++i) out[i] = in[8 + i];
    for (unsigned j = 6; j-- > 0;) {
        for (size_t i = blocks; i >= 1; --i) {
            const uint64_t t = (uint64_t)blocks * j + i;
            for (unsigned k = 0; k < 8; ++k) b[k] = A[k] ^ (uint8_t)(t >> (56 - 8 * k));
            cpy(b + 8, out + 8 * (i - 1), 8);
            wwpa_aes_decrypt(&a, b, b);
            cpy(A, b, 8);
            cpy(out + 8 * (i - 1), b + 8, 8);
        }
    }
    static const uint8_t iv[8] = {0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};
    const int ok = ct_eq(A, iv, 8);
    wipe(&a, sizeof a); wipe(b, sizeof b);
    if (!ok) {wipe(out, n - 8); return WWPA_ERR_UNWRAP;}
    return WWPA_OK;
}

// ---- PSK, PRF, PTK ----
int wwpa_psk(const char *pass, size_t pass_len, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[WWPA_PMK_LEN])
{
    if (!pass || pass_len < 8 || pass_len > 63 || ssid_len > 32 || (ssid_len && !ssid)) return WWPA_ERR_INVAL;
    for (size_t i = 0; i < pass_len; ++i) if (pass[i] < 32 || pass[i] > 126) return WWPA_ERR_INVAL;
    const uint8_t *key = (const uint8_t *)pass;
    for (unsigned blk = 1; blk <= 2; ++blk) {
        uint8_t u[WWPA_SHA1_LEN], t[WWPA_SHA1_LEN], cnt[4] = {0, 0, 0, (uint8_t)blk};
        const uint8_t *v[2] = {ssid, cnt};
        const size_t vl[2] = {ssid_len, 4};
        wwpa_hmac_sha1(key, pass_len, 2, v, vl, u);
        cpy(t, u, sizeof t);
        for (unsigned it = 1; it < 4096; ++it) {
            const uint8_t *v1[1] = {u};
            const size_t vl1[1] = {sizeof u};
            wwpa_hmac_sha1(key, pass_len, 1, v1, vl1, u);
            for (unsigned i = 0; i < WWPA_SHA1_LEN; ++i) t[i] ^= u[i];
        }
        cpy(pmk + (blk - 1) * WWPA_SHA1_LEN, t, blk == 1 ? WWPA_SHA1_LEN : WWPA_PMK_LEN - WWPA_SHA1_LEN);
        wipe(u, sizeof u); wipe(t, sizeof t);
    }
    return WWPA_OK;
}

void wwpa_prf(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t data_len, uint8_t *out,
              size_t out_len)
{
    size_t label_len = 0;
    while (label[label_len]) ++label_len;
    static const uint8_t zero = 0;
    uint8_t ctr = 0, h[WWPA_SHA1_LEN];
    for (size_t at = 0; at < out_len; ++ctr) {
        const uint8_t *v[4] = {(const uint8_t *)label, &zero, data, &ctr};
        const size_t vl[4] = {label_len, 1, data_len, 1};
        wwpa_hmac_sha1(key, key_len, 4, v, vl, h);
        const size_t k = out_len - at < WWPA_SHA1_LEN ? out_len - at : WWPA_SHA1_LEN;
        cpy(out + at, h, k);
        at += k;
    }
    wipe(h, sizeof h);
}

int wwpa_ptk_derive(const uint8_t pmk[WWPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                    const uint8_t anonce[WWPA_NONCE_LEN], const uint8_t snonce[WWPA_NONCE_LEN], unsigned tk_len, wwpa_ptk *ptk)
{
    if (tk_len != 16 && tk_len != 32) return WWPA_ERR_INVAL;
    uint8_t data[6 + 6 + 32 + 32], buf[WWPA_KCK_LEN + WWPA_KEK_LEN + 32];
    const int a_lo = cmp(aa, spa, 6) < 0, n_lo = cmp(anonce, snonce, 32) < 0;
    cpy(data, a_lo ? aa : spa, 6);
    cpy(data + 6, a_lo ? spa : aa, 6);
    cpy(data + 12, n_lo ? anonce : snonce, 32);
    cpy(data + 44, n_lo ? snonce : anonce, 32);
    const size_t len = WWPA_KCK_LEN + WWPA_KEK_LEN + tk_len;
    wwpa_prf(pmk, WWPA_PMK_LEN, "Pairwise key expansion", data, sizeof data, buf, len);
    cpy(ptk->kck, buf, WWPA_KCK_LEN);
    cpy(ptk->kek, buf + WWPA_KCK_LEN, WWPA_KEK_LEN);
    wipe(ptk->tk, sizeof ptk->tk);
    cpy(ptk->tk, buf + WWPA_KCK_LEN + WWPA_KEK_LEN, tk_len);
    ptk->tk_len = tk_len;
    wipe(buf, sizeof buf);
    return WWPA_OK;
}

int wwpa_ptk_derive_sha256(const uint8_t pmk[WWPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                           const uint8_t anonce[WWPA_NONCE_LEN], const uint8_t snonce[WWPA_NONCE_LEN], unsigned tk_len, wwpa_ptk *ptk)
{
    if (tk_len != 16 && tk_len != 32) return WWPA_ERR_INVAL;
    uint8_t data[6 + 6 + 32 + 32], buf[WWPA_KCK_LEN + WWPA_KEK_LEN + 32];
    const int a_lo = cmp(aa, spa, 6) < 0, n_lo = cmp(anonce, snonce, 32) < 0;
    cpy(data, a_lo ? aa : spa, 6);
    cpy(data + 6, a_lo ? spa : aa, 6);
    cpy(data + 12, n_lo ? anonce : snonce, 32);
    cpy(data + 44, n_lo ? snonce : anonce, 32);
    const size_t len = WWPA_KCK_LEN + WWPA_KEK_LEN + tk_len;
    wsae_kdf_sha256(pmk, WWPA_PMK_LEN, "Pairwise key expansion", data, sizeof data, buf, len * 8);
    cpy(ptk->kck, buf, WWPA_KCK_LEN);
    cpy(ptk->kek, buf + WWPA_KCK_LEN, WWPA_KEK_LEN);
    wipe(ptk->tk, sizeof ptk->tk);
    cpy(ptk->tk, buf + WWPA_KCK_LEN + WWPA_KEK_LEN, tk_len);
    ptk->tk_len = tk_len;
    wipe(buf, sizeof buf);
    return WWPA_OK;
}

// ---- EAPOL-Key ----
// Offsets from the start of the EAPOL frame.
#define F_DESC 4u
#define F_INFO 5u
#define F_KEYLEN 7u
#define F_REPLAY 9u
#define F_NONCE 17u
#define F_IV 49u
#define F_RSC 65u
#define F_ID 73u
#define F_MIC 81u
#define F_DLEN 97u
#define F_DATA 99u

int wwpa_key_parse(const uint8_t *frame, size_t len, wwpa_key *k)
{
    if (!frame || len < WWPA_EAPOL_HDR) return WWPA_ERR_INVAL;
    if (frame[1] != WWPA_EAPOL_TYPE_KEY) return WWPA_ERR_PROTO;
    const size_t body = be16(frame + 2);
    if (body < WWPA_KEY_HDR || WWPA_EAPOL_HDR + body > len) return WWPA_ERR_INVAL;
    if (frame[F_DESC] != WWPA_DESC_RSN) return WWPA_ERR_PROTO;
    k->eapol_version = frame[0];
    k->info = be16(frame + F_INFO);
    k->key_len = be16(frame + F_KEYLEN);
    k->replay = be64(frame + F_REPLAY);
    cpy(k->nonce, frame + F_NONCE, 32);
    cpy(k->iv, frame + F_IV, 16);
    cpy(k->rsc, frame + F_RSC, 8);
    cpy(k->mic, frame + F_MIC, 16);
    k->data_len = be16(frame + F_DLEN);
    if ((size_t)k->data_len + WWPA_KEY_HDR != body) return WWPA_ERR_INVAL;
    k->data = frame + F_DATA;
    return WWPA_OK;
}

long wwpa_key_build(const wwpa_key *k, uint8_t *out, size_t cap)
{
    const size_t len = F_DATA + k->data_len;
    if (!out || len > cap || (k->data_len && !k->data)) return WWPA_ERR_NOSPC;
    for (size_t i = 0; i < F_DATA; ++i) out[i] = 0;
    out[0] = k->eapol_version ? k->eapol_version : 2;
    out[1] = WWPA_EAPOL_TYPE_KEY;
    put_be16(out + 2, (uint16_t)(WWPA_KEY_HDR + k->data_len));
    out[F_DESC] = WWPA_DESC_RSN;
    put_be16(out + F_INFO, k->info);
    put_be16(out + F_KEYLEN, k->key_len);
    put_be64(out + F_REPLAY, k->replay);
    cpy(out + F_NONCE, k->nonce, 32);
    cpy(out + F_IV, k->iv, 16);
    cpy(out + F_RSC, k->rsc, 8);
    cpy(out + F_MIC, k->mic, 16);
    put_be16(out + F_DLEN, k->data_len);
    if (k->data_len) for (size_t i = k->data_len; i-- > 0;) out[F_DATA + i] = k->data[i];   // data may already be in place
    return (long)len;
}

int wwpa_key_mic(const uint8_t kck[WWPA_KCK_LEN], const uint8_t *frame, size_t len, uint8_t mic[WWPA_MIC_LEN])
{
    if (!frame || len < F_DATA) return WWPA_ERR_INVAL;
    static const uint8_t zero[WWPA_MIC_LEN];
    const uint8_t *v[3] = {frame, zero, frame + F_MIC + WWPA_MIC_LEN};
    const size_t vl[3] = {F_MIC, WWPA_MIC_LEN, len - F_MIC - WWPA_MIC_LEN};
    uint8_t h[WWPA_SHA1_LEN];
    wwpa_hmac_sha1(kck, WWPA_KCK_LEN, 3, v, vl, h);
    cpy(mic, h, WWPA_MIC_LEN);
    wipe(h, sizeof h);
    return WWPA_OK;
}

int wwpa_key_mic_cmac(const uint8_t kck[WWPA_KCK_LEN], const uint8_t *frame, size_t len, uint8_t mic[WWPA_MIC_LEN])
{
    if (!frame || len < F_DATA) return WWPA_ERR_INVAL;
    static const uint8_t zero[WWPA_MIC_LEN];
    const uint8_t *v[3] = {frame, zero, frame + F_MIC + WWPA_MIC_LEN};
    const size_t vl[3] = {F_MIC, WWPA_MIC_LEN, len - F_MIC - WWPA_MIC_LEN};
    wsae_aes_cmac(kck, 3, v, vl, mic);
    return WWPA_OK;
}
// the MIC of the supplicant's AKM
static int supp_mic(const wwpa_supp *s, const uint8_t kck[WWPA_KCK_LEN], const uint8_t *frame, size_t len, uint8_t mic[WWPA_MIC_LEN])
{
    return s->akm == WWPA_AKM_SAE ? wwpa_key_mic_cmac(kck, frame, len, mic) : wwpa_key_mic(kck, frame, len, mic);
}
static int supp_sign(const wwpa_supp *s, const uint8_t kck[WWPA_KCK_LEN], uint8_t *frame, size_t len)
{
    uint8_t mic[WWPA_MIC_LEN];
    const int r = supp_mic(s, kck, frame, len, mic);
    if (!r) cpy(frame + F_MIC, mic, WWPA_MIC_LEN);
    return r;
}
static int supp_verify(const wwpa_supp *s, const uint8_t kck[WWPA_KCK_LEN], const uint8_t *frame, size_t len)
{
    uint8_t mic[WWPA_MIC_LEN];
    const int r = supp_mic(s, kck, frame, len, mic);
    if (r) return r;
    return ct_eq(mic, frame + F_MIC, WWPA_MIC_LEN) ? WWPA_OK : WWPA_ERR_MIC;
}

int wwpa_key_sign(const uint8_t kck[WWPA_KCK_LEN], uint8_t *frame, size_t len)
{
    uint8_t mic[WWPA_MIC_LEN];
    const int r = wwpa_key_mic(kck, frame, len, mic);
    if (!r) cpy(frame + F_MIC, mic, WWPA_MIC_LEN);
    return r;
}

int wwpa_key_verify(const uint8_t kck[WWPA_KCK_LEN], const uint8_t *frame, size_t len)
{
    uint8_t mic[WWPA_MIC_LEN];
    const int r = wwpa_key_mic(kck, frame, len, mic);
    if (r) return r;
    return ct_eq(mic, frame + F_MIC, WWPA_MIC_LEN) ? WWPA_OK : WWPA_ERR_MIC;
}

int wwpa_kde_parse(const uint8_t *data, size_t len, wwpa_kdes *out)
{
    out->rsne = 0; out->rsne_len = 0; out->gtk_len = 0; out->gtk_idx = 0; out->gtk_tx = 0;
    wipe(out->gtk, sizeof out->gtk);
    size_t at = 0;
    while (at < len) {
        const uint8_t id = data[at];
        if (id == 0xdd && (at + 1 == len || data[at + 1] == 0)) break;   // padding: 0xdd followed by zeros
        if (at + 2 > len || at + 2 + data[at + 1] > len) return WWPA_ERR_PROTO;
        const uint8_t el = data[at + 1];
        const uint8_t *e = data + at + 2;
        if (id == 0x30) {
            if (out->rsne) return WWPA_ERR_PROTO;
            out->rsne = data + at; out->rsne_len = el + 2u;
        } else if (id == 0xdd && el >= 4 && e[0] == 0x00 && e[1] == 0x0f && e[2] == 0xac && e[3] == 1) {
            if (out->gtk_len || el < 6 + 5 || el - 6 > 32) return WWPA_ERR_PROTO;   // OUI, type, keyid/tx, reserved, GTK
            out->gtk_idx = e[4] & 3;
            out->gtk_tx = (e[4] >> 2) & 1;
            out->gtk_len = el - 6u;
            cpy(out->gtk, e + 6, out->gtk_len);
        }
        at += 2 + (size_t)el;
    }
    return WWPA_OK;
}

// ---- the supplicant ----
void wwpa_supp_init(wwpa_supp *s, const uint8_t pmk[WWPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
                    const uint8_t *own_rsne, unsigned own_rsne_len, const uint8_t *ap_rsne, unsigned ap_rsne_len)
{
    wipe(s, sizeof *s);
    cpy(s->pmk, pmk, WWPA_PMK_LEN);
    cpy(s->aa, aa, 6);
    cpy(s->spa, spa, 6);
    s->own_rsne = own_rsne; s->own_rsne_len = own_rsne_len;
    s->ap_rsne = ap_rsne; s->ap_rsne_len = ap_rsne_len;
}

void wwpa_supp_clear(wwpa_supp *s) {wipe(s, sizeof *s);}
void wwpa_supp_set_akm(wwpa_supp *s, unsigned akm) {s->akm = akm == WWPA_AKM_SAE ? WWPA_AKM_SAE : WWPA_AKM_PSK;}

// Key data of M3 can be at most this long (an RSNE, a GTK KDE and room for an IGTK/second RSNE).
#define MAX_KEY_DATA 512u

int wwpa_supp_rx(wwpa_supp *s, const uint8_t *frame, size_t len, const uint8_t snonce[WWPA_NONCE_LEN], uint8_t *out,
                 size_t cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    wwpa_key k;
    int r = wwpa_key_parse(frame, len, &k);
    if (r) return r;
    const uint16_t ver = s->akm == WWPA_AKM_SAE ? 0 : WWPA_KI_VER_HMAC_SHA1_AES;   // SAE: key descriptor version 0 (AKM-defined)
    if ((k.info & WWPA_KI_VER_MASK) != ver) return WWPA_ERR_PROTO;
    if (!(k.info & WWPA_KI_PAIRWISE) || !(k.info & WWPA_KI_ACK) || (k.info & (WWPA_KI_REQUEST | WWPA_KI_ERROR))) return WWPA_ERR_PROTO;
    // 12.7.2: a frame whose replay counter is not larger than the last valid one is discarded.
    if (s->replay_valid && k.replay <= s->replay) return WWPA_ERR_REPLAY;
    const size_t flen = WWPA_EAPOL_HDR + be16(frame + 2);
    wwpa_key o;
    wipe(&o, sizeof o);
    o.eapol_version = k.eapol_version;
    o.replay = k.replay;
    if (!(k.info & WWPA_KI_MIC)) {
        // Message 1: ANonce. A new M1 (retransmitted or fresh) restarts the handshake with a new SNonce.
        if (k.info & (WWPA_KI_INSTALL | WWPA_KI_SECURE | WWPA_KI_ENC_DATA)) return WWPA_ERR_PROTO;
        if (!snonce) return WWPA_ERR_INVAL;
        wwpa_ptk t;
        r = s->akm == WWPA_AKM_SAE ? wwpa_ptk_derive_sha256(s->pmk, s->aa, s->spa, k.nonce, snonce, 16, &t)
                                   : wwpa_ptk_derive(s->pmk, s->aa, s->spa, k.nonce, snonce, 16, &t);
        if (r) return r;
        o.info = ver | WWPA_KI_PAIRWISE | WWPA_KI_MIC;
        o.key_len = 0;
        cpy(o.nonce, snonce, 32);
        o.data = s->own_rsne; o.data_len = (uint16_t)s->own_rsne_len;
        const long n = wwpa_key_build(&o, out, cap);
        if (n < 0) {wipe(&t, sizeof t); return (int)n;}
        supp_sign(s, t.kck, out, (size_t)n);
        cpy(s->anonce, k.nonce, 32);
        cpy(s->snonce, snonce, 32);
        s->tptk = t;
        wipe(&t, sizeof t);
        s->replay = k.replay; s->replay_valid = 1;
        s->state = WWPA_GOT_M1;
        if (out_len) *out_len = (size_t)n;
        return WWPA_GOT_M1;
    }
    // Message 3: MIC, install, secure, encrypted key data, the same ANonce.
    if (s->state != WWPA_GOT_M1 && s->state != WWPA_DONE) return WWPA_ERR_STATE;
    const int resend = s->state == WWPA_DONE;
    const uint16_t need = WWPA_KI_MIC | WWPA_KI_INSTALL | WWPA_KI_SECURE | WWPA_KI_ENC_DATA;
    if ((k.info & need) != need) return WWPA_ERR_PROTO;
    if (cmp(k.nonce, s->anonce, 32)) return WWPA_ERR_PROTO;
    r = supp_verify(s, s->tptk.kck, frame, flen);
    if (r) return r;
    if (k.data_len < 24 || k.data_len % 8 || k.data_len - 8u > MAX_KEY_DATA) return WWPA_ERR_PROTO;
    uint8_t plain[MAX_KEY_DATA];
    r = wwpa_aes_unwrap(s->tptk.kek, WWPA_KEK_LEN, k.data, k.data_len, plain);
    if (r) return r;
    wwpa_kdes kd;
    r = wwpa_kde_parse(plain, k.data_len - 8u, &kd);
    if (!r && s->ap_rsne_len && (kd.rsne_len != s->ap_rsne_len || cmp(kd.rsne, s->ap_rsne, kd.rsne_len))) r = WWPA_ERR_PROTO;
    if (!r && !kd.gtk_len) r = WWPA_ERR_PROTO;
    if (r) {wipe(plain, sizeof plain); wipe(&kd, sizeof kd); return r;}
    o.info = ver | WWPA_KI_PAIRWISE | WWPA_KI_MIC | WWPA_KI_SECURE;
    const long n = wwpa_key_build(&o, out, cap);
    if (n < 0) {wipe(plain, sizeof plain); wipe(&kd, sizeof kd); return (int)n;}
    supp_sign(s, s->tptk.kck, out, (size_t)n);
    s->replay = k.replay;
    if (out_len) *out_len = (size_t)n;
    if (resend) {wipe(plain, sizeof plain); wipe(&kd, sizeof kd); return WWPA_M4_RESENT;}
    s->ptk = s->tptk;
    cpy(s->gtk, kd.gtk, 32);
    s->gtk_len = kd.gtk_len;
    s->gtk_idx = kd.gtk_idx;
    s->state = WWPA_DONE;
    wipe(plain, sizeof plain); wipe(&kd, sizeof kd);
    return WWPA_DONE;
}
