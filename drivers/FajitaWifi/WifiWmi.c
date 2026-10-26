// SPDX-License-Identifier: BSD-2-Clause-Patent
// WMI-TLV, station path, after ath10k wmi-tlv.c and wmi.c (Linux v6.16). See WifiWmi.h.
#include "WifiWmi.h"
#include "WifiAth10kIds.h"

static void zero(void *p, size_t n) {uint8_t *b = p; while (n--) *b++ = 0;}
static void copy(void *d, const void *s, size_t n) {uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++;}
static void p16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void p32(uint8_t *p, uint32_t v) {p16(p, v); p16(p + 2, v >> 16);}
static void p64(uint8_t *p, uint64_t v) {p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32));}
static uint32_t g16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static uint32_t g32(const uint8_t *p) {return g16(p) | g16(p + 2) << 16;}

void wwmi_init(wwmi *w, const wwmi_ops *ops)
{
    zero(w, sizeof *w);
    w->ops = *ops;
    w->num_msdu_desc = WA_TARGET_TLV_NUM_MSDU_DESC;
}

// ---- service bitmaps (wmi.h WMI_SERVICE_IS_ENABLED / WMI_EXT_SERVICE_IS_ENABLED) ----
static int svc_in(const uint32_t *bmap, uint32_t words, uint32_t len, uint32_t id)
{
    if (id < len) {   // 4 services per 32-bit word: word id / 4, bit id % 4
        const uint32_t wi = id / 4;
        return wi < words && ((bmap[wi] >> (id % 4)) & 1u);
    }
    {   // the extension: 28 services per word in bits 4..31
        const uint32_t wi = (id - len) / 28;
        return wi < words && ((bmap[wi] >> ((((id - len) % 28) & 0x1f) + 4)) & 1u);
    }
}
int wwmi_service(const wwmi *w, uint32_t id)
{
    if (id < WA_WMI_TLV_MAX_SERVICE) return svc_in(w->svc_main, WWMI_SERVICE_WORDS, w->svc_main_bytes, id);
    return svc_in(w->svc_ext, 16, WA_WMI_TLV_MAX_SERVICE, id);
}

// ---- TLV writer ----
typedef struct {uint8_t *b; uint32_t at, cap; int err;} tw;
static void tw_start(tw *t, wwmi *w, uint32_t cmd_id)
{
    t->b = w->cmd; t->at = 0; t->cap = sizeof w->cmd; t->err = 0;
    zero(w->cmd, 64);
    p32(t->b, cmd_id);
    t->at = 4;
}
// A TLV header; returns the value area (len bytes, zeroed), or a dummy on overflow.
static uint8_t *tw_tlv(tw *t, uint32_t tag, uint32_t len)
{
    static uint8_t dummy[8];
    if (t->err || t->at + 4 + len > t->cap || len > 0xffff) {t->err = WWMI_ERR_NOSPC; return dummy;}
    uint8_t *h = t->b + t->at;
    p16(h, len); p16(h + 2, tag);
    zero(h + 4, len);
    t->at += 4 + len;
    return h + 4;
}
static int tw_send(tw *t, wwmi *w)
{
    if (t->err) return t->err;
    ++w->cmds;
    const int rc = w->ops.send(w->ops.ctx, t->b, t->at);
    if (rc) ++w->cmd_fail;
    return rc;
}

// ---- events ----
typedef struct {uint32_t tag; const uint8_t *p; uint32_t len;} tlv;
static int tlv_walk(const uint8_t *d, uint32_t len, tlv *out, unsigned cap, unsigned *n)
{
    *n = 0;
    while (len) {
        if (len < 4) return WWMI_ERR_PROTO;
        const uint32_t l = g16(d), tag = g16(d + 2);
        d += 4; len -= 4;
        if (l > len) return WWMI_ERR_PROTO;
        if (*n < cap) {out[*n].tag = tag; out[*n].p = d; out[*n].len = l; ++*n;}
        d += l; len -= l;
    }
    return 0;
}
static const tlv *find(const tlv *t, unsigned n, uint32_t tag, uint32_t minlen, unsigned nth)
{
    for (unsigned i = 0; i < n; ++i)
        if (t[i].tag == tag && t[i].len >= minlen && nth-- == 0) return &t[i];
    return 0;
}

static void emit(wwmi *w, wwmi_event *e)
{
    ++w->events;
    if (w->ops.event) w->ops.event(w->ops.ctx, e);
}

// service ready (ath10k_wmi_tlv_op_pull_svc_rdy_ev + ath10k_wmi_event_service_ready_work)
static int ev_service_ready(wwmi *w, const uint8_t *d, uint32_t len)
{
    tlv t[24];
    unsigned n;
    if (tlv_walk(d, len, t, 24, &n)) return WWMI_ERR_PROTO;
    const tlv *ev = find(t, n, WA_WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT, 104, 0);
    const tlv *reg = find(t, n, WA_WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES, 36, 0);
    const tlv *reqs = find(t, n, WA_WMI_TLV_TAG_ARRAY_STRUCT, 0, 0);
    const tlv *bmap = find(t, n, WA_WMI_TLV_TAG_ARRAY_UINT32, 0, 0);
    if (!ev || !reg || !reqs || !bmap) return WWMI_ERR_PROTO;
    const uint8_t *e = ev->p;
    // abi at +4: abi_ver0, abi_ver1, ns0..ns3
    if (g32(e + 4) != WA_WMI_TLV_ABI_VER0 || g32(e + 12) != WA_WMI_TLV_ABI_VER_NS0 || g32(e + 16) != WA_WMI_TLV_ABI_VER_NS1 ||
        g32(e + 20) != WA_WMI_TLV_ABI_VER_NS2 || g32(e + 24) != WA_WMI_TLV_ABI_VER_NS3)
        return WWMI_ERR_OPNOTSUPP;
    w->fw_build = g32(e);
    w->fw_version0 = g32(e + 4); w->fw_version1 = g32(e + 8);
    w->phy_capab = g32(e + 28); w->num_rf_chains = g32(e + 36); w->ht_cap = g32(e + 40); w->vht_cap = g32(e + 44);
    w->vht_supp_mcs = g32(e + 48); w->min_tx_power = g32(e + 52); w->max_tx_power = g32(e + 56); w->sys_cap = g32(e + 60);
    w->num_mem_reqs = g32(e + 72);
    w->eeprom_rd = g32(reg->p);
    w->low_2g = g32(reg->p + 20); w->high_2g = g32(reg->p + 24); w->low_5g = g32(reg->p + 28); w->high_5g = g32(reg->p + 32);
    zero(w->svc_main, sizeof w->svc_main);
    w->svc_main_bytes = bmap->len;
    for (uint32_t i = 0; i < bmap->len / 4 && i < WWMI_SERVICE_WORDS; ++i) w->svc_main[i] = g32(bmap->p + 4 * i);
    // the host memory the firmware asks for
    tlv rq[WWMI_MAX_CHUNKS + 1];
    unsigned nrq;
    if (tlv_walk(reqs->p, reqs->len, rq, WWMI_MAX_CHUNKS + 1, &nrq) || nrq > WWMI_MAX_CHUNKS) return WWMI_ERR_PROTO;
    w->nchunks = 0;
    for (unsigned i = 0; i < nrq; ++i) {
        if (rq[i].tag != WA_WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ || rq[i].len < 16) return WWMI_ERR_PROTO;
        const uint32_t req_id = g32(rq[i].p), unit_size = g32(rq[i].p + 4), info = g32(rq[i].p + 8);
        uint32_t units = g32(rq[i].p + 12);
        if (info & WA_NUM_UNITS_IS_NUM_ACTIVE_PEERS) units = WA_TARGET_HL_TLV_NUM_PEERS + 1;
        else if (info & WA_NUM_UNITS_IS_NUM_PEERS) units = WA_TARGET_HL_TLV_NUM_PEERS + 1;
        else if (info & WA_NUM_UNITS_IS_NUM_VDEVS) units = WA_TARGET_TLV_NUM_VDEVS + 1;
        const uint32_t pool = units * ((unit_size + 3u) & ~3u);
        if (!pool) continue;
        if (w->nchunks >= WWMI_MAX_CHUNKS || !w->ops.dma_alloc) return WWMI_ERR_NOMEM;
        uint64_t iova = 0;
        if (!w->ops.dma_alloc(w->ops.ctx, pool, &iova)) return WWMI_ERR_NOMEM;
        w->chunk[w->nchunks].req_id = req_id; w->chunk[w->nchunks].iova = iova; w->chunk[w->nchunks].len = pool;
        ++w->nchunks;
    }
    w->service_ready = 1;
    wwmi_event ev2 = {WWMI_EV_SERVICE_READY, WA_WMI_TLV_SERVICE_READY_EVENTID, w->num_mem_reqs, w->nchunks, w->fw_build, 0, {0}, 0, 0};
    emit(w, &ev2);
    return 0;
}

int wwmi_rx(wwmi *w, const uint8_t *d, uint32_t len)
{
    if (len < 4) {++w->bad_events; return WWMI_ERR_PROTO;}
    const uint32_t id = g32(d) & WA_WMI_CMD_HDR_CMD_ID_MASK;
    d += 4; len -= 4;
    tlv t[16];
    unsigned n = 0;
    wwmi_event ev = {0, id, 0, 0, 0, 0, {0}, 0, 0};
    int rc = 0;
    switch (id) {
    case WA_WMI_TLV_SERVICE_READY_EVENTID:
        rc = ev_service_ready(w, d, len);
        break;
    case WA_WMI_TLV_SERVICE_AVAILABLE_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *s = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_SERVICE_AVAILABLE_EVENT, 4, 0);
        if (s) {   // __le32 ext_len, then the ext bitmap
            w->svc_ext_len = g32(s->p);
            zero(w->svc_ext, sizeof w->svc_ext);
            for (uint32_t i = 0; i < 16 && 4 + 4 * (i + 1) <= s->len; ++i) w->svc_ext[i] = g32(s->p + 4 + 4 * i);
            w->service_avail = 1;
        }
        ev.type = WWMI_EV_SERVICE_AVAILABLE;
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_READY_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *r = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_READY_EVENT, 36, 0);
        if (!r) {rc = WWMI_ERR_PROTO; break;}
        for (unsigned i = 0; i < 6; ++i) w->mac[i] = r->p[24 + i];
        w->ready_status = g32(r->p + 32);
        w->ready = 1;
        ev.type = WWMI_EV_READY; ev.a = w->ready_status; ev.b = g32(r->p); ev.c = g32(r->p + 4);
        for (unsigned i = 0; i < 6; ++i) ev.mac[i] = w->mac[i];
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_SCAN_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *s = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_SCAN_EVENT, 24, 0);
        if (!s) {rc = WWMI_ERR_PROTO; break;}
        ev.type = WWMI_EV_SCAN; ev.a = g32(s->p); ev.b = g32(s->p + 4); ev.c = g32(s->p + 8); ev.d = g32(s->p + 20);
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_MGMT_RX_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *h = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, 40, 0);
        const tlv *f = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_ARRAY_BYTE, 0, 0);
        if (!h || !f) {rc = WWMI_ERR_PROTO; break;}
        const uint32_t buf_len = g32(h->p + 16);
        if (buf_len > f->len) {rc = WWMI_ERR_PROTO; break;}
        ev.type = WWMI_EV_MGMT_RX; ev.a = g32(h->p); ev.b = g32(h->p + 4); ev.c = g32(h->p + 8);
        ev.d = g32(h->p + 20);
        ev.data = f->p; ev.len = buf_len;
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_MGMT_TX_COMPLETION_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *c = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT, 8, 0);
        if (!c) {rc = WWMI_ERR_PROTO; break;}
        ev.type = WWMI_EV_MGMT_TX_COMPL; ev.a = g32(c->p); ev.b = g32(c->p + 4);
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_VDEV_START_RESP_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *s = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT, 16, 0);
        if (!s) {rc = WWMI_ERR_PROTO; break;}
        ev.type = WWMI_EV_VDEV_START_RESP; ev.a = g32(s->p); ev.b = g32(s->p + 12); ev.c = g32(s->p + 8); ev.d = g32(s->p + 4);
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_VDEV_STOPPED_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *s = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_VDEV_STOPPED_EVENT, 4, 0);
        ev.type = WWMI_EV_VDEV_STOPPED; ev.a = s ? g32(s->p) : 0;
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_VDEV_DELETE_RESP_EVENTID:
        ev.type = WWMI_EV_VDEV_DELETE_RESP;
        emit(w, &ev);
        break;
    case WA_WMI_TLV_PEER_DELETE_RESP_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *s = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_PEER_DELETE_RESP_EVENT, 12, 0);
        if (!s) {rc = WWMI_ERR_PROTO; break;}
        ev.type = WWMI_EV_PEER_DELETE_RESP; ev.a = g32(s->p);
        for (unsigned i = 0; i < 6; ++i) ev.mac[i] = s->p[4 + i];
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_PEER_STA_KICKOUT_EVENTID: {
        rc = tlv_walk(d, len, t, 16, &n);
        const tlv *s = rc ? 0 : find(t, n, WA_WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT, 8, 0);
        if (!s) {rc = WWMI_ERR_PROTO; break;}
        ev.type = WWMI_EV_PEER_KICKOUT;
        for (unsigned i = 0; i < 6; ++i) ev.mac[i] = s->p[i];
        emit(w, &ev);
        break;
    }
    case WA_WMI_TLV_VDEV_INSTALL_KEY_COMPLETE_EVENTID:
        ev.type = WWMI_EV_INSTALL_KEY_COMPLETE;
        emit(w, &ev);
        break;
    case WA_WMI_TLV_CHAN_INFO_EVENTID:
        ev.type = WWMI_EV_CHAN_INFO;
        emit(w, &ev);
        break;
    default:
        ++w->unknown_events;
        w->last_unknown_id = id;
        ev.type = WWMI_EV_OTHER;
        emit(w, &ev);
        break;
    }
    if (rc) ++w->bad_events;
    return rc;
}

// ---- commands ----
int wwmi_cmd_init(wwmi *w)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_INIT_CMDID);
    uint8_t *cmd = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_INIT_CMD, 28);
    uint8_t *cfg = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG, 176);
    const uint32_t chunks_len = w->nchunks * (4 + 16);
    uint8_t *chunks = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_STRUCT, chunks_len);
    if (t.err) return t.err;
    p32(cmd + 0, WA_WMI_TLV_ABI_VER0); p32(cmd + 4, WA_WMI_TLV_ABI_VER1); p32(cmd + 8, WA_WMI_TLV_ABI_VER_NS0);
    p32(cmd + 12, WA_WMI_TLV_ABI_VER_NS1); p32(cmd + 16, WA_WMI_TLV_ABI_VER_NS2); p32(cmd + 20, WA_WMI_TLV_ABI_VER_NS3);
    p32(cmd + 24, w->nchunks);
    // wmi_tlv_resource_config, field by field (the order of struct wmi_tlv_resource_config)
    unsigned i = 0;
#define CFG(v) do {p32(cfg + 4 * i, (v)); ++i;} while (0)
    const int rx_reorder = wwmi_service(w, WA_WMI_TLV_SERVICE_RX_FULL_REORDER);
    CFG(WA_TARGET_TLV_NUM_VDEVS);                       // num_vdevs
    CFG(WA_TARGET_HL_TLV_NUM_PEERS);                    // num_peers (hw_params.num_peers)
    CFG(rx_reorder ? WA_TARGET_TLV_NUM_VDEVS : 0);      // num_offload_peers
    CFG(rx_reorder ? WA_TARGET_TLV_NUM_VDEVS : 0);      // num_offload_reorder_bufs
    CFG(2);                                             // num_peer_keys
    CFG(WA_TARGET_HL_TLV_NUM_PEERS * 2);                // num_tids
    CFG(WA_TARGET_HL_TLV_AST_SKID_LIMIT);               // ast_skid_limit
    CFG(0x7);                                           // tx_chain_mask
    CFG(0x7);                                           // rx_chain_mask
    CFG(0x64); CFG(0x64); CFG(0x64); CFG(0x28);         // rx_timeout_pri[4]
    CFG(WA_ATH10K_HW_TXRX_NATIVE_WIFI);                 // rx_decap_mode (ar->wmi.rx_decap_mode)
    CFG(4);                                             // scan_max_pending_reqs
    CFG(WA_TARGET_TLV_NUM_VDEVS);                       // bmiss_offload_max_vdev
    CFG(WA_TARGET_TLV_NUM_VDEVS);                       // roam_offload_max_vdev
    CFG(8);                                             // roam_offload_max_ap_profiles
    CFG(0); CFG(0); CFG(0);                             // num_mcast_groups, num_mcast_table_elems, mcast2ucast_mode
    CFG(0x400);                                         // tx_dbg_log_size
    CFG(WA_TARGET_HL_TLV_NUM_WDS_ENTRIES);              // num_wds_entries
    CFG(0); CFG(0); CFG(0); CFG(0);                     // dma_burst_size, mac_aggr_delim, rx_skip_defrag_..., vow_config
    CFG(2);                                             // gtk_offload_max_vdev
    CFG(w->num_msdu_desc);                              // num_msdu_desc (htt.max_num_pending_tx)
    CFG(2);                                             // max_frag_entries
    CFG(WA_TARGET_TLV_NUM_TDLS_VDEVS);                  // num_tdls_vdevs
    CFG(0x20);                                          // num_tdls_conn_table_entries
    CFG(2);                                             // beacon_tx_offload_max_vdev
    CFG(5);                                             // num_multicast_filter_entries
    CFG(WA_TARGET_TLV_NUM_WOW_PATTERNS);                // num_wow_filters
    CFG(6);                                             // num_keep_alive_pattern
    CFG(0);                                             // keep_alive_pattern_size
    CFG(1); CFG(1);                                     // max_tdls_concurrent_sleep_sta, ..._buffer_sta
    CFG(0);                                             // wmi_send_separate
    CFG(0); CFG(0); CFG(0);                             // num_ocb_vdevs, channels, schedules
    CFG(WA_WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL | (wwmi_service(w, WA_WMI_TLV_SERVICE_TX_DATA_MGMT_ACK_RSSI) ? WA_WMI_RSRC_CFG_FLAG_TX_ACK_RSSI : 0));
#undef CFG
    if (4 * i != 176) return WWMI_ERR_INVAL;   // the field list must cover the struct exactly
    const int ext = wwmi_service(w, WA_WMI_TLV_SERVICE_SUPPORT_EXTEND_ADDRESS);
    for (unsigned c = 0; c < w->nchunks; ++c) {
        uint8_t *h = chunks + c * (4 + 16);
        p16(h, 16); p16(h + 2, WA_WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK);
        p32(h + 4, w->chunk[c].req_id); p32(h + 8, (uint32_t)w->chunk[c].iova); p32(h + 12, w->chunk[c].len);
        p32(h + 16, ext ? (uint32_t)(w->chunk[c].iova >> 32) : 0);
    }
    return tw_send(&t, w);
}

int wwmi_pdev_set_param(wwmi *w, uint32_t param, uint32_t value)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_PDEV_SET_PARAM_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD, 12);
    p32(c, 0); p32(c + 4, param); p32(c + 8, value);        // pdev_id (unused), param_id, param_value
    return tw_send(&t, w);
}

int wwmi_pdev_set_regdomain(wwmi *w, uint32_t rd, uint32_t rd2g, uint32_t rd5g, uint32_t ctl2g, uint32_t ctl5g)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_PDEV_SET_REGDOMAIN_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_PDEV_SET_REGDOMAIN_CMD, 24);
    p32(c, 0); p32(c + 4, rd); p32(c + 8, rd2g); p32(c + 12, rd5g); p32(c + 16, ctl2g); p32(c + 20, ctl5g);
    return tw_send(&t, w);
}

static void put_channel(uint8_t *ci, const wwmi_chan *ch)
{
    // struct wmi_channel: mhz, band_center_freq1, band_center_freq2, flags (mode in the low byte), reginfo0, reginfo1
    p32(ci, ch->freq); p32(ci + 4, ch->center1); p32(ci + 8, ch->center2);
    p32(ci + 12, ch->flags | (ch->mode & 0x3f));
    // reginfo0: min_power, max_power, reg_power, reg_classid; reginfo1: antenna_max, max_tx_power
    ci[16] = (uint8_t)ch->min_power; ci[17] = (uint8_t)ch->max_power; ci[18] = (uint8_t)ch->reg_power; ci[19] = (uint8_t)ch->reg_class;
    ci[20] = (uint8_t)ch->antenna_max; ci[21] = (uint8_t)ch->max_power;
}

int wwmi_scan_chan_list(wwmi *w, const wwmi_chan *ch, unsigned n)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_SCAN_CHAN_LIST_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_SCAN_CHAN_LIST_CMD, 4);
    p32(c, n);
    uint8_t *arr = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_STRUCT, n * (4 + 24));
    if (t.err) return t.err;
    for (unsigned i = 0; i < n; ++i) {
        uint8_t *h = arr + i * (4 + 24);
        p16(h, 24); p16(h + 2, WA_WMI_TLV_TAG_STRUCT_CHANNEL);
        put_channel(h + 4, &ch[i]);
    }
    return tw_send(&t, w);
}

void wwmi_scan_init(wwmi_scan_arg *a)
{
    zero(a, sizeof *a);
    a->scan_req_id = 1;
    a->priority = WA_WMI_SCAN_PRIORITY_LOW;
    a->dwell_active = 50; a->dwell_passive = 150; a->min_rest = 50; a->max_rest = 500;
    a->max_scan_time = 20000; a->probe_delay = 5;
    a->notify_events = WA_WMI_SCAN_EVENT_STARTED | WA_WMI_SCAN_EVENT_COMPLETED | WA_WMI_SCAN_EVENT_BSS_CHANNEL |
                       WA_WMI_SCAN_EVENT_FOREIGN_CHANNEL | WA_WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT | WA_WMI_SCAN_EVENT_DEQUEUED;
    a->ctrl_flags |= WA_WMI_SCAN_CHAN_STAT_EVENT;
    a->n_bssids = 1;
    for (unsigned i = 0; i < 6; ++i) a->bssid[0][i] = 0xff;
}

int wwmi_start_scan(wwmi *w, const wwmi_scan_arg *a)
{
    if (a->n_channels > WWMI_MAX_SCAN_CHANS || a->n_ssids > 2 || a->n_bssids > 2 || a->ie_len > 256) return WWMI_ERR_INVAL;
    tw t;
    tw_start(&t, w, WA_WMI_TLV_START_SCAN_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_START_SCAN_CMD, 100);
    const uint32_t ie_len = (a->ie_len + 3u) & ~3u;
    uint8_t *chans = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_UINT32, a->n_channels * 4);
    uint8_t *ssids = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_FIXED_STRUCT, a->n_ssids * 36);
    uint8_t *bssids = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_FIXED_STRUCT, a->n_bssids * 8);
    uint8_t *ie = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_BYTE, ie_len);
    if (t.err) return t.err;
    // struct wmi_start_scan_common (60 bytes)
    p32(c + 0, WA_WMI_HOST_SCAN_REQ_ID_PREFIX | a->scan_id);
    p32(c + 4, WA_WMI_HOST_SCAN_REQUESTOR_ID_PREFIX | a->scan_req_id);
    p32(c + 8, a->vdev_id); p32(c + 12, a->priority); p32(c + 16, a->notify_events); p32(c + 20, a->dwell_active);
    p32(c + 24, a->dwell_passive); p32(c + 28, a->min_rest); p32(c + 32, a->max_rest); p32(c + 36, a->repeat_probe);
    p32(c + 40, a->probe_spacing); p32(c + 44, a->idle_time); p32(c + 48, a->max_scan_time); p32(c + 52, a->probe_delay);
    // "WMI-TLV inverts the logic behind WMI_SCAN_FILTER_PROBE_REQ"
    p32(c + 56, a->ctrl_flags ^ WA_WMI_SCAN_FILTER_PROBE_REQ);
    p32(c + 60, a->burst_duration);
    p32(c + 64, a->n_channels);
    p32(c + 68, a->n_bssids); p32(c + 72, a->n_ssids);
    p32(c + 76, a->ie_len);
    p32(c + 80, 3);                                  // num_probes
    // mac_addr / mac_mask (random MAC scanning) stay zero
    for (unsigned i = 0; i < a->n_channels; ++i) p32(chans + 4 * i, a->channels[i]);
    for (unsigned i = 0; i < a->n_ssids; ++i) {
        p32(ssids + i * 36, a->ssid_len[i]);
        copy(ssids + i * 36 + 4, a->ssid[i], a->ssid_len[i] > 32 ? 32 : a->ssid_len[i]);
    }
    for (unsigned i = 0; i < a->n_bssids; ++i) copy(bssids + i * 8, a->bssid[i], 6);
    if (a->ie_len) copy(ie, a->ie, a->ie_len);
    return tw_send(&t, w);
}

int wwmi_stop_scan(wwmi *w, uint32_t req_id, uint32_t scan_id, uint32_t vdev_id, int one)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_STOP_SCAN_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_STOP_SCAN_CMD, 16);
    // wmi_stop_scan_cmd: scan_req_id, scan_id, req_type, vdev_id; WMI_SCAN_STOP_ONE = 0x20000000, WMI_SCAN_STOP_VDEV_ALL = 0x20000001
    p32(c, WA_WMI_HOST_SCAN_REQUESTOR_ID_PREFIX | req_id); p32(c + 4, WA_WMI_HOST_SCAN_REQ_ID_PREFIX | scan_id);
    p32(c + 8, one ? 0x20000000u : 0x20000001u); p32(c + 12, vdev_id);
    return tw_send(&t, w);
}

int wwmi_vdev_create(wwmi *w, uint32_t vdev_id, uint32_t type, uint32_t subtype, const uint8_t mac[6])
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_VDEV_CREATE_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_VDEV_CREATE_CMD, 20);
    p32(c, vdev_id); p32(c + 4, type); p32(c + 8, subtype); copy(c + 12, mac, 6);
    return tw_send(&t, w);
}

int wwmi_vdev_delete(wwmi *w, uint32_t vdev_id)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_VDEV_DELETE_CMDID);
    p32(tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_VDEV_DELETE_CMD, 4), vdev_id);
    return tw_send(&t, w);
}

int wwmi_vdev_start(wwmi *w, const wwmi_vdev_start_arg *a, int restart)
{
    if (a->ssid_len > 32) return WWMI_ERR_INVAL;
    tw t;
    tw_start(&t, w, restart ? 0x5004u /* WMI_TLV_VDEV_RESTART_REQUEST_CMDID */ : WA_WMI_TLV_VDEV_START_REQUEST_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_VDEV_START_REQUEST_CMD, 72);
    uint8_t *ch = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_CHANNEL, 24);
    tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_STRUCT, 0);     // nested p2p noa descriptors: none
    if (t.err) return t.err;
    // vdev_id, requestor_id, bcn_intval, dtim_period, flags, ssid{len, ssid[32]}, bcn_tx_rate, bcn_tx_power, num_noa_descr, disable_hw_ack
    p32(c, a->vdev_id); p32(c + 8, a->bcn_intval); p32(c + 12, a->dtim_period); p32(c + 16, a->flags);
    if (a->ssid) {p32(c + 20, a->ssid_len); copy(c + 20 + 4, a->ssid, a->ssid_len);}
    p32(c + 56, a->bcn_tx_rate); p32(c + 60, a->bcn_tx_power); p32(c + 64, 0); p32(c + 68, a->disable_hw_ack);
    put_channel(ch, &a->chan);
    return tw_send(&t, w);
}

static int vdev_only(wwmi *w, uint32_t cmd, uint32_t tag, uint32_t vdev_id)
{
    tw t;
    tw_start(&t, w, cmd);
    p32(tw_tlv(&t, tag, 4), vdev_id);
    return tw_send(&t, w);
}
int wwmi_vdev_stop(wwmi *w, uint32_t vdev_id) {return vdev_only(w, WA_WMI_TLV_VDEV_STOP_CMDID, WA_WMI_TLV_TAG_STRUCT_VDEV_STOP_CMD, vdev_id);}
int wwmi_vdev_down(wwmi *w, uint32_t vdev_id) {return vdev_only(w, WA_WMI_TLV_VDEV_DOWN_CMDID, WA_WMI_TLV_TAG_STRUCT_VDEV_DOWN_CMD, vdev_id);}

int wwmi_vdev_up(wwmi *w, uint32_t vdev_id, uint32_t aid, const uint8_t bssid[6])
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_VDEV_UP_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_VDEV_UP_CMD, 16);
    p32(c, vdev_id); p32(c + 4, aid); copy(c + 8, bssid, 6);
    return tw_send(&t, w);
}

int wwmi_vdev_set_param(wwmi *w, uint32_t vdev_id, uint32_t param, uint32_t value)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_VDEV_SET_PARAM_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_VDEV_SET_PARAM_CMD, 12);
    p32(c, vdev_id); p32(c + 4, param); p32(c + 8, value);
    return tw_send(&t, w);
}

int wwmi_vdev_install_key(wwmi *w, const wwmi_key_arg *a)
{
    if ((a->key_cipher == WA_WMI_CIPHER_NONE) != (a->key == 0) || a->key_len > 64) return WWMI_ERR_INVAL;
    tw t;
    tw_start(&t, w, WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_CMD, 92);
    const uint32_t klen = (a->key_len + 3u) & ~3u;
    uint8_t *k = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_BYTE, klen);
    if (t.err) return t.err;
    // vdev_id, peer_macaddr(8), key_idx, key_flags, key_cipher, 3 x seq counters(8), wpi rsc(16), wpi tsc(16), key_len, txmic, rxmic
    p32(c, a->vdev_id);
    if (a->macaddr) copy(c + 4, a->macaddr, 6);
    p32(c + 12, a->key_idx); p32(c + 16, a->key_flags); p32(c + 20, a->key_cipher);
    p32(c + 80, a->key_len);
    p32(c + 80 + 4, a->txmic_len); p32(c + 80 + 8, a->rxmic_len);
    if (a->key) copy(k, a->key, a->key_len);
    return tw_send(&t, w);
}

int wwmi_peer_create(wwmi *w, uint32_t vdev_id, const uint8_t addr[6], uint32_t type)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_PEER_CREATE_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_PEER_CREATE_CMD, 16);
    p32(c, vdev_id); copy(c + 4, addr, 6); p32(c + 12, type);
    return tw_send(&t, w);
}

int wwmi_peer_delete(wwmi *w, uint32_t vdev_id, const uint8_t addr[6])
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_PEER_DELETE_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_PEER_DELETE_CMD, 12);
    p32(c, vdev_id); copy(c + 4, addr, 6);
    return tw_send(&t, w);
}

int wwmi_peer_set_param(wwmi *w, uint32_t vdev_id, const uint8_t addr[6], uint32_t param, uint32_t value)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_PEER_SET_PARAM_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_PEER_SET_PARAM_CMD, 20);
    p32(c, vdev_id); copy(c + 4, addr, 6); p32(c + 12, param); p32(c + 16, value);
    return tw_send(&t, w);
}

int wwmi_peer_assoc(wwmi *w, const wwmi_peer_assoc_arg *a)
{
    if (a->n_legacy > 32 || a->n_ht > 32) return WWMI_ERR_INVAL;
    tw t;
    tw_start(&t, w, WA_WMI_TLV_PEER_ASSOC_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD, 76);
    const uint32_t ll = (a->n_legacy + 3u) & ~3u, hl = (a->n_ht + 3u) & ~3u;
    uint8_t *leg = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_BYTE, ll);
    uint8_t *ht = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_BYTE, hl);
    uint8_t *vht = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_VHT_RATE_SET, 16);
    if (t.err) return t.err;
    // mac_addr(8), vdev_id, new_assoc, assoc_id, flags, caps, listen_intval, ht_caps, max_mpdu, mpdu_density, rate_caps, nss,
    // vht_caps, phy_mode, ht_info[2], num_legacy_rates, num_ht_rates
    copy(c, a->addr, 6);
    p32(c + 8, a->vdev_id); p32(c + 12, a->reassoc ? 0 : 1); p32(c + 16, a->aid); p32(c + 20, a->flags);
    p32(c + 24, a->caps); p32(c + 28, a->listen_intval); p32(c + 32, a->ht_caps); p32(c + 36, a->max_mpdu); p32(c + 40, a->mpdu_density);
    p32(c + 44, a->rate_caps); p32(c + 48, a->nss); p32(c + 52, a->vht_caps); p32(c + 56, a->phymode);
    p32(c + 60, a->ht_info[0]); p32(c + 64, a->ht_info[1]); p32(c + 68, a->n_legacy); p32(c + 72, a->n_ht);
    copy(leg, a->legacy_rates, a->n_legacy);
    copy(ht, a->ht_rates, a->n_ht);
    p32(vht, a->vht_rx_max); p32(vht + 4, a->vht_rx_mcs); p32(vht + 8, a->vht_tx_max); p32(vht + 12, a->vht_tx_mcs);
    return tw_send(&t, w);
}

int wwmi_set_psmode(wwmi *w, uint32_t vdev_id, uint32_t mode)
{
    tw t;
    tw_start(&t, w, WA_WMI_TLV_STA_POWERSAVE_MODE_CMDID);
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_STA_POWERSAVE_MODE_CMD, 8);
    p32(c, vdev_id); p32(c + 4, mode);
    return tw_send(&t, w);
}

int wwmi_mgmt_tx(wwmi *w, uint32_t vdev_id, uint32_t desc_id, const uint8_t *frame, uint32_t len, uint64_t paddr)
{
    if (!len || len > 2048) return WWMI_ERR_INVAL;
    tw t;
    tw_start(&t, w, WA_WMI_TLV_MGMT_TX_SEND_CMD);
    uint32_t buf_len = len < WWMI_MGMT_INLINE ? len : WWMI_MGMT_INLINE;
    buf_len = (buf_len + 3u) & ~3u;
    uint8_t *c = tw_tlv(&t, WA_WMI_TLV_TAG_STRUCT_MGMT_TX_CMD, 28);
    uint8_t *b = tw_tlv(&t, WA_WMI_TLV_TAG_ARRAY_BYTE, buf_len);
    if (t.err) return t.err;
    // vdev_id, desc_id, chanfreq, paddr (le64, unaligned), frame_len, buf_len
    p32(c, vdev_id); p32(c + 4, desc_id); p32(c + 8, 0); p64(c + 12, paddr);
    p32(c + 20, len); p32(c + 20 + 4, buf_len);
    copy(b, frame, len < buf_len ? len : buf_len);
    return tw_send(&t, w);
}
