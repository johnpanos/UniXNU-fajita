// SPDX-License-Identifier: BSD-2-Clause-Patent
// HTC over the WCN3990 copy engines (ath10k htc.c, Linux v6.16). See WifiHtc.h.
#include "WifiHtc.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}

void whtc_hdr(uint8_t *hdr, uint8_t eid, uint16_t len, uint8_t flags, uint8_t seq)
{
    hdr[0] = eid;
    hdr[1] = flags;
    put16(hdr + 2, len);
    hdr[4] = 0;
    hdr[5] = seq;
    hdr[6] = hdr[7] = 0;
}

static void ctrl_rx(whtc *h, uint8_t *data, uint32_t len)
{
    if (len < 2) {++h->rx_bad; return;}
    const uint16_t id = le16(data);
    if (id == WHTC_MSG_READY) {
        // ath10k_htc_wait_target
        if (h->state != WHTC_WAIT_READY) {++h->unsolicited; return;}
        if (len < 2 + 6) {h->state = WHTC_FAILED; return;}
        h->fw_credit_count = le16(data + 2);
        h->credit_size = le16(data + 4);
        h->max_endpoints = data[6];
        h->total_credits = 1;   // use_fw_tx_credits = false (WCN3990)
        h->state = h->credit_size && h->fw_credit_count ? WHTC_READY : WHTC_FAILED;
        return;
    }
    if (id == WHTC_MSG_CONNECT_RESP) {
        if (!h->conn_pending || len < 2 + 6) {++h->unsolicited; return;}
        const uint16_t svc = le16(data + 2);
        const uint8_t status = data[4], eid = data[5];
        const uint16_t max = le16(data + 6);
        h->conn_pending = 0;
        if (svc != h->conn_service || status != 0) {h->conn_status = status ? -(int)status : WHTC_ERR_PROTO; return;}
        if (eid >= WHTC_EP_COUNT || !max || h->ep[eid].service) {h->conn_status = WHTC_ERR_PROTO; return;}
        whtc_ep *ep = &h->ep[eid];
        memset(ep, 0, sizeof *ep);
        if (wsnoc_service_pipes(svc, &ep->ul, &ep->dl)) {h->conn_status = WHTC_ERR_PROTO; return;}
        ep->service = svc;
        ep->max_msg = max;
        ep->flow = svc == WHTC_SVC_WMI_CONTROL;
        ep->credits = ep->flow ? h->total_credits : 0;
        ep->credit_size = h->credit_size;
        ep->rx = h->conn_rx;
        ep->arg = h->conn_arg;
        h->conn_eid = eid;
        h->conn_status = 1;
        return;
    }
    ++h->unsolicited;
}

static void process_trailer(whtc *h, const uint8_t *t, uint32_t len)
{
    while (len >= 4) {
        const uint8_t id = t[0], rlen = t[1];
        if (rlen + 4u > len) {++h->rx_bad; return;}
        if (id == WHTC_RECORD_CREDITS) {
            for (unsigned i = 0; i + 4 <= rlen; i += 4) {
                const uint8_t eid = t[4 + i], credits = t[5 + i];
                if (eid >= WHTC_EP_COUNT) break;
                h->ep[eid].credits += credits;
                ++h->credit_reports;
            }
        }
        t += 4u + rlen;
        len -= 4u + rlen;
    }
}

// ath10k_htc_rx_completion_handler
static void htc_rx(whtc *h, uint8_t *data, uint32_t len)
{
    if (len < WHTC_HDR_LEN) {++h->rx_bad; return;}
    const uint8_t eid = data[0], flags = data[1];
    const uint16_t plen = le16(data + 2);
    if (eid >= WHTC_EP_COUNT || !h->ep[eid].service) {++h->rx_bad; return;}
    if (plen + WHTC_HDR_LEN > WHTC_MAX_LEN || len - WHTC_HDR_LEN < plen) {++h->rx_bad; return;}
    uint32_t body = plen;
    if (flags & WHTC_FLAG_TRAILER_PRESENT) {
        const uint8_t tlen = data[4];
        if (tlen < 4 || tlen > plen) {++h->rx_bad; return;}
        process_trailer(h, data + WHTC_HDR_LEN + plen - tlen, tlen);
        body = plen - tlen;
        whtc_kick(h);
    }
    if (!body) return;
    whtc_ep *ep = &h->ep[eid];
    ++ep->rx_count;
    if (eid == 0) ctrl_rx(h, data + WHTC_HDR_LEN, body);
    else if (ep->rx) ep->rx(h, eid, data + WHTC_HDR_LEN, body, ep->arg);
}

static void ce_rx(wce *ce, unsigned pipe, uint8_t *data, uint32_t len, void *arg)
{
    whtc *h = (whtc *)arg;
    (void)ce;
    if (pipe == WHTC_HTT_PIPE) {
        if (len < WHTC_HDR_LEN) {++h->rx_bad; return;}
        if (h->htt_direct) h->htt_direct(h, data + WHTC_HDR_LEN, len - WHTC_HDR_LEN, h->htt_arg);
        return;
    }
    htc_rx(h, data, len);
}

static void ce_tx_done(wce *ce, unsigned pipe, uintptr_t ctx, void *arg)
{
    (void)ce; (void)pipe; (void)ctx; (void)arg;   // copy sends: nothing to release
}

int whtc_init(whtc *h, wce *ce, void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *ctx)
{
    memset(h, 0, sizeof *h);
    h->ce = ce;
    h->alloc = alloc;
    h->free = free_fn;
    h->alloc_ctx = ctx;
    ce->rx = ce_rx;
    ce->tx_done = ce_tx_done;
    ce->arg = h;
    whtc_ep *ep = &h->ep[0];
    ep->service = WHTC_SVC_RSVD_CTRL;
    ep->max_msg = WHTC_MAX_CTRL;
    if (wsnoc_service_pipes(WHTC_SVC_RSVD_CTRL, &ep->ul, &ep->dl)) return WHTC_ERR_PROTO;
    return WHTC_OK;
}

static int raw_send(whtc *h, unsigned eid, const void *payload, uint32_t len)
{
    whtc_ep *ep = &h->ep[eid];
    uint8_t hdr[WHTC_HDR_LEN];
    whtc_hdr(hdr, (uint8_t)eid, (uint16_t)len, ep->flow ? WHTC_FLAG_NEED_CREDIT_UPDATE : 0, ep->seq);
    const int r = wce_send_copy(h->ce, ep->ul, hdr, WHTC_HDR_LEN, payload, len, eid, 0);
    if (r == WCE_OK) {++ep->seq; ++ep->tx_count;}
    return r == WCE_OK ? WHTC_OK : r == WCE_ERR_NOSPC ? WHTC_ERR_AGAIN : r;
}

static int credits_for(whtc_ep *ep, uint32_t len)
{
    const uint32_t sz = ep->credit_size ? ep->credit_size : 1;
    return (int)((len + WHTC_HDR_LEN + sz - 1) / sz);
}

void whtc_kick(whtc *h)
{
    while (h->qcount) {
        whtc_qent *e = &h->q[h->qhead];
        whtc_ep *ep = &h->ep[e->eid];
        const int need = credits_for(ep, e->len);
        if (ep->credits < need) return;
        if (raw_send(h, e->eid, e->data, e->len) != WHTC_OK) return;
        ep->credits -= need;
        if (h->free) h->free(e->data, e->len ? e->len : 1, h->alloc_ctx);
        e->data = NULL;
        h->qhead = (h->qhead + 1) % WHTC_QUEUE;
        --h->qcount;
    }
}

int whtc_send(whtc *h, unsigned eid, const void *payload, uint32_t len)
{
    if (eid >= WHTC_EP_COUNT || !h->ep[eid].service) return WHTC_ERR_INVAL;
    whtc_ep *ep = &h->ep[eid];
    if (len + WHTC_HDR_LEN > h->ce->pipe[ep->ul].src_sz_max) return WHTC_ERR_INVAL;
    if (!ep->flow) return raw_send(h, eid, payload, len);
    // in order behind anything already queued
    if (!h->qcount && ep->credits >= credits_for(ep, len)) {
        const int r = raw_send(h, eid, payload, len);
        if (r == WHTC_OK) ep->credits -= credits_for(ep, len);
        if (r != WHTC_ERR_AGAIN) return r;
    }
    if (h->qcount == WHTC_QUEUE || !h->alloc) return WHTC_ERR_AGAIN;
    uint8_t *copy = h->alloc(len ? len : 1, h->alloc_ctx);
    if (!copy) return WHTC_ERR_NOMEM;
    memcpy(copy, payload, len);
    whtc_qent *e = &h->q[(h->qhead + h->qcount) % WHTC_QUEUE];
    e->eid = (uint8_t)eid;
    e->len = len;
    e->data = copy;
    ++h->qcount;
    ++h->tx_queued;
    return WHTC_OK;
}

int whtc_connect(whtc *h, uint16_t service, whtc_rx_fn rx, void *arg)
{
    if (h->state != WHTC_READY || h->conn_pending) return WHTC_ERR_AGAIN;
    uint8_t ul, dl;
    if (wsnoc_service_pipes(service, &ul, &dl)) return WHTC_ERR_INVAL;
    uint8_t msg[8] = {0};
    put16(msg, WHTC_MSG_CONNECT);
    put16(msg + 2, service);
    uint16_t flags = 0;
    const uint8_t tx_alloc = service == WHTC_SVC_WMI_CONTROL ? (uint8_t)h->total_credits : 0;
    flags |= (uint16_t)(tx_alloc << WHTC_CONN_RECV_ALLOC_LSB);
    if (service != WHTC_SVC_WMI_CONTROL) flags |= WHTC_CONN_DISABLE_CREDIT_FLOW;
    put16(msg + 4, flags);
    h->conn_service = service;
    h->conn_rx = rx;
    h->conn_arg = arg;
    h->conn_status = 0;
    h->conn_eid = -1;
    const int r = raw_send(h, 0, msg, sizeof msg);
    if (r == WHTC_OK) h->conn_pending = 1;
    return r;
}

int whtc_connect_result(whtc *h, uint16_t service, unsigned *eid)
{
    if (h->conn_service != service) return WHTC_ERR_INVAL;
    if (h->conn_pending) return 0;
    if (h->conn_status == 1 && eid) *eid = (unsigned)h->conn_eid;
    return h->conn_status;
}

int whtc_start(whtc *h)
{
    uint8_t msg[2 + 10] = {0};   // hdr + ath10k_htc_setup_complete_extended
    put16(msg, WHTC_MSG_SETUP_COMPLETE_EX);
    return raw_send(h, 0, msg, sizeof msg);
}
