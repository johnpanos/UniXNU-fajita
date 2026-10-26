// SPDX-License-Identifier: BSD-2-Clause-Patent
// SMEM + SMP2P, after drivers/soc/qcom/smem.c and smp2p.c (Linux v6.16). See WifiSmem.h.
#include "WifiSmem.h"

// Offsets in struct smem_header / smem_ptable / smem_partition_header / smem_private_entry.
#define H_VERSION 64u
#define H_INITIALIZED 192u
#define H_FREE_OFFSET 196u
#define H_AVAILABLE 200u
#define H_RESERVED 204u
#define H_TOC 208u
#define PT_VERSION 4u
#define PT_NUM 8u
#define PT_ENTRY 32u
#define PT_ENTRY_BYTES 48u
#define PH_HOST0 4u
#define PH_HOST1 6u
#define PH_SIZE 8u
#define PH_FREE_UNCACHED 12u
#define PH_FREE_CACHED 16u

static inline void wmb_(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb ishst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}
static inline uint32_t r32(const wsmem *s, uint64_t off) {return *(volatile const uint32_t *)(s->base + off);}
static inline uint16_t r16(const wsmem *s, uint64_t off) {return *(volatile const uint16_t *)(s->base + off);}
static inline void w32(wsmem *s, uint64_t off, uint32_t v) {*(volatile uint32_t *)(s->base + off) = v;}
static inline void w16(wsmem *s, uint64_t off, uint16_t v) {*(volatile uint16_t *)(s->base + off) = v;}
static inline uint64_t align_up(uint64_t v, uint64_t a) {return a ? (v + a - 1) / a * a : v;}
static int magic_is(const wsmem *s, uint64_t off, const char m[4])
{
    for (unsigned i = 0; i < 4; ++i)
        if (s->base[off + i] != (uint8_t)m[i]) return 0;
    return 1;
}

static uint64_t ptable_off(const wsmem *s) {return s->size - WSMEM_PTABLE_BYTES;}

static int ptable_ok(const wsmem *s)
{
    const uint64_t pt = ptable_off(s);
    if (!magic_is(s, pt, "$TOC")) return WSMEM_ERR_NOENT;
    if (r32(s, pt + PT_VERSION) != 1) return WSMEM_ERR_INVAL;
    if (r32(s, pt + PT_NUM) > (WSMEM_PTABLE_BYTES - PT_ENTRY - 20) / PT_ENTRY_BYTES) return WSMEM_ERR_INVAL;
    return WSMEM_OK;
}

// qcom_smem_partition_header: magic, hosts and size must agree with the ptable entry.
static int part_header_ok(const wsmem *s, uint32_t offset, uint32_t size, uint16_t host0, uint16_t host1)
{
    if (!size || (uint64_t)offset + size > s->size || size < WSMEM_PART_HDR_BYTES) return 0;
    if (!magic_is(s, offset, "$PRT")) return 0;
    if (r16(s, offset + PH_HOST0) != host0 || r16(s, offset + PH_HOST1) != host1) return 0;
    if (r32(s, offset + PH_SIZE) != size) return 0;
    if (r32(s, offset + PH_FREE_UNCACHED) > size) return 0;
    return 1;
}

int wsmem_init(wsmem *s, volatile void *base, size_t size, unsigned local_host)
{
    s->base = (volatile uint8_t *)base;
    s->size = size;
    s->version = 0;
    s->item_count = WSMEM_ITEM_COUNT;
    s->global.size = 0;
    s->nparts = 0;
    for (unsigned i = 0; i < WSMEM_HOST_COUNT; ++i) s->part[i].size = 0;
    if (!base || size < WSMEM_HEADER_BYTES + WSMEM_PTABLE_BYTES || (size & 3) || ((uintptr_t)base & 7)) return WSMEM_ERR_INVAL;
    if (r32(s, H_INITIALIZED) != 1 || r32(s, H_RESERVED)) return WSMEM_ERR_NOTINIT;
    s->version = r32(s, H_VERSION + 4 * WSMEM_SBL_VERSION_INDEX) >> 16;
    if (s->version != WSMEM_GLOBAL_PART_VERSION) {
        if (s->version != WSMEM_GLOBAL_HEAP_VERSION) return WSMEM_ERR_INVAL;
        // Heap version: no global partition, items in the TOC; private partitions may still exist.
    }
    const int pt = ptable_ok(s);
    if (pt == WSMEM_ERR_INVAL) return pt;
    if (pt == WSMEM_OK) {
        const uint64_t ptab = ptable_off(s);
        const uint32_t n = r32(s, ptab + PT_NUM);
        const uint64_t info = ptab + PT_ENTRY + (uint64_t)n * PT_ENTRY_BYTES;
        if (magic_is(s, info, "SIII")) s->item_count = r16(s, info + 16);
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t e = ptab + PT_ENTRY + (uint64_t)i * PT_ENTRY_BYTES;
            const uint32_t off = r32(s, e), sz = r32(s, e + 4), cl = r32(s, e + 16);
            const uint16_t h0 = r16(s, e + 12), h1 = r16(s, e + 14);
            if (!off || !sz) continue;
            if (h0 == WSMEM_GLOBAL_HOST && h1 == WSMEM_GLOBAL_HOST) {
                if (s->version != WSMEM_GLOBAL_PART_VERSION) continue;
                if (s->global.size) return WSMEM_ERR_INVAL;
                if (!part_header_ok(s, off, sz, h0, h1)) return WSMEM_ERR_INVAL;
                s->global = (wsmem_part){off, sz, cl};
                continue;
            }
            unsigned remote;
            if (h0 == local_host) remote = h1;
            else if (h1 == local_host) remote = h0;
            else continue;
            if (remote >= WSMEM_HOST_COUNT || s->part[remote].size) return WSMEM_ERR_INVAL;
            if (!part_header_ok(s, off, sz, h0, h1)) return WSMEM_ERR_INVAL;
            s->part[remote] = (wsmem_part){off, sz, cl};
            ++s->nparts;
        }
    } else if (s->version == WSMEM_GLOBAL_PART_VERSION) {
        return WSMEM_ERR_INVAL;   // partitioned SMEM needs the ptable
    }
    if (s->version == WSMEM_GLOBAL_PART_VERSION && !s->global.size) return WSMEM_ERR_INVAL;
    if (s->item_count > WSMEM_ITEM_COUNT * 8) return WSMEM_ERR_INVAL;
    return WSMEM_OK;
}

static const wsmem_part *pick(const wsmem *s, unsigned host)
{
    if (host < WSMEM_HOST_COUNT && s->part[host].size) return &s->part[host];
    if (s->global.size) return &s->global;
    return 0;
}

static volatile void *get_private(wsmem *s, const wsmem_part *p, unsigned item, size_t *size, int *err)
{
    const uint64_t ph = p->offset, end_part = ph + p->size;
    uint64_t e = ph + WSMEM_PART_HDR_BYTES;
    const uint64_t end = ph + r32(s, ph + PH_FREE_UNCACHED);
    if (end > end_part) {*err = WSMEM_ERR_INVAL; return 0;}
    while (e < end) {
        if (e + WSMEM_PRIV_ENTRY_BYTES > end) {*err = WSMEM_ERR_INVAL; return 0;}
        if (r16(s, e) != WSMEM_CANARY) {*err = WSMEM_ERR_INVAL; return 0;}
        const uint32_t esz = r32(s, e + 4);
        const uint16_t pad_data = r16(s, e + 8), pad_hdr = r16(s, e + 10);
        const uint64_t itemoff = e + WSMEM_PRIV_ENTRY_BYTES + pad_hdr;
        if (esz > p->size || pad_data > esz || itemoff + esz > end_part) {*err = WSMEM_ERR_INVAL; return 0;}
        if (r16(s, e + 2) == item) {
            if (size) *size = esz - pad_data;
            return s->base + itemoff;
        }
        e = itemoff + esz;
    }
    // The cached list grows down from the end of the partition.
    const uint64_t step = align_up(WSMEM_PRIV_ENTRY_BYTES, p->cacheline ? p->cacheline : 1);
    if (step > p->size) {*err = WSMEM_ERR_INVAL; return 0;}
    const uint64_t cend = ph + r32(s, ph + PH_FREE_CACHED);
    if (cend > end_part || cend < ph) {*err = WSMEM_ERR_INVAL; return 0;}
    uint64_t c = end_part - step;
    while (c > cend) {
        if (r16(s, c) != WSMEM_CANARY) {*err = WSMEM_ERR_INVAL; return 0;}
        const uint32_t esz = r32(s, c + 4);
        const uint16_t pad_data = r16(s, c + 8);
        if (esz > p->size || pad_data > esz || c < ph + esz) {*err = WSMEM_ERR_INVAL; return 0;}
        if (r16(s, c + 2) == item) {
            if (size) *size = esz - pad_data;
            return s->base + (c - esz);
        }
        if (c < ph + esz + step) {*err = WSMEM_ERR_INVAL; return 0;}
        c = c - esz - step;
    }
    *err = WSMEM_ERR_NOENT;
    return 0;
}

static volatile void *get_global(wsmem *s, unsigned item, size_t *size, int *err)
{
    const uint64_t t = H_TOC + (uint64_t)item * 16;
    if (!r32(s, t)) {*err = WSMEM_ERR_NXIO; return 0;}
    const uint32_t off = r32(s, t + 4), sz = r32(s, t + 8), aux = r32(s, t + 12) & ~3u;
    if (aux) {*err = WSMEM_ERR_NOENT; return 0;}   // a second region (RPM msg RAM) is not mapped here
    if ((uint64_t)off + sz > s->size) {*err = WSMEM_ERR_INVAL; return 0;}
    if (size) *size = sz;
    return s->base + off;
}

volatile void *wsmem_get(wsmem *s, unsigned host, unsigned item, size_t *size, int *err)
{
    int dummy;
    if (!err) err = &dummy;
    *err = WSMEM_OK;
    if (!s->base) {*err = WSMEM_ERR_NOTINIT; return 0;}
    if (item >= s->item_count || (!pick(s, host) && item >= WSMEM_ITEM_COUNT)) {*err = WSMEM_ERR_INVAL; return 0;}
    const wsmem_part *p = pick(s, host);
    return p ? get_private(s, p, item, size, err) : get_global(s, item, size, err);
}

static int alloc_private(wsmem *s, const wsmem_part *p, unsigned item, size_t size)
{
    const uint64_t ph = p->offset, end_part = ph + p->size;
    uint64_t e = ph + WSMEM_PART_HDR_BYTES;
    const uint64_t end = ph + r32(s, ph + PH_FREE_UNCACHED), cached = ph + r32(s, ph + PH_FREE_CACHED);
    if (end > end_part || cached > end_part) return WSMEM_ERR_INVAL;
    while (e < end) {
        if (e + WSMEM_PRIV_ENTRY_BYTES > end || r16(s, e) != WSMEM_CANARY) return WSMEM_ERR_INVAL;
        if (r16(s, e + 2) == item) return WSMEM_ERR_EXIST;
        e = e + WSMEM_PRIV_ENTRY_BYTES + r16(s, e + 10) + r32(s, e + 4);
    }
    if (e > end_part) return WSMEM_ERR_INVAL;
    const uint64_t asz = align_up(size, 8);
    if (asz > 0xffffffffu || e + WSMEM_PRIV_ENTRY_BYTES + asz > cached) return WSMEM_ERR_NOSPC;
    w16(s, e, WSMEM_CANARY);
    w16(s, e + 2, (uint16_t)item);
    w32(s, e + 4, (uint32_t)asz);
    w16(s, e + 8, (uint16_t)(asz - size));
    w16(s, e + 10, 0);
    w32(s, e + 12, 0);
    wmb_();   // the entry before the free offset that publishes it
    w32(s, ph + PH_FREE_UNCACHED, (uint32_t)(e + WSMEM_PRIV_ENTRY_BYTES + asz - ph));
    return WSMEM_OK;
}

static int alloc_global(wsmem *s, unsigned item, size_t size)
{
    const uint64_t t = H_TOC + (uint64_t)item * 16;
    if (r32(s, t)) return WSMEM_ERR_EXIST;
    const uint64_t asz = align_up(size, 8);
    const uint32_t free_off = r32(s, H_FREE_OFFSET), avail = r32(s, H_AVAILABLE);
    if (asz > avail || (uint64_t)free_off + asz > s->size) return WSMEM_ERR_NOSPC;
    w32(s, t + 4, free_off);
    w32(s, t + 8, (uint32_t)asz);
    wmb_();
    w32(s, t, 1);
    w32(s, H_FREE_OFFSET, free_off + (uint32_t)asz);
    w32(s, H_AVAILABLE, avail - (uint32_t)asz);
    return WSMEM_OK;
}

int wsmem_alloc(wsmem *s, unsigned host, unsigned item, size_t size)
{
    if (!s->base) return WSMEM_ERR_NOTINIT;
    if (item < WSMEM_ITEM_LAST_FIXED || item >= s->item_count) return WSMEM_ERR_INVAL;
    const wsmem_part *p = pick(s, host);
    if (!p && item >= WSMEM_ITEM_COUNT) return WSMEM_ERR_INVAL;
    if (s->lock && s->lock(s->lock_ctx)) return WSMEM_ERR_LOCK;
    const int r = p ? alloc_private(s, p, item, size) : alloc_global(s, item, size);
    if (s->lock && s->unlock) s->unlock(s->lock_ctx);
    return r;
}

long wsmem_free_space(wsmem *s, unsigned host)
{
    if (!s->base) return WSMEM_ERR_NOTINIT;
    const wsmem_part *p = pick(s, host);
    if (!p) return r32(s, H_AVAILABLE);
    const uint32_t un = r32(s, p->offset + PH_FREE_UNCACHED), ca = r32(s, p->offset + PH_FREE_CACHED);
    if (ca > p->size || un > ca) return WSMEM_ERR_INVAL;
    return (long)(ca - un);
}

long wsmem_offset(const wsmem *s, const volatile void *p)
{
    const uintptr_t a = (uintptr_t)p, b = (uintptr_t)s->base;
    if (a < b || a >= b + s->size) return -1;
    return (long)(a - b);
}

// ---- SMP2P ----
// struct smp2p_smem_item (packed): magic@0, version u8@4 | features:24, local_pid u16@8, remote_pid u16@10,
// total_entries u16@12, valid_entries u16@14, flags u32@16, entries[16] {name[16], value u32}@20.
#define SP_VERSION 4u
#define SP_LOCAL 8u
#define SP_REMOTE 10u
#define SP_TOTAL 12u
#define SP_VALID 14u
#define SP_FLAGS 16u
#define SP_ENTRY 20u
#define SP_ENTRY_BYTES 20u

static inline uint32_t p32(volatile const uint8_t *b, unsigned off) {return *(volatile const uint32_t *)(b + off);}
static inline uint16_t p16(volatile const uint8_t *b, unsigned off) {return *(volatile const uint16_t *)(b + off);}
static inline void pw32(volatile uint8_t *b, unsigned off, uint32_t v) {*(volatile uint32_t *)(b + off) = v;}
static inline void pw16(volatile uint8_t *b, unsigned off, uint16_t v) {*(volatile uint16_t *)(b + off) = v;}

static void kick(wsmp2p *p)
{
    wmb_();
    if (p->kick) p->kick(p->kick_ctx);
}

int wsmp2p_init(wsmp2p *p, wsmem *smem, unsigned local_pid, unsigned remote_pid, unsigned out_item, unsigned in_item,
                void (*kick_fn)(void *), void *kick_ctx)
{
    p->smem = smem;
    p->local_pid = local_pid;
    p->remote_pid = remote_pid;
    p->out_item = out_item;
    p->in_item = in_item;
    p->out = p->in = 0;
    p->kick = kick_fn;
    p->kick_ctx = kick_ctx;
    p->negotiated = p->ssr_ack_enabled = p->ssr_ack = 0;
    p->in_valid = 0;
    for (unsigned i = 0; i < WSMP2P_MAX_ENTRY; ++i) p->in_last[i] = p->in_changed[i] = 0;
    int r = wsmem_alloc(smem, remote_pid, out_item, WSMP2P_ITEM_BYTES);
    if (r < 0 && r != WSMEM_ERR_EXIST) return r;
    size_t sz = 0;
    volatile uint8_t *out = (volatile uint8_t *)wsmem_get(smem, remote_pid, out_item, &sz, &r);
    if (!out) return r;
    if (sz < WSMP2P_ITEM_BYTES || ((uintptr_t)out & 3)) return WSMEM_ERR_INVAL;
    for (unsigned i = 0; i < WSMP2P_ITEM_BYTES; i += 4) pw32(out, i, 0);
    pw32(out, 0, WSMP2P_MAGIC);
    pw16(out, SP_LOCAL, (uint16_t)local_pid);
    pw16(out, SP_REMOTE, (uint16_t)remote_pid);
    pw16(out, SP_TOTAL, WSMP2P_MAX_ENTRY);
    pw16(out, SP_VALID, 0);
    pw32(out, SP_VERSION, (uint32_t)WSMP2P_FEATURE_SSR_ACK << 8);   // features, version still 0
    wmb_();
    out[SP_VERSION] = 1;   // publish
    p->out = out;
    kick(p);
    return WSMEM_OK;
}

int wsmp2p_add_entry(wsmp2p *p, const char *name)
{
    if (!p->out) return WSMEM_ERR_NOTINIT;
    const unsigned n = p16(p->out, SP_VALID);
    if (n >= WSMP2P_MAX_ENTRY) return WSMEM_ERR_NOSPC;
    const unsigned e = SP_ENTRY + n * SP_ENTRY_BYTES;
    uint8_t buf[WSMP2P_NAME] = {0};
    for (unsigned i = 0; i + 1 < WSMP2P_NAME && name[i]; ++i) buf[i] = (uint8_t)name[i];
    for (unsigned i = 0; i < WSMP2P_NAME; i += 4)
        pw32(p->out, e + i, (uint32_t)buf[i] | (uint32_t)buf[i + 1] << 8 | (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24);
    pw32(p->out, e + WSMP2P_NAME, 0);
    wmb_();
    pw16(p->out, SP_VALID, (uint16_t)(n + 1));
    kick(p);
    return (int)n;
}

int wsmp2p_update(wsmp2p *p, int entry, uint32_t mask, uint32_t value)
{
    if (!p->out || entry < 0 || (unsigned)entry >= p16(p->out, SP_VALID)) return WSMEM_ERR_INVAL;
    const unsigned off = SP_ENTRY + (unsigned)entry * SP_ENTRY_BYTES + WSMP2P_NAME;
    const uint32_t orig = p32(p->out, off), v = (orig & ~mask) | value;
    pw32(p->out, off, v);
    if (v != orig) kick(p);
    return WSMEM_OK;
}

int wsmp2p_irq(wsmp2p *p)
{
    if (!p->out) return 0;
    if (!p->in) {
        size_t sz = 0;
        int err = 0;
        volatile uint8_t *in = (volatile uint8_t *)wsmem_get(p->smem, p->remote_pid, p->in_item, &sz, &err);
        if (!in || sz < WSMP2P_ITEM_BYTES || ((uintptr_t)in & 3)) return 0;
        if (p32(in, 0) != WSMP2P_MAGIC) return 0;
        p->in = in;
    }
    if (!p->negotiated) {
        const uint32_t inw = p32(p->in, SP_VERSION), outw = p32(p->out, SP_VERSION);
        if ((inw & 0xff) && (inw & 0xff) == (outw & 0xff)) {
            const uint32_t feat = (outw >> 8) & (inw >> 8);
            pw32(p->out, SP_VERSION, (outw & 0xff) | feat << 8);
            p->ssr_ack_enabled = (feat & WSMP2P_FEATURE_SSR_ACK) != 0;
            p->negotiated = 1;
        }
    }
    if (!p->negotiated) return 0;
    int ack = 0;
    if (p->ssr_ack_enabled) {
        const int restart = (p32(p->in, SP_FLAGS) >> WSMP2P_RESTART_DONE_BIT) & 1;
        ack = restart != p->ssr_ack;
    }
    unsigned valid = p16(p->in, SP_VALID);
    if (valid > WSMP2P_MAX_ENTRY) valid = WSMP2P_MAX_ENTRY;
    for (unsigned i = 0; i < valid; ++i) {
        const uint32_t v = p32(p->in, SP_ENTRY + i * SP_ENTRY_BYTES + WSMP2P_NAME);
        if (i >= p->in_valid) p->in_last[i] = 0;   // a new entry: bits set from 0 count as changes
        p->in_changed[i] |= v ^ p->in_last[i];
        p->in_last[i] = v;
    }
    if (valid > p->in_valid) p->in_valid = valid;
    if (ack) {
        p->ssr_ack = !p->ssr_ack;
        uint32_t f = p32(p->out, SP_FLAGS) & ~(1u << WSMP2P_RESTART_ACK_BIT);
        if (p->ssr_ack) f |= 1u << WSMP2P_RESTART_ACK_BIT;
        pw32(p->out, SP_FLAGS, f);
        kick(p);
    }
    return 1;
}

int wsmp2p_take(wsmp2p *p, const char *name, uint32_t *value, uint32_t *changed)
{
    if (!p->in) return WSMEM_ERR_NOENT;
    for (unsigned i = 0; i < p->in_valid; ++i) {
        const unsigned e = SP_ENTRY + i * SP_ENTRY_BYTES;
        unsigned k = 0;
        for (; k < WSMP2P_NAME; ++k) {
            const char c = (char)p->in[e + k];
            if (c != name[k]) break;
            if (!c) {k = WSMP2P_NAME; break;}
        }
        if (k < WSMP2P_NAME) continue;
        if (value) *value = p->in_last[i];
        if (changed) *changed = p->in_changed[i];
        p->in_changed[i] = 0;
        return WSMEM_OK;
    }
    return WSMEM_ERR_NOENT;
}
