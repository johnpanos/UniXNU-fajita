// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 firmware model: the target side of HTC. See wfw.h.
#include "wfw.h"
#include "../WifiHtc.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}

int wfw_send(wfw *f, unsigned pipe, uint8_t eid, const void *payload, uint32_t len, uint8_t trailer_credits)
{
    uint8_t buf[4096];
    const uint32_t tl = trailer_credits ? 8u : 0u;
    if (WHTC_HDR_LEN + len + tl > sizeof buf) return -2;
    whtc_hdr(buf, eid, (uint16_t)(len + tl), tl ? WHTC_FLAG_TRAILER_PRESENT : 0, 0);
    buf[4] = (uint8_t)tl;
    if (len) memcpy(buf + WHTC_HDR_LEN, payload, len);
    if (tl) {
        uint8_t *t = buf + WHTC_HDR_LEN + len;
        t[0] = WHTC_RECORD_CREDITS; t[1] = 4; t[2] = t[3] = 0;
        t[4] = WFW_EID_WMI; t[5] = trailer_credits; t[6] = t[7] = 0;
        ++f->credit_returns;
    }
    return wsim_send(f->sim, pipe, buf, WHTC_HDR_LEN + len + tl);
}

int wfw_credit(wfw *f, uint8_t credits) {return wfw_send(f, WFW_CTRL_PIPE, 0, NULL, 0, credits);}

int wfw_ready(wfw *f)
{
    uint8_t m[8] = {0};
    put16(m, WHTC_MSG_READY);
    put16(m + 2, f->credit_count);
    put16(m + 4, f->credit_size);
    m[6] = 8;
    return wfw_send(f, WFW_CTRL_PIPE, 0, m, sizeof m, 0);
}

static void ctrl(wfw *f, const uint8_t *m, uint32_t len)
{
    ++f->ctrl_msgs;
    if (len < 2) return;
    const uint16_t id = le16(m);
    if (id == WHTC_MSG_CONNECT && len >= 8) {
        const uint16_t svc = le16(m + 2), flags = le16(m + 4);
        uint8_t r[8] = {0};
        put16(r, WHTC_MSG_CONNECT_RESP);
        put16(r + 2, svc);
        uint8_t eid = svc == WHTC_SVC_WMI_CONTROL ? WFW_EID_WMI : svc == WHTC_SVC_HTT_DATA_MSG ? WFW_EID_HTT : f->next_eid++;
        r[4] = 0;
        r[5] = eid;
        put16(r + 6, 1792);
        if (f->connects < 8) {f->conn_service[f->connects] = svc; f->conn_flags[f->connects] = flags;}
        ++f->connects;
        wfw_send(f, WFW_CTRL_PIPE, 0, r, sizeof r, 0);
        return;
    }
    if (id == WHTC_MSG_SETUP_COMPLETE_EX && len == 12) f->setup_complete = 1;
}

static void rx(wsim *s, unsigned ce, const uint8_t *data, uint32_t len, uint32_t transfer_id, void *arg)
{
    wfw *f = arg;
    (void)s;
    memcpy(f->last, data, len < sizeof f->last ? len : sizeof f->last);
    f->last_len = len;
    if (len < WHTC_HDR_LEN) {++f->unknown; return;}
    const uint8_t eid = data[0], flags = data[1], seq = data[5];
    const uint16_t plen = le16(data + 2);
    if (plen + WHTC_HDR_LEN > len) {++f->unknown; return;}
    if (ce == 4 && eid == WFW_EID_HTT && transfer_id == 0 && f->htt) {   // HTT TX_FRM bypasses HTC (no seq)
        ++f->htt_msgs;
        f->htt(f, data + WHTC_HDR_LEN, len - WHTC_HDR_LEN, ce);
        return;
    }
    if (transfer_id != eid) ++f->flag_errors;
    if (eid < 8) {
        if (seq != f->seq_seen[eid]) ++f->seq_errors;
        f->seq_seen[eid] = (uint8_t)(seq + 1);
    }
    if (eid == 0) {ctrl(f, data + WHTC_HDR_LEN, plen); return;}
    if (eid == WFW_EID_WMI) {
        if (!(flags & WHTC_FLAG_NEED_CREDIT_UPDATE)) ++f->flag_errors;
        ++f->wmi_cmds;
        if (f->wmi) f->wmi(f, data + WHTC_HDR_LEN, plen);
        if (f->auto_credit) wfw_credit(f, 1);
        return;
    }
    if (eid == WFW_EID_HTT) {
        if (flags & WHTC_FLAG_NEED_CREDIT_UPDATE) ++f->flag_errors;
        ++f->htt_msgs;
        if (f->htt) f->htt(f, data + WHTC_HDR_LEN, plen, ce);
        return;
    }
    ++f->unknown;
}

void wfw_init(wfw *f, wsim *s)
{
    memset(f, 0, sizeof *f);
    f->sim = s;
    f->credit_size = 1792;
    f->credit_count = 2;
    f->next_eid = 3;
    f->auto_credit = 1;
    s->fw = rx;
    s->fw_arg = f;
}
