// SPDX-License-Identifier: BSD-2-Clause-Patent
// HTT data path of the WCN3990 (ath10k htt_tx.c / htt_rx.c). See WifiHtt.h.
#include "WifiHtt.h"
#include "WifiAth10kIds.h"
#include "WifiHtc.h"
#include <string.h>

#define TXBUF_BYTES 44u        // ath10k_htt_txbuf_64: frags[2] (16), htc hdr (8), cmd hdr (1), tx desc (19)
#define TXBUF_SG_OFF 16u       // the first gather fragment starts at the HTC header
#define TXBUF_SG_LEN 28u
#define FRAG_BYTES 72u         // htt_msdu_ext_desc_64
#define FRAG_FIRST_OFF 24u     // tso_flag[5] + ip_id + flags + reserved
#define RXD_ATTENTION 4u       // offsetof(htt_rx_desc_v2, attention)
#define RXD_MSDU_START_INFO1 36u   // msdu_start (28) + info1 (8)
#define RXD_MSDU_END_INFO0 64u     // msdu_end (48) + info0 (16)
#define RXD_PAYLOAD 360u
#define DECAP_NWIFI 1u

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t le64(const uint8_t *p) {return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;}
static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16));}
static void put64(uint8_t *p, uint64_t v) {put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32));}

static int pow2(uint32_t v) {return v && !(v & (v - 1));}

int whtt_init(whtt *h, const whtt_ops *ops, const whtt_cfg *cfg)
{
    memset(h, 0, sizeof *h);
    h->ops = *ops;
    h->cfg = *cfg;
    if (!ops->ctl_send || !ops->tx_sg || !ops->dma_alloc || !ops->rx) return WHTT_ERR_INVAL;
    if (!pow2(cfg->rx_ring_size) || cfg->rx_ring_size < WA_HTT_RX_RING_SIZE_MIN || cfg->rx_ring_size > WA_HTT_RX_RING_SIZE_MAX) return WHTT_ERR_INVAL;
    if (!cfg->max_tx || cfg->max_tx > WHTT_MAX_TX) return WHTT_ERR_INVAL;
    if (!h->cfg.max_ampdu) h->cfg.max_ampdu = 64;
    if (!h->cfg.max_amsdu) h->cfg.max_amsdu = 3;
    h->size_mask = cfg->rx_ring_size - 1;
    h->fill_level = cfg->rx_ring_size / 2 - 1;      // HTT_RX_RING_FILL_LEVEL: the ring stays half empty
    h->pool_bufs = h->fill_level + 1;
    h->ring = ops->dma_alloc(ops->ctx, cfg->rx_ring_size * 8u, 8, &h->ring_iova);
    h->alloc_idx = ops->dma_alloc(ops->ctx, 4, 4, &h->alloc_idx_iova);
    h->pool = ops->dma_alloc(ops->ctx, h->pool_bufs * WHTT_RX_BUF, WHTT_RX_BUF, &h->pool_iova);
    h->txbuf = ops->dma_alloc(ops->ctx, cfg->max_tx * TXBUF_BYTES, 8, &h->txbuf_iova);
    h->frag = ops->dma_alloc(ops->ctx, cfg->max_tx * FRAG_BYTES, 8, &h->frag_iova);
    h->data = ops->dma_alloc(ops->ctx, cfg->max_tx * WHTT_TX_BUF, 8, &h->data_iova);
    if (!h->ring || !h->alloc_idx || !h->pool || !h->txbuf || !h->frag || !h->data) return WHTT_ERR_NOMEM;
    uint64_t unused;
    h->posted = ops->dma_alloc(ops->ctx, h->pool_bufs, 1, &unused);
    h->free_ids = ops->dma_alloc(ops->ctx, cfg->max_tx * 2u, 2, &unused);
    h->busy = ops->dma_alloc(ops->ctx, cfg->max_tx, 1, &unused);
    if (!h->posted || !h->free_ids || !h->busy) return WHTT_ERR_NOMEM;
    for (uint32_t i = 0; i < cfg->max_tx; i++) h->free_ids[h->nfree++] = (uint16_t)(cfg->max_tx - 1 - i);
    for (uint32_t i = 0; i < h->fill_level; i++) {   // post the first fill_level buffers
        uint8_t *b = h->pool + (size_t)i * WHTT_RX_BUF;
        memset(b + RXD_ATTENTION, 0, 4);             // attention cleared before posting
        h->ring[h->fill] = h->pool_iova + (uint64_t)i * WHTT_RX_BUF;
        h->posted[i] = 1;
        h->fill = (h->fill + 1) & h->size_mask;
    }
    if (ops->barrier) ops->barrier(ops->ctx);
    *h->alloc_idx = h->fill;
    return WHTT_OK;
}

int whtt_start(whtt *h)
{
    uint8_t m[4] = {WA_HTT_H2T_MSG_TYPE_VERSION_REQ, 0, 0, 0};   // htt_cmd_hdr + htt_ver_req (3 pad bytes)
    h->state = WHTT_VERSION_WAIT;
    int r = h->ops.ctl_send(h->ops.ctx, m, sizeof m);
    if (r) h->state = WHTT_FAILED;
    return r;
}

static int send_setup(whtt *h)
{
    int r;
    // FRAG_DESC_BANK_CFG: htt_cmd_hdr(1) + htt_frag_desc_bank_cfg64 (63): info, num_banks, desc_size, 4 x bank addr (64),
    // 4 x bank id (min, max), q_state (paddr, num_peers, num_tids, record_size, record_multiplier, pad[2]); no flow control: zeros.
    uint8_t f[64];
    memset(f, 0, sizeof f);
    f[0] = WA_HTT_H2T_MSG_TYPE_FRAG_DESC_BANK_CFG;
    f[1] = 0;                                   // info: depth type bytes, q state not valid
    f[2] = 1;                                   // num_banks
    f[3] = FRAG_BYTES;                          // desc_size
    put64(f + 4, h->frag_iova);                 // bank_base_addrs[0]
    put16(f + 4 + 32, 0);                       // bank_id[0].min
    put16(f + 4 + 32 + 2, (uint16_t)(h->cfg.max_tx - 1));
    if ((r = h->ops.ctl_send(h->ops.ctx, f, sizeof f))) return r;
    // RX_RING_CFG: htt_cmd_hdr + htt_rx_ring_setup_hdr(3) + htt_rx_ring_setup_ring64 (44)
    uint8_t g[1 + 3 + 44];
    memset(g, 0, sizeof g);
    g[0] = WA_HTT_H2T_MSG_TYPE_RX_RING_CFG;
    g[1] = 1;                                   // num_rings
    uint8_t *rg = g + 4;
    put64(rg, h->alloc_idx_iova);               // fw_idx_shadow_reg_paddr
    put64(rg + 8, h->ring_iova);                // rx_ring_base_paddr
    put16(rg + 16, (uint16_t)h->cfg.rx_ring_size);
    put16(rg + 18, WHTT_RX_BUF);
    put16(rg + 20, (uint16_t)(WA_HTT_RX_RING_FLAGS_MAC80211_HDR | WA_HTT_RX_RING_FLAGS_MSDU_PAYLOAD | WA_HTT_RX_RING_FLAGS_PPDU_START |
                              WA_HTT_RX_RING_FLAGS_PPDU_END | WA_HTT_RX_RING_FLAGS_MPDU_START | WA_HTT_RX_RING_FLAGS_MPDU_END |
                              WA_HTT_RX_RING_FLAGS_MSDU_START | WA_HTT_RX_RING_FLAGS_MSDU_END | WA_HTT_RX_RING_FLAGS_RX_ATTENTION |
                              WA_HTT_RX_RING_FLAGS_FRAG_INFO | WA_HTT_RX_RING_FLAGS_UNICAST_RX | WA_HTT_RX_RING_FLAGS_MULTICAST_RX |
                              WA_HTT_RX_RING_FLAGS_CTRL_RX | WA_HTT_RX_RING_FLAGS_MGMT_RX | WA_HTT_RX_RING_FLAGS_NULL_RX |
                              WA_HTT_RX_RING_FLAGS_PHY_DATA_RX));
    put16(rg + 22, (uint16_t)*h->alloc_idx);    // fw_idx_init_val
    // htt_rx_ring_rx_desc_offsets, in 4-byte units: mac80211 hdr, msdu payload, ppdu start/end, mpdu start/end, msdu start/end,
    // rx attention, frag info (htt_rx_desc_v2)
    put16(rg + 24, 296 / 4); put16(rg + 26, 360 / 4); put16(rg + 28, 108 / 4); put16(rg + 30, 148 / 4);
    put16(rg + 32, 16 / 4); put16(rg + 34, 104 / 4); put16(rg + 36, 28 / 4); put16(rg + 38, 48 / 4);
    put16(rg + 40, 4 / 4); put16(rg + 42, 8 / 4);
    if ((r = h->ops.ctl_send(h->ops.ctx, g, sizeof g))) return r;
    // AGGR_CFG v2
    uint8_t a[4] = {WA_HTT_H2T_MSG_TYPE_AGGR_CFG, h->cfg.max_ampdu, h->cfg.max_amsdu, 0};
    return h->ops.ctl_send(h->ops.ctx, a, sizeof a);
}

// ---- rx ----

static void repost(whtt *h, uint32_t idx)
{
    uint8_t *b = h->pool + (size_t)idx * WHTT_RX_BUF;
    memset(b + RXD_ATTENTION, 0, 4);
    h->ring[h->fill] = h->pool_iova + (uint64_t)idx * WHTT_RX_BUF;
    h->posted[idx] = 1;
    h->fill = (h->fill + 1) & h->size_mask;
}

// native-wifi MSDU (802.11 header, LLC/SNAP, payload) -> Ethernet II into out; returns the length or 0 to drop
static uint32_t nwifi_to_eth(whtt *h, const uint8_t *f, uint32_t len, uint8_t *out)
{
    if (len < 24) return 0;
    const unsigned fc0 = f[0], fc1 = f[1];
    if ((fc0 & 0x0c) != 0x08) {++h->rx_drop_nondata; return 0;}     // not a data frame
    const int to_ds = fc1 & 1, from_ds = (fc1 >> 1) & 1;
    uint32_t hl = 24;
    if (to_ds && from_ds) hl += 6;
    if (fc0 & 0x80) hl += 2;                                        // QoS
    if (h->cfg.nwifi_pad) hl = (hl + 3) & ~3u;
    if (len < hl + 8) return 0;
    const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16, *a4 = f + 24;
    const uint8_t *da, *sa;
    if (to_ds && from_ds) {da = a3; sa = a4;} else if (to_ds) {da = a3; sa = a2;} else if (from_ds) {da = a1; sa = a3;} else {da = a1; sa = a2;}
    const uint8_t *l = f + hl;
    if (!(l[0] == 0xaa && l[1] == 0xaa && l[2] == 3 && l[3] == 0 && l[4] == 0 && l[5] == 0)) {++h->rx_drop_fmt; return 0;}
    const uint32_t pl = len - hl - 8;
    if (14 + pl > WHTT_RX_BUF) return 0;
    memcpy(out, da, 6);
    memcpy(out + 6, sa, 6);
    out[12] = l[6]; out[13] = l[7];
    memcpy(out + 14, l + 8, pl);
    return 14 + pl;
}

static void rx_in_order(whtt *h, const uint8_t *m, uint32_t len)
{
    if (len < 8) {++h->rx_bad; return;}
    ++h->rx_ind;
    const uint32_t count = le16(m + 6);
    if (len < 8 + count * 12u) {++h->rx_bad; return;}
    if (h->rx_confused) return;
    const int offload = (m[1] & 0x20) != 0;   // HTT_RX_IN_ORD_IND_INFO_OFFLOAD_MASK: no rx descriptor, a 7-byte header
    const uint8_t *d = m + 8;
    for (uint32_t i = 0; i < count; i++, d += 12) {
        const uint64_t pa = le64(d);
        const uint32_t mlen = le16(d + 8);
        if (pa < h->pool_iova || pa >= h->pool_iova + (uint64_t)h->pool_bufs * WHTT_RX_BUF || ((pa - h->pool_iova) % WHTT_RX_BUF)) {
            ++h->rx_bad; h->rx_confused = 1; return;
        }
        const uint32_t idx = (uint32_t)((pa - h->pool_iova) / WHTT_RX_BUF);
        if (!h->posted[idx]) {++h->rx_bad; h->rx_confused = 1; return;}
        h->posted[idx] = 0;
        uint8_t *b = h->pool + (size_t)idx * WHTT_RX_BUF;
        if (offload) {   // ath10k_htt_rx_h_rx_offload: htt_rx_offload_msdu {msdu_len, peer_id, vdev_id, tid, fw_desc} + nwifi
            ++h->rx_offload;
            const uint32_t olen = le16(b);
            if (olen && olen <= WHTT_RX_BUF - 7) {
                uint8_t eth[WHTT_RX_BUF];
                const int pad = h->cfg.nwifi_pad;
                h->cfg.nwifi_pad = 0;   // the offloaded header is not padded
                const uint32_t n = nwifi_to_eth(h, b + 7, olen, eth);
                h->cfg.nwifi_pad = pad;
                if (n) {++h->rx_frames; h->ops.rx(h->ops.ctx, eth, n);}
            } else {
                ++h->rx_bad;
            }
            repost(h, idx);
            continue;
        }
        const uint32_t attn = le32(b + RXD_ATTENTION);
        const uint32_t decap = (le32(b + RXD_MSDU_START_INFO1) >> 8) & 3u;
        if (!(attn & WA_RX_ATTENTION_FLAGS_MSDU_DONE)) {++h->rx_bad; h->rx_confused = 1; return;}   // ath10k: incomplete frame
        if (attn & (WA_RX_ATTENTION_FLAGS_FCS_ERR | WA_RX_ATTENTION_FLAGS_DECRYPT_ERR | WA_RX_ATTENTION_FLAGS_TKIP_MIC_ERR |
                    WA_RX_ATTENTION_FLAGS_MPDU_LENGTH_ERR)) {
            ++h->rx_drop_attn;
        } else if (decap != DECAP_NWIFI) {
            ++h->rx_drop_decap;
        } else if (mlen > WHTT_RX_BUF - RXD_PAYLOAD) {
            ++h->rx_bad;
        } else {
            uint8_t eth[WHTT_RX_BUF];
            const uint32_t n = nwifi_to_eth(h, b + RXD_PAYLOAD, mlen, eth);
            if (n) {++h->rx_frames; h->ops.rx(h->ops.ctx, eth, n);}
        }
        repost(h, idx);
    }
    if (h->ops.barrier) h->ops.barrier(h->ops.ctx);
    *h->alloc_idx = h->fill;
}

static void tx_complete(whtt *h, const uint8_t *m, uint32_t len)
{
    if (len < 4) {++h->rx_bad; return;}
    const unsigned status = m[1] & 7u;        // HTT_DATA_TX_STATUS: 0 ok, 1 discard, 2 no ack, 3 postpone
    const uint32_t n = m[2];
    if (len < 4 + n * 2u) {++h->rx_bad; return;}
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t id = le16(m + 4 + 2 * i);
        if (id >= h->cfg.max_tx || !h->busy[id]) {++h->rx_bad; continue;}
        h->busy[id] = 0;
        h->free_ids[h->nfree++] = (uint16_t)id;
        ++h->tx_done;
        if (h->ops.tx_done) h->ops.tx_done(h->ops.ctx, status);
    }
}

void whtt_rx(whtt *h, const uint8_t *m, uint32_t len)
{
    if (!len) return;
    switch (m[0]) {
    case WA_HTT_TLV_T2H_MSG_TYPE_VERSION_CONF:
        if (len < 3 || h->state != WHTT_VERSION_WAIT) {++h->rx_bad; return;}
        h->ver_minor = m[1]; h->ver_major = m[2];
        if (h->ver_major != 2 && h->ver_major != 3) {h->state = WHTT_FAILED; return;}
        if (send_setup(h)) {h->state = WHTT_FAILED; return;}
        h->state = WHTT_READY;
        return;
    case WA_HTT_TLV_T2H_MSG_TYPE_RX_IN_ORD_PADDR_IND: rx_in_order(h, m, len); return;
    case WA_HTT_TLV_T2H_MSG_TYPE_TX_COMPL_IND: tx_complete(h, m, len); return;
    case WA_HTT_TLV_T2H_MSG_TYPE_SEC_IND:
        ++h->sec_ind;
        if (len >= 4 && h->ops.sec_ind) h->ops.sec_ind(h->ops.ctx, le16(m + 2), m[1]);
        return;
    case WA_HTT_TLV_T2H_MSG_TYPE_PEER_MAP: ++h->peer_map; return;
    default: ++h->other_msgs; return;
    }
}

// ---- tx ----

unsigned whtt_tx_free(const whtt *h) {return h->nfree;}

int whtt_tx(whtt *h, unsigned vdev, const uint8_t bssid[6], const uint8_t *eth, uint32_t len, int protect)
{
    if (h->state != WHTT_READY) return WHTT_ERR_BUSY;
    if (len < 14 || len > WHTT_MAX_ETH || vdev > 0x3f) return WHTT_ERR_INVAL;
    if (eth[12] < 0x06) return WHTT_ERR_INVAL;                 // 802.3 length field, not an EtherType
    if (!h->nfree) {++h->tx_full; return WHTT_ERR_BUSY;}
    const uint32_t id = h->free_ids[--h->nfree];
    uint8_t *f = h->data + (size_t)id * WHTT_TX_BUF;
    const uint64_t f_iova = h->data_iova + (uint64_t)id * WHTT_TX_BUF;
    // native-wifi data frame: to-DS, addr1 BSSID, addr2 SA, addr3 DA, LLC/SNAP
    memset(f, 0, 24);
    f[0] = 0x08;
    f[1] = (uint8_t)(0x01 | (protect ? 0x40 : 0));
    memcpy(f + 4, bssid, 6);
    memcpy(f + 10, eth + 6, 6);
    memcpy(f + 16, eth, 6);
    f[24] = 0xaa; f[25] = 0xaa; f[26] = 3; f[27] = f[28] = f[29] = 0;
    f[30] = eth[12]; f[31] = eth[13];
    memcpy(f + 32, eth + 14, len - 14);
    const uint32_t flen = 32 + len - 14;
    uint32_t pre = flen < WHTT_PREFETCH ? flen : WHTT_PREFETCH;
    pre = (pre + 3) & ~3u;
    // continuous fragment descriptor of this id: tso flags zero, one fragment
    uint8_t *fd = h->frag + (size_t)id * FRAG_BYTES;
    memset(fd, 0, FRAG_BYTES);
    put32(fd + FRAG_FIRST_OFF, (uint32_t)f_iova);
    put16(fd + FRAG_FIRST_OFF + 4, (uint16_t)(f_iova >> 32));
    put16(fd + FRAG_FIRST_OFF + 6, (uint16_t)flen);
    // txbuf: htc hdr, htt cmd hdr, tx descriptor
    uint8_t *tb = h->txbuf + (size_t)id * TXBUF_BYTES;
    memset(tb, 0, TXBUF_BYTES);
    uint8_t *x = tb + TXBUF_SG_OFF;
    whtc_hdr(x, h->cfg.eid, (uint16_t)(1 + 19 + pre), 0, 0);
    x[8] = WA_HTT_H2T_MSG_TYPE_TX_FRM;
    uint8_t flags0 = (uint8_t)(WA_HTT_DATA_TX_DESC_FLAGS0_MAC_HDR_PRESENT | (WA_ATH10K_HW_TXRX_NATIVE_WIFI << WA_HTT_DATA_TX_DESC_FLAGS0_PKT_TYPE_LSB));
    if (!protect) flags0 |= WA_HTT_DATA_TX_DESC_FLAGS0_NO_ENCRYPT;
    const uint16_t flags1 = (uint16_t)(vdev | (WA_HTT_DATA_TX_EXT_TID_NON_QOS_MCAST_BCAST << WA_HTT_DATA_TX_DESC_FLAGS1_EXT_TID_LSB) |
                                       WA_HTT_DATA_TX_DESC_FLAGS1_POSTPONED);
    x[9] = flags0;
    put16(x + 10, flags1);
    put16(x + 12, (uint16_t)flen);
    put16(x + 14, (uint16_t)id);
    put64(x + 16, h->frag_iova + (uint64_t)id * FRAG_BYTES);
    put32(x + 24, WA_HTT_INVALID_PEERID);
    if (h->ops.barrier) h->ops.barrier(h->ops.ctx);
    h->busy[id] = 1;
    int r = h->ops.tx_sg(h->ops.ctx, h->txbuf_iova + (uint64_t)id * TXBUF_BYTES + TXBUF_SG_OFF, TXBUF_SG_LEN, f_iova, pre);
    if (r) {
        h->busy[id] = 0;
        h->free_ids[h->nfree++] = (uint16_t)id;
        ++h->tx_err;
        return WHTT_ERR_IO;
    }
    ++h->tx_frames;
    return WHTT_OK;
}
