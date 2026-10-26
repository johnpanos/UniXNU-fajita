// SPDX-License-Identifier: BSD-2-Clause-Patent
// QRTR node + name service on one link, after net/qrtr/af_qrtr.c and ns.c (Linux v6.16). See WifiRouter.h.
#include "WifiRouter.h"

#define T_DATA 1u
#define T_HELLO 2u
#define T_BYE 3u
#define T_NEW_SERVER 4u
#define T_DEL_SERVER 5u
#define T_DEL_CLIENT 6u
#define T_RESUME_TX 7u
#define T_NEW_LOOKUP 10u
#define T_DEL_LOOKUP 11u
#define VER_1 1u
#define VER_2 3u
#define HDR1 32u
#define HDR2 16u
#define CTRL_BYTES 20u
#define PORT_CTRL_LEGACY 0xffffu
#define FIRST_EPHEMERAL 0x4000u

static void put32(uint8_t *p, uint32_t v) {for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i);}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint16_t get16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static void copy(uint8_t *d, const uint8_t *s, size_t n) {while (n--) *d++ = *s++;}
static void zero(void *p, size_t n) {uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;}

void wqrtr_init(wqrtr *r, int (*xmit)(void *, const uint8_t *, size_t), void *xmit_ctx,
                void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *alloc_ctx)
{
    zero(r, sizeof *r);
    r->xmit = xmit; r->xmit_ctx = xmit_ctx;
    r->alloc = alloc; r->free = free_fn; r->alloc_ctx = alloc_ctx;
    r->local_nid = WQR_LOCAL_NID;
    r->remote_nid = WQR_NID_AUTO;
    r->next_port = FIRST_EPHEMERAL;
}

unsigned wqrtr_flush(wqrtr *r)
{
    while (r->txq_count) {
        const unsigned i = r->txq_head;
        if (!r->xmit || r->xmit(r->xmit_ctx, r->txq[i].p, r->txq[i].len) < 0) break;
        ++r->tx_pkts;
        r->free(r->txq[i].p, r->txq[i].len, r->alloc_ctx);
        r->txq[i].p = 0;
        r->txq_head = (i + 1) % WQR_TXQ;
        --r->txq_count;
    }
    return r->txq_count;
}

void wqrtr_reset(wqrtr *r)
{
    while (r->txq_count) {
        const unsigned i = r->txq_head;
        r->free(r->txq[i].p, r->txq[i].len, r->alloc_ctx);
        r->txq[i].p = 0;
        r->txq_head = (i + 1) % WQR_TXQ;
        --r->txq_count;
    }
    for (unsigned i = 0; i < WQR_MAX_REMOTE_SRV; ++i) r->remote[i].used = 0;
    for (unsigned i = 0; i < WQR_MAX_FLOWS; ++i) r->flows[i].used = 0;
    r->hello_seen = r->hello_sent = 0;
    r->remote_nid = WQR_NID_AUTO;
}

// qrtr_node_enqueue: a v1 header, the payload padded to 4.
static int enqueue(wqrtr *r, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
                   int confirm, const void *data, size_t len)
{
    if (len > WQR_MAX_PKT) return WQR_ERR_INVAL;
    if (r->txq_count >= WQR_TXQ || !r->alloc) {++r->dropped; return WQR_ERR_NOMEM;}
    const size_t total = HDR1 + ((len + 3) & ~(size_t)3);
    uint8_t *p = (uint8_t *)r->alloc(total, r->alloc_ctx);
    if (!p) {++r->dropped; return WQR_ERR_NOMEM;}
    put32(p, VER_1); put32(p + 4, type); put32(p + 8, src_node); put32(p + 12, src_port);
    put32(p + 16, confirm ? 1 : 0); put32(p + 20, (uint32_t)len); put32(p + 24, dst_node); put32(p + 28, dst_port);
    copy(p + HDR1, (const uint8_t *)data, len);
    zero(p + HDR1 + len, total - HDR1 - len);
    const unsigned slot = (r->txq_head + r->txq_count) % WQR_TXQ;
    r->txq[slot].p = p;
    r->txq[slot].len = total;
    ++r->txq_count;
    wqrtr_flush(r);
    return WQR_OK;
}

// A name-service packet from our CTRL port to the remote's.
static int ctrl(wqrtr *r, uint32_t cmd, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint8_t pkt[CTRL_BYTES];
    put32(pkt, cmd); put32(pkt + 4, a); put32(pkt + 8, b); put32(pkt + 12, c); put32(pkt + 16, d);
    return enqueue(r, cmd, r->local_nid, WQR_PORT_CTRL, r->remote_nid, WQR_PORT_CTRL, 0, pkt, sizeof pkt);
}

int wqrtr_hello(wqrtr *r) {r->hello_sent = 1; return ctrl(r, T_HELLO, 0, 0, 0, 0);}

static int port_index(const wqrtr *r, uint32_t port)
{
    for (int i = 0; i < WQR_MAX_PORTS; ++i) if (r->ports[i].used && r->ports[i].port == port) return i;
    return -1;
}

uint32_t wqrtr_bind(wqrtr *r, uint32_t port, wqr_rx_fn rx, void *ctx)
{
    if (port == WQR_PORT_CTRL) return 0;
    if (!port) {
        for (unsigned tries = 0; tries < 0x10000 && !port; ++tries) {
            const uint32_t p = r->next_port++;
            if (r->next_port >= 0x7fffffffu) r->next_port = FIRST_EPHEMERAL;
            if (port_index(r, p) < 0) port = p;
        }
    } else if (port_index(r, port) >= 0) {
        return 0;
    }
    for (int i = 0; i < WQR_MAX_PORTS; ++i) {
        if (r->ports[i].used) continue;
        r->ports[i].used = 1; r->ports[i].port = port; r->ports[i].rx = rx; r->ports[i].ctx = ctx;
        return port;
    }
    return 0;
}

void wqrtr_unbind(wqrtr *r, uint32_t port)
{
    const int i = port_index(r, port);
    if (i < 0) return;
    r->ports[i].used = 0;
    // qrtr_port_remove: DEL_CLIENT to everyone; the name service then withdraws the port's servers.
    if (r->remote_nid != WQR_NID_AUTO) ctrl(r, T_DEL_CLIENT, r->local_nid, port, 0, 0);
    for (unsigned s = 0; s < WQR_MAX_LOCAL_SRV; ++s) {
        wqr_server *v = &r->local[s];
        if (!v->used || v->port != port) continue;
        if (r->hello_seen) ctrl(r, T_DEL_SERVER, v->service, v->instance, r->local_nid, port);
        v->used = 0;
    }
}

int wqrtr_publish(wqrtr *r, uint32_t port, uint32_t service, uint32_t version, uint32_t instance)
{
    if (port_index(r, port) < 0) return WQR_ERR_NOENT;
    for (unsigned s = 0; s < WQR_MAX_LOCAL_SRV; ++s) {
        wqr_server *v = &r->local[s];
        if (v->used) continue;
        *v = (wqr_server){service, instance << 8 | version, r->local_nid, port, 1};
        if (r->hello_seen) ctrl(r, T_NEW_SERVER, v->service, v->instance, r->local_nid, port);
        return WQR_OK;
    }
    return WQR_ERR_NOMEM;
}

const wqr_server *wqrtr_lookup(const wqrtr *r, uint32_t service, uint32_t instance)
{
    for (unsigned s = 0; s < WQR_MAX_REMOTE_SRV; ++s) {
        const wqr_server *v = &r->remote[s];
        if (v->used && v->service == service && (!instance || v->instance == instance)) return v;
    }
    return 0;
}

static int flow_index(wqrtr *r, uint32_t node, uint32_t port, int create)
{
    int free_slot = -1;
    for (int i = 0; i < WQR_MAX_FLOWS; ++i) {
        if (r->flows[i].used && r->flows[i].node == node && r->flows[i].port == port) return i;
        if (!r->flows[i].used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    r->flows[free_slot].used = 1; r->flows[free_slot].node = node; r->flows[free_slot].port = port;
    r->flows[free_slot].pending = 0;
    return free_slot;
}

int wqrtr_send(wqrtr *r, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data, size_t len)
{
    if (!len || len > WQR_MAX_PKT - HDR1) return WQR_ERR_INVAL;
    // qrtr_tx_wait: confirm_rx at the low watermark, block at the high one until RESUME_TX.
    int confirm = 1;
    const int f = flow_index(r, dst_node, dst_port, 1);
    if (f >= 0) {
        if (r->flows[f].pending >= WQR_FLOW_HIGH) return WQR_ERR_BUSY;
        ++r->flows[f].pending;
        confirm = r->flows[f].pending == WQR_FLOW_LOW;
    }
    const int ret = enqueue(r, T_DATA, r->local_nid, src_port, dst_node, dst_port, confirm, data, len);
    if (ret && f >= 0) --r->flows[f].pending;
    return ret;
}

static void event(wqrtr *r, int what, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    if (r->event) r->event(r, what, service, instance, node, port, r->event_ctx);
}

static void del_remote(wqrtr *r, uint32_t node, uint32_t port, int any_port)
{
    for (unsigned s = 0; s < WQR_MAX_REMOTE_SRV; ++s) {
        wqr_server *v = &r->remote[s];
        if (!v->used || v->node != node || (!any_port && v->port != port)) continue;
        v->used = 0;
        event(r, WQR_EV_DEL_SERVER, v->service, v->instance, v->node, v->port);
    }
}

static void ns_packet(wqrtr *r, uint32_t src_node, const uint8_t *b, size_t len)
{
    if (len < 4) {++r->bad; return;}
    const uint32_t cmd = get32(b);
    const uint32_t a = len >= 8 ? get32(b + 4) : 0, c = len >= 12 ? get32(b + 8) : 0;
    const uint32_t d = len >= 16 ? get32(b + 12) : 0, e = len >= 20 ? get32(b + 16) : 0;
    switch (cmd) {
    case T_HELLO:
        // ctrl_cmd_hello: say hello back and announce our servers.
        {
            const int reply = !(r->hello_once && (r->hello_seen || r->hello_sent));
            const int announce = !(r->hello_once && r->hello_seen);
            r->hello_seen = 1;
            if (reply) ctrl(r, T_HELLO, 0, 0, 0, 0);
            for (unsigned s = 0; s < WQR_MAX_LOCAL_SRV && announce; ++s)
                if (r->local[s].used) ctrl(r, T_NEW_SERVER, r->local[s].service, r->local[s].instance, r->local_nid, r->local[s].port);
        }
        event(r, WQR_EV_HELLO, 0, 0, src_node, 0);
        break;
    case T_BYE:
        del_remote(r, src_node, 0, 1);
        for (unsigned i = 0; i < WQR_MAX_FLOWS; ++i) if (r->flows[i].node == src_node) r->flows[i].used = 0;
        r->hello_seen = 0;
        event(r, WQR_EV_BYE, 0, 0, src_node, 0);
        break;
    case T_NEW_SERVER: {
        if (len < CTRL_BYTES) {++r->bad; break;}
        if (d == r->local_nid) break;   // our own, echoed
        int slot = -1;
        for (unsigned s = 0; s < WQR_MAX_REMOTE_SRV; ++s) {
            wqr_server *v = &r->remote[s];
            if (v->used && v->node == d && v->port == e) {slot = (int)s; break;}   // server_add replaces node:port
            if (!v->used && slot < 0) slot = (int)s;
        }
        if (slot < 0) {++r->remote_full; event(r, WQR_EV_NEW_SERVER, a, c, d, e); break;}   // never lose the event
        r->remote[slot] = (wqr_server){a, c, d, e, 1};
        event(r, WQR_EV_NEW_SERVER, a, c, d, e);
        break;
    }
    case T_DEL_SERVER:
        if (len < CTRL_BYTES) {++r->bad; break;}
        del_remote(r, d, e, 0);
        break;
    case T_DEL_CLIENT:
        // client form: node, port
        del_remote(r, a, c, 0);
        {
            const int f = flow_index(r, a, c, 0);
            if (f >= 0) r->flows[f].used = 0;
        }
        event(r, WQR_EV_DEL_CLIENT, 0, 0, a, c);
        break;
    case T_NEW_LOOKUP:
    case T_DEL_LOOKUP:
        break;   // only local observers may look up (ctrl_cmd_new_lookup)
    default:
        ++r->bad;
        break;
    }
}

int wqrtr_input(wqrtr *r, const uint8_t *pkt, size_t len)
{
    if (!len || (len & 3)) {++r->bad; return WQR_ERR_INVAL;}
    uint32_t type, src_node, src_port, dst_node, dst_port, size, hdrlen;
    int confirm;
    switch (pkt[0]) {
    case VER_1:
        if (len < HDR1) {++r->bad; return WQR_ERR_INVAL;}
        type = get32(pkt + 4); src_node = get32(pkt + 8); src_port = get32(pkt + 12); confirm = get32(pkt + 16) != 0;
        size = get32(pkt + 20); dst_node = get32(pkt + 24); dst_port = get32(pkt + 28);
        hdrlen = HDR1;
        break;
    case VER_2:
        if (len < HDR2) {++r->bad; return WQR_ERR_INVAL;}
        type = pkt[1]; confirm = (pkt[2] & 1) != 0; hdrlen = HDR2 + pkt[3];
        size = get32(pkt + 4);
        src_node = get16(pkt + 8); src_port = get16(pkt + 10); dst_node = get16(pkt + 12); dst_port = get16(pkt + 14);
        if (src_port == 0xfffeu) src_port = WQR_PORT_CTRL;
        if (dst_port == 0xfffeu) dst_port = WQR_PORT_CTRL;
        break;
    default:
        ++r->bad;
        return WQR_ERR_INVAL;
    }
    (void)dst_node;   // one hop: af_qrtr does not route on it either
    if (dst_port == PORT_CTRL_LEGACY) dst_port = WQR_PORT_CTRL;
    if (!size || (uint64_t)hdrlen + ((size + 3ull) & ~3ull) != len) {++r->bad; return WQR_ERR_INVAL;}
    if ((type == T_NEW_SERVER || type == T_RESUME_TX) && size < CTRL_BYTES) {++r->bad; return WQR_ERR_INVAL;}
    if (dst_port != WQR_PORT_CTRL && type != T_DATA && type != T_RESUME_TX) {++r->bad; return WQR_ERR_INVAL;}
    ++r->rx_pkts;
    if (r->remote_nid == WQR_NID_AUTO && src_node != r->local_nid) r->remote_nid = src_node;   // qrtr_node_assign
    const uint8_t *body = pkt + hdrlen;
    if (type == T_RESUME_TX) {
        const int f = flow_index(r, get32(body + 4), get32(body + 8), 0);
        if (f >= 0) r->flows[f].pending = 0;
        wqrtr_flush(r);
        return WQR_OK;
    }
    if (dst_port == WQR_PORT_CTRL) {
        ns_packet(r, src_node, body, size);
        return WQR_OK;
    }
    const int i = port_index(r, dst_port);
    if (i < 0) {
        if (r->ndrop_log < 8) {
            unsigned k = r->ndrop_log++;
            r->drop_log[k].src_node = src_node; r->drop_log[k].src_port = src_port; r->drop_log[k].dst_port = dst_port; r->drop_log[k].size = size;
            for (unsigned b = 0; b < 12; ++b) r->drop_log[k].head[b] = b < size ? body[b] : 0;
        }
        ++r->dropped;
        return WQR_ERR_NOENT;
    }
    if (r->ports[i].rx) r->ports[i].rx(r, dst_port, src_node, src_port, body, size, r->ports[i].ctx);
    if (confirm) {   // qrtr_send_resume_tx, from the receiving port
        uint8_t c[CTRL_BYTES];
        put32(c, T_RESUME_TX); put32(c + 4, r->local_nid); put32(c + 8, dst_port); put32(c + 12, 0); put32(c + 16, 0);
        enqueue(r, T_RESUME_TX, r->local_nid, dst_port, src_node, src_port, 0, c, sizeof c);
    }
    return WQR_OK;
}
