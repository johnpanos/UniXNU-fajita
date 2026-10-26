// SPDX-License-Identifier: BSD-2-Clause-Patent
// SAE (WPA3-Personal) and its hashes. See WifiSae.h.
#include "WifiSae.h"
#include "WifiWpa.h"
#include <string.h>

// ============================================================ SHA-256 (FIPS 180-4)
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror(uint32_t x, unsigned n) {return x >> n | x << (32 - n);}
static void sha256_block(wsae_sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
void wsae_sha256_init(wsae_sha256_ctx *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->bytes = 0;
    c->fill = 0;
}
void wsae_sha256_update(wsae_sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->bytes += n;
    while (n) {
        const size_t k = 64 - c->fill < n ? 64 - c->fill : n;
        memcpy(c->buf + c->fill, p, k);
        c->fill += (unsigned)k; p += k; n -= k;
        if (c->fill == 64) {sha256_block(c, c->buf); c->fill = 0;}
    }
}
void wsae_sha256_final(wsae_sha256_ctx *c, uint8_t out[WSAE_SHA256_LEN])
{
    const uint64_t bits = c->bytes * 8;
    uint8_t pad = 0x80;
    wsae_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56) wsae_sha256_update(c, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    wsae_sha256_update(c, len, 8);
    for (int i = 0; i < 8; ++i) {out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];}
}
void wsae_sha256(const void *data, size_t n, uint8_t out[WSAE_SHA256_LEN])
{
    wsae_sha256_ctx c;
    wsae_sha256_init(&c);
    wsae_sha256_update(&c, data, n);
    wsae_sha256_final(&c, out);
}
void wsae_hmac_sha256(const uint8_t *key, size_t key_len, unsigned nv, const uint8_t *const *v, const size_t *vlen,
                      uint8_t out[WSAE_SHA256_LEN])
{
    uint8_t k[64] = {0}, pad[64], inner[32];
    if (key_len > 64) wsae_sha256(key, key_len, k); else memcpy(k, key, key_len);
    wsae_sha256_ctx c;
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    wsae_sha256_init(&c);
    wsae_sha256_update(&c, pad, 64);
    for (unsigned i = 0; i < nv; ++i) wsae_sha256_update(&c, v[i], vlen[i]);
    wsae_sha256_final(&c, inner);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    wsae_sha256_init(&c);
    wsae_sha256_update(&c, pad, 64);
    wsae_sha256_update(&c, inner, 32);
    wsae_sha256_final(&c, out);
}
void wsae_kdf_sha256(const uint8_t *key, size_t key_len, const char *label, const uint8_t *ctx, size_t ctx_len, uint8_t *out,
                     size_t bits)
{
    const size_t bytes = (bits + 7) / 8;
    size_t label_len = 0;
    while (label[label_len]) ++label_len;
    uint8_t ib[2], lb[2] = {(uint8_t)bits, (uint8_t)(bits >> 8)};
    const uint8_t *v[4] = {ib, (const uint8_t *)label, ctx, lb};
    const size_t vl[4] = {2, label_len, ctx_len, 2};
    size_t at = 0;
    for (unsigned i = 1; at < bytes; ++i) {
        uint8_t h[32];
        ib[0] = (uint8_t)i; ib[1] = (uint8_t)(i >> 8);
        wsae_hmac_sha256(key, key_len, 4, v, vl, h);
        const size_t n = bytes - at < 32 ? bytes - at : 32;
        memcpy(out + at, h, n);
        at += n;
    }
    if (bits % 8) out[bytes - 1] &= (uint8_t)(0xff << (8 - bits % 8));
}

// ============================================================ AES-128-CMAC (RFC 4493)
static void dbl(uint8_t b[16])
{
    const uint8_t carry = b[0] >> 7;
    for (int i = 0; i < 15; ++i) b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7);
    b[15] = (uint8_t)(b[15] << 1) ^ (uint8_t)(carry * 0x87);
}
void wsae_aes_cmac(const uint8_t key[16], unsigned nv, const uint8_t *const *v, const size_t *vlen, uint8_t mac[16])
{
    wwpa_aes a;
    wwpa_aes_init(&a, key, 16);
    uint8_t k1[16] = {0}, k2[16];
    wwpa_aes_encrypt(&a, k1, k1);
    dbl(k1);
    memcpy(k2, k1, 16);
    dbl(k2);
    size_t total = 0;
    for (unsigned i = 0; i < nv; ++i) total += vlen[i];
    uint8_t x[16] = {0}, blk[16];
    unsigned fill = 0, piece = 0;
    size_t off = 0, done = 0;
    // every complete block but the last goes through the cipher as it fills
    while (done < total) {
        while (piece < nv && off == vlen[piece]) {++piece; off = 0;}
        blk[fill++] = v[piece][off++];
        ++done;
        if (fill == 16 && done < total) {
            for (int i = 0; i < 16; ++i) x[i] ^= blk[i];
            wwpa_aes_encrypt(&a, x, x);
            fill = 0;
        }
    }
    if (fill == 16) {for (int i = 0; i < 16; ++i) x[i] ^= blk[i] ^ k1[i];}
    else {
        blk[fill++] = 0x80;
        while (fill < 16) blk[fill++] = 0;
        for (int i = 0; i < 16; ++i) x[i] ^= blk[i] ^ k2[i];
    }
    wwpa_aes_encrypt(&a, x, mac);
}

// ============================================================ P-256 field arithmetic (Montgomery, 8 x 32-bit limbs, LE)
typedef uint32_t fe[8];
typedef struct {fe m, r2, one; uint32_t minv;} modulus;
static modulus P, N;   // the field prime and the group order; initialised once (constant data)
static int g_init;
static const uint8_t kP[32] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                               0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t kN[32] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                               0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51};
static const uint8_t kB[32] = {0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
                               0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b};
static fe B_m, A_m;   // b and a = -3 in Montgomery form

static void fe_from_be(fe r, const uint8_t b[32])
{
    for (int i = 0; i < 8; ++i)
        r[i] = (uint32_t)b[31 - 4 * i] | (uint32_t)b[30 - 4 * i] << 8 | (uint32_t)b[29 - 4 * i] << 16 | (uint32_t)b[28 - 4 * i] << 24;
}
static void fe_to_be(uint8_t b[32], const fe a)
{
    for (int i = 0; i < 8; ++i) {b[31 - 4 * i] = (uint8_t)a[i]; b[30 - 4 * i] = (uint8_t)(a[i] >> 8); b[29 - 4 * i] = (uint8_t)(a[i] >> 16); b[28 - 4 * i] = (uint8_t)(a[i] >> 24);}
}
static void fe_copy(fe r, const fe a) {for (int i = 0; i < 8; ++i) r[i] = a[i];}
static uint32_t fe_is_zero(const fe a) {uint32_t x = 0; for (int i = 0; i < 8; ++i) x |= a[i]; return (uint32_t)(((uint64_t)x - 1) >> 63);}   // 1 if zero
static uint32_t fe_eq(const fe a, const fe b) {uint32_t x = 0; for (int i = 0; i < 8; ++i) x |= a[i] ^ b[i]; return (uint32_t)(((uint64_t)x - 1) >> 63);}
// r = mask ? a : b (mask 0 or 0xffffffff)
static void fe_sel(fe r, uint32_t mask, const fe a, const fe b) {for (int i = 0; i < 8; ++i) r[i] = (a[i] & mask) | (b[i] & ~mask);}
// r = a - b, returns the borrow (0/1)
static uint32_t sub8(fe r, const fe a, const fe b)
{
    uint64_t bw = 0;
    for (int i = 0; i < 8; ++i) {const uint64_t d = (uint64_t)a[i] - b[i] - bw; r[i] = (uint32_t)d; bw = (d >> 32) & 1;}
    return (uint32_t)bw;
}
static uint32_t add8(fe r, const fe a, const fe b)
{
    uint64_t c = 0;
    for (int i = 0; i < 8; ++i) {c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32;}
    return (uint32_t)c;
}
// 1 if a < b (constant time)
static uint32_t fe_lt(const fe a, const fe b) {fe t; return sub8(t, a, b);}
static void mod_add(fe r, const fe a, const fe b, const modulus *m)
{
    fe s, t;
    const uint32_t c = add8(s, a, b);
    const uint32_t bw = sub8(t, s, m->m);
    fe_sel(r, (uint32_t)0 - (c | (bw ^ 1)), t, s);   // subtract when it overflowed or s >= m
}
static void mod_sub(fe r, const fe a, const fe b, const modulus *m)
{
    fe d, t;
    const uint32_t bw = sub8(d, a, b);
    add8(t, d, m->m);
    fe_sel(r, (uint32_t)0 - bw, t, d);
}
// CIOS Montgomery multiplication: r = a * b / 2^256 mod m (a, b < m)
static void mont_mul(fe r, const fe a, const fe b, const modulus *m)
{
    uint32_t t[10] = {0};
    for (int i = 0; i < 8; ++i) {
        uint64_t c = 0;
        for (int j = 0; j < 8; ++j) {c += (uint64_t)t[j] + (uint64_t)a[j] * b[i]; t[j] = (uint32_t)c; c >>= 32;}
        c += t[8]; t[8] = (uint32_t)c; t[9] = (uint32_t)(c >> 32);
        const uint32_t u = t[0] * m->minv;
        c = ((uint64_t)t[0] + (uint64_t)u * m->m[0]) >> 32;
        for (int j = 1; j < 8; ++j) {c += (uint64_t)t[j] + (uint64_t)u * m->m[j]; t[j - 1] = (uint32_t)c; c >>= 32;}
        c += t[8]; t[7] = (uint32_t)c; c >>= 32;
        t[8] = t[9] + (uint32_t)c; t[9] = 0;
    }
    fe s;
    const uint32_t bw = sub8(s, t, m->m);
    fe_sel(r, (uint32_t)0 - (t[8] | (bw ^ 1)), s, t);
}
static void mod_init(modulus *m, const uint8_t be[32])
{
    fe_from_be(m->m, be);
    uint32_t x = m->m[0];   // Newton: x = m0^-1 mod 2^32
    for (int i = 0; i < 5; ++i) x *= 2 - m->m[0] * x;
    m->minv = (uint32_t)0 - x;
    fe r = {1, 0, 0, 0, 0, 0, 0, 0};   // 2^512 mod m by doubling (m > 2^255 so one subtraction per step suffices)
    for (int i = 0; i < 512; ++i) mod_add(r, r, r, m);
    fe_copy(m->r2, r);
    const fe one = {1, 0, 0, 0, 0, 0, 0, 0};
    mont_mul(m->one, one, m->r2, m);   // R mod m
}
static void to_mont(fe r, const fe a, const modulus *m) {mont_mul(r, a, m->r2, m);}
static void from_mont(fe r, const fe a, const modulus *m) {const fe one = {1, 0, 0, 0, 0, 0, 0, 0}; mont_mul(r, a, one, m);}
// r = a^e (Montgomery in and out), e big-endian bytes; square-and-multiply-always with constant-time selection
static void mont_pow(fe r, const fe a, const uint8_t e[32], const modulus *m)
{
    fe acc, t;
    fe_copy(acc, m->one);
    for (int i = 0; i < 256; ++i) {
        mont_mul(acc, acc, acc, m);
        mont_mul(t, acc, a, m);
        const uint32_t bit = (e[i / 8] >> (7 - i % 8)) & 1;
        fe_sel(acc, (uint32_t)0 - bit, t, acc);
    }
    fe_copy(r, acc);
}
static uint8_t kPm2[32], kPp1d4[32], kPm1d2[32];   // p - 2, (p + 1) / 4, (p - 1) / 2
static void init_curve(void)
{
    if (g_init) return;
    mod_init(&P, kP);
    mod_init(&N, kN);
    fe b, three = {3, 0, 0, 0, 0, 0, 0, 0}, t;
    fe_from_be(b, kB);
    to_mont(B_m, b, &P);
    to_mont(t, three, &P);
    mod_sub(A_m, (fe){0, 0, 0, 0, 0, 0, 0, 0}, t, &P);
    fe two = {2, 0, 0, 0, 0, 0, 0, 0}, one = {1, 0, 0, 0, 0, 0, 0, 0}, v;
    sub8(v, P.m, two); fe_to_be(kPm2, v);
    add8(v, P.m, one);   // p + 1 (no overflow: p < 2^256 - 1), then / 4
    for (int i = 0; i < 8; ++i) v[i] = v[i] >> 2 | (i < 7 ? v[i + 1] << 30 : 0);
    fe_to_be(kPp1d4, v);
    sub8(v, P.m, one);
    for (int i = 0; i < 8; ++i) v[i] = v[i] >> 1 | (i < 7 ? v[i + 1] << 31 : 0);
    fe_to_be(kPm1d2, v);
    g_init = 1;
}
static void fe_inv(fe r, const fe a) {mont_pow(r, a, kPm2, &P);}
// y^2 = x^3 - 3x + b (Montgomery)
static void y_sqr(fe r, const fe x)
{
    fe t, u;
    mont_mul(t, x, x, &P);
    mont_mul(t, t, x, &P);
    mont_mul(u, A_m, x, &P);
    mod_add(t, t, u, &P);
    mod_add(r, t, B_m, &P);
}
// Legendre symbol of a (Montgomery): 1, -1 or 0
static int legendre(const fe a)
{
    fe t, m1;
    mont_pow(t, a, kPm1d2, &P);
    mod_sub(m1, (fe){0, 0, 0, 0, 0, 0, 0, 0}, P.one, &P);
    return fe_eq(t, P.one) ? 1 : fe_eq(t, m1) ? -1 : 0;
}

// ============================================================ points (Jacobian, a = -3)
static void pt_set_inf(wsae_point *p) {fe_copy(p->x, P.one); fe_copy(p->y, P.one); memset(p->z, 0, sizeof p->z);}
static void pt_dbl(wsae_point *r, const wsae_point *p)
{
    if (fe_is_zero(p->z) || fe_is_zero(p->y)) {pt_set_inf(r); return;}
    fe delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    mont_mul(delta, p->z, p->z, &P);
    mont_mul(gamma, p->y, p->y, &P);
    mont_mul(beta, p->x, gamma, &P);
    mod_sub(t1, p->x, delta, &P);
    mod_add(t2, p->x, delta, &P);
    mont_mul(alpha, t1, t2, &P);
    mod_add(t1, alpha, alpha, &P); mod_add(alpha, t1, alpha, &P);   // 3 * (X - delta)(X + delta)
    mont_mul(x3, alpha, alpha, &P);
    mod_add(t1, beta, beta, &P); mod_add(t1, t1, t1, &P); mod_add(t2, t1, t1, &P);   // t1 = 4 beta, t2 = 8 beta
    mod_sub(x3, x3, t2, &P);
    mod_add(z3, p->y, p->z, &P);
    mont_mul(z3, z3, z3, &P);
    mod_sub(z3, z3, gamma, &P);
    mod_sub(z3, z3, delta, &P);
    mod_sub(t1, t1, x3, &P);
    mont_mul(y3, alpha, t1, &P);
    mont_mul(t2, gamma, gamma, &P);
    mod_add(t2, t2, t2, &P); mod_add(t2, t2, t2, &P); mod_add(t2, t2, t2, &P);   // 8 gamma^2
    mod_sub(y3, y3, t2, &P);
    fe_copy(r->x, x3); fe_copy(r->y, y3); fe_copy(r->z, z3);
}
static void pt_add(wsae_point *r, const wsae_point *p, const wsae_point *q)
{
    if (fe_is_zero(p->z)) {*r = *q; return;}
    if (fe_is_zero(q->z)) {*r = *p; return;}
    fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;
    mont_mul(z1z1, p->z, p->z, &P);
    mont_mul(z2z2, q->z, q->z, &P);
    mont_mul(u1, p->x, z2z2, &P);
    mont_mul(u2, q->x, z1z1, &P);
    mont_mul(s1, p->y, q->z, &P); mont_mul(s1, s1, z2z2, &P);
    mont_mul(s2, q->y, p->z, &P); mont_mul(s2, s2, z1z1, &P);
    mod_sub(h, u2, u1, &P);
    mod_sub(rr, s2, s1, &P);
    if (fe_is_zero(h)) {
        if (fe_is_zero(rr)) {pt_dbl(r, p); return;}
        pt_set_inf(r);
        return;
    }
    mod_add(rr, rr, rr, &P);
    mod_add(i, h, h, &P); mont_mul(i, i, i, &P);
    mont_mul(j, h, i, &P);
    mont_mul(v, u1, i, &P);
    mont_mul(x3, rr, rr, &P);
    mod_sub(x3, x3, j, &P);
    mod_sub(x3, x3, v, &P); mod_sub(x3, x3, v, &P);
    mod_sub(t, v, x3, &P);
    mont_mul(y3, rr, t, &P);
    mont_mul(t, s1, j, &P); mod_add(t, t, t, &P);
    mod_sub(y3, y3, t, &P);
    mod_add(z3, p->z, q->z, &P); mont_mul(z3, z3, z3, &P);
    mod_sub(z3, z3, z1z1, &P); mod_sub(z3, z3, z2z2, &P);
    mont_mul(z3, z3, h, &P);
    fe_copy(r->x, x3); fe_copy(r->y, y3); fe_copy(r->z, z3);
}
static void pt_cswap(wsae_point *a, wsae_point *b, uint32_t bit)
{
    const uint32_t m = (uint32_t)0 - bit;
    for (int i = 0; i < 8; ++i) {
        uint32_t t = (a->x[i] ^ b->x[i]) & m; a->x[i] ^= t; b->x[i] ^= t;
        t = (a->y[i] ^ b->y[i]) & m; a->y[i] ^= t; b->y[i] ^= t;
        t = (a->z[i] ^ b->z[i]) & m; a->z[i] ^= t; b->z[i] ^= t;
    }
}
// r = k * p (Montgomery ladder over all 256 bits; k plain big-endian)
static void pt_mul(wsae_point *r, const wsae_point *p, const uint8_t k[32])
{
    wsae_point r0, r1;
    pt_set_inf(&r0);
    r1 = *p;
    for (int i = 0; i < 256; ++i) {
        const uint32_t bit = (k[i / 8] >> (7 - i % 8)) & 1;
        pt_cswap(&r0, &r1, bit);
        pt_add(&r1, &r0, &r1);
        pt_dbl(&r0, &r0);
        pt_cswap(&r0, &r1, bit);
    }
    *r = r0;
}
// affine (Montgomery) coordinates; 0 when p is the point at infinity
static int pt_affine(fe x, fe y, const wsae_point *p)
{
    if (fe_is_zero(p->z)) return 0;
    fe zi, zi2;
    fe_inv(zi, p->z);
    mont_mul(zi2, zi, zi, &P);
    mont_mul(x, p->x, zi2, &P);
    mont_mul(zi2, zi2, zi, &P);
    mont_mul(y, p->y, zi2, &P);
    return 1;
}
// big-endian x||y -> point; 0 unless both coordinates are < p and the point is on the curve
static int pt_from_bin(wsae_point *p, const uint8_t xy[64])
{
    fe x, y, t, u;
    fe_from_be(x, xy); fe_from_be(y, xy + 32);
    if (!fe_lt(x, P.m) || !fe_lt(y, P.m)) return 0;
    to_mont(p->x, x, &P); to_mont(p->y, y, &P); fe_copy(p->z, P.one);
    y_sqr(t, p->x);
    mont_mul(u, p->y, p->y, &P);
    return (int)fe_eq(t, u);
}
static int pt_to_bin(uint8_t xy[64], const wsae_point *p)
{
    fe x, y, t;
    if (!pt_affine(x, y, p)) return 0;
    from_mont(t, x, &P); fe_to_be(xy, t);
    from_mont(t, y, &P); fe_to_be(xy + 32, t);
    return 1;
}

int wsae_p256_mul(const uint8_t x[32], const uint8_t y[32], const uint8_t k[32], uint8_t ox[32], uint8_t oy[32])
{
    init_curve();
    uint8_t xy[64];
    memcpy(xy, x, 32); memcpy(xy + 32, y, 32);
    wsae_point p, r;
    if (!pt_from_bin(&p, xy)) return WSAE_ERR_INVAL;
    pt_mul(&r, &p, k);
    if (!pt_to_bin(xy, &r)) return WSAE_ERR_INVAL;
    memcpy(ox, xy, 32); memcpy(oy, xy + 32, 32);
    return 0;
}

// ============================================================ SAE
// a random value in [1, m - 1] (plain)
static int rand_below(wsae *s, fe out, const fe m, unsigned min)
{
    for (unsigned tries = 0; tries < 100; ++tries) {
        uint8_t b[32];
        s->rng(s->rng_ctx, b, 32);
        fe v, lim;
        fe_from_be(v, b);
        fe_copy(lim, m);
        if (fe_lt(v, lim) && !(v[0] < min && !(v[1] | v[2] | v[3] | v[4] | v[5] | v[6] | v[7]))) {fe_copy(out, v); return 0;}
    }
    return WSAE_ERR_RNG;
}
// dragonfly_is_quadratic_residue_blind: 1 if v (Montgomery) is a quadratic residue, hiding v from timing
static int qr_blind(wsae *s, const fe v, const fe qr, const fe qnr)
{
    fe r, rm, num;
    if (rand_below(s, r, P.m, 1)) return -1;
    to_mont(rm, r, &P);
    mont_mul(num, v, rm, &P);
    mont_mul(num, num, rm, &P);
    const uint32_t odd = r[0] & 1;
    fe f;
    fe_sel(f, (uint32_t)0 - odd, qr, qnr);
    mont_mul(num, num, f, &P);
    const int l = legendre(num);
    return odd ? l == 1 : l == -1;
}
static uint32_t ct_lt_be(const uint8_t *a, const uint8_t *b, size_t n)   // 1 if a < b, constant time
{
    uint32_t lt = 0, eq = 1;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t x = a[i], y = b[i];
        lt |= eq & (uint32_t)((x - y) >> 31);
        eq &= (uint32_t)(((x ^ y) - 1) >> 31);
    }
    return lt;
}

static int derive_pwe(wsae *s, const uint8_t *pw, size_t pw_len)
{
    fe qr = {0}, qnr = {0};
    int have_qr = 0, have_qnr = 0;
    for (unsigned tries = 0; tries < 200 && !(have_qr && have_qnr); ++tries) {   // dragonfly_get_random_qr_qnr
        fe t, tm;
        if (rand_below(s, t, P.m, 1)) return WSAE_ERR_RNG;
        to_mont(tm, t, &P);
        const int l = legendre(tm);
        if (l == 1 && !have_qr) {fe_copy(qr, tm); have_qr = 1;}
        else if (l == -1 && !have_qnr) {fe_copy(qnr, tm); have_qnr = 1;}
    }
    if (!have_qr || !have_qnr) return WSAE_ERR_RNG;
    uint8_t stub[256], tmp[256];
    if (pw_len > sizeof stub) return WSAE_ERR_INVAL;
    s->rng(s->rng_ctx, stub, (uint32_t)pw_len);
    // key = max(addr) || min(addr)
    uint8_t key[12];
    const int own_hi = memcmp(s->own, s->peer, 6) > 0;
    memcpy(key, own_hi ? s->own : s->peer, 6);
    memcpy(key + 6, own_hi ? s->peer : s->own, 6);
    uint8_t x_bin[32] = {0}, found = 0, seed_odd = 0;
    unsigned counter;
    for (counter = 1; counter <= WSAE_MIN_ITER || !found; ++counter) {
        if (counter > WSAE_MAX_ITER) break;
        // after the point is found, the loop hashes a random stub instead of the password
        for (size_t i = 0; i < pw_len; ++i) tmp[i] = (uint8_t)((stub[i] & found) | (pw[i] & (uint8_t)~found));
        const uint8_t c = (uint8_t)counter;
        const uint8_t *v[2] = {tmp, &c};
        const size_t vl[2] = {pw_len, 1};
        uint8_t seed[32], value[32];
        wsae_hmac_sha256(key, 12, 2, v, vl, seed);
        wsae_kdf_sha256(seed, 32, "SAE Hunting and Pecking", kP, 32, value, 256);
        const uint32_t in_range = ct_lt_be(value, kP, 32);
        fe x, xm, ys;
        fe_from_be(x, value);
        { fe t; const uint32_t bw = sub8(t, x, P.m); fe_sel(x, (uint32_t)0 - (bw ^ 1), t, x); }   // < p either way (x < 2p)
        to_mont(xm, x, &P);
        y_sqr(ys, xm);
        const int res = qr_blind(s, ys, qr, qnr);
        if (res < 0) return WSAE_ERR_RNG;
        const uint8_t take = (uint8_t)((0u - (uint32_t)(res & (int)in_range)) & ~(uint32_t)found & 0xff);   // this iteration found it first
        for (int i = 0; i < 32; ++i) x_bin[i] = (uint8_t)((value[i] & take) | (x_bin[i] & (uint8_t)~take));
        seed_odd = (uint8_t)(((seed[31] & 1) & take) | (seed_odd & (uint8_t)~take));
        found |= take;
        memset(seed, 0, sizeof seed);
    }
    memset(tmp, 0, sizeof tmp);
    s->pwe_iterations = counter - 1;
    if (!found) return WSAE_ERR_NOPWE;
    // y = sqrt(x^3 + ax + b); PWE = (x, y) if LSB(y) == LSB(pwd-seed) else (x, p - y)
    fe x, xm, ys, y, yp, ny, yout;
    fe_from_be(x, x_bin);
    to_mont(xm, x, &P);
    y_sqr(ys, xm);
    mont_pow(y, ys, kPp1d4, &P);
    from_mont(yp, y, &P);
    fe py;
    sub8(py, P.m, yp);
    const uint32_t same = (uint32_t)0 - (uint32_t)((yp[0] & 1) == seed_odd);
    fe_sel(yout, same, yp, py);
    (void)ny;
    fe_copy(s->pwe.x, xm);
    to_mont(s->pwe.y, yout, &P);
    fe_copy(s->pwe.z, P.one);
    {   // the square root must exist (it does: x was accepted as a residue)
        fe chk;
        mont_mul(chk, s->pwe.y, s->pwe.y, &P);
        if (!fe_eq(chk, ys)) return WSAE_ERR_NOPWE;
    }
    s->have_pwe = 1;
    return WSAE_OK;
}

// commit-scalar = (rand + mask) mod r; COMMIT-ELEMENT = inverse(mask * PWE)
static int make_commit(wsae *s, const fe rnd, const fe mask)
{
    fe sc;
    mod_add(sc, rnd, mask, &N);   // both < r, so one conditional subtraction reduces
    const fe one = {1, 0, 0, 0, 0, 0, 0, 0};
    if (fe_is_zero(sc) || fe_eq(sc, one)) return WSAE_ERR_RNG;   // must be > 1
    fe_copy(s->rand, rnd);
    fe_to_be(s->own_scalar, sc);
    uint8_t mb[32];
    fe_to_be(mb, mask);
    wsae_point e;
    pt_mul(&e, &s->pwe, mb);
    memset(mb, 0, sizeof mb);
    mod_sub(e.y, (fe){0, 0, 0, 0, 0, 0, 0, 0}, e.y, &P);   // inverse: (x, -y)
    if (!pt_to_bin(s->own_element, &e)) return WSAE_ERR_RNG;
    s->have_commit = 1;
    s->have_keys = 0;
    return WSAE_OK;
}

int wsae_init(wsae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *pw, size_t pw_len, wsae_rng_fn rng, void *rng_ctx)
{
    init_curve();
    memset(s, 0, sizeof *s);
    if (!rng || !pw || !pw_len) return WSAE_ERR_INVAL;
    s->rng = rng; s->rng_ctx = rng_ctx;
    memcpy(s->own, own, 6); memcpy(s->peer, peer, 6);
    s->send_confirm = 1;
    int r = derive_pwe(s, pw, pw_len);
    if (r) return r;
    for (unsigned tries = 0; tries < 10; ++tries) {
        fe rnd, mask;
        if (rand_below(s, rnd, N.m, 2) || rand_below(s, mask, N.m, 2)) return WSAE_ERR_RNG;
        r = make_commit(s, rnd, mask);
        memset(mask, 0, sizeof mask);
        if (r != WSAE_ERR_RNG) return r;
    }
    return WSAE_ERR_RNG;
}

int wsae_set_commit(wsae *s, const uint8_t rand_be[32], const uint8_t mask_be[32])
{
    if (!s->have_pwe) return WSAE_ERR_INVAL;
    fe rnd, mask;
    fe_from_be(rnd, rand_be); fe_from_be(mask, mask_be);
    if (!fe_lt(rnd, N.m) || !fe_lt(mask, N.m)) return WSAE_ERR_INVAL;
    return make_commit(s, rnd, mask);
}

long wsae_write_commit(const wsae *s, const uint8_t *token, size_t token_len, uint8_t *out, size_t cap)
{
    if (!s->have_commit || token_len > WSAE_TOKEN_MAX || cap < WSAE_COMMIT_LEN + token_len) return WSAE_ERR_INVAL;
    out[0] = (uint8_t)WSAE_GROUP; out[1] = 0;
    if (token_len) memcpy(out + 2, token, token_len);
    memcpy(out + 2 + token_len, s->own_scalar, 32);
    memcpy(out + 2 + token_len + 32, s->own_element, 64);
    return (long)(WSAE_COMMIT_LEN + token_len);
}

int wsae_process_commit(wsae *s, const uint8_t *body, size_t len)
{
    if (!s->have_commit) return WSAE_ERR_INVAL;
    if (len < 2) return WSAE_ERR_PEER;
    if ((body[0] | body[1] << 8) != WSAE_GROUP) return WSAE_ERR_GROUP;
    if (len < WSAE_COMMIT_LEN) return WSAE_ERR_PEER;
    const uint8_t *sc = body + 2, *el = body + 2 + 32;
    // 1 < scalar < r
    fe ps;
    fe_from_be(ps, sc);
    const fe one = {1, 0, 0, 0, 0, 0, 0, 0};
    if (fe_is_zero(ps) || fe_eq(ps, one) || !fe_lt(ps, N.m)) return WSAE_ERR_PEER;
    // the element: coordinates < p, on the curve (pt_from_bin); a reflected commit is refused
    wsae_point pe;
    if (!pt_from_bin(&pe, el)) return WSAE_ERR_PEER;
    if (!memcmp(sc, s->own_scalar, 32) && !memcmp(el, s->own_element, 64)) return WSAE_ERR_PEER;
    memcpy(s->peer_scalar, sc, 32);
    memcpy(s->peer_element, el, 64);
    // K = rand * (peer-scalar * PWE + PEER-ELEMENT); k = x(K)
    wsae_point t, k;
    pt_mul(&t, &s->pwe, sc);
    pt_add(&t, &t, &pe);
    uint8_t rb[32];
    fe_to_be(rb, s->rand);
    pt_mul(&k, &t, rb);
    memset(rb, 0, sizeof rb);
    uint8_t kxy[64];
    if (!pt_to_bin(kxy, &k)) return WSAE_ERR_PEER;   // the point at infinity
    // keyseed = HMAC-SHA256(<0>32, k); KCK || PMK = KDF-512(keyseed, "SAE KCK and PMK", (scalar + peer-scalar) mod r)
    static const uint8_t zero[32];
    const uint8_t *v[1] = {kxy};
    const size_t vl[1] = {32};
    uint8_t keyseed[32];
    wsae_hmac_sha256(zero, 32, 1, v, vl, keyseed);
    fe os, sum;
    fe_from_be(os, s->own_scalar);
    mod_add(sum, os, ps, &N);
    uint8_t sb[32], keys[64];
    fe_to_be(sb, sum);
    wsae_kdf_sha256(keyseed, 32, "SAE KCK and PMK", sb, 32, keys, 512);
    memcpy(s->kck, keys, 32);
    memcpy(s->pmk, keys + 32, 32);
    memcpy(s->pmkid, sb, 16);
    memset(keys, 0, sizeof keys); memset(keyseed, 0, sizeof keyseed); memset(kxy, 0, sizeof kxy);
    s->have_keys = 1;
    return WSAE_OK;
}

// confirm = HMAC-SHA256(KCK, send-confirm || scalar1 || element1 || scalar2 || element2)
static void confirm_hash(const wsae *s, uint16_t sc, int ours, uint8_t out[32])
{
    const uint8_t scb[2] = {(uint8_t)sc, (uint8_t)(sc >> 8)};
    const uint8_t *a_s = ours ? s->own_scalar : s->peer_scalar, *a_e = ours ? s->own_element : s->peer_element;
    const uint8_t *b_s = ours ? s->peer_scalar : s->own_scalar, *b_e = ours ? s->peer_element : s->own_element;
    const uint8_t *v[5] = {scb, a_s, a_e, b_s, b_e};
    const size_t vl[5] = {2, 32, 64, 32, 64};
    wsae_hmac_sha256(s->kck, 32, 5, v, vl, out);
}

long wsae_write_confirm(wsae *s, uint8_t *out, size_t cap)
{
    if (!s->have_keys || cap < WSAE_CONFIRM_LEN) return WSAE_ERR_INVAL;
    const uint16_t sc = s->send_confirm;
    out[0] = (uint8_t)sc; out[1] = (uint8_t)(sc >> 8);
    confirm_hash(s, sc, 1, out + 2);
    if (s->send_confirm < 0xffff) ++s->send_confirm;
    return WSAE_CONFIRM_LEN;
}

int wsae_check_confirm(wsae *s, const uint8_t *body, size_t len)
{
    if (!s->have_keys) return WSAE_ERR_INVAL;
    if (len < WSAE_CONFIRM_LEN) return WSAE_ERR_CONFIRM;
    const uint16_t sc = (uint16_t)(body[0] | body[1] << 8);
    uint8_t want[32];
    confirm_hash(s, sc, 0, want);
    uint8_t d = 0;
    for (int i = 0; i < 32; ++i) d |= want[i] ^ body[2 + i];
    if (d) return WSAE_ERR_CONFIRM;
    s->peer_send_confirm = sc;
    return WSAE_OK;
}

void wsae_clear(wsae *s)
{
    volatile uint8_t *p = (volatile uint8_t *)s;
    for (size_t i = 0; i < sizeof *s; ++i) p[i] = 0;
}
