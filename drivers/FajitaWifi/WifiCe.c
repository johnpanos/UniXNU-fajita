// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 copy engines + SNOC HIF (ath10k ce.c / snoc.c, Linux v6.16). See WifiCe.h.
#include "WifiCe.h"
#include <string.h>

#define DELTA(mask, from, to) ((uint32_t)((int)(to) - (int)(from)) & (mask))
#define INCR(mask, i) (((i) + 1u) & (mask))

void wdma_init(wdma *d, void *cpu, uint64_t iova, uint32_t size)
{
    d->cpu = (uint8_t *)cpu;
    d->iova = iova;
    d->size = size;
    d->used = 0;
}

void *wdma_alloc(wdma *d, uint32_t size, uint32_t align, uint64_t *iova)
{
    if (!align) align = 8;
    const uint64_t base = d->iova + d->used;
    const uint64_t at = (base + align - 1) & ~(uint64_t)(align - 1);
    const uint64_t off = at - d->iova;
    if (off > d->size || size > d->size - off) return NULL;
    d->used = (uint32_t)(off + size);
    uint8_t *p = d->cpu + off;
    for (uint32_t i = 0; i < size; ++i) ((volatile uint8_t *)p)[i] = 0;
    if (iova) *iova = at;
    return p;
}

uint64_t wdma_iova(const wdma *d, const void *cpu)
{
    const uint8_t *p = (const uint8_t *)cpu;
    if (p < d->cpu || p >= d->cpu + d->size) return 0;
    return d->iova + (uint64_t)(p - d->cpu);
}

void *wdma_cpu(const wdma *d, uint64_t iova, uint32_t len)
{
    if (iova < d->iova || iova - d->iova > d->size || len > d->size - (iova - d->iova)) return NULL;
    return d->cpu + (iova - d->iova);
}

static uint32_t base(unsigned id) {return WCE_CE0_BASE + WCE_CE_STRIDE * id;}
static uint32_t rd(wce *ce, uint32_t off) {return ce->io.rd(ce->io.ctx, off);}
static void wr(wce *ce, uint32_t off, uint32_t v) {ce->io.wr(ce->io.ctx, off, v);}
static void barrier(wce *ce) {if (ce->io.barrier) ce->io.barrier(ce->io.ctx);}
static void rmw(wce *ce, uint32_t off, uint32_t clear, uint32_t set) {wr(ce, off, (rd(ce, off) & ~clear) | set);}

static uint32_t pow2(uint32_t n)
{
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static int has_shadow(unsigned id) {return id == 0 || id == 3 || id == 4 || id == 5 || id == 7;}

// ath10k_ce_src_ring_read_index_get / dest: DDR for DIS_INTR pipes (rri_on_ddr), the register otherwise.
static uint32_t src_rri(wce *ce, wce_pipe *p)
{
    if (p->attr & WSNOC_CE_ATTR_DIS_INTR) return ce->rri[p->id] & 0xffffu;
    return rd(ce, base(p->id) + WCE_CURRENT_SRRI);
}
static uint32_t dst_rri(wce *ce, wce_pipe *p)
{
    if (p->attr & WSNOC_CE_ATTR_DIS_INTR) return (ce->rri[p->id] >> 16) & 0xffffu;
    return rd(ce, base(p->id) + WCE_CURRENT_DRRI);
}

static volatile uint8_t *desc_at(wce_ring *r, uint32_t i) {return r->desc + (size_t)i * WCE_DESC_BYTES;}
static void put32(volatile uint8_t *p, uint32_t v) {*(volatile uint32_t *)p = v;}   // little-endian host and target
static uint16_t get16(volatile uint8_t *p) {return *(volatile uint16_t *)p;}
static void put16(volatile uint8_t *p, uint16_t v) {*(volatile uint16_t *)p = v;}

static int ring_alloc(wce *ce, wce_ring *r, uint32_t n)
{
    r->nentries = pow2(n);
    r->mask = r->nentries - 1;
    r->desc = wdma_alloc(ce->dma, r->nentries * WCE_DESC_BYTES, 64, &r->desc_iova);
    r->ctx = ce->alloc ? ce->alloc(r->nentries * sizeof(uintptr_t), ce->alloc_ctx) : NULL;
    if (!r->desc || !r->ctx) return WCE_ERR_NOMEM;
    memset(r->ctx, 0, r->nentries * sizeof(uintptr_t));
    return WCE_OK;
}

int wce_init(wce *ce, const wce_io *io, wdma *dma, const wsnoc_host_ce *cfg, uint32_t dst_limit,
             void *(*alloc)(size_t, void *), void *alloc_ctx)
{
    memset(ce, 0, sizeof *ce);
    ce->io = *io;
    ce->dma = dma;
    ce->alloc = alloc;
    ce->alloc_ctx = alloc_ctx;
    if (!cfg) cfg = wsnoc_host_ce_config;
    // ath10k_ce_alloc_rri
    ce->rri = wdma_alloc(dma, WSNOC_CE_COUNT * 4, 64, &ce->rri_iova);
    if (!ce->rri) return WCE_ERR_NOMEM;
    wr(ce, WCE_RRI_LOW, (uint32_t)ce->rri_iova);
    wr(ce, WCE_RRI_HIGH, (uint32_t)(ce->rri_iova >> 32) & WCE_DESC_ADDR_HI_MASK);
    for (unsigned i = 0; i < WSNOC_CE_COUNT; ++i) rmw(ce, base(i) + WCE_CTRL1, 0, WCE_CTRL1_RRI_UPD);
    // ath10k_snoc_init_pipes -> ath10k_ce_init_pipe
    for (unsigned i = 0; i < WSNOC_CE_COUNT; ++i) {
        wce_pipe *p = &ce->pipe[i];
        const uint32_t b = base(i);
        p->id = i;
        p->attr = cfg[i].flags;
        p->src_sz_max = cfg[i].src_sz_max;
        p->buf_sz = cfg[i].src_sz_max;   // ath10k_snoc_alloc_pipes: pipe->buf_sz = attr.src_sz_max
        if (cfg[i].src_nentries) {
            if (ring_alloc(ce, &p->src, cfg[i].src_nentries)) return WCE_ERR_NOMEM;
            p->txbuf = wdma_alloc(dma, p->src.nentries * p->src_sz_max, 64, &p->txbuf_iova);
            if (!p->txbuf) return WCE_ERR_NOMEM;
            p->src.sw_index = src_rri(ce, p) & p->src.mask;
            p->src.hw_index = p->src.sw_index;
            p->src.write_index = rd(ce, b + WCE_SR_WR_INDEX) & p->src.mask;
            wr(ce, b + WCE_SR_BA_LO, (uint32_t)p->src.desc_iova);
            wr(ce, b + WCE_SR_BA_HI, (uint32_t)(p->src.desc_iova >> 32) & WCE_DESC_ADDR_HI_MASK);
            wr(ce, b + WCE_SR_SIZE, p->src.nentries);
            rmw(ce, b + WCE_CTRL1, WCE_CTRL1_DMAX_MASK, p->src_sz_max & WCE_CTRL1_DMAX_MASK);
            rmw(ce, b + WCE_CTRL1, WCE_CTRL1_SRC_SWAP, 0);
            rmw(ce, b + WCE_SRC_WM, 0xffff0000u, 0);
            rmw(ce, b + WCE_SRC_WM, 0x0000ffffu, p->src.nentries & 0xffffu);
        }
        uint32_t dn = cfg[i].dest_nentries;
        if (dn && dst_limit && dn > dst_limit) dn = dst_limit;
        if (dn && p->buf_sz) {
            if (ring_alloc(ce, &p->dst, dn)) return WCE_ERR_NOMEM;
            p->rxbuf = wdma_alloc(dma, p->dst.nentries * p->buf_sz, 64, &p->rxbuf_iova);
            if (!p->rxbuf) return WCE_ERR_NOMEM;
            p->dst.sw_index = dst_rri(ce, p) & p->dst.mask;
            p->dst.write_index = rd(ce, b + WCE_DST_WR_INDEX) & p->dst.mask;
            wr(ce, b + WCE_DR_BA_LO, (uint32_t)p->dst.desc_iova);
            rmw(ce, b + WCE_DR_BA_HI, WCE_DESC_ADDR_HI_MASK, (uint32_t)(p->dst.desc_iova >> 32) & WCE_DESC_ADDR_HI_MASK);
            wr(ce, b + WCE_DR_SIZE, p->dst.nentries);
            rmw(ce, b + WCE_CTRL1, WCE_CTRL1_DST_SWAP, 0);
            rmw(ce, b + WCE_DST_WM, 0xffff0000u, 0);
            rmw(ce, b + WCE_DST_WM, 0x0000ffffu, p->dst.nentries & 0xffffu);
        }
    }
    // ath10k_ce_enable_interrupts: copy complete on where a callback exists and DIS_INTR is clear; watermarks off.
    // (ath10k has send/recv callbacks on CE 0..5 and 9..11.)
    for (unsigned i = 0; i < WSNOC_CE_COUNT; ++i) {
        wce_pipe *p = &ce->pipe[i];
        const int cb = i <= 5 || i >= 9;
        const uint32_t b = base(i);
        if (!(p->attr & WSNOC_CE_ATTR_DIS_INTR) && cb && (cfg[i].src_nentries || cfg[i].dest_nentries))
            rmw(ce, b + WCE_HOST_IE, 0, WCE_HOST_IE_CC);
        else
            rmw(ce, b + WCE_HOST_IE, WCE_HOST_IE_CC, 0);
        rmw(ce, b + WCE_HOST_IE, WCE_HOST_WM_MASK, 0);
    }
    return WCE_OK;
}

static int rx_post(wce *ce, wce_pipe *p, uint32_t slot)
{
    wce_ring *r = &p->dst;
    if (DELTA(r->mask, r->write_index, r->sw_index - 1) == 0) return WCE_ERR_NOSPC;
    volatile uint8_t *d = desc_at(r, r->write_index);
    const uint64_t a = (p->rxbuf_iova + (uint64_t)slot * p->buf_sz) & WCE_DESC_ADDR_MASK;
    put32(d, (uint32_t)a);
    put32(d + 4, (uint32_t)(a >> 32));
    put16(d + 8, 0);   // nbytes
    put16(d + 10, 0);
    put32(d + 12, 0);
    r->ctx[r->write_index] = slot;
    r->write_index = INCR(r->mask, r->write_index);
    barrier(ce);
    wr(ce, base(p->id) + WCE_DST_WR_INDEX, r->write_index);
    return WCE_OK;
}

int wce_start(wce *ce)
{
    for (unsigned i = 0; i < WSNOC_CE_COUNT; ++i) {
        wce_pipe *p = &ce->pipe[i];
        if (!p->dst.nentries) continue;
        // __ath10k_ce_rx_num_free_bufs worth of buffers; slot k owns rxbuf[k]
        uint32_t n = DELTA(p->dst.mask, p->dst.write_index, p->dst.sw_index - 1);
        for (uint32_t k = 0; k < n; ++k)
            if (rx_post(ce, p, k)) break;
    }
    ce->started = 1;
    return WCE_OK;
}

int wce_send(wce *ce, unsigned pipe, uint64_t iova, uint32_t len, uint32_t transfer_id, int gather, uintptr_t ctx)
{
    if (pipe >= WSNOC_CE_COUNT) return WCE_ERR_INVAL;
    wce_pipe *p = &ce->pipe[pipe];
    wce_ring *r = &p->src;
    if (!r->nentries) return WCE_ERR_INVAL;
    if (len > p->src_sz_max || len > 0xffffu) return WCE_ERR_INVAL;
    const uint32_t sw = ce->rri[pipe] & 0xffffu;   // rri_on_ddr: the room check uses the DDR read index
    if (DELTA(r->mask, r->write_index, (sw & r->mask) - 1) == 0) {
        ++p->full;
        return WCE_ERR_NOSPC;
    }
    uint32_t flags = gather ? WCE_SEND_FLAG_GATHER : 0;
    uint16_t dflags = (uint16_t)((transfer_id << WCE_DESC_META_LSB) & WCE_DESC_META_MASK);
    if (gather) dflags |= WCE_DESC_FLAG_GATHER;
    uint32_t hi = flags | ((uint32_t)(iova >> 32) & WCE_DESC_ADDR_HI_MASK);
    if (gather) hi |= WCE_DESC_GATHER_HI;
    else hi &= ~WCE_DESC_GATHER_HI;
    volatile uint8_t *d = desc_at(r, r->write_index);
    put32(d, (uint32_t)iova);
    put32(d + 4, hi);
    put16(d + 8, (uint16_t)len);
    put16(d + 10, dflags);
    put32(d + 12, 0);
    r->ctx[r->write_index] = ctx;
    r->write_index = INCR(r->mask, r->write_index);
    ++p->tx;
    if (!gather) {
        barrier(ce);
        if (has_shadow(pipe)) wr(ce, WCE_SHADOW_SRC_WR(pipe), r->write_index);
        else wr(ce, base(pipe) + WCE_SR_WR_INDEX, r->write_index);
    }
    return WCE_OK;
}

void wce_send_revert(wce *ce, unsigned pipe)
{
    wce_ring *r = &ce->pipe[pipe].src;
    if (!r->nentries || r->write_index == r->sw_index) return;
    r->write_index = (r->write_index - 1) & r->mask;
    put16(desc_at(r, r->write_index) + 8, 0);
    r->ctx[r->write_index] = 0;
    --ce->pipe[pipe].tx;
}

int wce_send_copy(wce *ce, unsigned pipe, const void *hdr, uint32_t hlen, const void *data, uint32_t len,
                  uint32_t transfer_id, uintptr_t ctx)
{
    if (pipe >= WSNOC_CE_COUNT) return WCE_ERR_INVAL;
    wce_pipe *p = &ce->pipe[pipe];
    if (!p->src.nentries || !p->txbuf) return WCE_ERR_INVAL;
    if (hlen + len > p->src_sz_max) return WCE_ERR_INVAL;
    const uint32_t slot = p->src.write_index;
    uint8_t *buf = p->txbuf + (size_t)slot * p->src_sz_max;
    volatile uint8_t *vb = buf;
    for (uint32_t i = 0; i < hlen; ++i) vb[i] = ((const uint8_t *)hdr)[i];
    for (uint32_t i = 0; i < len; ++i) vb[hlen + i] = ((const uint8_t *)data)[i];
    return wce_send(ce, pipe, p->txbuf_iova + (uint64_t)slot * p->src_sz_max, hlen + len, transfer_id, 0, ctx);
}

unsigned wce_src_free(wce *ce, unsigned pipe)
{
    wce_ring *r = &ce->pipe[pipe].src;
    if (!r->nentries) return 0;
    return DELTA(r->mask, r->write_index, r->sw_index - 1);
}

uint32_t wce_intr_summary(wce *ce)
{
    return (rd(ce, WCE_INTR_SUMMARY) & 0x00fff000u) >> 12;
}

static unsigned service(wce *ce, wce_pipe *p)
{
    unsigned n = 0;
    const uint32_t b = base(p->id);
    wr(ce, b + WCE_HOST_IS, WCE_HOST_IE_CC | WCE_HOST_WM_MASK);   // ath10k_ce_engine_int_status_clear
    // rx: _ath10k_ce_completed_recv_next_nolock_64, then the buffer goes back to the ring
    if (p->dst.nentries) {
        wce_ring *r = &p->dst;
        barrier(ce);
        for (uint32_t guard = 0; guard < r->nentries; ++guard) {
            volatile uint8_t *d = desc_at(r, r->sw_index);
            const uint16_t nbytes = get16(d + 8);
            if (!nbytes) break;
            put16(d + 8, 0);
            const uint32_t slot = (uint32_t)r->ctx[r->sw_index];
            r->sw_index = INCR(r->mask, r->sw_index);
            ++p->rx;
            ++n;
            if (nbytes > p->buf_sz) ++p->rx_bad;
            else if (ce->rx) ce->rx(ce, p->id, p->rxbuf + (size_t)slot * p->buf_sz, nbytes, ce->arg);
            rx_post(ce, p, slot);
        }
    }
    // tx: _ath10k_ce_completed_send_next_nolock_64 (rri_on_ddr: read index from ath10k_ce_src_ring_read_index_get)
    if (p->src.nentries) {
        wce_ring *r = &p->src;
        for (uint32_t guard = 0; guard < r->nentries; ++guard) {
            if (r->hw_index == r->sw_index) {
                const uint32_t ri = src_rri(ce, p);
                if (ri == 0xffffffffu) break;
                r->hw_index = ri & r->mask;
            }
            const uint32_t ri = src_rri(ce, p) & r->mask;
            if (ri == r->sw_index) break;
            const uintptr_t ctx = r->ctx[r->sw_index];
            r->ctx[r->sw_index] = 0;
            put16(desc_at(r, r->sw_index) + 8, 0);
            r->sw_index = INCR(r->mask, r->sw_index);
            ++p->tx_done;
            ++n;
            if (ce->tx_done) ce->tx_done(ce, p->id, ctx, ce->arg);
        }
    }
    return n;
}

unsigned wce_poll(wce *ce)
{
    unsigned n = 0;
    if (!ce->started) return 0;
    for (unsigned i = 0; i < WSNOC_CE_COUNT; ++i) {
        wce_pipe *p = &ce->pipe[i];
        if (p->src.nentries || p->dst.nentries) n += service(ce, p);
    }
    return n;
}
