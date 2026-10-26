// SPDX-License-Identifier: BSD-2-Clause-Patent
// Layer 6: the HTT data path (WifiHtt.c) against Linux's structure layouts (ath10k_layout.h) and a small target model: the
// setup messages, TX_FRM descriptors decoded the way the firmware reads them (HTC header, htt command, tx descriptor,
// continuous fragment descriptor, prefetch), TX_COMPL_IND, and in-order rx indications over a ring of rx buffers.
#include "tcheck.h"
#include <string.h>
#include "../WifiHtt.h"
#include "../WifiAth10kIds.h"
#include "ath10k_layout.h"

#define ARENA (1u << 22)
#define IOVA0 0x40000000ull
static uint8_t g_arena[ARENA];
static uint32_t g_used;
static void *adma(void *c, uint32_t size, uint32_t align, uint64_t *iova)
{
    (void)c;
    uint32_t o = (g_used + align - 1) & ~(align - 1);
    if (o + size > ARENA) return NULL;
    g_used = o + size;
    memset(g_arena + o, 0, size);
    *iova = IOVA0 + o;
    return g_arena + o;
}
static uint8_t *host(uint64_t iova) {return g_arena + (iova - IOVA0);}
static void abar(void *c) {(void)c;}

typedef struct {uint8_t d[256]; uint32_t len;} ctlrec;
static ctlrec g_ctl[16];
static unsigned g_nctl;
static int g_ctl_rc;
static int actl(void *c, const void *m, uint32_t len)
{
    (void)c;
    if (g_ctl_rc) return g_ctl_rc;
    if (g_nctl < 16 && len <= 256) {memcpy(g_ctl[g_nctl].d, m, len); g_ctl[g_nctl].len = len; ++g_nctl;}
    return 0;
}
typedef struct {uint8_t a[64]; uint32_t alen; uint8_t b[256]; uint32_t blen; uint64_t ia, ib;} sgrec;
static sgrec g_sg[512];
static unsigned g_nsg;
static int g_sg_rc;
static int asg(void *c, uint64_t i0, uint32_t l0, uint64_t i1, uint32_t l1)
{
    (void)c;
    if (g_sg_rc) return g_sg_rc;
    sgrec *r = &g_sg[g_nsg++ % 512];
    r->ia = i0; r->alen = l0; r->ib = i1; r->blen = l1;
    memcpy(r->a, host(i0), l0 < 64 ? l0 : 64);
    memcpy(r->b, host(i1), l1 < 256 ? l1 : 256);
    return 0;
}
static uint8_t g_rxf[64][2048];
static uint32_t g_rxl[64];
static unsigned g_nrx;
static void arx(void *c, const uint8_t *eth, uint32_t len)
{
    (void)c;
    if (g_nrx < 64 && len <= 2048) {memcpy(g_rxf[g_nrx], eth, len); g_rxl[g_nrx] = len;}
    ++g_nrx;
}
static unsigned g_txd[4];
static void atd(void *c, unsigned st) {(void)c; if (st < 4) ++g_txd[st];}
static unsigned g_sec_peer, g_sec_flags, g_nsec;
static void asec(void *c, unsigned p, unsigned f) {(void)c; g_sec_peer = p; g_sec_flags = f; ++g_nsec;}

static uint32_t le32(const uint8_t *p) {return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t le16(const uint8_t *p) {return p[0] | p[1] << 8;}
static uint64_t le64(const uint8_t *p) {return le32(p) | (uint64_t)le32(p + 4) << 32;}
static void w16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void w32(uint8_t *p, uint32_t v) {w16(p, v); w16(p + 2, v >> 16);}
static void w64(uint8_t *p, uint64_t v) {w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32));}

static const uint8_t BSSID[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
static const uint8_t MYMAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t PEER[6] = {0x00, 0x1c, 0x2d, 0x3e, 0x4f, 0x50};

static whtt *new_h(uint32_t ring, uint32_t max_tx, int pad)
{
    whtt *h = t_talloc(sizeof *h);
    const whtt_ops ops = {NULL, actl, asg, adma, abar, arx, atd, asec};
    const whtt_cfg cfg = {ring, max_tx, 2, 0, 0, pad};
    g_used = 0; g_nctl = 0; g_nsg = 0; g_nrx = 0; g_ctl_rc = 0; g_sg_rc = 0; g_nsec = 0;
    memset(g_txd, 0, sizeof g_txd);
    CHECK(whtt_init(h, &ops, &cfg) == 0);
    return h;
}

static void ready(whtt *h)
{
    CHECK(whtt_start(h) == 0);
    const uint8_t conf[3] = {WA_HTT_TLV_T2H_MSG_TYPE_VERSION_CONF, 0, 3};
    whtt_rx(h, conf, sizeof conf);
}

static void t_htt_init_setup(void)
{
    whtt *h = new_h(128, 32, 1);
    CHECK(h->fill_level == 63 && h->fill == 63 && *h->alloc_idx == 63);
    for (unsigned i = 0; i < 63; ++i) CHECK(h->ring[i] == IOVA0 + (h->pool_iova - IOVA0) + (uint64_t)i * 2048);
    CHECK(h->ring[63] == 0);
    // validation
    whtt *bad = t_talloc(sizeof *bad);
    const whtt_ops ops = {NULL, actl, asg, adma, abar, arx, atd, asec};
    whtt_cfg c1 = {100, 32, 2, 0, 0, 1}, c2 = {128, 0, 2, 0, 0, 1}, c3 = {4096, 32, 2, 0, 0, 1}, c4 = {64, 32, 2, 0, 0, 1};
    CHECK(whtt_init(bad, &ops, &c1) == WHTT_ERR_INVAL && whtt_init(bad, &ops, &c2) == WHTT_ERR_INVAL);
    CHECK(whtt_init(bad, &ops, &c3) == WHTT_ERR_INVAL && whtt_init(bad, &ops, &c4) == WHTT_ERR_INVAL);
    // tx before setup is refused
    uint8_t eth[60] = {0};
    CHECK(whtt_tx(h, 0, BSSID, eth, sizeof eth, 0) == WHTT_ERR_BUSY);
    // setup
    CHECK(whtt_start(h) == 0 && g_nctl == 1 && g_ctl[0].len == 4 && g_ctl[0].d[0] == WA_HTT_H2T_MSG_TYPE_VERSION_REQ);
    CHECK(h->state == WHTT_VERSION_WAIT);
    const uint8_t conf[3] = {WA_HTT_TLV_T2H_MSG_TYPE_VERSION_CONF, 0, 3};
    whtt_rx(h, conf, sizeof conf);
    CHECK(h->state == WHTT_READY && g_nctl == 4 && h->ver_major == 3);
    // frag desc bank cfg
    const ctlrec *f = &g_ctl[1];
    CHECK(f->len == 1 + WL_SIZEOF_htt_frag_desc_bank_cfg64 && f->d[0] == WA_HTT_H2T_MSG_TYPE_FRAG_DESC_BANK_CFG);
    CHECK(f->d[1] == 0 && f->d[2] == 1 && f->d[3] == WL_SIZEOF_htt_msdu_ext_desc_64);
    CHECK(le64(f->d + 4) == h->frag_iova && le16(f->d + 4 + 32) == 0 && le16(f->d + 4 + 34) == 31);
    for (unsigned i = 4 + 40; i < f->len; ++i) CHECK(f->d[i] == 0 || i < 4 + 40);   // q state: no flow control
    // rx ring cfg
    const ctlrec *r = &g_ctl[2];
    CHECK(r->len == 1 + WL_SIZEOF_htt_rx_ring_setup_hdr + WL_SIZEOF_htt_rx_ring_setup_ring64 && r->d[0] == WA_HTT_H2T_MSG_TYPE_RX_RING_CFG && r->d[1] == 1);
    const uint8_t *rg = r->d + 1 + WL_SIZEOF_htt_rx_ring_setup_hdr;
    CHECK(le64(rg) == h->alloc_idx_iova && le64(rg + 8) == h->ring_iova);
    CHECK(le16(rg + 16) == 128 && le16(rg + 18) == 2048 && le16(rg + 22) == 63);
    CHECK(le16(rg + 20) == 0xffff);
    const uint8_t *of = rg + WL_OFF_htt_rx_ring_setup_ring64__offsets;
    CHECK(le16(of + 0) * 4u == WL_OFF_htt_rx_desc_v2__rx_hdr_status && le16(of + 2) * 4u == WL_OFF_htt_rx_desc_v2__msdu_payload);
    CHECK(le16(of + 4) * 4u == WL_OFF_htt_rx_desc_v2__ppdu_start && le16(of + 6) * 4u == WL_OFF_htt_rx_desc_v2__ppdu_end);
    CHECK(le16(of + 8) * 4u == WL_OFF_htt_rx_desc_v2__mpdu_start && le16(of + 10) * 4u == WL_OFF_htt_rx_desc_v2__mpdu_end);
    CHECK(le16(of + 12) * 4u == WL_OFF_htt_rx_desc_v2__msdu_start && le16(of + 14) * 4u == WL_OFF_htt_rx_desc_v2__msdu_end);
    CHECK(le16(of + 16) * 4u == WL_OFF_htt_rx_desc_v2__attention && le16(of + 18) * 4u == WL_OFF_htt_rx_desc_v2__frag_info);
    CHECK(WHTT_RX_DESC_SIZE == WL_SIZEOF_htt_rx_desc_v2);
    // aggr cfg v2
    const ctlrec *a = &g_ctl[3];
    CHECK(a->len == 1 + WL_SIZEOF_htt_aggr_conf_v2 && a->d[0] == WA_HTT_H2T_MSG_TYPE_AGGR_CFG && a->d[1] == 64 && a->d[2] == 3 && a->d[3] == 0);
    t_tfree_all();
}

static void t_htt_version(void)
{
    whtt *h = new_h(128, 8, 1);
    CHECK(whtt_start(h) == 0);
    const uint8_t conf[3] = {WA_HTT_TLV_T2H_MSG_TYPE_VERSION_CONF, 0, 1};
    whtt_rx(h, conf, sizeof conf);
    CHECK(h->state == WHTT_FAILED && g_nctl == 1);
    whtt *h2 = new_h(128, 8, 1);
    g_ctl_rc = -5;
    CHECK(whtt_start(h2) == -5 && h2->state == WHTT_FAILED);
    g_ctl_rc = 0;
    // a setup message failing after the version confirm
    whtt *h3 = new_h(128, 8, 1);
    CHECK(whtt_start(h3) == 0);
    g_ctl_rc = -5;
    const uint8_t ok[3] = {WA_HTT_TLV_T2H_MSG_TYPE_VERSION_CONF, 0, 2};
    whtt_rx(h3, ok, sizeof ok);
    CHECK(h3->state == WHTT_FAILED);
    t_tfree_all();
}

static void mketh(uint8_t *e, const uint8_t *dst, const uint8_t *src, unsigned type, unsigned plen)
{
    memcpy(e, dst, 6); memcpy(e + 6, src, 6);
    e[12] = (uint8_t)(type >> 8); e[13] = (uint8_t)type;
    for (unsigned i = 0; i < plen; ++i) e[14 + i] = (uint8_t)(i * 7 + 3);
}

static void t_htt_tx(void)
{
    whtt *h = new_h(128, 4, 1);
    ready(h);
    uint8_t eth[14 + 100];
    mketh(eth, PEER, MYMAC, 0x0800, 100);
    CHECK(whtt_tx(h, 1, BSSID, eth, sizeof eth, 1) == 0 && g_nsg == 1);
    sgrec *s = &g_sg[0];
    CHECK(s->alen == 28 && s->blen == 52);                       // 32 + 100 = 132 byte frame, prefetch 50 rounded to 52
    const unsigned id = le16(s->a + 8 + 1 + 1 + 2 + 2);
    CHECK(s->a[0] == 2 && s->a[1] == 0 && le16(s->a + 2) == 1 + WL_SIZEOF_htt_data_tx_desc_64 + 52 && s->a[5] == 0);
    CHECK(s->a[8] == WA_HTT_H2T_MSG_TYPE_TX_FRM);
    const uint8_t *tx = s->a + 9;
    CHECK(tx[0] == (WA_HTT_DATA_TX_DESC_FLAGS0_MAC_HDR_PRESENT | (WA_ATH10K_HW_TXRX_NATIVE_WIFI << 5)));
    const unsigned f1 = le16(tx + 1);
    CHECK((f1 & 0x3f) == 1 && ((f1 >> 6) & 0x1f) == WA_HTT_DATA_TX_EXT_TID_NON_QOS_MCAST_BCAST && (f1 & WA_HTT_DATA_TX_DESC_FLAGS1_POSTPONED));
    CHECK(le16(tx + 3) == 132 && id < 4 && le32(tx + 5 + 2 + 2 + 0 - 4 + 4) != 0xffffffffu);
    CHECK(le32(tx + (WL_SIZEOF_htt_data_tx_desc_64 - 4)) == WA_HTT_INVALID_PEERID);
    // the fragment descriptor
    const uint64_t fp = le64(tx + 7);
    CHECK(fp == h->frag_iova + (uint64_t)id * WL_SIZEOF_htt_msdu_ext_desc_64);
    const uint8_t *fd = host(fp);
    CHECK(le32(fd + 0) == 0 && le32(fd + 16) == 0);                                 // tso flags clear
    const uint64_t dp = le32(fd + 24) | (uint64_t)le16(fd + 28) << 32;
    CHECK(le16(fd + 30) == 132 && dp == s->ib);
    for (unsigned i = 32; i < 72; ++i) CHECK(fd[i] == 0);
    // the native-wifi frame in the data buffer
    const uint8_t *f = host(dp);
    CHECK(f[0] == 0x08 && f[1] == 0x41 && !memcmp(f + 4, BSSID, 6) && !memcmp(f + 10, MYMAC, 6) && !memcmp(f + 16, PEER, 6));
    CHECK(!memcmp(f + 24, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8) && !memcmp(f + 32, eth + 14, 100));
    CHECK(!memcmp(s->b, f, 52));
    // an unprotected frame: no-encrypt, protected bit clear
    mketh(eth, PEER, MYMAC, 0x888e, 40);
    CHECK(whtt_tx(h, 0, BSSID, eth, 14 + 40, 0) == 0);
    const sgrec *s2 = &g_sg[1];
    CHECK((s2->a[9] & WA_HTT_DATA_TX_DESC_FLAGS0_NO_ENCRYPT) && s2->blen == 52);
    const uint8_t *f2 = host(s2->ib);
    CHECK(f2[1] == 0x01 && f2[30] == 0x88 && f2[31] == 0x8e);
    const unsigned id2 = le16(s2->a + 14);
    CHECK(id2 != id);
    // a short frame: prefetch is the whole frame rounded up to 4
    mketh(eth, PEER, MYMAC, 0x0806, 1);
    CHECK(whtt_tx(h, 0, BSSID, eth, 15, 1) == 0 && g_sg[2].blen == 36 && le16(g_sg[2].a + 12) == 33);
    // exhaust the ids
    CHECK(whtt_tx(h, 0, BSSID, eth, 15, 1) == 0 && whtt_tx_free(h) == 0);
    CHECK(whtt_tx(h, 0, BSSID, eth, 15, 1) == WHTT_ERR_BUSY && h->tx_full == 1);
    // completion of two ids, one acked, then reuse
    uint8_t cm[8] = {WA_HTT_TLV_T2H_MSG_TYPE_TX_COMPL_IND, 0, 2, 0};
    w16(cm + 4, id); w16(cm + 6, id2);
    whtt_rx(h, cm, sizeof cm);
    CHECK(g_txd[0] == 2 && whtt_tx_free(h) == 2 && h->tx_done == 2);
    cm[1] = 2; cm[2] = 1;                              // no-ack, one msdu
    w16(cm + 4, le16(g_sg[2].a + 14));
    whtt_rx(h, cm, 6);
    CHECK(g_txd[2] == 1 && whtt_tx_free(h) == 3);
    // a completion for an id that is not pending, or a short one
    w16(cm + 4, id);
    whtt_rx(h, cm, 6);
    CHECK(h->rx_bad == 1 && whtt_tx_free(h) == 3);
    whtt_rx(h, cm, 3);
    CHECK(h->rx_bad == 2);
    // errors
    CHECK(whtt_tx(h, 0, BSSID, eth, 10, 1) == WHTT_ERR_INVAL && whtt_tx(h, 64, BSSID, eth, 15, 1) == WHTT_ERR_INVAL);
    eth[12] = 0x00; eth[13] = 0x20;
    CHECK(whtt_tx(h, 0, BSSID, eth, 15, 1) == WHTT_ERR_INVAL);
    eth[12] = 0x08; eth[13] = 0x00;
    g_sg_rc = -28;
    unsigned fr = whtt_tx_free(h);
    CHECK(whtt_tx(h, 0, BSSID, eth, 15, 1) == WHTT_ERR_IO && whtt_tx_free(h) == fr && h->tx_err == 1);
    g_sg_rc = 0;
    // a maximum frame
    uint8_t big[1514];
    mketh(big, PEER, MYMAC, 0x0800, 1500);
    CHECK(whtt_tx(h, 0, BSSID, big, 1514, 1) == 0 && le16(g_sg[g_nsg - 1].a + 12) == 1532);
    CHECK(whtt_tx(h, 0, BSSID, big, 1515, 1) == WHTT_ERR_INVAL);
    t_tfree_all();
}

// target side of rx: fill the next posted buffer with a frame and build the in-order indication
static unsigned g_tgt_ring;     // the target's read position in the ring
typedef struct {uint8_t msg[1 + 7 + 12 * 8]; uint32_t n;} ind;
static void ind_begin(ind *i, unsigned peer)
{
    memset(i, 0, sizeof *i);
    i->msg[0] = WA_HTT_TLV_T2H_MSG_TYPE_RX_IN_ORD_PADDR_IND;
    w16(i->msg + 2, peer);
}
static uint8_t *ind_buf(whtt *h, ind *i, uint32_t mlen)
{
    const uint64_t pa = h->ring[g_tgt_ring];
    g_tgt_ring = (g_tgt_ring + 1) & h->size_mask;
    uint8_t *b = host(pa);
    memset(b, 0, 360);
    w32(b + WL_OFF_htt_rx_desc_v2__attention, WA_RX_ATTENTION_FLAGS_MSDU_DONE);
    w32(b + WL_OFF_htt_rx_desc_v2__msdu_start + WL_OFF_rx_msdu_start_common__info1, 1u << WA_RX_MSDU_START_INFO1_DECAP_FORMAT_LSB);
    w32(b + WL_OFF_htt_rx_desc_v2__msdu_end + WL_OFF_rx_msdu_end_common__info0, WA_RX_MSDU_END_INFO0_LAST_MSDU);
    uint8_t *d = i->msg + 8 + 12 * i->n++;
    w64(d, pa);
    w16(d + 8, mlen);
    w16(i->msg + 6, i->n);
    return b + WL_SIZEOF_htt_rx_desc_v2;
}
static uint32_t nwifi(uint8_t *p, unsigned fc1, int qos, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3, unsigned type, unsigned plen, int pad)
{
    memset(p, 0, 40);
    p[0] = qos ? 0x88 : 0x08;
    p[1] = (uint8_t)fc1;
    memcpy(p + 4, a1, 6); memcpy(p + 10, a2, 6); memcpy(p + 16, a3, 6);
    uint32_t hl = qos ? 26 : 24;
    if (pad) hl = (hl + 3) & ~3u;
    memcpy(p + hl, "\xaa\xaa\x03\x00\x00\x00", 6);
    p[hl + 6] = (uint8_t)(type >> 8); p[hl + 7] = (uint8_t)type;
    for (unsigned i = 0; i < plen; ++i) p[hl + 8 + i] = (uint8_t)(i * 5 + 1);
    return hl + 8 + plen;
}
static void send(whtt *h, ind *i) {whtt_rx(h, i->msg, 8 + 12 * i->n);}

static void t_htt_rx(void)
{
    whtt *h = new_h(128, 8, 1);
    ready(h);
    g_tgt_ring = 0;
    ind in;
    // from the AP: from-DS, addr1 DA (us), addr2 BSSID, addr3 SA
    ind_begin(&in, 5);
    uint32_t n = nwifi(ind_buf(h, &in, 0), 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 60, 1);
    w16(in.msg + 8 + 8, n);
    send(h, &in);
    CHECK(g_nrx == 1 && g_rxl[0] == 14 + 60 && !memcmp(g_rxf[0], MYMAC, 6) && !memcmp(g_rxf[0] + 6, PEER, 6));
    CHECK(g_rxf[0][12] == 0x08 && g_rxf[0][13] == 0x00 && g_rxf[0][14] == 1 && g_rxf[0][15] == 6);
    CHECK(h->fill == 64 && *h->alloc_idx == 64 && h->ring[63] != 0 && h->rx_frames == 1);
    CHECK(h->posted[0] == 1);
    CHECK(le32(host(h->ring[63]) + 4) == 0);                    // attention cleared on the reposted buffer
    // a QoS data frame with a padded header (26 -> 28) and an A-MSDU worth of three frames in one indication
    ind_begin(&in, 5);
    for (unsigned k = 0; k < 3; ++k) {
        uint8_t *b = ind_buf(h, &in, 0);
        uint32_t m = nwifi(b, 0x02, 1, MYMAC, BSSID, PEER, 0x86dd, 20 + k, 1);
        w16(in.msg + 8 + 12 * k + 8, m);
    }
    send(h, &in);
    CHECK(g_nrx == 4 && g_rxl[1] == 34 && g_rxl[2] == 35 && g_rxl[3] == 36 && g_rxf[1][12] == 0x86);
    // a header with no padding (feature present) is read the same when the flag is off
    whtt *h2 = new_h(128, 8, 0);
    ready(h2);
    g_tgt_ring = 0;
    ind_begin(&in, 5);
    n = nwifi(ind_buf(h2, &in, 0), 0x02, 1, MYMAC, BSSID, PEER, 0x0800, 10, 0);
    w16(in.msg + 8 + 8, n);
    send(h2, &in);
    CHECK(g_nrx == 1 && g_rxl[0] == 24 && g_rxf[0][14] == 1);
    // other addressing: to-DS (we are the AP: DA=addr3, SA=addr2), 4-address (DA=addr3, SA=addr4), IBSS
    whtt *h3 = new_h(128, 8, 1);
    ready(h3);
    g_tgt_ring = 0;
    ind_begin(&in, 5);
    n = nwifi(ind_buf(h3, &in, 0), 0x01, 0, BSSID, PEER, MYMAC, 0x0800, 4, 1);
    w16(in.msg + 8 + 8, n);
    send(h3, &in);
    CHECK(g_nrx == 1 && !memcmp(g_rxf[0], MYMAC, 6) && !memcmp(g_rxf[0] + 6, PEER, 6));
    ind_begin(&in, 5);
    n = nwifi(ind_buf(h3, &in, 0), 0x00, 0, MYMAC, PEER, BSSID, 0x0800, 4, 1);
    w16(in.msg + 8 + 8, n);
    send(h3, &in);
    CHECK(g_nrx == 2 && !memcmp(g_rxf[1], MYMAC, 6) && !memcmp(g_rxf[1] + 6, PEER, 6));
    t_tfree_all();
}

static void t_htt_rx_errors(void)
{
    ind in;
    uint32_t n;
    whtt *h = new_h(128, 8, 1);
    ready(h);
    g_tgt_ring = 0;
    // FCS error, decap not native-wifi, non-data frame, no SNAP: dropped but the buffers are reposted
    ind_begin(&in, 5);
    uint8_t *b = ind_buf(h, &in, 0);
    n = nwifi(b, 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 20, 1); w16(in.msg + 8 + 8, n);
    w32(b - WL_SIZEOF_htt_rx_desc_v2 + WL_OFF_htt_rx_desc_v2__attention, WA_RX_ATTENTION_FLAGS_MSDU_DONE | WA_RX_ATTENTION_FLAGS_FCS_ERR);
    b = ind_buf(h, &in, 0);
    n = nwifi(b, 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 20, 1); w16(in.msg + 8 + 12 + 8, n);
    w32(b - WL_SIZEOF_htt_rx_desc_v2 + WL_OFF_htt_rx_desc_v2__msdu_start + WL_OFF_rx_msdu_start_common__info1, 2u << 8);
    b = ind_buf(h, &in, 0);
    n = nwifi(b, 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 20, 1); w16(in.msg + 8 + 24 + 8, n);
    b[0] = 0x80;                                              // a beacon, not data
    b = ind_buf(h, &in, 0);
    n = nwifi(b, 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 20, 1); w16(in.msg + 8 + 36 + 8, n);
    b[24] = 0;                                                // LLC broken
    send(&*h, &in);
    CHECK(g_nrx == 0 && h->rx_drop_attn == 1 && h->rx_drop_decap == 1 && h->rx_drop_nondata == 1 && h->rx_drop_fmt == 1);
    CHECK(h->fill == 63 + 4 && !h->rx_confused);
    // a frame that is too short for its header
    ind_begin(&in, 5);
    ind_buf(h, &in, 10);
    send(h, &in);
    CHECK(g_nrx == 0 && !h->rx_confused);
    // truncated indications
    whtt_rx(h, in.msg, 5);
    in.msg[6] = 7;
    whtt_rx(h, in.msg, 8 + 12);
    CHECK(h->rx_bad == 2);
    // an indication for a buffer the target was never given confuses the receiver and stops it
    ind_begin(&in, 5);
    ind_buf(h, &in, 0);
    w64(in.msg + 8, IOVA0 + 12345);
    send(h, &in);
    CHECK(h->rx_confused);
    uint32_t before = g_nrx;
    ind_begin(&in, 5);
    n = nwifi(ind_buf(h, &in, 0), 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 10, 1); w16(in.msg + 8 + 8, n);
    send(h, &in);
    CHECK(g_nrx == before);
    // msdu not done (stale descriptor) is fatal too
    whtt *h2 = new_h(128, 8, 1);
    ready(h2);
    g_tgt_ring = 0;
    ind_begin(&in, 5);
    b = ind_buf(h2, &in, 0);
    w32(b - WL_SIZEOF_htt_rx_desc_v2 + WL_OFF_htt_rx_desc_v2__attention, 0);
    send(h2, &in);
    CHECK(h2->rx_confused && h2->rx_bad == 1);
    // a buffer delivered twice
    whtt *h3 = new_h(128, 8, 1);
    ready(h3);
    g_tgt_ring = 0;
    ind_begin(&in, 5);
    n = nwifi(ind_buf(h3, &in, 0), 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 10, 1); w16(in.msg + 8 + 8, n);
    send(h3, &in);
    CHECK(g_nrx == 1);
    uint8_t again[8 + 12];
    memcpy(again, in.msg, sizeof again);
    w64(again + 8, h3->pool_iova);                          // buffer 0 is back in the ring: delivering it twice in a row
    h3->posted[0] = 0;
    whtt_rx(h3, again, sizeof again);
    CHECK(h3->rx_confused);
    t_tfree_all();
}

static void t_htt_ring_wrap(void)
{
    // thousands of frames through the 128-entry ring: the posted count stays at the fill level, indices wrap, no buffer lost
    whtt *h = new_h(128, 8, 1);
    ready(h);
    g_tgt_ring = 0;
    ind in;
    unsigned total = 0;
    for (unsigned round = 0; round < 2000; ++round) {
        ind_begin(&in, 5);
        const unsigned cnt = 1 + round % 8;
        for (unsigned k = 0; k < cnt; ++k) {
            uint32_t m = nwifi(ind_buf(h, &in, 0), 0x02, 0, MYMAC, BSSID, PEER, 0x0800, 30, 1);
            w16(in.msg + 8 + 12 * k + 8, m);
        }
        send(h, &in);
        total += cnt;
        unsigned posted = 0;
        for (unsigned i = 0; i < h->pool_bufs; ++i) posted += h->posted[i];
        if (posted != 63) {CHECK(posted == 63); break;}
    }
    CHECK(h->rx_frames == total && !h->rx_confused && h->rx_bad == 0);
    CHECK(*h->alloc_idx == h->fill);
    t_tfree_all();
}

static void t_htt_misc(void)
{
    whtt *h = new_h(128, 8, 1);
    ready(h);
    uint8_t sec[4] = {WA_HTT_TLV_T2H_MSG_TYPE_SEC_IND, 0x23, 0x07, 0x00};
    whtt_rx(h, sec, sizeof sec);
    CHECK(g_nsec == 1 && g_sec_peer == 7 && g_sec_flags == 0x23 && h->sec_ind == 1);
    uint8_t pm[10] = {WA_HTT_TLV_T2H_MSG_TYPE_PEER_MAP, 0, 7, 0};
    whtt_rx(h, pm, sizeof pm);
    uint8_t unk[4] = {0x55, 0, 0, 0};
    whtt_rx(h, unk, sizeof unk);
    whtt_rx(h, unk, 0);
    CHECK(h->peer_map == 1 && h->other_msgs == 1);
    // a version confirm in the wrong state
    uint8_t conf[3] = {WA_HTT_TLV_T2H_MSG_TYPE_VERSION_CONF, 0, 3};
    unsigned before = g_nctl;
    whtt_rx(h, conf, sizeof conf);
    CHECK(g_nctl == before && h->rx_bad == 1);
    // the layout sizes the encoders rely on
    CHECK(WL_SIZEOF_ath10k_htt_txbuf_64 == 44 && WL_OFF_ath10k_htt_txbuf_64__htc_hdr == 16 && WL_OFF_ath10k_htt_txbuf_64__cmd_tx == 25);
    CHECK(WL_SIZEOF_htt_data_tx_desc_64 == 19 && WL_OFF_htt_data_tx_desc_64__frags_paddr == 7 && WL_OFF_htt_data_tx_desc_64__peerid == 15);
    CHECK(WL_SIZEOF_htt_msdu_ext_desc_64 == 72 && WL_SIZEOF_htt_rx_in_ord_ind == 7 && WL_SIZEOF_htt_rx_in_ord_msdu_desc_ext == 12);
    CHECK(WL_SIZEOF_htt_data_tx_completion == 3);
    CHECK(WL_OFF_htt_rx_desc_v2__attention == 4 && WL_OFF_htt_rx_desc_v2__msdu_start + WL_OFF_rx_msdu_start_common__info1 == 36);
    CHECK(WL_OFF_htt_rx_desc_v2__msdu_end + WL_OFF_rx_msdu_end_common__info0 == 64 && WL_OFF_htt_rx_desc_v2__msdu_payload == 360);
    t_tfree_all();
}

void t_htt(void)
{
    RUN(t_htt_init_setup);
    RUN(t_htt_version);
    RUN(t_htt_tx);
    RUN(t_htt_rx);
    RUN(t_htt_rx_errors);
    RUN(t_htt_ring_wrap);
    RUN(t_htt_misc);
}
