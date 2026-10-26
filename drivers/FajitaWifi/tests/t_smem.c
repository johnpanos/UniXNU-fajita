// SPDX-License-Identifier: BSD-2-Clause-Patent
// SMEM + SMP2P host tests against a synthetic SMEM image, with both ends (APPS and MODEM) running over the same memory.
#include "tcheck.h"
#include "../WifiSmem.h"
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *m, size_t off, uint32_t v) {memcpy(m + off, &v, 4);}
static void put16(uint8_t *m, size_t off, uint16_t v) {memcpy(m + off, &v, 2);}
static uint32_t get32(const uint8_t *m, size_t off) {uint32_t v; memcpy(&v, m + off, 4); return v;}

static void part(uint8_t *m, size_t pt_entry, uint32_t off, uint32_t size, uint16_t h0, uint16_t h1, uint32_t cl)
{
    put32(m, pt_entry, off); put32(m, pt_entry + 4, size); put16(m, pt_entry + 12, h0); put16(m, pt_entry + 14, h1);
    put32(m, pt_entry + 16, cl);
    memcpy(m + off, "$PRT", 4); put16(m, off + 4, h0); put16(m, off + 6, h1); put32(m, off + 8, size);
    put32(m, off + 12, 32); put32(m, off + 16, size);
}

void t_smem_image(uint8_t *mem, int heap_only)
{
    memset(mem, 0, T_SMEM_SIZE);
    put32(mem, 64 + 4 * 7, (heap_only ? 11u : 12u) << 16);
    put32(mem, 192, 1);
    put32(mem, 196, 0x3000);                          // free_offset
    put32(mem, 200, T_SMEM_SIZE - 4096 - 0x3000);     // available
    if (heap_only) return;
    const size_t pt = T_SMEM_SIZE - 4096;
    memcpy(mem + pt, "$TOC", 4); put32(mem, pt + 4, 1); put32(mem, pt + 8, 2);
    part(mem, pt + 32, 0x10000, 0x40000, 0xfffe, 0xfffe, 0x80);
    part(mem, pt + 32 + 48, 0x50000, 0x20000, 0, 1, 0x80);
    memcpy(mem + pt + 32 + 2 * 48, "SIII", 4); put16(mem, pt + 32 + 2 * 48 + 16, 512);
}

static int locks, unlocks;
static int tlock(void *c) {(void)c; ++locks; return 0;}
static void tunlock(void *c) {(void)c; ++unlocks;}
static int kicks_ap, kicks_mss;
static void kick_ap(void *c) {(void)c; ++kicks_ap;}
static void kick_mss(void *c) {(void)c; ++kicks_mss;}

static void t_smem_partitioned(void)
{
    uint8_t *mem = aligned_alloc(4096, T_SMEM_SIZE);
    t_smem_image(mem, 0);
    wsmem ap, mss;
    CHECK(wsmem_init(&ap, mem, T_SMEM_SIZE, WSMEM_HOST_APPS) == 0);
    CHECK(ap.version == 12 && ap.item_count == 512 && ap.global.size == 0x40000 && ap.nparts == 1);
    CHECK(ap.part[WSMEM_HOST_MODEM].offset == 0x50000);
    CHECK(wsmem_init(&mss, mem, T_SMEM_SIZE, WSMEM_HOST_MODEM) == 0 && mss.part[WSMEM_HOST_APPS].offset == 0x50000);
    ap.lock = tlock; ap.unlock = tunlock;
    // Fixed items are refused; a new one lands in the APPS<->MODEM partition, padded to 8.
    CHECK(wsmem_alloc(&ap, 1, 3, 16) == WSMEM_ERR_INVAL);
    CHECK(wsmem_alloc(&ap, 1, 478, 30) == 0 && locks == 1 && unlocks == 1);
    CHECK(wsmem_alloc(&ap, 1, 478, 30) == WSMEM_ERR_EXIST);
    size_t sz = 0; int err = 1;
    volatile uint8_t *it = wsmem_get(&ap, 1, 478, &sz, &err);
    CHECK(it && err == 0 && sz == 30 && wsmem_offset(&ap, it) == 0x50000 + 32 + 16);
    CHECK(get32(mem, 0x50000 + 12) == 32 + 16 + 32);   // free_uncached advanced by header + ALIGN(30, 8)
    // The other end sees the same item.
    volatile uint8_t *it2 = wsmem_get(&mss, 0, 478, &sz, &err);
    CHECK(it2 == it && sz == 30);
    CHECK(wsmem_get(&ap, 1, 479, &sz, &err) == NULL && err == WSMEM_ERR_NOENT);
    // An unknown host falls back to the global partition.
    CHECK(wsmem_alloc(&ap, 7, 100, 8) == 0);
    it = wsmem_get(&ap, 7, 100, &sz, &err);
    CHECK(it && wsmem_offset(&ap, it) == 0x10000 + 32 + 16);
    CHECK(wsmem_get(&ap, 1, 100, &sz, &err) == NULL);   // not in the modem partition
    // A cached entry (grows down from the end, cacheline 0x80), as the modem allocates its fifos.
    {
        const size_t end = 0x50000 + 0x20000, step = 0x80, esz = 64;
        const size_t e = end - step;
        put16(mem, e, WSMEM_CANARY); put16(mem, e + 2, 480); put32(mem, e + 4, esz); put16(mem, e + 8, 0);
        put32(mem, 0x50000 + 16, (uint32_t)(e - esz - 0x50000));
        memset(mem + e - esz, 0x5a, esz);
        it = wsmem_get(&ap, 1, 480, &sz, &err);
        CHECK(it && sz == esz && wsmem_offset(&ap, it) == (long)(e - esz) && it[0] == 0x5a);
    }
    CHECK(wsmem_free_space(&ap, 1) == (long)(get32(mem, 0x50000 + 16) - get32(mem, 0x50000 + 12)));
    // Out of space.
    CHECK(wsmem_alloc(&ap, 1, 200, 0x20000) == WSMEM_ERR_NOSPC);
    // A broken canary is detected, not walked past.
    put16(mem, 0x50000 + 32, 0x1234);
    CHECK(wsmem_get(&ap, 1, 478, &sz, &err) == NULL && err == WSMEM_ERR_INVAL);
    CHECK(wsmem_alloc(&ap, 1, 201, 8) == WSMEM_ERR_INVAL);
    put16(mem, 0x50000 + 32, WSMEM_CANARY);
    // Bad images.
    put32(mem, 192, 0);
    CHECK(wsmem_init(&ap, mem, T_SMEM_SIZE, 0) == WSMEM_ERR_NOTINIT);
    t_smem_image(mem, 0);
    memcpy(mem + 0x50000, "$PRX", 4);
    CHECK(wsmem_init(&ap, mem, T_SMEM_SIZE, 0) == WSMEM_ERR_INVAL);
    t_smem_image(mem, 0);
    put32(mem, T_SMEM_SIZE - 4096 + 8, 0x7fffffff);   // absurd ptable count
    CHECK(wsmem_init(&ap, mem, T_SMEM_SIZE, 0) == WSMEM_ERR_INVAL);
    t_smem_image(mem, 0);
    put32(mem, 0x50000 + 12, 0x30000);                 // free_uncached past the partition
    CHECK(wsmem_init(&ap, mem, T_SMEM_SIZE, 0) == WSMEM_ERR_INVAL);
    free(mem);
}

static void t_smem_heap(void)
{
    uint8_t *mem = aligned_alloc(4096, T_SMEM_SIZE);
    t_smem_image(mem, 1);
    wsmem s;
    CHECK(wsmem_init(&s, mem, T_SMEM_SIZE, 0) == 0 && s.version == 11 && !s.global.size);
    const uint32_t avail = get32(mem, 200);
    CHECK(wsmem_alloc(&s, 1, 435, 340) == 0);
    size_t sz; int err;
    volatile uint8_t *it = wsmem_get(&s, 1, 435, &sz, &err);
    CHECK(it && sz == 344 && wsmem_offset(&s, it) == 0x3000);
    CHECK(get32(mem, 196) == 0x3000 + 344 && get32(mem, 200) == avail - 344);
    CHECK(wsmem_alloc(&s, 1, 435, 8) == WSMEM_ERR_EXIST);
    CHECK(wsmem_get(&s, 1, 436, &sz, &err) == NULL && err == WSMEM_ERR_NXIO);
    CHECK(wsmem_alloc(&s, 1, 600, 8) == WSMEM_ERR_INVAL);
    free(mem);
}

static void t_smp2p(void)
{
    uint8_t *mem = aligned_alloc(4096, T_SMEM_SIZE);
    t_smem_image(mem, 0);
    wsmem aps, mss_s;
    CHECK(wsmem_init(&aps, mem, T_SMEM_SIZE, 0) == 0 && wsmem_init(&mss_s, mem, T_SMEM_SIZE, 1) == 0);
    wsmp2p ap, mss;
    kicks_ap = kicks_mss = 0;
    // AP: out 435, in 428 (sdm845 smp2p-mpss); the modem is the mirror image.
    CHECK(wsmp2p_init(&ap, &aps, 0, 1, 435, 428, kick_mss, 0) == 0 && kicks_mss == 1);
    CHECK(wsmp2p_irq(&ap) == 0);   // the modem has not allocated its item yet
    int out = wsmp2p_add_entry(&ap, "master-kernel");
    CHECK(out == 0 && kicks_mss == 2);
    CHECK(wsmp2p_init(&mss, &mss_s, 1, 0, 428, 435, kick_ap, 0) == 0);
    int in = wsmp2p_add_entry(&mss, "slave-kernel");
    CHECK(in == 0);
    CHECK(wsmp2p_irq(&ap) == 1 && ap.negotiated && ap.ssr_ack_enabled);
    CHECK(wsmp2p_irq(&mss) == 1 && mss.negotiated);
    uint32_t v = 9, ch = 9;
    CHECK(wsmp2p_take(&ap, "slave-kernel", &v, &ch) == 0 && v == 0 && ch == 0);
    CHECK(wsmp2p_take(&ap, "nope", &v, &ch) == WSMEM_ERR_NOENT);
    // The modem raises "ready" (bit 1) then "handover" (bit 2).
    const int k = kicks_ap;
    CHECK(wsmp2p_update(&mss, in, 1u << WSMP2P_MSS_READY, 1u << WSMP2P_MSS_READY) == 0 && kicks_ap == k + 1);
    CHECK(wsmp2p_update(&mss, in, 1u << WSMP2P_MSS_READY, 1u << WSMP2P_MSS_READY) == 0 && kicks_ap == k + 1);   // no change, no kick
    wsmp2p_irq(&ap);
    CHECK(wsmp2p_take(&ap, "slave-kernel", &v, &ch) == 0 && v == 2 && ch == 2);
    CHECK(wsmp2p_take(&ap, "slave-kernel", &v, &ch) == 0 && ch == 0);
    wsmp2p_update(&mss, in, 1u << WSMP2P_MSS_HANDOVER, 1u << WSMP2P_MSS_HANDOVER);
    wsmp2p_irq(&ap);
    CHECK(wsmp2p_take(&ap, "slave-kernel", &v, &ch) == 0 && v == 0x6 && ch == 4);
    // The AP's stop bit reaches the modem.
    wsmp2p_update(&ap, out, 1u << WSMP2P_MSS_STOP, 1u << WSMP2P_MSS_STOP);
    wsmp2p_irq(&mss);
    CHECK(wsmp2p_take(&mss, "master-kernel", &v, &ch) == 0 && v == 1 && ch == 1);
    // SSR: the modem flags restart-done; the AP acks by toggling its restart-ack flag, once.
    volatile uint8_t *min = mss.out;
    *(volatile uint32_t *)(min + 16) |= 1u << WSMP2P_RESTART_DONE_BIT;
    wsmp2p_irq(&ap);
    CHECK(((*(volatile uint32_t *)(ap.out + 16)) >> WSMP2P_RESTART_ACK_BIT & 1) == 1);
    wsmp2p_irq(&ap);
    CHECK(((*(volatile uint32_t *)(ap.out + 16)) >> WSMP2P_RESTART_ACK_BIT & 1) == 1);
    // Entry table full.
    for (int i = 1; i < 16; ++i) CHECK(wsmp2p_add_entry(&ap, "e") == i);
    CHECK(wsmp2p_add_entry(&ap, "e") == WSMEM_ERR_NOSPC);
    CHECK(wsmp2p_update(&ap, 16, 1, 1) == WSMEM_ERR_INVAL);
    free(mem);
}

// ---- read-only inspection ----
typedef struct {unsigned n; unsigned item[64]; uint32_t off[64], size[64]; int cached[64];} t_items;
static void collect(void *ctx, unsigned item, uint32_t off, uint32_t size, int cached)
{
    t_items *t = ctx;
    if (t->n < 64) {t->item[t->n] = item; t->off[t->n] = off; t->size[t->n] = size; t->cached[t->n] = cached;}
    ++t->n;
}

static void t_smem_inspect(void)
{
    uint8_t *mem = aligned_alloc(4096, T_SMEM_SIZE);
    t_smem_image(mem, 0);
    wsmem ap, mss_s;
    CHECK(wsmem_init(&ap, mem, T_SMEM_SIZE, 0) == 0 && wsmem_init(&mss_s, mem, T_SMEM_SIZE, 1) == 0);
    CHECK(wsmem_ptable_count(&ap) == 2);
    wsmem_ptentry e;
    CHECK(wsmem_ptable_entry(&ap, 1, &e) == 0 && e.offset == 0x50000 && e.size == 0x20000 && e.host0 == 0 && e.host1 == 1
          && e.cacheline == 0x80);
    CHECK(wsmem_ptable_entry(&ap, 2, &e) == WSMEM_ERR_INVAL);
    // Two uncached items and one cached one in APPS<->MODEM, one in global.
    CHECK(wsmem_alloc(&ap, 1, 478, 30) == 0 && wsmem_alloc(&ap, 1, 435, 340) == 0 && wsmem_alloc(&ap, 7, 100, 8) == 0);
    const size_t end = 0x50000 + 0x20000, ce = end - 0x80;
    put16(mem, ce, WSMEM_CANARY); put16(mem, ce + 2, 428); put32(mem, ce + 4, 344); put16(mem, ce + 8, 4);
    put32(mem, 0x50000 + 16, (uint32_t)(ce - 344 - 0x50000));
    t_items t = {0};
    CHECK(wsmem_items(&ap, 1, collect, &t) == 3 && t.n == 3);
    CHECK(t.item[0] == 478 && t.size[0] == 30 && !t.cached[0] && t.item[1] == 435 && t.size[1] == 340);
    CHECK(t.item[2] == 428 && t.size[2] == 340 && t.cached[2] && t.off[2] == ce - 344);
    for (unsigned i = 0; i < 3; ++i) {   // the walk agrees with wsmem_get
        size_t sz = 0; int err = 0;
        volatile uint8_t *it = wsmem_get(&ap, 1, t.item[i], &sz, &err);
        CHECK(it && wsmem_offset(&ap, it) == (long)t.off[i] && sz == t.size[i]);
    }
    t_items g = {0};
    CHECK(wsmem_items(&ap, WSMEM_GLOBAL_HOST, collect, &g) == 1 && g.item[0] == 100);
    CHECK(wsmem_items(&ap, 3, collect, &g) == WSMEM_ERR_NOENT);   // no APPS<->3 partition (global is asked for by name)
    // SMP2P peek: the AP's outbound item after init + one entry, unchanged by the peek.
    wsmp2p p;
    CHECK(wsmp2p_init(&p, &ap, 0, 1, 435, 428, 0, 0) == 0 && wsmp2p_add_entry(&p, "master-kernel") == 0);
    wsmp2p_update(&p, 0, 1, 1);
    uint8_t *before = malloc(T_SMEM_SIZE);
    memcpy(before, mem, T_SMEM_SIZE);
    size_t sz = 0; int err = 0;
    volatile uint8_t *it = wsmem_get(&ap, 1, 435, &sz, &err);
    wsmp2p_view v;
    CHECK(wsmp2p_peek(it, sz, &v) == 0 && v.magic == WSMP2P_MAGIC && v.version == 1 && v.features == 1 && v.local_pid == 0
          && v.remote_pid == 1 && v.total == 16 && v.valid == 1 && v.nentries == 1);
    CHECK(!strcmp(v.entry[0].name, "master-kernel") && v.entry[0].value == 1);
    t_items t2 = {0};
    wsmem_items(&ap, 1, collect, &t2);
    wsmem_ptable_count(&ap);
    CHECK(!memcmp(before, mem, T_SMEM_SIZE));   // inspection never writes
    // The modem's inbound item (cached, all zero): a bad magic, reported as such.
    it = wsmem_get(&ap, 1, 428, &sz, &err);
    CHECK(wsmp2p_peek(it, sz, &v) == WSMEM_ERR_NOENT && v.magic == 0);
    CHECK(wsmp2p_peek(it, 8, &v) == WSMEM_ERR_INVAL);
    // valid > total is decoded but flagged.
    it = wsmem_get(&ap, 1, 435, &sz, &err);
    put16(mem, (size_t)wsmem_offset(&ap, it) + 12, 0);
    CHECK(wsmp2p_peek(it, sz, &v) == WSMEM_ERR_INVAL && v.nentries == 1);
    // A broken canary stops the walk with an error after the items before it.
    put16(mem, t.off[1] - 16, 0x1234);
    t_items t3 = {0};
    CHECK(wsmem_items(&ap, 1, collect, &t3) == WSMEM_ERR_INVAL && t3.n == 1);
    free(before);
    free(mem);
}


// The layout the phone's SBL leaves (smem-snap.py, 2026-10-25; NOTES-wifi.md "Lower layers"): 15 ptable entries with
// cacheline 0x20, the ptable and the header listed as host 0xffff entries, SIII with 615 items (past the 512 TOC), the
// global partition at 0x1000+0x88000 holding items above 512, partitions between other hosts, and an empty
// APPS<->MODEM partition at 0xb9000+0x7c000. Then what stage 3 will do on it: GLINK's items and the SMP2P out item.
static void t_smem_phone_layout(void)
{
    uint8_t *mem = aligned_alloc(4096, T_SMEM_SIZE);
    memset(mem, 0, T_SMEM_SIZE);
    put32(mem, 64 + 4 * 7, 0x000c0000);
    put32(mem, 192, 1); put32(mem, 196, 0x1000); put32(mem, 200, 0);
    const size_t pt = T_SMEM_SIZE - 4096;
    memcpy(mem + pt, "$TOC", 4); put32(mem, pt + 4, 1); put32(mem, pt + 8, 6);
    // {offset, size, flags, host0, host1, cacheline}
    static const uint32_t ents[6][6] = {{0x1ff000, 0x1000, 1, 0xffff, 0xffff, 0}, {0x1af000, 0x30000, 3, 0, 2, 0x20},
                                        {0x153000, 0xc000, 3, 2, 3, 0x20}, {0xb9000, 0x7c000, 3, 0, 1, 0x20},
                                        {0x1000, 0x88000, 3, 0xfffe, 0xfffe, 0x20}, {0, 0x1000, 3, 0xffff, 0xffff, 0}};
    for (unsigned i = 0; i < 6; ++i) {
        const size_t e = pt + 32 + 48 * i;
        put32(mem, e, ents[i][0]); put32(mem, e + 4, ents[i][1]); put32(mem, e + 8, ents[i][2]);
        put16(mem, e + 12, (uint16_t)ents[i][3]); put16(mem, e + 14, (uint16_t)ents[i][4]); put32(mem, e + 16, ents[i][5]);
        if (ents[i][3] == 0xffff) continue;
        const uint32_t o = ents[i][0];
        memcpy(mem + o, "$PRT", 4); put16(mem, o + 4, (uint16_t)ents[i][3]); put16(mem, o + 6, (uint16_t)ents[i][4]);
        put32(mem, o + 8, ents[i][1]); put32(mem, o + 12, 32); put32(mem, o + 16, ents[i][1]);
        if (ents[i][3] == 2) memcpy(mem + o, "XXXX", 4);   // adsp<->slpi: not ours, never even validated
    }
    memcpy(mem + pt + 32 + 6 * 48, "SIII", 4); put32(mem, pt + 32 + 6 * 48 + 4, T_SMEM_SIZE);
    put32(mem, pt + 32 + 6 * 48 + 8, 0x86000000u); put16(mem, pt + 32 + 6 * 48 + 16, 615);
    // Two global items, one above 512, with data padding (sizes that are not multiples of 8, as on the phone).
    put16(mem, 0x1020, WSMEM_CANARY); put16(mem, 0x1022, 469); put32(mem, 0x1024, 0x1000); put16(mem, 0x1028, 0);
    put16(mem, 0x2030, WSMEM_CANARY); put16(mem, 0x2032, 604); put32(mem, 0x2034, 0x390); put16(mem, 0x2038, 3);
    put32(mem, 0x1000 + 12, 0x2040 + 0x390 - 0x1000);
    wsmem s;
    CHECK(wsmem_init(&s, mem, T_SMEM_SIZE, WSMEM_HOST_APPS) == 0);
    CHECK(s.version == 12 && s.item_count == 615 && s.nparts == 2 && s.global.offset == 0x1000 && s.global.size == 0x88000);
    CHECK(s.part[1].offset == 0xb9000 && s.part[1].cacheline == 0x20 && s.part[2].size == 0x30000 && !s.part[3].size);
    t_items g = {0};
    CHECK(wsmem_items(&s, WSMEM_GLOBAL_HOST, collect, &g) == 2 && g.item[1] == 604 && g.size[1] == 0x38d && g.off[1] == 0x2040);
    size_t sz = 0; int err = 0;
    CHECK(wsmem_get(&s, 1, 604, &sz, &err) == NULL && err == WSMEM_ERR_NOENT);   // modem partition first: absent there
    CHECK(wsmem_get(&s, 7, 604, &sz, &err) && sz == 0x38d);                     // no APPS<->tz partition: global
    CHECK(wsmem_items(&s, 1, collect, &g) == 0);
    // Stage 3 on this layout: GLINK descriptor + TX fifo, the SMP2P out item, then the modem's cached RX fifo (480,
    // cacheline 0x20: header step 0x20).
    CHECK(wsmem_alloc(&s, 1, 478, 32) == 0 && wsmem_alloc(&s, 1, 479, 16384) == 0);
    wsmp2p p;
    CHECK(wsmp2p_init(&p, &s, 0, 1, 435, 428, 0, 0) == 0 && wsmp2p_add_entry(&p, "master-kernel") == 0);
    const size_t end = 0xb9000 + 0x7c000, ce = end - 0x20;
    put16(mem, ce, WSMEM_CANARY); put16(mem, ce + 2, 480); put32(mem, ce + 4, 16384); put16(mem, ce + 8, 0);
    put32(mem, 0xb9000 + 16, (uint32_t)(ce - 16384 - 0xb9000));
    t_items t = {0};
    CHECK(wsmem_items(&s, 1, collect, &t) == 4 && t.item[2] == 435 && t.item[3] == 480 && t.cached[3] && t.off[3] == ce - 16384);
    CHECK(wsmem_get(&s, 1, 480, &sz, &err) && sz == 16384);
    CHECK(wsmem_free_space(&s, 1) == (long)(ce - 16384 - 0xb9000) - (long)(32 + 16 + 32 + 16 + 16384 + 16 + 344));
    wsmp2p_view v;
    volatile uint8_t *it = wsmem_get(&s, 1, 435, &sz, &err);
    CHECK(wsmp2p_peek(it, sz, &v) == 0 && v.valid == 1 && !strcmp(v.entry[0].name, "master-kernel"));
    free(mem);
}

// The real thing: a SMEM snapshot pulled from the phone (scripts/fajita/test/smem-snap.py; .cache only, never committed).
// Only the parts the AP may read are in it (header, ptable, the APPS and global partitions); the rest is zero.
static void t_smem_real(void)
{
    if (!t_smem_img) {printf("skip t_smem_real (no SMEM snapshot)\n"); return;}
    size_t len = 0;
    uint8_t *raw = t_slurp(t_smem_img, &len);
    CHECK(raw && len == T_SMEM_SIZE);
    if (!raw || len != T_SMEM_SIZE) {free(raw); return;}
    uint8_t *mem = aligned_alloc(4096, len);
    memcpy(mem, raw, len);
    wsmem s;
    const int r = wsmem_init(&s, mem, len, WSMEM_HOST_APPS);
    printf("  real smem: init %d version %u items %u parts %u global %x+%x\n", r, s.version, s.item_count, s.nparts,
           s.global.offset, s.global.size);
    CHECK(r == 0 && s.version == WSMEM_GLOBAL_PART_VERSION && s.global.size);
    CHECK(s.part[WSMEM_HOST_MODEM].size != 0);   // APPS<->MODEM exists before the modem boots (SBL makes it)
    for (unsigned h = 0; h < WSMEM_HOST_COUNT; ++h) {
        if (!s.part[h].size) continue;
        t_items t = {0};
        const int n = wsmem_items(&s, h, collect, &t);
        CHECK(n >= 0);
        for (unsigned i = 0; i < t.n && i < 64; ++i) {
            size_t sz = 0; int err = 0;
            volatile uint8_t *it = wsmem_get(&s, h, t.item[i], &sz, &err);
            // A duplicate item number would make get return the first; real SMEM has none.
            CHECK(it && wsmem_offset(&s, it) == (long)t.off[i] && sz == t.size[i]);
        }
    }
    t_items g = {0};
    CHECK(wsmem_items(&s, WSMEM_GLOBAL_HOST, collect, &g) >= 0);
    CHECK(!memcmp(raw, mem, len));   // the walks never wrote
    // What stage 3 will write, on this copy only: GLINK's descriptor + TX fifo and the SMP2P out item.
    const long before = wsmem_free_space(&s, WSMEM_HOST_MODEM);
    CHECK(wsmem_alloc(&s, WSMEM_HOST_MODEM, 478, 32) == 0 && wsmem_alloc(&s, WSMEM_HOST_MODEM, 479, 16384) == 0);
    wsmp2p p;
    CHECK(wsmp2p_init(&p, &s, 0, 1, 435, 428, 0, 0) == 0);
    CHECK(wsmem_free_space(&s, WSMEM_HOST_MODEM) == before - (16 + 32) - (16 + 16384) - (16 + 344));
    t_items t = {0};
    CHECK(wsmem_items(&s, WSMEM_HOST_MODEM, collect, &t) == 3 && t.item[0] == 478 && t.item[2] == 435);
    free(mem);
    free(raw);
}

void t_smem(void)
{
    RUN(t_smem_partitioned);
    RUN(t_smem_heap);
    RUN(t_smp2p);
    RUN(t_smem_inspect);
    RUN(t_smem_phone_layout);
    RUN(t_smem_real);
}
