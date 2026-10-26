// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WCN3990 device bring-up above wlfw FW_READY. See WifiDev.h.
#include "WifiDev.h"
#include "WifiAth10kIds.h"
#include <string.h>

static const char *const kNames[] = {"off", "htc-wait", "connect-htt", "connect-wmi", "svc-ready-wait", "ready-wait",
                                     "htt-wait", "running", "failed"};
const char *wdev_state_name(unsigned s) {return s <= WDEV_FAILED ? kNames[s] : "?";}

static uint32_t now(wdev *d) {return d->ops.now_ms(d->ops.ctx);}
static void logs(wdev *d, const char *step, int rc, uint32_t val) {if (d->ops.log) d->ops.log(d->ops.ctx, step, rc, val);}
static void enter(wdev *d, unsigned s)
{
    d->state = s;
    d->step_at = now(d);
    if (s < WDEV_FAILED) d->steps_ms[s] = d->step_at - d->t0;
    logs(d, wdev_state_name(s), 0, d->step_at - d->t0);
}
static void fail(wdev *d, int err)
{
    d->failed_in = d->state;
    d->err = err;
    logs(d, wdev_state_name(d->state), err ? err : -1, now(d) - d->t0);
    d->state = WDEV_FAILED;
}

// ---- adapters between the layers ----
static void *dma_alloc4(void *ctx, uint32_t size, uint32_t align, uint64_t *iova)
{
    wdev *d = (wdev *)ctx;
    return wdma_alloc(&d->dma, size ? size : 1, align < 8 ? 8 : align, iova);
}
static void *dma_alloc3(void *ctx, uint32_t size, uint64_t *iova) {return dma_alloc4(ctx, size, 4096, iova);}
static void barrier(void *ctx) {wdev *d = (wdev *)ctx; if (d->io.barrier) d->io.barrier(d->io.ctx);}

static int wmi_send(void *ctx, const void *buf, uint32_t len)
{
    wdev *d = (wdev *)ctx;
    const int r = whtc_send(&d->htc, d->wmi_eid, buf, len);
    if (r) ++d->wmi_send_fail;
    return r;
}
static void wmi_event(void *ctx, const wwmi_event *ev)
{
    wdev *d = (wdev *)ctx;
    if (d->mlme_up) wmlme_wmi_event(&d->mlme, ev);
}
static void htc_wmi_rx(whtc *h, unsigned eid, uint8_t *data, uint32_t len, void *arg)
{
    (void)h; (void)eid;
    wdev *d = (wdev *)arg;
    wwmi_rx(&d->wmi, data, len);
}
static void htc_htt_rx(whtc *h, unsigned eid, uint8_t *data, uint32_t len, void *arg)
{
    (void)h; (void)eid;
    wdev *d = (wdev *)arg;
    whtt_rx(&d->htt, data, len);
}
static void htt_direct(whtc *h, uint8_t *data, uint32_t len, void *arg)
{
    (void)h;
    wdev *d = (wdev *)arg;
    whtt_rx(&d->htt, data, len);
}
static int htt_ctl_send(void *ctx, const void *msg, uint32_t len)
{
    wdev *d = (wdev *)ctx;
    return whtc_send(&d->htc, d->htt_eid, msg, len);
}
// ath10k_snoc_hif_tx_sg: every fragment but the last with the gather flag; on failure the unpublished ones are reverted.
static int htt_tx_sg(void *ctx, uint64_t iova0, uint32_t len0, uint64_t iova1, uint32_t len1)
{
    wdev *d = (wdev *)ctx;
    const unsigned pipe = d->htc.ep[d->htt_eid].ul;
    if (wce_src_free(&d->ce, pipe) < 2) {++d->htt_tx_fail; return WCE_ERR_NOSPC;}
    int r = wce_send(&d->ce, pipe, iova0, len0, 0, 1, 0);
    if (r) {++d->htt_tx_fail; return r;}
    r = wce_send(&d->ce, pipe, iova1, len1, 0, 0, 0);
    if (r) {wce_send_revert(&d->ce, pipe); ++d->htt_tx_fail;}
    return r;
}
static void htt_rx(void *ctx, const uint8_t *eth, uint32_t len)
{
    wdev *d = (wdev *)ctx;
    if (len < 14) return;
    if (eth[12] == 0x88 && eth[13] == 0x8e) {   // EAPOL: to the supplicant
        if (d->mlme_up) wmlme_eapol_rx(&d->mlme, eth + 6, eth + 14, len - 14);
        else ++d->eapol_drop;
        return;
    }
    ++d->rx_eth;
    if (d->ops.rx) d->ops.rx(d->ops.ctx, eth, len);
}
static void htt_tx_done(void *ctx, unsigned status) {(void)ctx; (void)status;}
static void htt_sec_ind(void *ctx, unsigned peer, unsigned flags) {(void)ctx; (void)peer; (void)flags;}

static int mlme_eapol_tx(void *ctx, const uint8_t dst[6], const uint8_t *eapol, uint32_t len)
{
    wdev *d = (wdev *)ctx;
    uint8_t f[WHTT_MAX_ETH];
    if (len + 14 > sizeof f) return WHTT_ERR_INVAL;
    memcpy(f, dst, 6);
    memcpy(f + 6, d->mac, 6);
    f[12] = 0x88; f[13] = 0x8e;
    memcpy(f + 14, eapol, len);
    return whtt_tx(&d->htt, d->mlme.vdev_id, d->mlme.target.bssid, f, len + 14, 0);
}
static void mlme_random(void *ctx, uint8_t *out, uint32_t n) {wdev *d = (wdev *)ctx; d->ops.random(d->ops.ctx, out, n);}
static uint32_t mlme_now(void *ctx) {return now((wdev *)ctx);}
static void mlme_state(void *ctx, unsigned st, unsigned reason)
{
    wdev *d = (wdev *)ctx;
    if (d->ops.link) d->ops.link(d->ops.ctx, st, reason);
}

int wdev_start(wdev *d, const wdev_ops *ops, const wce_io *io, void *arena, uint64_t arena_iova, uint32_t arena_size)
{
    memset(d, 0, sizeof *d);
    d->ops = *ops;
    d->io = *io;
    if (!ops->now_ms || !ops->random || !ops->alloc) return WCE_ERR_INVAL;
    d->t0 = now(d);
    wdma_init(&d->dma, arena, arena_iova, arena_size);
    // ath10k_snoc_hif_power_up (after wlan_enable): rri, pipes; ath10k_snoc_hif_start: rx buffers
    int r = wce_init(&d->ce, io, &d->dma, NULL, WDEV_DST_LIMIT, ops->alloc, ops->ctx);
    if (r) {fail(d, r); return r;}
    r = whtc_init(&d->htc, &d->ce, ops->alloc, ops->free, ops->ctx);
    if (r) {fail(d, r); return r;}
    d->htc.htt_direct = htt_direct;
    d->htc.htt_arg = d;
    const wwmi_ops wo = {d, wmi_send, dma_alloc3, wmi_event};
    wwmi_init(&d->wmi, &wo);
    d->wmi.num_msdu_desc = WDEV_MAX_TX;   // the HTT pending tx limit is ours: the resource config says the same
    r = wce_start(&d->ce);
    if (r) {fail(d, r); return r;}
    enter(d, WDEV_HTC_WAIT);
    return 0;
}

static void step(wdev *d)
{
    int r;
    unsigned eid = 0;
    switch (d->state) {
    case WDEV_HTC_WAIT:
        if (d->htc.state == WHTC_FAILED) {fail(d, WHTC_ERR_PROTO); return;}
        if (d->htc.state != WHTC_READY) break;
        // ath10k_htt_connect, then ath10k_wmi_connect
        r = whtc_connect(&d->htc, WHTC_SVC_HTT_DATA_MSG, htc_htt_rx, d);
        if (r == WHTC_ERR_AGAIN) break;
        if (r) {fail(d, r); return;}
        enter(d, WDEV_CONNECT_HTT);
        return;
    case WDEV_CONNECT_HTT:
        r = whtc_connect_result(&d->htc, WHTC_SVC_HTT_DATA_MSG, &eid);
        if (!r) break;
        if (r < 0) {fail(d, r); return;}
        d->htt_eid = eid;
        r = whtc_connect(&d->htc, WHTC_SVC_WMI_CONTROL, htc_wmi_rx, d);
        if (r == WHTC_ERR_AGAIN) break;
        if (r) {fail(d, r); return;}
        enter(d, WDEV_CONNECT_WMI);
        return;
    case WDEV_CONNECT_WMI:
        r = whtc_connect_result(&d->htc, WHTC_SVC_WMI_CONTROL, &eid);
        if (!r) break;
        if (r < 0) {fail(d, r); return;}
        d->wmi_eid = eid;
        {   // the HTT layer needs its endpoint before ath10k_htt_setup
            const whtt_ops ho = {d, htt_ctl_send, htt_tx_sg, dma_alloc4, barrier, htt_rx, htt_tx_done, htt_sec_ind};
            const whtt_cfg hc = {WDEV_RX_RING, WDEV_MAX_TX, (uint8_t)d->htt_eid, 0, 0, 1};
            r = whtt_init(&d->htt, &ho, &hc);
            if (r) {fail(d, r); return;}
        }
        r = whtc_start(&d->htc);
        if (r) {fail(d, r); return;}
        enter(d, WDEV_SVC_READY_WAIT);
        return;
    case WDEV_SVC_READY_WAIT:
        if (!d->wmi.service_ready) break;
        r = wwmi_cmd_init(&d->wmi);
        if (r) {fail(d, r); return;}
        enter(d, WDEV_READY_WAIT);
        return;
    case WDEV_READY_WAIT:
        if (!d->wmi.ready) break;
        memcpy(d->mac, d->wmi.mac, 6);
        if (!(d->mac[0] | d->mac[1] | d->mac[2] | d->mac[3] | d->mac[4] | d->mac[5])) {memcpy(d->mac, d->ops.mac, 6); d->mac_fallback = 1;}
        r = whtt_start(&d->htt);
        if (r) {fail(d, r); return;}
        enter(d, WDEV_HTT_WAIT);
        return;
    case WDEV_HTT_WAIT:
        if (d->htt.state == WHTT_FAILED) {fail(d, WHTT_ERR_PROTO); return;}
        if (d->htt.state != WHTT_READY) break;
        {   // ath10k_regd_update: the EEPROM regdomain, then the channels; ath10k_add_interface: the station vdev
            // ath10k_start: PMF for QoS data (802.11w; KittyCorner requires PMF)
            r = wwmi_pdev_set_param(&d->wmi, WA_WMI_TLV_PDEV_PARAM_PMF_QOS, 1);
            if (r) {fail(d, r); return;}
            const uint32_t rd = d->wmi.eeprom_rd;
            r = wwmi_pdev_set_regdomain(&d->wmi, rd, rd, rd, 0x10 /* CTL_FCC */, 0x10);
            if (r) {fail(d, r); return;}
            wwmi_chan ch[WWMI_MAX_SCAN_CHANS];
            const unsigned n = wmlme_channels(ch, WWMI_MAX_SCAN_CHANS);
            r = wwmi_scan_chan_list(&d->wmi, ch, n);
            if (r) {fail(d, r); return;}
            const wmlme_ops mo = {d, mlme_eapol_tx, mlme_random, mlme_now, dma_alloc3, mlme_state};
            wmlme_init(&d->mlme, &mo, &d->wmi, d->mac, 0);
            d->mlme_up = 1;
            r = wmlme_start(&d->mlme);
            if (r) {fail(d, r); return;}
        }
        enter(d, WDEV_RUNNING);
        return;
    default:
        return;
    }
    if (d->state != WDEV_RUNNING && d->state != WDEV_FAILED && d->state != WDEV_OFF && now(d) - d->step_at > WDEV_STEP_TIMEOUT_MS)
        fail(d, -110 /* ETIMEDOUT */);
}

void wdev_poll(wdev *d)
{
    if (d->state == WDEV_OFF || d->state == WDEV_FAILED) return;
    ++d->polls;
    d->ce_work += wce_poll(&d->ce);
    whtc_kick(&d->htc);
    step(d);
    if (d->mlme_up) wmlme_poll(&d->mlme);
}

int wdev_scan(wdev *d, const char *target)
{
    if (d->state != WDEV_RUNNING) return WMLME_ERR_BUSY;
    if (target) wmlme_set_target(&d->mlme, target);
    return wmlme_scan(&d->mlme, NULL, 0);
}

int wdev_connect(wdev *d, const char *ssid, const char *pass)
{
    if (d->state != WDEV_RUNNING) return WMLME_ERR_BUSY;
    return wmlme_connect(&d->mlme, ssid, pass);
}

int wdev_tx(wdev *d, const uint8_t *eth, uint32_t len)
{
    if (d->state != WDEV_RUNNING || d->mlme.state != WMLME_CONNECTED) return WHTT_ERR_BUSY;
    return whtt_tx(&d->htt, d->mlme.vdev_id, d->mlme.target.bssid, eth, len, 1);
}
