// SPDX-License-Identifier: BSD-2-Clause-Patent
// Layer 5b: WMI-TLV (WifiWmi.c) against Linux's structure layouts (ath10k_layout.h, generated from the Linux headers) and a
// small target model: the service-ready / ready / scan / management events, and every command the station path sends, decoded
// back TLV by TLV with the field offsets of the Linux structs.
#include "tcheck.h"
#include <string.h>
#include "../WifiWmi.h"
#include "../WifiAth10kIds.h"
#include "ath10k_layout.h"

#define MAXCMD 64
typedef struct {uint8_t d[WWMI_CMD_MAX]; uint32_t len;} cmdrec;
static cmdrec g_cmd[MAXCMD];
static unsigned g_ncmd;
static int g_send_rc;
static int tsend(void *c, const void *b, uint32_t len)
{
    (void)c;
    if (g_send_rc) return g_send_rc;
    if (g_ncmd < MAXCMD) {memcpy(g_cmd[g_ncmd].d, b, len); g_cmd[g_ncmd].len = len; ++g_ncmd;}
    return 0;
}
static uint8_t g_dma[1 << 16];
static uint32_t g_dma_used;
static unsigned g_dma_allocs;
static void *tdma(void *c, uint32_t size, uint64_t *iova)
{
    (void)c;
    if (g_dma_used + size > sizeof g_dma) return NULL;
    void *p = g_dma + g_dma_used;
    *iova = 0x1c00000000ull + 0x80000 + g_dma_used;
    g_dma_used += (size + 63) & ~63u;
    ++g_dma_allocs;
    return p;
}
static wwmi_event g_ev[32];
static uint8_t g_evdata[32][512];
static unsigned g_nev;
static void tev(void *c, const wwmi_event *e)
{
    (void)c;
    if (g_nev < 32) {
        g_ev[g_nev] = *e;
        if (e->data && e->len <= 512) {memcpy(g_evdata[g_nev], e->data, e->len); g_ev[g_nev].data = g_evdata[g_nev];}
        ++g_nev;
    }
}
static wwmi *new_w(void)
{
    wwmi *w = t_talloc(sizeof *w);
    const wwmi_ops ops = {NULL, tsend, tdma, tev};
    wwmi_init(w, &ops);
    g_ncmd = 0; g_nev = 0; g_send_rc = 0; g_dma_used = 0; g_dma_allocs = 0;
    return w;
}

static uint32_t le32(const uint8_t *p) {return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t le16(const uint8_t *p) {return p[0] | p[1] << 8;}
static void w32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint8_t *put_tlv(uint8_t *p, uint32_t tag, uint32_t len)
{
    p[0] = (uint8_t)len; p[1] = (uint8_t)(len >> 8); p[2] = (uint8_t)tag; p[3] = (uint8_t)(tag >> 8);
    memset(p + 4, 0, len);
    return p + 4;
}

// TLV reader of a recorded command: returns the n-th TLV (skipping the 4-byte command id)
typedef struct {const uint8_t *v; uint32_t tag, len;} rtlv;
static int nth_tlv(const cmdrec *c, unsigned n, rtlv *out)
{
    uint32_t at = 4;
    for (unsigned i = 0;; ++i) {
        if (at + 4 > c->len) return 0;
        const uint32_t len = le16(c->d + at), tag = le16(c->d + at + 2);
        if (at + 4 + len > c->len) return 0;
        if (i == n) {out->v = c->d + at + 4; out->tag = tag; out->len = len; return 1;}
        at += 4 + len;
    }
}
static unsigned count_tlv(const cmdrec *c)
{
    rtlv t;
    unsigned n = 0;
    while (nth_tlv(c, n, &t)) ++n;
    return n;
}
static uint32_t cmd_id(const cmdrec *c) {return le32(c->d);}

// ---- the layouts the encoders are written against ----
static void t_wmi_layouts(void)
{
    CHECK(WL_SIZEOF_wmi_tlv_init_cmd == 28 && WL_SIZEOF_wmi_tlv_resource_config == 176 && WL_SIZEOF_host_memory_chunk_tlv == 16);
    CHECK(WL_SIZEOF_wmi_tlv_pdev_set_param_cmd == 12 && WL_SIZEOF_wmi_tlv_pdev_set_rd_cmd == 24 && WL_SIZEOF_wmi_channel == 24);
    CHECK(WL_SIZEOF_wmi_tlv_scan_chan_list_cmd == 4 && WL_SIZEOF_wmi_tlv_start_scan_cmd == 100 && WL_SIZEOF_wmi_ssid == 36);
    CHECK(WL_SIZEOF_wmi_mac_addr == 8 && WL_SIZEOF_wmi_vdev_create_cmd == 20 && WL_SIZEOF_wmi_vdev_up_cmd == 16);
    CHECK(WL_SIZEOF_wmi_vdev_set_param_cmd == 12 && WL_SIZEOF_wmi_vdev_install_key_cmd == 92 && WL_SIZEOF_wmi_tlv_peer_create_cmd == 16);
    CHECK(WL_SIZEOF_wmi_peer_delete_cmd == 12 && WL_SIZEOF_wmi_peer_set_param_cmd == 20 && WL_SIZEOF_wmi_tlv_peer_assoc_cmd == 76);
    CHECK(WL_SIZEOF_wmi_vht_rate_set == 16 && WL_SIZEOF_wmi_tlv_mgmt_tx_cmd == 28 && WL_SIZEOF_wmi_tlv_vdev_start_cmd == 72);
    CHECK(WL_SIZEOF_wmi_tlv_svc_rdy_ev == 104 && WL_SIZEOF_wmi_tlv_rdy_ev == 36 && WL_SIZEOF_hal_reg_capabilities == 36);
    CHECK(WL_SIZEOF_wlan_host_mem_req == 16 && WL_SIZEOF_wmi_scan_event == 24 && WL_SIZEOF_wmi_tlv_mgmt_rx_ev == 40);
    CHECK(WL_SIZEOF_wmi_vdev_start_response_event == 16 && WL_SIZEOF_wmi_peer_sta_kickout_event == 8 && WL_SIZEOF_wmi_tlv_mgmt_tx_compl_ev == 20);
    // the offsets the encoders/decoders hard-code
    CHECK(WL_OFF_wmi_tlv_svc_rdy_ev__num_mem_reqs == 72 && WL_OFF_wmi_tlv_rdy_ev__mac_addr == 24 && WL_OFF_wmi_tlv_rdy_ev__status == 32);
    CHECK(WL_OFF_wmi_tlv_mgmt_rx_ev__buf_len == 16 && WL_OFF_wmi_tlv_mgmt_rx_ev__status == 20 && WL_OFF_wmi_start_scan_common__scan_ctrl_flags == 56);
    CHECK(WL_OFF_wmi_tlv_start_scan_cmd__num_channels == 64 && WL_OFF_wmi_tlv_start_scan_cmd__ie_len == 76 && WL_OFF_wmi_tlv_start_scan_cmd__mac_addr == 84);
    CHECK(WL_OFF_wmi_tlv_vdev_start_cmd__flags == 16 && WL_OFF_wmi_tlv_vdev_start_cmd__ssid == 20);
    CHECK(WL_OFF_wmi_vdev_install_key_cmd__key_cipher == 20 && WL_OFF_wmi_vdev_install_key_cmd__key_len == 80);
    CHECK(WL_OFF_wmi_tlv_peer_assoc_cmd__flags == 20 && WL_OFF_wmi_tlv_peer_assoc_cmd__phy_mode == 56 && WL_OFF_wmi_tlv_peer_assoc_cmd__mac_addr == 0);
    CHECK(WL_OFF_wmi_tlv_mgmt_tx_cmd__frame_len == 20 && WL_OFF_wmi_tlv_mgmt_tx_cmd__paddr == 12);
    CHECK(WL_OFF_wmi_tlv_resource_config__rx_decap_mode == 52 && WL_OFF_wmi_tlv_resource_config__num_msdu_desc == 112 &&
          WL_OFF_wmi_tlv_resource_config__host_capab == 172);
}

// ---- the target's service ready / ready events ----
static uint32_t build_service_ready(uint8_t *out, unsigned n_reqs, const uint32_t req[][4], const uint32_t *bmap, unsigned bwords, int bad_abi)
{
    uint8_t *p = out;
    w32(p, WA_WMI_TLV_SERVICE_READY_EVENTID);
    p += 4;
    uint8_t *e = put_tlv(p, WA_WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT, 104);
    p += 4 + 104;
    w32(e, 0x1234567u);
    w32(e + 4, bad_abi ? 0x1000001u : WA_WMI_TLV_ABI_VER0); w32(e + 8, WA_WMI_TLV_ABI_VER1);
    w32(e + 12, WA_WMI_TLV_ABI_VER_NS0); w32(e + 16, WA_WMI_TLV_ABI_VER_NS1); w32(e + 20, WA_WMI_TLV_ABI_VER_NS2); w32(e + 24, WA_WMI_TLV_ABI_VER_NS3);
    w32(e + 28, 0x4000); w32(e + 36, 2); w32(e + 40, 0x1ef); w32(e + 44, 0x339b); w32(e + 48, 0xfffa); w32(e + 52, 2); w32(e + 56, 60);
    w32(e + 60, 0x7); w32(e + 72, n_reqs);
    uint8_t *r = put_tlv(p, WA_WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES, 36);
    p += 4 + 36;
    w32(r, 0x10); w32(r + 20, 2312); w32(r + 24, 2732); w32(r + 28, 4915); w32(r + 32, 5825);
    uint8_t *arr = put_tlv(p, WA_WMI_TLV_TAG_ARRAY_STRUCT, n_reqs * 20);
    for (unsigned i = 0; i < n_reqs; ++i) {
        uint8_t *q = arr + i * 20;
        q[0] = 16; q[1] = 0; q[2] = (uint8_t)WA_WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ; q[3] = 0;
        for (unsigned k = 0; k < 4; ++k) w32(q + 4 + 4 * k, req[i][k]);
    }
    p += 4 + n_reqs * 20;
    uint8_t *b = put_tlv(p, WA_WMI_TLV_TAG_ARRAY_UINT32, bwords * 4);
    for (unsigned i = 0; i < bwords; ++i) w32(b + 4 * i, bmap[i]);
    p += 4 + bwords * 4;
    uint8_t *d = put_tlv(p, WA_WMI_TLV_TAG_ARRAY_UINT32, 8);   // the second array (dbs hw modes): ignored
    (void)d;
    p += 4 + 8;
    return (uint32_t)(p - out);
}

static void set_svc(uint32_t *bm, uint32_t id) {bm[id / 4] |= 1u << (id % 4);}

static void t_wmi_service_ready(void)
{
    wwmi *w = new_w();
    uint32_t bm[32] = {0};
    set_svc(bm, WA_WMI_TLV_SERVICE_RX_FULL_REORDER);
    set_svc(bm, WA_WMI_TLV_SERVICE_MGMT_TX_WMI);
    // memory requests: IRAM-ish fixed (id 3: 4 units of 100 B), per-peer (id 4: units from NUM_PEERS flag), per-vdev, zero-sized
    const uint32_t req[][4] = {{3, 100, 0, 4}, {4, 63, WA_NUM_UNITS_IS_NUM_PEERS, 1}, {5, 32, WA_NUM_UNITS_IS_NUM_VDEVS, 1}, {6, 16, 0, 0}};
    uint8_t ev[1024];
    uint32_t n = build_service_ready(ev, 4, req, bm, 32, 0);
    CHECK(wwmi_rx(w, ev, n) == 0);
    CHECK(w->service_ready && g_nev == 1 && g_ev[0].type == WWMI_EV_SERVICE_READY);
    CHECK(w->fw_build == 0x1234567u && w->num_rf_chains == 2 && w->ht_cap == 0x1ef && w->vht_cap == 0x339b && w->sys_cap == 7);
    CHECK(w->low_2g == 2312 && w->high_5g == 5825 && w->eeprom_rd == 0x10 && w->svc_main_bytes == 128);
    CHECK(wwmi_service(w, WA_WMI_TLV_SERVICE_RX_FULL_REORDER) && wwmi_service(w, WA_WMI_TLV_SERVICE_MGMT_TX_WMI));
    CHECK(!wwmi_service(w, WA_WMI_TLV_SERVICE_RX_FULL_REORDER + 1) && !wwmi_service(w, 5));
    // chunks: 4*100, 34 peers * 64 (63 rounded to 4), 5 vdevs * 32; the zero-sized one is skipped
    CHECK(w->nchunks == 3 && w->chunk[0].req_id == 3 && w->chunk[0].len == 400);
    CHECK(w->chunk[1].req_id == 4 && w->chunk[1].len == (WA_TARGET_HL_TLV_NUM_PEERS + 1) * 64);
    CHECK(w->chunk[2].req_id == 5 && w->chunk[2].len == (WA_TARGET_TLV_NUM_VDEVS + 1) * 32 && g_dma_allocs == 3);
    // an ABI mismatch is refused
    wwmi *w2 = new_w();
    n = build_service_ready(ev, 0, req, bm, 32, 1);
    CHECK(wwmi_rx(w2, ev, n) == WWMI_ERR_OPNOTSUPP && !w2->service_ready);
    // truncated events
    wwmi *w3 = new_w();
    n = build_service_ready(ev, 1, req, bm, 32, 0);
    CHECK(wwmi_rx(w3, ev, 30) == WWMI_ERR_PROTO && wwmi_rx(w3, ev, n - 20) == WWMI_ERR_PROTO && !w3->service_ready);
    // the extended services (SERVICE_AVAILABLE): 28 per word in bits 4.. of the ext bitmap
    uint8_t av[64];
    w32(av, WA_WMI_TLV_SERVICE_AVAILABLE_EVENTID);
    uint8_t *s = put_tlv(av + 4, WA_WMI_TLV_TAG_STRUCT_SERVICE_AVAILABLE_EVENT, 12);
    w32(s, 2);
    const uint32_t id = WA_WMI_TLV_SERVICE_SUPPORT_EXTEND_ADDRESS;   // 0xb3: (179 - 128) = 51 -> word 1, bit 23 + 4
    w32(s + 4, 0); w32(s + 8, 1u << (((id - 128) % 28) + 4));
    CHECK(!wwmi_service(w, id));
    CHECK(wwmi_rx(w, av, 4 + 4 + 12) == 0 && w->service_avail && wwmi_service(w, id));
    CHECK(!wwmi_service(w, id + 1) && !wwmi_service(w, WA_WMI_TLV_SERVICE_TX_DATA_MGMT_ACK_RSSI));
    // INIT: three TLVs, the ABI version, the chunk list with the upper address half because the extension is on
    g_ncmd = 0;
    CHECK(wwmi_cmd_init(w) == 0 && g_ncmd == 1);
    const cmdrec *c = &g_cmd[0];
    rtlv t;
    CHECK(cmd_id(c) == WA_WMI_TLV_INIT_CMDID && count_tlv(c) == 3);
    CHECK(nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_INIT_CMD && t.len == 28);
    CHECK(le32(t.v) == WA_WMI_TLV_ABI_VER0 && le32(t.v + 4) == WA_WMI_TLV_ABI_VER1 && le32(t.v + 8) == WA_WMI_TLV_ABI_VER_NS0 &&
          le32(t.v + 12) == WA_WMI_TLV_ABI_VER_NS1 && le32(t.v + 24) == 3);
    CHECK(nth_tlv(c, 1, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG && t.len == 176);
    CHECK(le32(t.v) == WA_TARGET_TLV_NUM_VDEVS && le32(t.v + 4) == WA_TARGET_HL_TLV_NUM_PEERS && le32(t.v + 8) == WA_TARGET_TLV_NUM_VDEVS);
    CHECK(le32(t.v + 16) == 2 && le32(t.v + 20) == WA_TARGET_HL_TLV_NUM_PEERS * 2 && le32(t.v + 24) == WA_TARGET_HL_TLV_AST_SKID_LIMIT);
    CHECK(le32(t.v + WL_OFF_wmi_tlv_resource_config__rx_decap_mode) == WA_ATH10K_HW_TXRX_NATIVE_WIFI);
    CHECK(le32(t.v + WL_OFF_wmi_tlv_resource_config__num_msdu_desc) == WA_TARGET_TLV_NUM_MSDU_DESC);
    CHECK(le32(t.v + WL_OFF_wmi_tlv_resource_config__host_capab) == WA_WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL);
    CHECK(nth_tlv(c, 2, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_STRUCT && t.len == 3 * 20);
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t *h = t.v + i * 20;
        CHECK(le16(h) == 16 && le16(h + 2) == WA_WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK);
        CHECK(le32(h + 4) == w->chunk[i].req_id && le32(h + 8) == (uint32_t)w->chunk[i].iova && le32(h + 12) == w->chunk[i].len);
        CHECK(le32(h + 16) == (uint32_t)(w->chunk[i].iova >> 32) && le32(h + 16) == 0x1c);
    }
    CHECK(c->len == 4 + 4 + 28 + 4 + 176 + 4 + 60);
    // without the extension the high half is zero
    wwmi *w4 = new_w();
    n = build_service_ready(ev, 1, req, bm, 32, 0);
    CHECK(wwmi_rx(w4, ev, n) == 0);
    g_ncmd = 0;
    CHECK(wwmi_cmd_init(w4) == 0 && nth_tlv(&g_cmd[0], 2, &t) && le32(t.v + 16) == 0);
    // a failed send is reported and counted
    g_send_rc = -5;
    CHECK(wwmi_cmd_init(w4) == -5 && w4->cmd_fail == 1);
}

static void t_wmi_ready_scan(void)
{
    wwmi *w = new_w();
    uint8_t ev[256];
    // READY: abi(24) + mac(8) + status
    w32(ev, WA_WMI_TLV_READY_EVENTID);
    uint8_t *r = put_tlv(ev + 4, WA_WMI_TLV_TAG_STRUCT_READY_EVENT, 36);
    w32(r, 0x32040163u); w32(r + 4, 0x35);
    static const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(r + 24, mac, 6);
    w32(r + 32, 0);
    CHECK(wwmi_rx(w, ev, 4 + 4 + 36) == 0 && w->ready && !memcmp(w->mac, mac, 6) && g_nev == 1 && g_ev[0].type == WWMI_EV_READY);
    // scan events
    w32(ev, WA_WMI_TLV_SCAN_EVENTID);
    uint8_t *s = put_tlv(ev + 4, WA_WMI_TLV_TAG_STRUCT_SCAN_EVENT, 24);
    w32(s, WA_WMI_SCAN_EVENT_COMPLETED); w32(s + 4, 1); w32(s + 8, 2437); w32(s + 12, 0xa001); w32(s + 16, 0xa000); w32(s + 20, 0);
    CHECK(wwmi_rx(w, ev, 4 + 4 + 24) == 0 && g_nev == 2 && g_ev[1].type == WWMI_EV_SCAN && g_ev[1].a == WA_WMI_SCAN_EVENT_COMPLETED &&
          g_ev[1].b == 1 && g_ev[1].c == 2437 && g_ev[1].d == 0);
    CHECK(wwmi_rx(w, ev, 4 + 4 + 20) == WWMI_ERR_PROTO && w->bad_events == 1);
    // management frame: header + frame bytes (array byte, padded)
    w32(ev, WA_WMI_TLV_MGMT_RX_EVENTID);
    uint8_t *h = put_tlv(ev + 4, WA_WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, 40);
    w32(h, 6); w32(h + 4, 40); w32(h + 8, 0); w32(h + 12, 1); w32(h + 16, 10); w32(h + 20, 0);
    uint8_t *f = put_tlv(ev + 4 + 44, WA_WMI_TLV_TAG_ARRAY_BYTE, 12);
    memcpy(f, "0123456789", 10);
    CHECK(wwmi_rx(w, ev, 4 + 44 + 4 + 12) == 0 && g_nev == 3 && g_ev[2].type == WWMI_EV_MGMT_RX && g_ev[2].a == 2437 &&   /* channel 6 */ g_ev[2].b == 40 &&
          g_ev[2].len == 10 && !memcmp(g_ev[2].data, "0123456789", 10) && g_ev[2].d == 0);
    w32(h + 16, 13);   // buf_len beyond the array
    CHECK(wwmi_rx(w, ev, 4 + 44 + 4 + 12) == WWMI_ERR_PROTO);
    // vdev start response, stopped, peer delete, kickout, tx complete
    w32(ev, WA_WMI_TLV_VDEV_START_RESP_EVENTID);
    uint8_t *v = put_tlv(ev + 4, WA_WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT, 16);
    w32(v, 0); w32(v + 4, 9); w32(v + 8, 0); w32(v + 12, 0);
    CHECK(wwmi_rx(w, ev, 4 + 4 + 16) == 0 && g_ev[g_nev - 1].type == WWMI_EV_VDEV_START_RESP && g_ev[g_nev - 1].a == 0 && g_ev[g_nev - 1].b == 0);
    w32(ev, WA_WMI_TLV_PEER_STA_KICKOUT_EVENTID);
    uint8_t *k = put_tlv(ev + 4, WA_WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT, 8);
    memcpy(k, mac, 6);
    CHECK(wwmi_rx(w, ev, 4 + 4 + 8) == 0 && g_ev[g_nev - 1].type == WWMI_EV_PEER_KICKOUT && !memcmp(g_ev[g_nev - 1].mac, mac, 6));
    w32(ev, WA_WMI_TLV_MGMT_TX_COMPLETION_EVENTID);
    uint8_t *x = put_tlv(ev + 4, WA_WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT, 20);
    w32(x, 7); w32(x + 4, 0);
    CHECK(wwmi_rx(w, ev, 4 + 4 + 20) == 0 && g_ev[g_nev - 1].type == WWMI_EV_MGMT_TX_COMPL && g_ev[g_nev - 1].a == 7 && g_ev[g_nev - 1].b == 0);
    w32(ev, 0x99999);
    CHECK(wwmi_rx(w, ev, 4) == 0 && w->unknown_events == 1 && w->last_unknown_id == 0x99999);
    CHECK(wwmi_rx(w, ev, 2) == WWMI_ERR_PROTO);
}

static void t_wmi_commands(void)
{
    wwmi *w = new_w();
    rtlv t;
    static const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, bssid[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    // pdev set param / regdomain
    CHECK(wwmi_pdev_set_param(w, WA_WMI_TLV_PDEV_PARAM_PMF_QOS, 1) == 0);
    cmdrec *c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_PDEV_SET_PARAM_CMDID && count_tlv(c) == 1 && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD &&
          t.len == 12 && le32(t.v + 4) == WA_WMI_TLV_PDEV_PARAM_PMF_QOS && le32(t.v + 8) == 1 && c->len == 4 + 4 + 12);
    CHECK(wwmi_pdev_set_regdomain(w, 0x64, 0x10, 0x20, 1, 2) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_PDEV_SET_REGDOMAIN_CMDID && nth_tlv(c, 0, &t) && t.len == 24 && le32(t.v + 4) == 0x64 && le32(t.v + 20) == 2);
    // channel list
    wwmi_chan ch[2] = {{2412, 2412, 0, WA_WMI_CHAN_FLAG_ALLOW_HT, 0, 60, 30, 0, 3, WA_MODE_11G}, {5180, 5180, 0, WA_WMI_CHAN_FLAG_ALLOW_HT | WA_WMI_CHAN_FLAG_PASSIVE, 0, 60, 30, 1, 3, WA_MODE_11A}};
    CHECK(wwmi_scan_chan_list(w, ch, 2) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_SCAN_CHAN_LIST_CMDID && count_tlv(c) == 2 && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_SCAN_CHAN_LIST_CMD && le32(t.v) == 2);
    CHECK(nth_tlv(c, 1, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_STRUCT && t.len == 2 * 28);
    CHECK(le16(t.v) == 24 && le16(t.v + 2) == WA_WMI_TLV_TAG_STRUCT_CHANNEL && le32(t.v + 4) == 2412 && le32(t.v + 8) == 2412 &&
          le32(t.v + 16) == (WA_WMI_CHAN_FLAG_ALLOW_HT | WA_MODE_11G) && t.v[21] == 60 && t.v[22] == 30 && t.v[24] == 3 && t.v[25] == 60);
    CHECK(le32(t.v + 28 + 4) == 5180 && (le32(t.v + 28 + 16) & WA_WMI_CHAN_FLAG_PASSIVE) && t.v[28 + 4 + 19] == 1);
    // start scan: wildcard SSID, two channels
    wwmi_scan_arg a;
    wwmi_scan_init(&a);
    a.vdev_id = 0; a.scan_id = 0; a.n_ssids = 1; a.ssid_len[0] = 0; a.n_channels = 2; a.channels[0] = 2412; a.channels[1] = 5180;
    static const uint8_t ie[5] = {0xdd, 3, 1, 2, 3};
    a.ie = ie; a.ie_len = 5;
    CHECK(wwmi_start_scan(w, &a) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_START_SCAN_CMDID && count_tlv(c) == 5);
    CHECK(nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_START_SCAN_CMD && t.len == 100);
    CHECK(le32(t.v) == (WA_WMI_HOST_SCAN_REQ_ID_PREFIX | 0) && le32(t.v + 4) == (WA_WMI_HOST_SCAN_REQUESTOR_ID_PREFIX | 1));
    CHECK(le32(t.v + 12) == WA_WMI_SCAN_PRIORITY_LOW && le32(t.v + 20) == 50 && le32(t.v + 24) == 150 && le32(t.v + 48) == 20000 && le32(t.v + 52) == 5);
    CHECK(le32(t.v + WL_OFF_wmi_start_scan_common__scan_ctrl_flags) == (WA_WMI_SCAN_CHAN_STAT_EVENT ^ WA_WMI_SCAN_FILTER_PROBE_REQ));
    CHECK(le32(t.v + WL_OFF_wmi_tlv_start_scan_cmd__num_channels) == 2 && le32(t.v + 68) == 1 && le32(t.v + 72) == 1 &&
          le32(t.v + WL_OFF_wmi_tlv_start_scan_cmd__ie_len) == 5 && le32(t.v + 80) == 3);
    CHECK(nth_tlv(c, 1, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_UINT32 && t.len == 8 && le32(t.v) == 2412 && le32(t.v + 4) == 5180);
    CHECK(nth_tlv(c, 2, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_FIXED_STRUCT && t.len == 36 && le32(t.v) == 0);
    CHECK(nth_tlv(c, 3, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_FIXED_STRUCT && t.len == 8 && t.v[0] == 0xff && t.v[5] == 0xff);
    CHECK(nth_tlv(c, 4, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_BYTE && t.len == 8 && !memcmp(t.v, ie, 5) && t.v[5] == 0);
    a.n_channels = WWMI_MAX_SCAN_CHANS + 1;
    CHECK(wwmi_start_scan(w, &a) == WWMI_ERR_INVAL);
    CHECK(wwmi_stop_scan(w, 1, 0, 0, 1) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_STOP_SCAN_CMDID && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_STOP_SCAN_CMD && t.len == 16 &&
          le32(t.v) == (WA_WMI_HOST_SCAN_REQUESTOR_ID_PREFIX | 1) && le32(t.v + 4) == WA_WMI_HOST_SCAN_REQ_ID_PREFIX && le32(t.v + 8) == 0x20000000u);
    // vdev create / up / down / delete / set param
    CHECK(wwmi_vdev_create(w, 0, WA_WMI_VDEV_TYPE_STA, WA_WMI_TLV_VDEV_SUBTYPE_NONE, mac) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_VDEV_CREATE_CMDID && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_VDEV_CREATE_CMD && t.len == 20 &&
          le32(t.v + 4) == WA_WMI_VDEV_TYPE_STA && !memcmp(t.v + 12, mac, 6));
    CHECK(wwmi_vdev_up(w, 0, 5, bssid) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_VDEV_UP_CMDID && nth_tlv(c, 0, &t) && t.len == 16 && le32(t.v + 4) == 5 && !memcmp(t.v + 8, bssid, 6));
    CHECK(wwmi_vdev_down(w, 0) == 0 && cmd_id(&g_cmd[g_ncmd - 1]) == WA_WMI_TLV_VDEV_DOWN_CMDID);
    CHECK(wwmi_vdev_delete(w, 0) == 0 && cmd_id(&g_cmd[g_ncmd - 1]) == WA_WMI_TLV_VDEV_DELETE_CMDID);
    CHECK(wwmi_vdev_stop(w, 0) == 0 && cmd_id(&g_cmd[g_ncmd - 1]) == WA_WMI_TLV_VDEV_STOP_CMDID);
    CHECK(wwmi_vdev_set_param(w, 0, WA_WMI_TLV_VDEV_PARAM_TX_ENCAP_TYPE, WA_ATH10K_HW_TXRX_NATIVE_WIFI) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_VDEV_SET_PARAM_CMDID && nth_tlv(c, 0, &t) && t.len == 12 && le32(t.v + 4) == WA_WMI_TLV_VDEV_PARAM_TX_ENCAP_TYPE);
    // vdev start: ssid, channel, empty noa array
    wwmi_vdev_start_arg va;
    memset(&va, 0, sizeof va);
    va.vdev_id = 0; va.bcn_intval = 100; va.dtim_period = 1; va.flags = WA_WMI_VDEV_START_PMF_ENABLED;
    va.ssid = (const uint8_t *)"KittyCorner"; va.ssid_len = 11;
    va.chan = ch[0];
    CHECK(wwmi_vdev_start(w, &va, 0) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_VDEV_START_REQUEST_CMDID && count_tlv(c) == 3 && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_VDEV_START_REQUEST_CMD &&
          t.len == 72);
    CHECK(le32(t.v + 8) == 100 && le32(t.v + 12) == 1 && le32(t.v + WL_OFF_wmi_tlv_vdev_start_cmd__flags) == WA_WMI_VDEV_START_PMF_ENABLED);
    CHECK(le32(t.v + WL_OFF_wmi_tlv_vdev_start_cmd__ssid) == 11 && !memcmp(t.v + WL_OFF_wmi_tlv_vdev_start_cmd__ssid + 4, "KittyCorner", 11));
    CHECK(nth_tlv(c, 1, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_CHANNEL && t.len == 24 && le32(t.v) == 2412);
    CHECK(nth_tlv(c, 2, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_STRUCT && t.len == 0);
    CHECK(wwmi_vdev_start(w, &va, 1) == 0 && cmd_id(&g_cmd[g_ncmd - 1]) == 0x5004u);
    va.ssid_len = 33;
    CHECK(wwmi_vdev_start(w, &va, 0) == WWMI_ERR_INVAL);
    // peer create / delete / set param
    CHECK(wwmi_peer_create(w, 0, bssid, WA_WMI_TLV_PEER_TYPE_BSS) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_PEER_CREATE_CMDID && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_PEER_CREATE_CMD && t.len == 16 &&
          !memcmp(t.v + 4, bssid, 6) && le32(t.v + 12) == 1);
    CHECK(wwmi_peer_set_param(w, 0, bssid, WA_WMI_PEER_AUTHORIZE, 1) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_PEER_SET_PARAM_CMDID && nth_tlv(c, 0, &t) && t.len == 20 && le32(t.v + 12) == WA_WMI_PEER_AUTHORIZE && le32(t.v + 16) == 1);
    CHECK(wwmi_peer_delete(w, 0, bssid) == 0 && cmd_id(&g_cmd[g_ncmd - 1]) == WA_WMI_TLV_PEER_DELETE_CMDID);
    // peer assoc
    wwmi_peer_assoc_arg pa;
    memset(&pa, 0, sizeof pa);
    memcpy(pa.addr, bssid, 6);
    pa.vdev_id = 0; pa.aid = 5; pa.flags = WA_WMI_TLV_PEER_QOS | WA_WMI_TLV_PEER_NEED_PTK_4_WAY | WA_WMI_TLV_PEER_HT; pa.caps = 0x1431; pa.listen_intval = 1;
    pa.nss = 1; pa.phymode = WA_MODE_11NG_HT20;
    static const uint8_t rates[5] = {0x82, 0x84, 0x8b, 0x96, 0x0c};
    memcpy(pa.legacy_rates, rates, 5); pa.n_legacy = 5;
    pa.ht_rates[0] = 0; pa.ht_rates[1] = 1; pa.n_ht = 2;
    CHECK(wwmi_peer_assoc(w, &pa) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_PEER_ASSOC_CMDID && count_tlv(c) == 4 && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD && t.len == 76);
    CHECK(!memcmp(t.v + WL_OFF_wmi_tlv_peer_assoc_cmd__mac_addr, bssid, 6) && le32(t.v + 12) == 1 && le32(t.v + 16) == 5 &&
          le32(t.v + WL_OFF_wmi_tlv_peer_assoc_cmd__flags) == pa.flags && le32(t.v + 24) == 0x1431 && le32(t.v + 48) == 1 &&
          le32(t.v + WL_OFF_wmi_tlv_peer_assoc_cmd__phy_mode) == WA_MODE_11NG_HT20 && le32(t.v + 68) == 5 && le32(t.v + 72) == 2);
    CHECK(nth_tlv(c, 1, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_BYTE && t.len == 8 && !memcmp(t.v, rates, 5));
    CHECK(nth_tlv(c, 2, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_BYTE && t.len == 4 && t.v[1] == 1);
    CHECK(nth_tlv(c, 3, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_VHT_RATE_SET && t.len == 16);
    pa.n_legacy = 33;
    CHECK(wwmi_peer_assoc(w, &pa) == WWMI_ERR_INVAL);
    // install key: pairwise CCMP and a group key
    uint8_t key[16];
    for (unsigned i = 0; i < 16; ++i) key[i] = (uint8_t)(0xa0 + i);
    wwmi_key_arg ka = {0, bssid, 0, WA_WMI_KEY_PAIRWISE, WA_WMI_CIPHER_AES_CCM, 16, 0, 0, key};
    CHECK(wwmi_vdev_install_key(w, &ka) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID && count_tlv(c) == 2 && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_CMD && t.len == 92);
    CHECK(!memcmp(t.v + 4, bssid, 6) && le32(t.v + 12) == 0 && le32(t.v + 16) == WA_WMI_KEY_PAIRWISE &&
          le32(t.v + WL_OFF_wmi_vdev_install_key_cmd__key_cipher) == WA_WMI_CIPHER_AES_CCM && le32(t.v + WL_OFF_wmi_vdev_install_key_cmd__key_len) == 16);
    CHECK(nth_tlv(c, 1, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_BYTE && t.len == 16 && !memcmp(t.v, key, 16));
    ka.key = NULL;
    CHECK(wwmi_vdev_install_key(w, &ka) == WWMI_ERR_INVAL);
    wwmi_key_arg none = {0, bssid, 0, WA_WMI_KEY_PAIRWISE, WA_WMI_CIPHER_NONE, 0, 0, 0, NULL};
    CHECK(wwmi_vdev_install_key(w, &none) == 0);   // deleting a key
    // power save mode
    CHECK(wwmi_set_psmode(w, 0, WA_WMI_STA_PS_MODE_DISABLED) == 0 && cmd_id(&g_cmd[g_ncmd - 1]) == WA_WMI_TLV_STA_POWERSAVE_MODE_CMDID);
    // management frame: 100 bytes, the first 64 inline, the DMA address and both lengths
    uint8_t frame[100];
    for (unsigned i = 0; i < sizeof frame; ++i) frame[i] = (uint8_t)i;
    CHECK(wwmi_mgmt_tx(w, 0, 7, frame, sizeof frame, 0x1c00040000ull) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(cmd_id(c) == WA_WMI_TLV_MGMT_TX_SEND_CMD && count_tlv(c) == 2 && nth_tlv(c, 0, &t) && t.tag == WA_WMI_TLV_TAG_STRUCT_MGMT_TX_CMD && t.len == 28);
    CHECK(le32(t.v + 4) == 7 && le32(t.v + WL_OFF_wmi_tlv_mgmt_tx_cmd__paddr) == 0x00040000u && le32(t.v + WL_OFF_wmi_tlv_mgmt_tx_cmd__paddr + 4) == 0x1c &&
          le32(t.v + WL_OFF_wmi_tlv_mgmt_tx_cmd__frame_len) == 100 && le32(t.v + 24) == 64);
    CHECK(nth_tlv(c, 1, &t) && t.tag == WA_WMI_TLV_TAG_ARRAY_BYTE && t.len == 64 && !memcmp(t.v, frame, 64));
    CHECK(wwmi_mgmt_tx(w, 0, 8, frame, 0, 0) == WWMI_ERR_INVAL);
    // a short management frame is padded to a multiple of 4
    CHECK(wwmi_mgmt_tx(w, 0, 9, frame, 10, 0x1c00040000ull) == 0);
    c = &g_cmd[g_ncmd - 1];
    CHECK(nth_tlv(c, 0, &t) && le32(t.v + 24) == 12 && nth_tlv(c, 1, &t) && t.len == 12);
}

void t_wmi(void)
{
    RUN(t_wmi_layouts);
    RUN(t_wmi_service_ready);
    RUN(t_wmi_ready_scan);
    RUN(t_wmi_commands);
}
