// SPDX-License-Identifier: BSD-2-Clause-Patent
// GLINK loopback: the AP end (wglink_smem_init, as on the phone) and a simulated modem end (the mirrored pipes over the
// same SMEM descriptor, with the modem allocating FIFO 1), pumped against each other.
#include "tcheck.h"
#include "../WifiSmem.h"
#include "../WifiGlink.h"
#include <stdlib.h>
#include <string.h>

static long live_allocs;
static void *t_alloc(size_t n, void *c) {(void)c; ++live_allocs; return malloc(n);}
static void t_free(void *p, size_t n, void *c) {(void)c; (void)n; --live_allocs; free(p);}
static int kicks[2];
static void kick0(void *c) {(void)c; ++kicks[0];}
static void kick1(void *c) {(void)c; ++kicks[1];}

typedef struct {uint8_t buf[16384]; size_t len; int count; int ch;} rxbox;
static void on_rx(wglink *g, int ch, const uint8_t *d, size_t n, void *ctx)
{
    (void)g;
    rxbox *b = ctx;
    if (n <= sizeof b->buf) memcpy(b->buf, d, n);
    b->len = n; b->ch = ch; ++b->count;
}
static int events[2][8];
static void on_ev(wglink *g, int ch, int ev, void *ctx) {(void)g; (void)ch; ++events[(intptr_t)ctx][ev & 7];}

typedef struct {
    uint8_t *mem;
    wsmem aps, mss_s;
    wglink ap, mss;
} rig;

static void pump(rig *r)
{
    for (int i = 0; i < 64; ++i) {
        const int a = wglink_rx(&r->ap), b = wglink_rx(&r->mss);
        CHECK(a >= 0 && b >= 0);
        if (a <= 0 && b <= 0) return;
    }
    CHECK(!"pump did not settle");
}

static void rig_up(rig *r)
{
    r->mem = aligned_alloc(4096, T_SMEM_SIZE);
    t_smem_image(r->mem, 0);
    CHECK(wsmem_init(&r->aps, r->mem, T_SMEM_SIZE, 0) == 0 && wsmem_init(&r->mss_s, r->mem, T_SMEM_SIZE, 1) == 0);
    memset(events, 0, sizeof events);
    CHECK(wglink_smem_init(&r->ap, &r->aps, 1, kick1, 0, t_alloc, t_free, 0) == 0);
    r->ap.event = on_ev; r->ap.event_ctx = (void *)0;
    // The modem: FIFO 1 is its TX; descriptor words 2/3 are its tail/head; FIFO 0 is its RX.
    CHECK(wsmem_alloc(&r->mss_s, 0, WGLINK_SMEM_FIFO1, WGLINK_FIFO_BYTES) == 0);
    size_t sz; int err;
    volatile uint32_t *desc = wsmem_get(&r->mss_s, 0, WGLINK_SMEM_DESC, &sz, &err);
    volatile uint8_t *f0 = wsmem_get(&r->mss_s, 0, WGLINK_SMEM_FIFO0, &sz, &err);
    volatile uint8_t *f1 = wsmem_get(&r->mss_s, 0, WGLINK_SMEM_FIFO1, &sz, &err);
    CHECK(desc && f0 && f1 && sz == WGLINK_FIFO_BYTES);
    wglink_pipe tx = {&desc[2], &desc[3], f1, WGLINK_FIFO_BYTES}, rx = {&desc[0], &desc[1], f0, WGLINK_FIFO_BYTES};
    CHECK(wglink_init(&r->mss, &tx, &rx, kick0, 0, t_alloc, t_free, 0) == 0);
    r->mss.event = on_ev; r->mss.event_ctx = (void *)1;
}

static void rig_down(rig *r)
{
    for (int i = 0; i < WGLINK_MAX_CH; ++i) {
        for (int k = 0; k < WGLINK_MAX_INTENTS; ++k) {
            if (r->ap.ch[i].li[k].used) t_free(r->ap.ch[i].li[k].data, 0, 0);
            if (r->mss.ch[i].li[k].used) t_free(r->mss.ch[i].li[k].data, 0, 0);
        }
    }
    free(r->mem);
}

static void t_glink_basic(void)
{
    rig r;
    live_allocs = 0;
    rig_up(&r);
    pump(&r);
    CHECK(r.ap.version_acked && r.mss.version_acked && r.ap.features == 1 && r.mss.features == 1);
    // The modem opens IPCRTR first (as on the phone); the AP answers through create_remote.
    rxbox mbox = {0}, abox = {0};
    int mch = wglink_open(&r.mss, "IPCRTR", on_rx, &mbox, 1024, 5);
    CHECK(mch >= 0);
    pump(&r);
    CHECK(events[0][WGLINK_EV_REMOTE_OPEN] == 1 && !wglink_ready(&r.mss, mch));
    int ach = wglink_find(&r.ap, "IPCRTR");
    CHECK(ach >= 0 && !wglink_ready(&r.ap, ach));
    CHECK(wglink_open(&r.ap, "IPCRTR", on_rx, &abox, 1024, 5) == ach);
    CHECK(wglink_open(&r.ap, "IPCRTR", on_rx, &abox, 1024, 5) == WGLINK_ERR_BUSY);
    pump(&r);
    CHECK(wglink_ready(&r.ap, ach) && wglink_ready(&r.mss, mch));
    CHECK(events[0][WGLINK_EV_READY] == 1 && events[1][WGLINK_EV_READY] == 1);
    CHECK(live_allocs == 10);   // 5 x 1 KiB intents each way
    int nri = 0;
    for (int k = 0; k < WGLINK_MAX_INTENTS; ++k) nri += r.ap.ch[ach].ri[k].used;
    CHECK(nri == 5);
    // Small messages both ways.
    uint8_t msg[16384];
    for (unsigned i = 0; i < sizeof msg; ++i) msg[i] = (uint8_t)(i * 7 + 3);
    CHECK(wglink_send(&r.ap, ach, msg, 100) == 0);
    pump(&r);
    CHECK(mbox.count == 1 && mbox.len == 100 && !memcmp(mbox.buf, msg, 100) && mbox.ch == mch);
    CHECK(wglink_send(&r.mss, mch, msg + 1, 1000) == 0);
    pump(&r);
    CHECK(abox.count == 1 && abox.len == 1000 && !memcmp(abox.buf, msg + 1, 1000));
    // The reusable intents come back (RX_DONE_W_REUSE).
    for (int k = 0; k < WGLINK_MAX_INTENTS; ++k) CHECK(!r.ap.ch[ach].ri[k].in_use);
    // A message larger than any intent: RX_INTENT_REQ, the remote grants a one-shot intent, retry succeeds.
    CHECK(wglink_send(&r.ap, ach, msg, 3000) == WGLINK_ERR_AGAIN);
    CHECK(wglink_send(&r.ap, ach, msg, 3000) == WGLINK_ERR_AGAIN);   // only one request outstanding
    pump(&r);
    CHECK(r.ap.ch[ach].intent_req_result == 1 && !r.ap.ch[ach].intent_req_pending);
    CHECK(live_allocs == 11);
    CHECK(wglink_send(&r.ap, ach, msg, 3000) == 0);
    pump(&r);
    CHECK(mbox.count == 2 && mbox.len == 3000 && !memcmp(mbox.buf, msg, 3000));
    CHECK(live_allocs == 10);   // one-shot intent freed after RX_DONE, and dropped on the AP side
    nri = 0;
    for (int k = 0; k < WGLINK_MAX_INTENTS; ++k) nri += r.ap.ch[ach].ri[k].used;
    CHECK(nri == 5);
    // A 12000-byte message goes as two chunks (8 KiB + rest) into one intent.
    CHECK(wglink_send(&r.mss, mch, msg, 12000) == WGLINK_ERR_AGAIN);
    pump(&r);
    const uint32_t before = r.mss.tx_cmds;
    CHECK(wglink_send(&r.mss, mch, msg, 12000) == 0 && r.mss.tx_cmds == before + 2);
    pump(&r);
    CHECK(abox.count == 2 && abox.len == 12000 && !memcmp(abox.buf, msg, 12000));
    // Too big for the fifo.
    CHECK(wglink_send(&r.ap, ach, msg, 16384) == WGLINK_ERR_INVAL && !r.ap.ch[ach].intent_req_pending);
    // Intents exhausted without pumping: five go, the sixth asks for an intent.
    for (int i = 0; i < 5; ++i) CHECK(wglink_send(&r.ap, ach, msg + i, 512) == 0);
    CHECK(wglink_send(&r.ap, ach, msg, 512) == WGLINK_ERR_AGAIN);
    pump(&r);
    CHECK(mbox.count == 2 + 5 && !memcmp(mbox.buf, msg + 4, 512));
    CHECK(wglink_send(&r.ap, ach, msg, 512) == 0);   // the granted one-shot intent (or a returned one)
    pump(&r);
    // Fifo wrap: many odd-sized messages keep their content and order.
    int ok = 1;
    for (int i = 0; i < 300; ++i) {
        const size_t n = 1 + (i * 37) % 1000;
        if (wglink_send(&r.mss, mch, msg + (i % 50), n) != 0) {ok = 0; break;}
        pump(&r);
        if (abox.len != n || memcmp(abox.buf, msg + (i % 50), n)) {ok = 0; break;}
    }
    CHECK(ok && abox.count == 2 + 300);
    CHECK(r.ap.errors == 0 && r.mss.errors == 0);
    // Close from the AP: the modem sees REMOTE_CLOSE and acks; the AP sees CLOSED and frees its intents.
    CHECK(wglink_close(&r.ap, ach) == 0);
    CHECK(wglink_send(&r.ap, ach, msg, 10) == WGLINK_ERR_NOTCONN);
    pump(&r);
    CHECK(events[1][WGLINK_EV_REMOTE_CLOSE] == 1 && events[0][WGLINK_EV_CLOSED] == 1);
    CHECK(!wglink_ready(&r.mss, mch) && live_allocs == 5);
    rig_down(&r);
}

static void t_glink_local_first(void)
{
    rig r;
    live_allocs = 0;
    rig_up(&r);
    rxbox abox = {0}, mbox = {0};
    const int ach = wglink_open(&r.ap, "test", on_rx, &abox, 256, 2);
    CHECK(ach >= 0);
    pump(&r);
    CHECK(!wglink_ready(&r.ap, ach) && events[1][WGLINK_EV_REMOTE_OPEN] == 1);
    const int mch = wglink_open(&r.mss, "test", on_rx, &mbox, 256, 2);
    pump(&r);
    CHECK(wglink_ready(&r.ap, ach) && wglink_ready(&r.mss, mch));
    CHECK(wglink_send(&r.ap, ach, "hello", 5) == 0);
    pump(&r);
    CHECK(mbox.count == 1 && mbox.len == 5 && !memcmp(mbox.buf, "hello", 5));
    // A second channel coexists.
    rxbox b2 = {0};
    const int a2 = wglink_open(&r.ap, "two", on_rx, &b2, 64, 1), m2 = wglink_open(&r.mss, "two", on_rx, &b2, 64, 1);
    pump(&r);
    CHECK(a2 >= 0 && m2 >= 0 && a2 != ach && wglink_ready(&r.ap, a2));
    CHECK(wglink_send(&r.mss, m2, "xy", 2) == 0);
    pump(&r);
    CHECK(b2.count == 1 && b2.ch == a2 && abox.count == 0);
    CHECK(wglink_open(&r.ap, "a-name-that-is-far-too-long-for-glink", on_rx, 0, 1, 1) == WGLINK_ERR_INVAL);
    rig_down(&r);
}

static void t_glink_faults(void)
{
    rig r;
    live_allocs = 0;
    rig_up(&r);
    pump(&r);
    // A version with fewer features: the AP narrows its features and re-acks.
    uint8_t m[8] = {0, 0, 1, 0, 0, 0, 0, 0};   // VERSION v1 features 0
    wglink_pipe *tx = &r.mss.tx;
    uint32_t head = *tx->head;
    memcpy((uint8_t *)tx->fifo + head, m, 8);
    *tx->head = head + 8;
    CHECK(wglink_rx(&r.ap) == 1 && r.ap.features == 0);
    pump(&r);
    CHECK(r.mss.features == 0);
    // An unknown command stops the AP's rx loop with a protocol error.
    head = *tx->head;
    m[0] = 0x55; m[1] = 0; m[2] = 0;
    memcpy((uint8_t *)tx->fifo + head, m, 8);
    *tx->head = head + 8;
    CHECK(wglink_rx(&r.ap) == WGLINK_ERR_PROTO && r.ap.last_error_cmd == 0x55);
    // Corrupt indices are refused, not followed.
    *r.ap.rx.head = 0x7fffffff;
    CHECK(wglink_rx(&r.ap) == WGLINK_ERR_PROTO);
    rig_down(&r);
    // Without the remote's FIFO 1 the AP end simply sees nothing to read.
    uint8_t *mem = aligned_alloc(4096, T_SMEM_SIZE);
    t_smem_image(mem, 0);
    wsmem s; wglink g;
    CHECK(wsmem_init(&s, mem, T_SMEM_SIZE, 0) == 0);
    CHECK(wglink_smem_init(&g, &s, 1, 0, 0, t_alloc, t_free, 0) == 0);
    CHECK(wglink_rx(&g) == 0 && g.rx.fifo == NULL);
    CHECK(wglink_tx_avail(&g) == WGLINK_FIFO_BYTES - 8 - 16);   // the VERSION sent, minus the two reserves
    free(mem);
}

void t_glink(void)
{
    RUN(t_glink_basic);
    RUN(t_glink_local_first);
    RUN(t_glink_faults);
}
