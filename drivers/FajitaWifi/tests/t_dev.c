// SPDX-License-Identifier: BSD-2-Clause-Patent
// WifiDev: the bring-up above FW_READY on the CE simulator + HTC firmware model, with a WMI/HTT target that answers like
// the WCN3990: SERVICE_READY after HTC setup, READY after INIT, VERSION_CONF after VERSION_REQ, and a scan that reports
// the test network. Checks the ath10k order (HTT connect, WMI connect, setup complete, INIT, HTT setup, regdomain,
// channel list, vdev), the arena use, and the failures (no HTC READY, a refused connect).
#include "tcheck.h"
#include "wsim.h"
#include "wfw.h"
#include "../WifiDev.h"
#include "../WifiAth10kIds.h"
#include <stdlib.h>
#include <string.h>

void *t_talloc(size_t n);
void t_tfree_all(void);

static void w32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t r32(const uint8_t *p) {return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;}
static uint8_t *put_tlv(uint8_t *p, uint32_t tag, uint32_t len)
{
    p[0] = (uint8_t)len; p[1] = (uint8_t)(len >> 8); p[2] = (uint8_t)tag; p[3] = (uint8_t)(tag >> 8);
    memset(p + 4, 0, len);
    return p + 4;
}

typedef struct {
    wsim s;
    wfw f;
    wdev d;
    uint32_t now;
    int no_ready, refuse_wmi, svc_ready_sent;
    uint32_t cmd_order[64]; unsigned ncmd;
    unsigned htt_ver_req, htt_setup_msgs, links;
    unsigned scans;
    const char *ssid;
} rig;

static const uint8_t kBssid[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
static const uint8_t kMac[6] = {0x00, 0x03, 0x7f, 0x12, 0x34, 0x56};

static void wmi_ev(rig *g, const uint8_t *ev, uint32_t len) {CHECK(wfw_send(&g->f, WFW_WMI_PIPE, WFW_EID_WMI, ev, len, 0) == 0);}

static void send_service_ready(rig *g)
{
    uint8_t out[512], *p = out;
    w32(p, WA_WMI_TLV_SERVICE_READY_EVENTID); p += 4;
    uint8_t *e = put_tlv(p, WA_WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT, 104); p += 4 + 104;
    w32(e, 0x1234567u);
    w32(e + 4, WA_WMI_TLV_ABI_VER0); w32(e + 8, WA_WMI_TLV_ABI_VER1);
    w32(e + 12, WA_WMI_TLV_ABI_VER_NS0); w32(e + 16, WA_WMI_TLV_ABI_VER_NS1); w32(e + 20, WA_WMI_TLV_ABI_VER_NS2); w32(e + 24, WA_WMI_TLV_ABI_VER_NS3);
    w32(e + 28, 0x4000); w32(e + 36, 2); w32(e + 40, 0x1ef); w32(e + 44, 0x339b); w32(e + 48, 0xfffa); w32(e + 52, 2); w32(e + 56, 60);
    w32(e + 60, 0x7); w32(e + 72, 1);
    uint8_t *r = put_tlv(p, WA_WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES, 36); p += 4 + 36;
    w32(r, 0x6a); w32(r + 20, 2312); w32(r + 24, 2732); w32(r + 28, 4915); w32(r + 32, 5825);
    uint8_t *arr = put_tlv(p, WA_WMI_TLV_TAG_ARRAY_STRUCT, 20); p += 4 + 20;
    arr[0] = 16; arr[2] = (uint8_t)WA_WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ;
    w32(arr + 4, 1); w32(arr + 8, 65536); w32(arr + 12, 0); w32(arr + 16, 1);   // one 64 KiB chunk (req, unit size, info, units)
    uint8_t *b = put_tlv(p, WA_WMI_TLV_TAG_ARRAY_UINT32, 8 * 4); p += 4 + 32;
    (void)b;
    put_tlv(p, WA_WMI_TLV_TAG_ARRAY_UINT32, 8); p += 4 + 8;
    wmi_ev(g, out, (uint32_t)(p - out));
}
static void send_ready(rig *g)
{
    uint8_t ev[64];
    w32(ev, WA_WMI_TLV_READY_EVENTID);
    uint8_t *r = put_tlv(ev + 4, WA_WMI_TLV_TAG_STRUCT_READY_EVENT, 36);
    w32(r, 0x32040163u); w32(r + 4, 0x35);
    memcpy(r + 24, kMac, 6);
    wmi_ev(g, ev, 4 + 4 + 36);
}
static void send_scan_event(rig *g, uint32_t type, uint32_t freq)
{
    uint8_t e[40];
    w32(e, WA_WMI_TLV_SCAN_EVENTID);
    uint8_t *p = put_tlv(e + 4, WA_WMI_TLV_TAG_STRUCT_SCAN_EVENT, 24);
    w32(p, type); w32(p + 8, freq); w32(p + 12, 0xa001); w32(p + 16, 0xa000);
    wmi_ev(g, e, 4 + 4 + 24);
}
static void send_beacon(rig *g, const char *ssid, uint32_t freq, uint32_t snr)
{
    uint8_t f[256] = {0};
    f[0] = 0x80;
    memset(f + 4, 0xff, 6); memcpy(f + 10, kBssid, 6); memcpy(f + 16, kBssid, 6);
    uint32_t n = 24 + 8;
    f[n++] = 100; f[n++] = 0;                // beacon interval
    f[n++] = 0x31; f[n++] = 0x04;           // capability: ESS, privacy, short slot
    const uint32_t sl = (uint32_t)strlen(ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
    static const uint8_t rates[] = {1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    memcpy(f + n, rates, sizeof rates); n += sizeof rates;
    static const uint8_t rsn[] = {0x30, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0};
    memcpy(f + n, rsn, sizeof rsn); n += sizeof rsn;
    uint8_t e[400];
    w32(e, WA_WMI_TLV_MGMT_RX_EVENTID);
    uint8_t *h = put_tlv(e + 4, WA_WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, 40);
    w32(h, freq); w32(h + 4, snr); w32(h + 16, n);
    const uint32_t padded = (n + 3) & ~3u;
    uint8_t *b = put_tlv(e + 4 + 44, WA_WMI_TLV_TAG_ARRAY_BYTE, padded);
    memcpy(b, f, n);
    wmi_ev(g, e, 4 + 44 + 4 + padded);
}

static void fw_wmi(wfw *f, const uint8_t *cmd, uint32_t len)
{
    rig *g = f->arg;
    if (len < 4) return;
    const uint32_t id = r32(cmd);
    if (g->ncmd < 64) g->cmd_order[g->ncmd++] = id;
    if (id == WA_WMI_TLV_INIT_CMDID) send_ready(g);
    if (id == WA_WMI_TLV_START_SCAN_CMDID) {
        ++g->scans;
        send_scan_event(g, WA_WMI_SCAN_EVENT_STARTED, 0);
        send_scan_event(g, WA_WMI_SCAN_EVENT_FOREIGN_CHANNEL, 2437);
        send_beacon(g, g->ssid, 6, 50);   // the firmware reports the channel number
        send_beacon(g, "", 6, 52);   // a hidden-SSID beacon of another BSS... same BSSID here: must not wipe the SSID
        send_scan_event(g, WA_WMI_SCAN_EVENT_COMPLETED, 0);
    }
}
static void fw_htt(wfw *f, const uint8_t *msg, uint32_t len, unsigned ce)
{
    rig *g = f->arg;
    (void)ce;
    if (!len) return;
    if (msg[0] == WA_HTT_H2T_MSG_TYPE_VERSION_REQ) {
        ++g->htt_ver_req;
        const uint8_t conf[4] = {WA_HTT_TLV_T2H_MSG_TYPE_VERSION_CONF, 0, 3, 0};
        CHECK(wfw_send(f, WFW_HTT_PIPE, WFW_EID_HTT, conf, sizeof conf, 0) == 0);
    } else {
        ++g->htt_setup_msgs;
    }
}

static uint32_t tnow(void *c) {return ((rig *)c)->now;}
static void trandom(void *c, uint8_t *o, uint32_t n) {(void)c; for (uint32_t i = 0; i < n; ++i) o[i] = (uint8_t)(i * 7 + 1);}
static void tlink(void *c, unsigned st, unsigned r) {(void)st; (void)r; ++((rig *)c)->links;}
static void *talloc(size_t n, void *c) {(void)c; return t_talloc(n);}
static void tfree(void *p, size_t n, void *c) {(void)p; (void)n; (void)c;}

static rig *new_rig(const char *ssid)
{
    rig *g = t_talloc(sizeof *g);
    wsim_init(&g->s, 8u << 20);
    wfw_init(&g->f, &g->s);
    g->f.wmi = fw_wmi;
    g->f.htt = fw_htt;
    g->f.arg = g;
    g->ssid = ssid;
    g->now = 1000;
    return g;
}
static int start(rig *g)
{
    const wdev_ops ops = {g, tnow, trandom, NULL, tlink, NULL, talloc, tfree, {0x02, 0xfa, 0, 0, 0, 1}};
    const wce_io io = wsim_io(&g->s);
    return wdev_start(&g->d, &ops, &io, g->s.mem, g->s.iova, g->s.size);
}
// The target: HTC READY once the rx buffers are posted; SERVICE_READY after SETUP_COMPLETE_EX.
static void run(rig *g, unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) {
        if (!g->no_ready && g->d.state == WDEV_HTC_WAIT && g->d.htc.state == WHTC_WAIT_READY && i == 1) CHECK(wfw_ready(&g->f) == 0);
        if (g->f.setup_complete && !g->svc_ready_sent) {g->svc_ready_sent = 1; send_service_ready(g);}
        wdev_poll(&g->d);
        ++g->now;
    }
}
static int find_cmd(const rig *g, uint32_t id)
{
    for (unsigned i = 0; i < g->ncmd; ++i) if (g->cmd_order[i] == id) return (int)i;
    return -1;
}

static void t_dev_bringup(void)
{
    rig *g = new_rig("KittyCorner");
    CHECK(start(g) == 0 && g->d.state == WDEV_HTC_WAIT);
    run(g, 50);
    CHECK(g->d.state == WDEV_RUNNING);
    CHECK(g->f.connects == 2 && g->f.conn_service[0] == WHTC_SVC_HTT_DATA_MSG && g->f.conn_service[1] == WHTC_SVC_WMI_CONTROL);   // ath10k order
    CHECK(g->f.setup_complete && g->d.htt_eid == WFW_EID_HTT && g->d.wmi_eid == WFW_EID_WMI);
    CHECK(g->d.wmi.nchunks == 1 && g->d.wmi.chunk[0].len == 65536);   // the host memory chunk from the arena
    CHECK(!memcmp(g->d.mac, kMac, 6) && !memcmp(g->d.mlme.mac, kMac, 6));
    CHECK(g->htt_ver_req == 1 && g->htt_setup_msgs == 3 && g->d.htt.state == WHTT_READY);   // FRAG_DESC_BANK, RX_RING_CFG, AGGR_CFG
    const int init = find_cmd(g, WA_WMI_TLV_INIT_CMDID), rd = find_cmd(g, WA_WMI_TLV_PDEV_SET_REGDOMAIN_CMDID);
    const int chl = find_cmd(g, WA_WMI_TLV_SCAN_CHAN_LIST_CMDID), vdev = find_cmd(g, WA_WMI_TLV_VDEV_CREATE_CMDID);
    const int pmf = find_cmd(g, WA_WMI_TLV_PDEV_SET_PARAM_CMDID);
    CHECK(init == 0 && pmf > init && rd > pmf && chl > rd && vdev > chl);
    CHECK(g->d.dma.used <= g->d.dma.size && g->d.dma.used < (4u << 20));   // fits the phone's arena with room to spare
    // a scan sees the network
    CHECK(wdev_scan(&g->d, "KittyCorner") == 0);
    run(g, 20);
    CHECK(g->scans == 1 && g->d.mlme.state == WMLME_SCAN_DONE);
    const wmlme_bss *b = wmlme_find(&g->d.mlme, "KittyCorner");
    CHECK(b && b->rsn_ok && b->freq == 2437 && b->rssi == 52 - 95 &&   /* the later, stronger beacon of the BSS */ !memcmp(b->bssid, kBssid, 6));
    CHECK(g->d.htt_tx_fail == 0 && g->d.wmi_send_fail == 0 && g->f.seq_errors == 0 && g->f.flag_errors == 0);
    wsim_free(&g->s);
    t_tfree_all();
}

static void t_dev_failures(void)
{
    rig *g = new_rig("x");
    g->no_ready = 1;   // the target never says HTC READY: a timeout in htc-wait, not a hang
    CHECK(start(g) == 0);
    run(g, WDEV_STEP_TIMEOUT_MS + 10);
    CHECK(g->d.state == WDEV_FAILED && g->d.failed_in == WDEV_HTC_WAIT && g->d.err == -110);
    run(g, 10);   // stays failed, does nothing
    CHECK(g->d.state == WDEV_FAILED);
    wsim_free(&g->s);
    // no SERVICE_READY: times out in svc-ready-wait
    rig *h = new_rig("x");
    h->svc_ready_sent = 1;
    CHECK(start(h) == 0);
    run(h, WDEV_STEP_TIMEOUT_MS + 50);
    CHECK(h->d.state == WDEV_FAILED && h->d.failed_in == WDEV_SVC_READY_WAIT);
    CHECK(wdev_scan(&h->d, NULL) == WMLME_ERR_BUSY);
    wsim_free(&h->s);
    t_tfree_all();
}

void t_dev(void)
{
    RUN(t_dev_bringup);
    RUN(t_dev_failures);
}
