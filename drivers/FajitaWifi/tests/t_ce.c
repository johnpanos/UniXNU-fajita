// SPDX-License-Identifier: BSD-2-Clause-Patent
// Layer 5a: the copy engines and HTC against the target simulator (wsim + the HTC firmware model).
#include "tcheck.h"
#include "wsim.h"
#include "wfw.h"
#include "../WifiCe.h"
#include "../WifiHtc.h"
#include <stdlib.h>
#include <string.h>

static void *halloc(size_t n, void *c) {(void)c; return t_talloc(n);}
static void hfree(void *p, size_t n, void *c) {(void)c; (void)n; (void)p;}

static uint32_t rx_pipe[256], rx_len[256], rx_n;
static uint8_t rx_first[256];
static void count_rx(wce *ce, unsigned pipe, uint8_t *data, uint32_t len, void *arg)
{
    (void)ce; (void)arg;
    if (rx_n < 256) {rx_pipe[rx_n] = pipe; rx_len[rx_n] = len; rx_first[rx_n] = data[0];}
    ++rx_n;
}
static uintptr_t done_ctx[64];
static unsigned done_n;
static void count_tx(wce *ce, unsigned pipe, uintptr_t ctx, void *arg)
{
    (void)ce; (void)pipe; (void)arg;
    if (done_n < 64) done_ctx[done_n] = ctx;
    ++done_n;
}

static void t_ce_rings(void)
{
    wsim s;
    wsim_init(&s, 8u << 20);
    wdma d;
    wdma_init(&d, s.mem, s.iova, s.size);
    wce ce;
    const wce_io io = wsim_io(&s);
    CHECK(wce_init(&ce, &io, &d, NULL, 0, halloc, NULL) == WCE_OK);
    // RRI word programmed and every CE's ctrl1 has the update bit
    CHECK(wsim_reg(&s, WCE_RRI_LOW) == (uint32_t)ce.rri_iova && wsim_reg(&s, WCE_RRI_HIGH) == 0);
    for (unsigned i = 0; i < WSNOC_CE_COUNT; ++i) CHECK(wsim_reg(&s, WCE_CE0_BASE + 0x1000 * i + WCE_CTRL1) & WCE_CTRL1_RRI_UPD);
    // CE0: 16 x 2048 source ring, dmax 2048; CE4: 2048 entries, dmax 256; CE1 dest 512
    const uint32_t b0 = WCE_CE0_BASE, b4 = WCE_CE0_BASE + 0x4000, b1 = WCE_CE0_BASE + 0x1000;
    CHECK(wsim_reg(&s, b0 + WCE_SR_SIZE) == 16 && (wsim_reg(&s, b0 + WCE_CTRL1) & 0xffff) == 2048);
    CHECK(wsim_reg(&s, b0 + WCE_SR_BA_LO) == (uint32_t)ce.pipe[0].src.desc_iova);
    CHECK(wsim_reg(&s, b4 + WCE_SR_SIZE) == 2048 && (wsim_reg(&s, b4 + WCE_CTRL1) & 0xffff) == 256);
    CHECK(wsim_reg(&s, b1 + WCE_DR_SIZE) == 512 && wsim_reg(&s, b1 + WCE_SR_SIZE) == 0);
    CHECK((wsim_reg(&s, b0 + WCE_SRC_WM) & 0xffff) == 16 && (wsim_reg(&s, b1 + WCE_DST_WM) & 0xffff) == 512);
    // copy-complete interrupts: on for CE0..3 and 5, off for CE4 (DIS_INTR) and CE6..8; watermarks off
    CHECK(wsim_reg(&s, b0 + WCE_HOST_IE) == WCE_HOST_IE_CC && wsim_reg(&s, b4 + WCE_HOST_IE) == 0);
    CHECK(wsim_reg(&s, WCE_CE0_BASE + 0x7000 + WCE_HOST_IE) == 0);
    CHECK(ce.dma->used < ce.dma->size);
    // descriptors 16-byte aligned, inside the arena
    for (unsigned i = 0; i < WSNOC_CE_COUNT; ++i) {
        if (ce.pipe[i].src.nentries) CHECK((ce.pipe[i].src.desc_iova & 15) == 0 && ce.pipe[i].src.desc_iova >= s.iova);
        if (ce.pipe[i].dst.nentries) CHECK((ce.pipe[i].dst.desc_iova & 15) == 0);
    }
    // start: every destination ring posted to nentries - 1
    CHECK(wce_start(&ce) == WCE_OK);
    CHECK(wsim_reg(&s, b1 + WCE_DST_WR_INDEX) == 511);
    CHECK(wsim_reg(&s, WCE_CE0_BASE + 0x2000 + WCE_DST_WR_INDEX) == 63);
    CHECK(wsim_reg(&s, WCE_CE0_BASE + 0x7000 + WCE_DST_WR_INDEX) == 1);   // CE7: 2 entries
    CHECK(wsim_reg(&s, WCE_CE0_BASE + 0x6000 + WCE_DST_WR_INDEX) == 0);   // CE6: no ring
    // a posted descriptor points into the CE's rx buffer block
    {
        const uint8_t *dd = wsim_dma(&s, ce.pipe[2].dst.desc_iova + 16 * 3, 16);
        uint32_t lo; memcpy(&lo, dd, 4);
        CHECK(lo == (uint32_t)(ce.pipe[2].rxbuf_iova + 3 * 2048));
    }
    ce.rx = count_rx;
    ce.tx_done = count_tx;
    // rx: 200 messages through CE2 (64 entries) with polls in between: the ring wraps, buffers are reposted
    rx_n = 0;
    unsigned sent = 0;
    for (unsigned k = 0; k < 200; ++k) {
        uint8_t m[100];
        memset(m, (int)k, sizeof m);
        if (wsim_send(&s, 2, m, 20 + k % 80) == 0) ++sent;
        if (k % 16 == 15) wce_poll(&ce);
    }
    wce_poll(&ce);
    CHECK(sent == 200 && rx_n == 200);
    int order = 1;
    for (unsigned k = 0; k < 200 && k < 256; ++k) order &= rx_pipe[k] == 2 && rx_len[k] == 20 + k % 80 && rx_first[k] == (uint8_t)k;
    CHECK(order);
    CHECK(wsim_reg(&s, WCE_CE0_BASE + 0x2000 + WCE_HOST_IS) == 0);   // status cleared by the poll
    // no buffer: 64 sends without a poll fill the 63 posted buffers, the 64th finds none
    unsigned ok = 0;
    for (unsigned k = 0; k < 64; ++k) ok += wsim_send(&s, 2, "x", 1) == 0;
    CHECK(ok == 63 && s.no_buffer == 1);
    rx_n = 0;
    wce_poll(&ce);
    CHECK(rx_n == 63);
    // tx: CE0 copy sends, shadow write index, completion from the source read index register
    done_n = 0;
    s.fw = NULL;
    CHECK(wce_send_copy(&ce, 0, "hdr", 3, "payload", 7, 5, 0x1234) == WCE_OK);
    CHECK(s.shadow_writes[0] == 1 && s.src_msgs[0] == 1);
    {
        const uint8_t *dd = wsim_dma(&s, ce.pipe[0].src.desc_iova, 16);
        uint16_t nb, fl; memcpy(&nb, dd + 8, 2); memcpy(&fl, dd + 10, 2);
        CHECK(nb == 10 && fl == (5u << 4));
    }
    wce_poll(&ce);
    CHECK(done_n == 1 && done_ctx[0] == 0x1234);
    // a full source ring: the target stalls CE0, 15 sends fit (16 entries), the 16th is refused
    s.stall_src[0] = 1;
    unsigned fit = 0;
    for (unsigned k = 0; k < 16; ++k) fit += wce_send_copy(&ce, 0, NULL, 0, "abcd", 4, 1, 100 + k) == WCE_OK;
    CHECK(fit == 15 && ce.pipe[0].full == 1);
    wsim_drain(&s, 0);
    done_n = 0;
    wce_poll(&ce);
    CHECK(done_n == 15 && done_ctx[14] == 114);
    CHECK(wce_send_copy(&ce, 0, NULL, 0, "abcd", 4, 1, 0) == WCE_OK);
    wce_poll(&ce);
    // gather on CE4 (DIS_INTR): two fragments, one write-index publication, completion through the DDR RRI word
    uint64_t ia, ib;
    uint8_t *a = wdma_alloc(&d, 32, 8, &ia), *bb = wdma_alloc(&d, 64, 8, &ib);
    CHECK(a && bb);
    memcpy(a, "FRAG-A--", 8);
    memcpy(bb, "frag-b", 6);
    const uint32_t sw_before = s.shadow_writes[4];
    static uint8_t got[64];
    static uint32_t got_len;
    s.fw = NULL;
    CHECK(wce_send(&ce, 4, ia, 8, 0, 1, 0) == WCE_OK);
    CHECK(s.shadow_writes[4] == sw_before && s.src_msgs[4] == 0);   // not published yet
    CHECK(wce_send(&ce, 4, ib, 6, 0, 0, 77) == WCE_OK);
    CHECK(s.shadow_writes[4] == sw_before + 1 && s.src_msgs[4] == 1);
    memcpy(got, s.gather[4], 0);
    got_len = 0;
    (void)got; (void)got_len;
    {
        const uint8_t *dd = wsim_dma(&s, ce.pipe[4].src.desc_iova, 16);
        uint32_t hi; uint16_t fl; memcpy(&hi, dd + 4, 4); memcpy(&fl, dd + 10, 2);
        CHECK((hi & WCE_DESC_GATHER_HI) && (hi & WCE_SEND_FLAG_GATHER) && (fl & WCE_DESC_FLAG_GATHER));
    }
    CHECK((ce.rri[4] & 0xffff) == 2);
    done_n = 0;
    wce_poll(&ce);
    CHECK(done_n == 2 && done_ctx[1] == 77);
    // revert of an unpublished gather fragment
    CHECK(wce_send(&ce, 4, ia, 8, 0, 1, 0) == WCE_OK);
    wce_send_revert(&ce, 4);
    CHECK(ce.pipe[4].src.write_index == 2);
    // oversize sends are refused
    CHECK(wce_send(&ce, 4, ia, 300, 0, 0, 0) == WCE_ERR_INVAL);
    // the interrupt summary reflects copy-complete status
    wsim_send(&s, 9, "z", 1);
    CHECK(wce_intr_summary(&ce) == (1u << 9));
    wce_poll(&ce);
    CHECK(wce_intr_summary(&ce) == 0);
    // a capped destination ring
    wsim s2;
    wsim_init(&s2, 4u << 20);
    wdma d2;
    wdma_init(&d2, s2.mem, s2.iova, s2.size);
    wce ce2;
    const wce_io io2 = wsim_io(&s2);
    CHECK(wce_init(&ce2, &io2, &d2, NULL, 64, halloc, NULL) == WCE_OK);
    CHECK(wsim_reg(&s2, b1 + WCE_DR_SIZE) == 64 && d2.used < (2u << 20));
    // an arena too small fails cleanly
    wsim s3;
    wsim_init(&s3, 64u << 10);
    wdma d3;
    wdma_init(&d3, s3.mem, s3.iova, s3.size);
    wce ce3;
    const wce_io io3 = wsim_io(&s3);
    CHECK(wce_init(&ce3, &io3, &d3, NULL, 0, halloc, NULL) == WCE_ERR_NOMEM);
    wsim_free(&s); wsim_free(&s2); wsim_free(&s3);
    t_tfree_all();
}

static unsigned wmi_rx_n;
static uint8_t wmi_rx_last[64];
static void wmi_rx(whtc *h, unsigned eid, uint8_t *data, uint32_t len, void *arg)
{
    (void)h; (void)arg;
    if (eid == WFW_EID_WMI) {++wmi_rx_n; memcpy(wmi_rx_last, data, len < 64 ? len : 64);}
}
static unsigned htt_direct_n, htt_direct_len;
static uint8_t htt_direct_first;
static void htt_direct(whtc *h, uint8_t *data, uint32_t len, void *arg)
{
    (void)h; (void)arg;
    ++htt_direct_n; htt_direct_len = len; htt_direct_first = data[0];
}
static unsigned fw_wmi_n;
static uint8_t fw_wmi_first[16];
static void fw_wmi(wfw *f, const uint8_t *cmd, uint32_t len)
{
    (void)f; (void)len;
    if (fw_wmi_n < 16) fw_wmi_first[fw_wmi_n] = cmd[0];
    ++fw_wmi_n;
}

static void t_htc(void)
{
    wsim s;
    wsim_init(&s, 8u << 20);
    wfw f;
    wfw_init(&f, &s);
    wdma d;
    wdma_init(&d, s.mem, s.iova, s.size);
    wce ce;
    const wce_io io = wsim_io(&s);
    CHECK(wce_init(&ce, &io, &d, NULL, 0, halloc, NULL) == WCE_OK);
    whtc h;
    CHECK(whtc_init(&h, &ce, halloc, hfree, NULL) == WHTC_OK);
    CHECK(h.ep[0].ul == 0 && h.ep[0].dl == 2);
    h.htt_direct = htt_direct;
    CHECK(wce_start(&ce) == WCE_OK);
    CHECK(whtc_connect(&h, WHTC_SVC_WMI_CONTROL, wmi_rx, NULL) == WHTC_ERR_AGAIN);   // not ready yet
    CHECK(wfw_ready(&f) == 0);
    wce_poll(&ce);
    CHECK(h.state == WHTC_READY && h.credit_size == 1792 && h.fw_credit_count == 2 && h.total_credits == 1);
    // connect WMI control: credit allocation 1 in the flags, flow control left on
    CHECK(whtc_connect(&h, WHTC_SVC_WMI_CONTROL, wmi_rx, NULL) == WHTC_OK);
    CHECK(whtc_connect_result(&h, WHTC_SVC_WMI_CONTROL, NULL) == 0);
    CHECK(whtc_connect(&h, WHTC_SVC_HTT_DATA_MSG, NULL, NULL) == WHTC_ERR_AGAIN);   // one at a time
    wce_poll(&ce);
    unsigned eid = 99;
    CHECK(whtc_connect_result(&h, WHTC_SVC_WMI_CONTROL, &eid) == 1 && eid == WFW_EID_WMI);
    CHECK(f.conn_service[0] == WHTC_SVC_WMI_CONTROL && f.conn_flags[0] == (1u << 8));
    CHECK(h.ep[eid].ul == 3 && h.ep[eid].dl == 2 && h.ep[eid].flow && h.ep[eid].credits == 1 && h.ep[eid].max_msg == 1792);
    // connect HTT: credit flow disabled
    CHECK(whtc_connect(&h, WHTC_SVC_HTT_DATA_MSG, NULL, NULL) == WHTC_OK);
    wce_poll(&ce);
    CHECK(whtc_connect_result(&h, WHTC_SVC_HTT_DATA_MSG, &eid) == 1 && eid == WFW_EID_HTT);
    CHECK(f.conn_flags[1] == WHTC_CONN_DISABLE_CREDIT_FLOW && h.ep[eid].ul == 4 && h.ep[eid].dl == 1 && !h.ep[eid].flow);
    // a refused connect
    CHECK(whtc_connect(&h, WHTC_SVC_WMI_DATA_BE, NULL, NULL) == WHTC_OK);
    wce_poll(&ce);
    CHECK(whtc_connect_result(&h, WHTC_SVC_WMI_DATA_BE, &eid) == 1);   // the model accepts with a fresh eid
    CHECK(whtc_connect(&h, WHTC_SVC_HTT_LOG_MSG, NULL, NULL) == WHTC_ERR_INVAL);   // no ul pipe on snoc
    CHECK(whtc_start(&h) == WHTC_OK);
    CHECK(f.setup_complete == 1);
    // WMI flow control: one credit, three commands; the 2nd and 3rd wait for credit returns, in order
    f.wmi = fw_wmi;
    f.auto_credit = 0;
    const uint8_t c1[] = {0x11, 0, 0, 0}, c2[] = {0x22, 0, 0, 0}, c3[] = {0x33, 0, 0, 0};
    CHECK(whtc_send(&h, WFW_EID_WMI, c1, 4) == WHTC_OK);
    CHECK(whtc_send(&h, WFW_EID_WMI, c2, 4) == WHTC_OK);
    CHECK(whtc_send(&h, WFW_EID_WMI, c3, 4) == WHTC_OK);
    CHECK(fw_wmi_n == 1 && h.qcount == 2 && h.ep[WFW_EID_WMI].credits == 0);
    wfw_credit(&f, 1);
    wce_poll(&ce);
    CHECK(fw_wmi_n == 2 && h.qcount == 1);
    // credit in the trailer of a WMI event, delivered with the event body
    const uint8_t ev[] = {0xaa, 0xbb, 0xcc, 0xdd};
    wfw_send(&f, WFW_WMI_PIPE, WFW_EID_WMI, ev, 4, 1);
    wce_poll(&ce);
    CHECK(fw_wmi_n == 3 && h.qcount == 0 && wmi_rx_n == 1 && wmi_rx_last[0] == 0xaa);
    CHECK(fw_wmi_first[0] == 0x11 && fw_wmi_first[1] == 0x22 && fw_wmi_first[2] == 0x33);
    CHECK(f.seq_errors == 0 && f.flag_errors == 0);
    // HTT over HTC (eid 2 on CE4), no credit flag
    const uint8_t verreq[4] = {0};
    CHECK(whtc_send(&h, WFW_EID_HTT, verreq, 4) == WHTC_OK);
    CHECK(f.htt_msgs == 1 && f.flag_errors == 0);
    // CE5: HTT straight to the HTT handler, header stripped
    uint8_t m[16] = {0};
    whtc_hdr(m, WFW_EID_HTT, 8, 0, 0);
    m[8] = 0x7;
    wsim_send(&s, 5, m, 16);
    wce_poll(&ce);
    CHECK(htt_direct_n == 1 && htt_direct_len == 8 && htt_direct_first == 0x7);
    // malformed: unknown eid, length beyond the transfer, bad trailer
    const uint32_t bad0 = h.rx_bad;
    whtc_hdr(m, 7, 4, 0, 0);
    wsim_send(&s, 2, m, 12);
    whtc_hdr(m, WFW_EID_WMI, 100, 0, 0);
    wsim_send(&s, 2, m, 12);
    whtc_hdr(m, WFW_EID_WMI, 4, WHTC_FLAG_TRAILER_PRESENT, 0);
    m[4] = 9;
    wsim_send(&s, 2, m, 12);
    wce_poll(&ce);
    CHECK(h.rx_bad == bad0 + 3 && wmi_rx_n == 1);
    // oversize payload refused
    static uint8_t big[2100];
    CHECK(whtc_send(&h, WFW_EID_WMI, big, sizeof big) == WHTC_ERR_INVAL);
    wsim_free(&s);
    t_tfree_all();
}

void t_ce(void)
{
    RUN(t_ce_rings);
    RUN(t_htc);
}
