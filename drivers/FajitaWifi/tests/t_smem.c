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
    // The modem raises "ready" (bit 2) then "handover" (bit 3).
    const int k = kicks_ap;
    CHECK(wsmp2p_update(&mss, in, 1u << WSMP2P_MSS_READY, 1u << WSMP2P_MSS_READY) == 0 && kicks_ap == k + 1);
    CHECK(wsmp2p_update(&mss, in, 1u << WSMP2P_MSS_READY, 1u << WSMP2P_MSS_READY) == 0 && kicks_ap == k + 1);   // no change, no kick
    wsmp2p_irq(&ap);
    CHECK(wsmp2p_take(&ap, "slave-kernel", &v, &ch) == 0 && v == 4 && ch == 4);
    CHECK(wsmp2p_take(&ap, "slave-kernel", &v, &ch) == 0 && ch == 0);
    wsmp2p_update(&mss, in, 1u << WSMP2P_MSS_HANDOVER, 1u << WSMP2P_MSS_HANDOVER);
    wsmp2p_irq(&ap);
    CHECK(wsmp2p_take(&ap, "slave-kernel", &v, &ch) == 0 && v == 0xc && ch == 8);
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

void t_smem(void)
{
    RUN(t_smem_partitioned);
    RUN(t_smem_heap);
    RUN(t_smp2p);
}
