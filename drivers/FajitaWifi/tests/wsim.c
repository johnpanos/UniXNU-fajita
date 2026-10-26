// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 copy-engine target simulator. See wsim.h.
#include "wsim.h"
#include <stdlib.h>
#include <string.h>

static uint32_t cebase(unsigned ce) {return WCE_CE0_BASE + WCE_CE_STRIDE * ce;}
uint32_t wsim_reg(wsim *s, uint32_t off) {return s->reg[off / 4];}
static void setreg(wsim *s, uint32_t off, uint32_t v) {s->reg[off / 4] = v;}

void *wsim_dma(wsim *s, uint64_t iova, uint32_t len)
{
    if (iova < s->iova || iova - s->iova > s->size || len > s->size - (iova - s->iova)) {++s->dma_faults; return NULL;}
    return s->mem + (iova - s->iova);
}

static void update_rri(wsim *s, unsigned ce)
{
    setreg(s, cebase(ce) + WCE_CURRENT_SRRI, s->srri[ce]);
    setreg(s, cebase(ce) + WCE_CURRENT_DRRI, s->drri[ce]);
    if (!(wsim_reg(s, cebase(ce) + WCE_CTRL1) & WCE_CTRL1_RRI_UPD)) return;
    const uint64_t rri = wsim_reg(s, WCE_RRI_LOW) | (uint64_t)(wsim_reg(s, WCE_RRI_HIGH) & 0x1f) << 32;
    uint32_t *w = wsim_dma(s, rri + 4u * ce, 4);
    if (w) *w = (s->srri[ce] & 0xffffu) | (s->drri[ce] & 0xffffu) << 16;
}

static void src_kick(wsim *s, unsigned ce, uint32_t write_index)
{
    if (s->stall_src[ce]) return;
    const uint32_t b = cebase(ce), n = wsim_reg(s, b + WCE_SR_SIZE);
    if (!n) return;
    const uint64_t ring = wsim_reg(s, b + WCE_SR_BA_LO) | (uint64_t)(wsim_reg(s, b + WCE_SR_BA_HI) & 0x1f) << 32;
    while (s->srri[ce] != write_index % n) {
        const uint8_t *d = wsim_dma(s, ring + 16ull * s->srri[ce], 16);
        if (!d) return;
        uint32_t lo, hi;
        uint16_t nbytes, flags;
        memcpy(&lo, d, 4); memcpy(&hi, d + 4, 4); memcpy(&nbytes, d + 8, 2); memcpy(&flags, d + 10, 2);
        const uint64_t a = lo | (uint64_t)(hi & 0x1f) << 32;
        const void *src = wsim_dma(s, a, nbytes);
        if (src && s->glen[ce] + nbytes <= sizeof s->gather[ce]) {
            memcpy(s->gather[ce] + s->glen[ce], src, nbytes);
            s->glen[ce] += nbytes;
        }
        s->srri[ce] = (s->srri[ce] + 1) % n;
        update_rri(s, ce);
        if (!(flags & WCE_DESC_FLAG_GATHER)) {
            const uint32_t len = s->glen[ce];
            s->glen[ce] = 0;
            ++s->src_msgs[ce];
            uint8_t copy[4096];
            memcpy(copy, s->gather[ce], len);
            if (s->fw) s->fw(s, ce, copy, len, (flags & WCE_DESC_META_MASK) >> WCE_DESC_META_LSB, s->fw_arg);
        }
    }
}

void wsim_drain(wsim *s, unsigned ce)
{
    s->stall_src[ce] = 0;
    src_kick(s, ce, wsim_reg(s, cebase(ce) + WCE_SR_WR_INDEX));
}

static uint32_t rd(void *ctx, uint32_t off)
{
    wsim *s = ctx;
    ++s->reads;
    if (off >= WSIM_REG_BYTES || (off & 3)) return 0xffffffffu;
    if (off == WCE_INTR_SUMMARY) {
        uint32_t sum = 0;
        for (unsigned ce = 0; ce < WSNOC_CE_COUNT; ++ce)
            if (wsim_reg(s, cebase(ce) + WCE_HOST_IS) & WCE_HOST_IE_CC) sum |= 1u << (12 + ce);
        return sum;
    }
    return s->reg[off / 4];
}

static void wr(void *ctx, uint32_t off, uint32_t v)
{
    wsim *s = ctx;
    ++s->writes;
    if (off >= WSIM_REG_BYTES || (off & 3)) return;
    if (off >= WCE_SHADOW_SRC_WR(0) && off < WCE_SHADOW_SRC_WR(WSNOC_CE_COUNT)) {
        const unsigned ce = (off - WCE_SHADOW_SRC_WR(0)) / 4;
        ++s->shadow_writes[ce];
        setreg(s, cebase(ce) + WCE_SR_WR_INDEX, v);
        src_kick(s, ce, v);
        return;
    }
    if (off >= WCE_CE0_BASE && off < WCE_CE0_BASE + WCE_CE_STRIDE * WSNOC_CE_COUNT) {
        const unsigned ce = (off - WCE_CE0_BASE) / WCE_CE_STRIDE;
        const uint32_t r = (off - WCE_CE0_BASE) % WCE_CE_STRIDE;
        if (r == WCE_HOST_IS || r == WCE_MISC_IS) {setreg(s, off, wsim_reg(s, off) & ~v); return;}
        if (r == WCE_CURRENT_SRRI || r == WCE_CURRENT_DRRI) return;   // read-only
        setreg(s, off, v);
        if (r == WCE_SR_WR_INDEX) src_kick(s, ce, v);
        if (r == WCE_CTRL1) update_rri(s, ce);
        return;
    }
    setreg(s, off, v);
}

static void barrier(void *ctx) {++((wsim *)ctx)->barriers;}

wce_io wsim_io(wsim *s)
{
    wce_io io = {s, rd, wr, barrier};
    return io;
}

void wsim_init(wsim *s, uint32_t dma_bytes)
{
    memset(s, 0, sizeof *s);
    s->reg = calloc(WSIM_REG_BYTES / 4, 4);
    s->mem = calloc(dma_bytes, 1);
    s->iova = WSIM_DMA_IOVA;
    s->size = dma_bytes;
}

void wsim_free(wsim *s)
{
    free(s->reg);
    free(s->mem);
    s->reg = NULL;
    s->mem = NULL;
}

int wsim_send(wsim *s, unsigned ce, const void *data, uint32_t len)
{
    const uint32_t b = cebase(ce), n = wsim_reg(s, b + WCE_DR_SIZE);
    if (!n) return -2;
    if (s->drri[ce] == wsim_reg(s, b + WCE_DST_WR_INDEX) % n) {++s->no_buffer; return -1;}
    const uint64_t ring = wsim_reg(s, b + WCE_DR_BA_LO) | (uint64_t)(wsim_reg(s, b + WCE_DR_BA_HI) & 0x1f) << 32;
    uint8_t *d = wsim_dma(s, ring + 16ull * s->drri[ce], 16);
    if (!d) return -2;
    uint32_t lo, hi;
    memcpy(&lo, d, 4); memcpy(&hi, d + 4, 4);
    const uint64_t a = lo | (uint64_t)(hi & 0x1f) << 32;
    const uint32_t dmax = wsim_reg(s, b + WCE_CTRL1) & WCE_CTRL1_DMAX_MASK;
    (void)dmax;
    uint8_t *dst = wsim_dma(s, a, len);
    if (!dst || !len || len > 0xffff) return -2;
    memcpy(dst, data, len);
    const uint16_t nb = (uint16_t)len;
    memcpy(d + 8, &nb, 2);
    s->drri[ce] = (s->drri[ce] + 1) % n;
    update_rri(s, ce);
    setreg(s, b + WCE_HOST_IS, wsim_reg(s, b + WCE_HOST_IS) | WCE_HOST_IE_CC);
    return 0;
}
