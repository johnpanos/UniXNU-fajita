// SPDX-License-Identifier: BSD-2-Clause-Patent
// GLINK over SMEM + the native protocol, after qcom_glink_smem.c and qcom_glink_native.c (Linux v6.16). See WifiGlink.h.
#include "WifiGlink.h"

#define CMD_VERSION 0u
#define CMD_VERSION_ACK 1u
#define CMD_OPEN 2u
#define CMD_CLOSE 3u
#define CMD_OPEN_ACK 4u
#define CMD_INTENT 5u
#define CMD_RX_DONE 6u
#define CMD_RX_INTENT_REQ 7u
#define CMD_RX_INTENT_REQ_ACK 8u
#define CMD_TX_DATA 9u
#define CMD_CLOSE_ACK 11u
#define CMD_TX_DATA_CONT 12u
#define CMD_READ_NOTIF 13u
#define CMD_RX_DONE_W_REUSE 14u
#define CMD_SIGNALS 15u
#define VERSION_1 1u
#define FIFO_FULL_RESERVE 8u
#define TX_BLOCKED_CMD_RESERVE 8u

static inline void wmb_(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb ish" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline uint32_t align8(uint32_t v) {return (v + 7u) & ~7u;}
static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i);}
static uint16_t get16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void zero(void *p, size_t n) {uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;}

// FIFO copies: 32-bit stores/loads where the FIFO offset is word aligned, bytes otherwise (never unaligned accesses).
static void fifo_write(volatile uint8_t *fifo, uint32_t off, const uint8_t *src, uint32_t n)
{
    while (n && (off & 3)) {fifo[off++] = *src++; --n;}
    while (n >= 4) {*(volatile uint32_t *)(fifo + off) = get32(src); off += 4; src += 4; n -= 4;}
    while (n) {fifo[off++] = *src++; --n;}
}
static void fifo_read(const volatile uint8_t *fifo, uint32_t off, uint8_t *dst, uint32_t n)
{
    while (n && (off & 3)) {*dst++ = fifo[off++]; --n;}
    while (n >= 4) {put32(dst, *(const volatile uint32_t *)(fifo + off)); off += 4; dst += 4; n -= 4;}
    while (n) {*dst++ = fifo[off++]; --n;}
}

static int rx_ready(wglink *g)
{
    if (g->rx.fifo) return 1;
    if (!g->smem) return 0;
    size_t len = 0;
    int err = 0;
    volatile uint8_t *f = (volatile uint8_t *)wsmem_get(g->smem, g->remote_pid, g->rx_item, &len, &err);
    if (!f || len < 64 || len > 0x100000 || ((uintptr_t)f & 7)) return 0;
    g->rx.fifo = f;
    g->rx.length = (uint32_t)len;
    return 1;
}

// Bytes in the RX fifo; 0xffffffff if the indices are corrupt.
static uint32_t rx_avail(wglink *g)
{
    if (!rx_ready(g)) return 0;
    const uint32_t head = *g->rx.head, tail = *g->rx.tail;
    if (head >= g->rx.length || tail >= g->rx.length) return 0xffffffffu;
    return head < tail ? g->rx.length - tail + head : head - tail;
}
static void rx_peek(wglink *g, uint32_t offset, void *dst, uint32_t n)
{
    uint32_t tail = *g->rx.tail + offset;
    while (tail >= g->rx.length) tail -= g->rx.length;
    const uint32_t first = n < g->rx.length - tail ? n : g->rx.length - tail;
    fifo_read(g->rx.fifo, tail, (uint8_t *)dst, first);
    if (first != n) fifo_read(g->rx.fifo, 0, (uint8_t *)dst + first, n - first);
}
static void rx_advance(wglink *g, uint32_t n)
{
    uint32_t tail = *g->rx.tail + n;
    while (tail >= g->rx.length) tail -= g->rx.length;
    *g->rx.tail = tail;
}

size_t wglink_tx_avail(wglink *g)
{
    const uint32_t head = *g->tx.head, tail = *g->tx.tail;
    if (head >= g->tx.length || tail >= g->tx.length) return 0;
    uint32_t avail = tail <= head ? g->tx.length - head + tail : tail - head;
    return avail < FIFO_FULL_RESERVE + TX_BLOCKED_CMD_RESERVE ? 0 : avail - FIFO_FULL_RESERVE - TX_BLOCKED_CMD_RESERVE;
}
static uint32_t tx_write_one(wglink *g, uint32_t head, const uint8_t *data, uint32_t n)
{
    const uint32_t first = n < g->tx.length - head ? n : g->tx.length - head;
    if (first) fifo_write(g->tx.fifo, head, data, first);
    if (first != n) fifo_write(g->tx.fifo, 0, data + first, n - first);
    head += n;
    if (head >= g->tx.length) head -= g->tx.length;
    return head;
}
static void tx_write(wglink *g, const void *hdr, uint32_t hlen, const void *data, uint32_t dlen)
{
    uint32_t head = *g->tx.head;
    head = tx_write_one(g, head, (const uint8_t *)hdr, hlen);
    if (dlen) head = tx_write_one(g, head, (const uint8_t *)data, dlen);
    head = align8(head);
    if (head >= g->tx.length) head -= g->tx.length;
    wmb_();   // fifo contents before the head
    *g->tx.head = head;
    ++g->tx_cmds;
}
static void tx_kick(wglink *g)
{
    wmb_();
    if (g->kick) g->kick(g->kick_ctx);
}

// The fifo is full: ask the remote, once, to kick us when it has read (uses the TX_BLOCKED_CMD_RESERVE bytes).
static void blocked(wglink *g)
{
    if (g->read_notify_sent) return;
    uint8_t m[8];
    put16(m, CMD_READ_NOTIF); put16(m + 2, 0); put32(m + 4, 0);
    tx_write(g, m, 8, 0, 0);
    tx_kick(g);
    g->read_notify_sent = 1;
}

// qcom_glink_tx without waiting: AGAIN when the fifo lacks room (after one READ_NOTIF asking the remote to kick us).
static int tx(wglink *g, const void *hdr, uint32_t hlen, const void *data, uint32_t dlen)
{
    const uint32_t tlen = hlen + dlen;
    if (tlen >= g->tx.length) return WGLINK_ERR_INVAL;
    if (wglink_tx_avail(g) < align8(tlen)) {
        blocked(g);
        return WGLINK_ERR_AGAIN;
    }
    g->read_notify_sent = 0;
    if (hlen >= 8) {
        const uint8_t *hb = (const uint8_t *)hdr;
        const unsigned i = g->ntrace++ & 63;
        g->trace[i].dir = 't'; g->trace[i].cmd = get16(hb); g->trace[i].p1 = get16(hb + 2); g->trace[i].p2 = get32(hb + 4);
        g->trace[i].len = tlen;
    }
    tx_write(g, hdr, hlen, data, dlen);
    tx_kick(g);
    return WGLINK_OK;
}
static int tx_cmd(wglink *g, uint16_t cmd, uint16_t p1, uint32_t p2)
{
    uint8_t m[8];
    put16(m, cmd); put16(m + 2, p1); put32(m + 4, p2);
    return tx(g, m, 8, 0, 0);
}

static int common_init(wglink *g)
{
    g->features = WGLINK_FEATURES;
    g->version_acked = 0;
    g->read_notify_sent = 0;
    g->next_lcid = 1;
    g->event = 0;
    g->event_ctx = 0;
    zero(g->ch, sizeof g->ch);
    g->rx_cmds = g->tx_cmds = g->rx_msgs = g->tx_msgs = g->errors = g->last_error_cmd = 0;
    if (!g->tx.fifo || !g->tx.head || !g->tx.tail || g->tx.length < 64 || (g->tx.length & 7)) return WGLINK_ERR_INVAL;
    return tx_cmd(g, CMD_VERSION, VERSION_1, g->features);
}

int wglink_init(wglink *g, const wglink_pipe *txp, const wglink_pipe *rxp, void (*kick)(void *), void *kick_ctx,
                void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *alloc_ctx)
{
    g->tx = *txp;
    g->rx = *rxp;
    g->smem = 0;
    g->kick = kick; g->kick_ctx = kick_ctx;
    g->alloc = alloc; g->free = free_fn; g->alloc_ctx = alloc_ctx;
    return common_init(g);
}

int wglink_smem_init(wglink *g, wsmem *smem, unsigned remote_pid, void (*kick)(void *), void *kick_ctx,
                     void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *alloc_ctx)
{
    int r = wsmem_alloc(smem, remote_pid, WGLINK_SMEM_DESC, 32);
    if (r && r != WSMEM_ERR_EXIST) return r;
    size_t size = 0;
    volatile uint32_t *desc = (volatile uint32_t *)wsmem_get(smem, remote_pid, WGLINK_SMEM_DESC, &size, &r);
    if (!desc) return r;
    if (size != 32 || ((uintptr_t)desc & 3)) return WGLINK_ERR_INVAL;
    r = wsmem_alloc(smem, remote_pid, WGLINK_SMEM_FIFO0, WGLINK_FIFO_BYTES);
    if (r && r != WSMEM_ERR_EXIST) return r;
    volatile uint8_t *fifo = (volatile uint8_t *)wsmem_get(smem, remote_pid, WGLINK_SMEM_FIFO0, &size, &r);
    if (!fifo) return r;
    g->tx = (wglink_pipe){&desc[0], &desc[1], fifo, (uint32_t)size};
    g->rx = (wglink_pipe){&desc[2], &desc[3], 0, 0};
    g->smem = smem;
    g->remote_pid = remote_pid;
    g->rx_item = WGLINK_SMEM_FIFO1;
    g->kick = kick; g->kick_ctx = kick_ctx;
    g->alloc = alloc; g->free = free_fn; g->alloc_ctx = alloc_ctx;
    return common_init(g);
}

static int by_lcid(const wglink *g, uint16_t lcid)
{
    if (!lcid) return -1;
    for (int i = 0; i < WGLINK_MAX_CH; ++i) if (g->ch[i].used && g->ch[i].lcid == lcid) return i;
    return -1;
}
static int by_rcid(const wglink *g, uint16_t rcid)
{
    if (!rcid) return -1;
    for (int i = 0; i < WGLINK_MAX_CH; ++i) if (g->ch[i].used && g->ch[i].rcid == rcid) return i;
    return -1;
}
static int name_eq(const char *a, const char *b)
{
    for (unsigned i = 0; i < WGLINK_NAME; ++i) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}
int wglink_find(const wglink *g, const char *name)
{
    for (int i = 0; i < WGLINK_MAX_CH; ++i) if (g->ch[i].used && name_eq(g->ch[i].name, name)) return i;
    return -1;
}
static int new_channel(wglink *g, const char *name)
{
    for (int i = 0; i < WGLINK_MAX_CH; ++i) {
        if (g->ch[i].used) continue;
        wglink_channel *c = &g->ch[i];
        zero(c, sizeof *c);
        c->used = 1;
        c->filling = -1;
        c->intent_req_result = -1;
        c->next_liid = 1;
        unsigned k = 0;
        for (; k + 1 < WGLINK_NAME && name[k]; ++k) c->name[k] = name[k];
        c->name[k] = 0;
        return i;
    }
    return -1;
}
static void free_local_intents(wglink *g, wglink_channel *c)
{
    for (int i = 0; i < WGLINK_MAX_INTENTS; ++i) {
        if (c->li[i].used && c->li[i].data && g->free) g->free(c->li[i].data, c->li[i].size, g->alloc_ctx);
        c->li[i].used = 0;
        c->li[i].data = 0;
    }
    c->filling = -1;
}
static void maybe_release(wglink *g, int ch)
{
    wglink_channel *c = &g->ch[ch];
    if (!c->lcid && !c->rcid) {free_local_intents(g, c); c->used = 0;}
}

// qcom_glink_alloc_intent + qcom_glink_advertise_intent
static int add_intent(wglink *g, int ch, uint32_t size, int reuse)
{
    wglink_channel *c = &g->ch[ch];
    int slot = -1;
    for (int i = 0; i < WGLINK_MAX_INTENTS; ++i) if (!c->li[i].used) {slot = i; break;}
    if (slot < 0 || !g->alloc || !size) return WGLINK_ERR_NOMEM;
    uint8_t *data = (uint8_t *)g->alloc(size, g->alloc_ctx);
    if (!data) return WGLINK_ERR_NOMEM;
    wglink_lintent *li = &c->li[slot];
    li->id = c->next_liid++;
    if (!c->next_liid) c->next_liid = 1;
    li->size = size;
    li->offset = 0;
    li->data = data;
    li->reuse = reuse;
    li->used = 1;
    uint8_t m[16];
    put16(m, CMD_INTENT); put16(m + 2, c->lcid); put32(m + 4, 1); put32(m + 8, size); put32(m + 12, li->id);
    const int r = tx(g, m, 16, 0, 0);
    if (r) {g->free(data, size, g->alloc_ctx); li->used = 0; li->data = 0;}
    return r;
}

static void check_ready(wglink *g, int ch)
{
    wglink_channel *c = &g->ch[ch];
    if (c->ready || !c->want || !c->open_acked || !c->rcid || !c->ack_sent) return;
    c->ready = 1;
    // qcom_glink_announce_create: the default intents.
    for (unsigned i = 0; i < c->intent_count; ++i) if (add_intent(g, ch, c->intent_size, 1)) {++g->errors; break;}
    if (g->event) g->event(g, ch, WGLINK_EV_READY, g->event_ctx);
}

int wglink_open(wglink *g, const char *name, wglink_rx_fn rx, void *rx_ctx, unsigned intent_size, unsigned intent_count)
{
    if (!name || !name[0]) return WGLINK_ERR_INVAL;
    unsigned nlen = 0;
    while (name[nlen]) ++nlen;
    if (nlen + 1 > WGLINK_NAME) return WGLINK_ERR_INVAL;
    int ch = wglink_find(g, name);
    if (ch >= 0 && g->ch[ch].want) return WGLINK_ERR_BUSY;
    if (ch < 0) ch = new_channel(g, name);
    if (ch < 0) return WGLINK_ERR_NOMEM;
    wglink_channel *c = &g->ch[ch];
    c->want = 1;
    c->rx = rx;
    c->rx_ctx = rx_ctx;
    c->intent_size = intent_size ? intent_size : 1024;
    c->intent_count = intent_count > WGLINK_MAX_INTENTS ? WGLINK_MAX_INTENTS : intent_count;
    // lcid: cyclic from 1
    for (unsigned tries = 0; tries < 0xffff; ++tries) {
        uint16_t id = g->next_lcid++;
        if (!g->next_lcid) g->next_lcid = 1;
        if (id && by_lcid(g, id) < 0) {c->lcid = id; break;}
    }
    if (c->rcid) {   // qcom_glink_create_remote: ack the remote's open first
        if (tx_cmd(g, CMD_OPEN_ACK, c->rcid, 0) == 0) c->ack_sent = 1;
    }
    uint8_t m[8 + WGLINK_NAME];
    zero(m, sizeof m);
    put16(m, CMD_OPEN); put16(m + 2, c->lcid); put32(m + 4, nlen + 1);
    for (unsigned i = 0; i < nlen; ++i) m[8 + i] = (uint8_t)name[i];
    const int r = tx(g, m, align8(8 + nlen + 1), 0, 0);
    if (r) {c->want = 0; c->lcid = 0; maybe_release(g, ch); return r;}
    c->open_sent = 1;
    return ch;
}

int wglink_close(wglink *g, int ch)
{
    if (ch < 0 || ch >= WGLINK_MAX_CH || !g->ch[ch].used || !g->ch[ch].lcid) return WGLINK_ERR_INVAL;
    wglink_channel *c = &g->ch[ch];
    const int r = tx_cmd(g, CMD_CLOSE, c->lcid, 0);
    if (r) return r;
    c->closing = 1;
    c->ready = 0;
    c->want = 0;
    return WGLINK_OK;
}

int wglink_ready(const wglink *g, int ch)
{
    return ch >= 0 && ch < WGLINK_MAX_CH && g->ch[ch].used && g->ch[ch].ready;
}

int wglink_send(wglink *g, int ch, const void *data, size_t len)
{
    if (!wglink_ready(g, ch)) return WGLINK_ERR_NOTCONN;
    if (!len || len > 0x100000) return WGLINK_ERR_INVAL;
    // All chunks must fit in the fifo at once (no partial message is ever left in it).
    size_t need = 0;
    for (size_t off = 0; off < len;) {
        const size_t chunk = len - off > WGLINK_CHUNK ? WGLINK_CHUNK : len - off;
        need += align8((uint32_t)(16 + chunk));
        off += chunk;
    }
    if (need + FIFO_FULL_RESERVE + TX_BLOCKED_CMD_RESERVE >= g->tx.length) return WGLINK_ERR_INVAL;
    wglink_channel *c = &g->ch[ch];
    // __qcom_glink_send: the smallest free remote intent that fits
    int best = -1;
    for (int i = 0; i < WGLINK_MAX_INTENTS; ++i) {
        const wglink_rintent *r = &c->ri[i];
        if (!r->used || r->in_use || r->size < len) continue;
        if (best < 0 || r->size < c->ri[best].size) best = i;
        if (r->size == len) break;
    }
    if (best < 0) {
        if (!c->intent_req_pending) {
            uint8_t m[8];
            put16(m, CMD_RX_INTENT_REQ); put16(m + 2, c->lcid); put32(m + 4, (uint32_t)len);
            if (tx(g, m, 8, 0, 0) == 0) {c->intent_req_pending = 1; c->intent_req_result = -1;}
        }
        return WGLINK_ERR_AGAIN;
    }
    if (wglink_tx_avail(g) < need) {
        blocked(g);
        return WGLINK_ERR_AGAIN;
    }
    const uint8_t *p = (const uint8_t *)data;
    for (size_t off = 0; off < len;) {
        const size_t chunk = len - off > WGLINK_CHUNK ? WGLINK_CHUNK : len - off;
        uint8_t h[16];
        put16(h, off ? CMD_TX_DATA_CONT : CMD_TX_DATA); put16(h + 2, c->lcid); put32(h + 4, c->ri[best].id);
        put32(h + 8, (uint32_t)chunk); put32(h + 12, (uint32_t)(len - off - chunk));
        const int r = tx(g, h, 16, p + off, (uint32_t)chunk);
        if (r) return r;   // cannot happen: space was checked
        off += chunk;
    }
    c->ri[best].in_use = 1;
    ++g->tx_msgs;
    return WGLINK_OK;
}

// ---- receive ----
static void rx_open(wglink *g, uint16_t rcid, const char *name)
{
    if (by_rcid(g, rcid) >= 0) {++g->errors; return;}
    int ch = -1;
    for (int i = 0; i < WGLINK_MAX_CH; ++i)
        if (g->ch[i].used && !g->ch[i].rcid && name_eq(g->ch[i].name, name)) {ch = i; break;}
    if (ch < 0) ch = new_channel(g, name);
    if (ch < 0) {++g->errors; return;}
    wglink_channel *c = &g->ch[ch];
    c->rcid = rcid;
    if (c->want && c->open_sent) {   // create_local: the remote's open completes ours
        if (tx_cmd(g, CMD_OPEN_ACK, rcid, 0) == 0) c->ack_sent = 1;
        check_ready(g, ch);
    } else if (g->event) {
        g->event(g, ch, WGLINK_EV_REMOTE_OPEN, g->event_ctx);
    }
}

static void rx_data(wglink *g, const uint8_t hdr[16])
{
    const uint16_t rcid = get16(hdr + 2);
    const uint32_t liid = get32(hdr + 4), chunk = get32(hdr + 8), left = get32(hdr + 12);
    const int ch = by_rcid(g, rcid);
    if (ch < 0) {++g->errors; return;}
    wglink_channel *c = &g->ch[ch];
    wglink_lintent *li = 0;
    for (int i = 0; i < WGLINK_MAX_INTENTS; ++i) if (c->li[i].used && c->li[i].id == liid) {li = &c->li[i]; break;}
    if (!li || li->size - li->offset < chunk) {++g->errors; return;}
    rx_peek(g, 16, li->data + li->offset, chunk);
    li->offset += chunk;
    if (left) return;
    ++g->rx_msgs;
    const uint32_t n = li->offset;
    li->offset = 0;
    if (c->rx) c->rx(g, ch, li->data, n, c->rx_ctx);
    // qcom_glink_rx_done
    if (li->reuse) {
        tx_cmd(g, CMD_RX_DONE_W_REUSE, c->lcid, li->id);
    } else {
        tx_cmd(g, CMD_RX_DONE, c->lcid, li->id);
        if (g->free) g->free(li->data, li->size, g->alloc_ctx);
        li->data = 0;
        li->used = 0;
    }
}

int wglink_rx(wglink *g)
{
    int handled = 0;
    for (;;) {
        const uint32_t avail = rx_avail(g);
        if (avail == 0xffffffffu) return WGLINK_ERR_PROTO;
        if (avail < 8) break;
        uint8_t m[16];
        rx_peek(g, 0, m, 8);
        const uint16_t cmd = get16(m), p1 = get16(m + 2);
        const uint32_t p2 = get32(m + 4);
        int ch;
        {   // trace every command once (a partial message is re-peeked on the next call: skip the repeats)
            const unsigned last = (g->ntrace - 1) & 63;
            if (!(g->trace[last].dir == 'r' && g->trace[last].cmd == cmd && g->trace[last].p1 == p1 && g->trace[last].p2 == p2 &&
                  g->trace[last].len == avail)) {
                const unsigned i = g->ntrace++ & 63;
                g->trace[i].dir = 'r'; g->trace[i].cmd = cmd; g->trace[i].p1 = p1; g->trace[i].p2 = p2; g->trace[i].len = avail;
            }
        }
        switch (cmd) {
        case CMD_VERSION:
            rx_advance(g, 8);
            if (p1 == 0) break;
            if (p1 == VERSION_1) g->features &= p2;
            tx_cmd(g, CMD_VERSION_ACK, VERSION_1, g->features);
            break;
        case CMD_VERSION_ACK:
            rx_advance(g, 8);
            if (p1 == 0) break;
            if (p1 == VERSION_1 && p2 == g->features) {g->version_acked = 1; break;}
            if (p1 == VERSION_1) g->features &= p2;
            tx_cmd(g, CMD_VERSION, VERSION_1, g->features);
            break;
        case CMD_OPEN: {
            const uint32_t nlen = p2 & 0xffff, total = align8(8 + nlen);
            if (avail < total) return handled;
            char name[WGLINK_NAME];
            uint8_t raw[WGLINK_NAME];
            const uint32_t take = nlen < WGLINK_NAME ? nlen : WGLINK_NAME;
            rx_peek(g, 8, raw, take);
            unsigned k = 0;
            for (; k < take && k + 1 < WGLINK_NAME && raw[k]; ++k) name[k] = (char)raw[k];
            name[k] = 0;
            rx_advance(g, total);
            rx_open(g, p1, name);
            break;
        }
        case CMD_OPEN_ACK:
            rx_advance(g, 8);
            ch = by_lcid(g, p1);
            if (ch < 0) {++g->errors; g->last_error_cmd = cmd; return WGLINK_ERR_PROTO;}   // Linux stops too (-EINVAL)
            g->ch[ch].open_acked = 1;
            check_ready(g, ch);
            break;
        case CMD_CLOSE:
            rx_advance(g, 8);
            ch = by_rcid(g, p1);
            if (ch < 0) {++g->errors; break;}
            tx_cmd(g, CMD_CLOSE_ACK, p1, 0);
            g->ch[ch].rcid = 0;
            g->ch[ch].ready = 0;
            g->ch[ch].ack_sent = 0;
            zero(g->ch[ch].ri, sizeof g->ch[ch].ri);
            if (g->event) g->event(g, ch, WGLINK_EV_REMOTE_CLOSE, g->event_ctx);
            maybe_release(g, ch);
            break;
        case CMD_CLOSE_ACK:
            rx_advance(g, 8);
            ch = by_lcid(g, p1);
            if (ch < 0) {++g->errors; break;}
            g->ch[ch].lcid = 0;
            g->ch[ch].open_sent = g->ch[ch].open_acked = g->ch[ch].closing = 0;
            free_local_intents(g, &g->ch[ch]);
            if (g->event) g->event(g, ch, WGLINK_EV_CLOSED, g->event_ctx);
            maybe_release(g, ch);
            break;
        case CMD_INTENT: {
            if (p2 > 64) {++g->errors; g->last_error_cmd = cmd; return WGLINK_ERR_PROTO;}
            const uint32_t msglen = 8 + p2 * 8;
            if (avail < msglen) return handled;
            ch = by_rcid(g, p1);
            if (ch >= 0) {
                for (uint32_t i = 0; i < p2; ++i) {
                    uint8_t pair[8];
                    rx_peek(g, 8 + i * 8, pair, 8);
                    const uint32_t size = get32(pair), iid = get32(pair + 4);
                    int slot = -1;
                    for (int s = 0; s < WGLINK_MAX_INTENTS; ++s) {
                        if (g->ch[ch].ri[s].used && g->ch[ch].ri[s].id == iid) {slot = -2; break;}   // idr_alloc fails
                        if (!g->ch[ch].ri[s].used && slot == -1) slot = s;
                    }
                    if (slot == -1) {
                        // The table is full of the small intents the modem advertised at open (16 x 128 bytes on the phone) and this
                        // one is the grant for a large message: Linux keeps remote intents in an idr without a limit. Replace the
                        // smallest idle intent that is smaller than the new one.
                        int victim = -1;
                        for (int s = 0; s < WGLINK_MAX_INTENTS; ++s) {
                            const wglink_rintent *q = &g->ch[ch].ri[s];
                            if (q->used && !q->in_use && q->size < size && (victim < 0 || q->size < g->ch[ch].ri[victim].size)) victim = s;
                        }
                        if (victim >= 0) {slot = victim; ++g->evicted;}
                    }
                    if (slot < 0) {++g->errors; ++g->err_by_site[1]; continue;}
                    g->ch[ch].ri[slot] = (wglink_rintent){iid, size, 0, 1};
                }
            } else {
                ++g->errors;
            }
            rx_advance(g, align8(msglen));
            break;
        }
        case CMD_RX_DONE:
        case CMD_RX_DONE_W_REUSE:
            rx_advance(g, 8);
            ch = by_rcid(g, p1);
            if (ch < 0) {++g->errors; break;}
            for (int s = 0; s < WGLINK_MAX_INTENTS; ++s) {
                wglink_rintent *r = &g->ch[ch].ri[s];
                if (!r->used || r->id != p2) continue;
                r->in_use = 0;
                if (cmd == CMD_RX_DONE) r->used = 0;
                break;
            }
            break;
        case CMD_RX_INTENT_REQ:
            rx_advance(g, 8);
            ch = by_rcid(g, p1);
            if (ch < 0) {++g->errors; break;}
            {
                const int ok = g->ch[ch].lcid && add_intent(g, ch, p2, 0) == 0;
                tx_cmd(g, CMD_RX_INTENT_REQ_ACK, g->ch[ch].lcid, ok);
            }
            break;
        case CMD_RX_INTENT_REQ_ACK:
            rx_advance(g, 8);
            ch = by_rcid(g, p1);
            if (ch < 0) {++g->errors; break;}
            g->ch[ch].intent_req_pending = 0;
            g->ch[ch].intent_req_result = p2 ? 1 : 0;
            break;
        case CMD_TX_DATA:
        case CMD_TX_DATA_CONT: {
            if (avail < 16) return handled;
            rx_peek(g, 0, m, 16);
            const uint32_t chunk = get32(m + 8);
            if (chunk > g->rx.length) {++g->errors; g->last_error_cmd = cmd; return WGLINK_ERR_PROTO;}
            if (avail < 16 + chunk) return handled;
            rx_data(g, m);
            rx_advance(g, align8(16 + chunk));
            break;
        }
        case CMD_READ_NOTIF:
            rx_advance(g, 8);
            tx_kick(g);
            break;
        case CMD_SIGNALS:
            rx_advance(g, 8);
            break;
        default:
            ++g->errors;
            g->last_error_cmd = cmd;
            return WGLINK_ERR_PROTO;
        }
        ++g->rx_cmds;
        ++handled;
    }
    return handled;
}
