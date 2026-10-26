// SPDX-License-Identifier: BSD-2-Clause-Patent
// The wlfw client against a fixture wlfw server on the simulated modem's QRTR node: every request is decoded and checked
// field by field against qmi_wlfw_v01.c, and answered with hand-encoded responses and indications. The board data is the
// phone's own image/bdwlan.102 (through the FAT reader) when the modem partition is given, checked against board-2.bin's
// qmi-board-id=102 entry when that is given too.
#include "tcheck.h"
#include "../WifiRouter.h"
#include "../WifiWlfw.h"
#include "../WifiSnoc.h"
#include "../WifiQmi.h"
#include "../WifiFat.h"
#include "../WifiBoard.h"
#include <stdlib.h>
#include <string.h>

static void *w_alloc(size_t n, void *c) {(void)c; return malloc(n);}
static void w_free(void *p, size_t n, void *c) {(void)c; (void)n; free(p);}
typedef struct {uint8_t *p[256]; size_t len[256]; unsigned n;} q_t;
static int q_xmit(void *ctx, const uint8_t *pkt, size_t len)
{
    q_t *q = ctx;
    if (q->n == 256) return -11;
    q->p[q->n] = malloc(len); memcpy(q->p[q->n], pkt, len); q->len[q->n++] = len;
    return 0;
}
static int q_deliver(q_t *q, wqrtr *r)
{
    int n = 0;
    while (q->n) {
        uint8_t *p = q->p[0]; size_t l = q->len[0];
        memmove(q->p, q->p + 1, (q->n - 1) * sizeof q->p[0]); memmove(q->len, q->len + 1, (q->n - 1) * sizeof q->len[0]);
        --q->n;
        wqrtr_input(r, p, l); free(p); ++n;
    }
    return n;
}

// ---- the fixture server ----
typedef struct {
    wqrtr *r;
    uint32_t port;
    int early_msa_ind, fw_ready_in_status, bad_region, drop_cap, bdf_end_fail;
    // what it saw
    int ind_ok, host_cap_ok, msa_info_ok, cal_ok, cfg_ok, mode_ok;
    unsigned bdf_segs, requests[64];
    uint8_t bdf[65536];
    uint32_t bdf_len, bdf_total;
    uint32_t mode;
} srv_t;

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t rd64(const uint8_t *p) {return rd32(p) | (uint64_t)rd32(p + 4) << 32;}

static void reply(srv_t *s, uint32_t node, uint32_t port, uint8_t *b, long n) {CHECK(n > 0); wqrtr_send(s->r, s->port, node, port, b, (size_t)n);}
static void ind(srv_t *s, uint32_t node, uint32_t port, uint16_t id)
{
    uint8_t b[16]; wqmi_writer w; wqmi_begin(&w, b, sizeof b, WQMI_INDICATION, 0, id); reply(s, node, port, b, wqmi_end(&w));
}

static void srv_rx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *m, size_t len, void *ctx)
{
    (void)r; (void)port;
    srv_t *s = ctx;
    wqmi_hdr h;
    CHECK(wqmi_parse_hdr(m, len, &h) == 0 && h.type == WQMI_REQUEST);
    if (h.msg_id < 64) ++s->requests[h.msg_id];
    uint8_t b[512];
    wqmi_writer w;
    wqmi_begin(&w, b, sizeof b, WQMI_RESPONSE, h.txn, h.msg_id);
    uint16_t vl;
    const uint8_t *v;
    switch (h.msg_id) {
    case WLFW_IND_REGISTER:
        s->ind_ok = (v = wqmi_find(m, len, 0x10, &vl)) && vl == 1 && v[0] == 1
                 && (v = wqmi_find(m, len, 0x13, &vl)) && vl == 1 && v[0] == 1
                 && (v = wqmi_find(m, len, 0x15, &vl)) && vl == 4 && rd32(v) == WLFW_CLIENT_ID
                 && !wqmi_find(m, len, 0x11, &vl) && !wqmi_find(m, len, 0x1a, &vl);
        wqmi_result(&w, 0, 0);
        if (s->fw_ready_in_status) wqmi_u64(&w, 0x10, WLFW_FW_READY_BIT);
        break;
    case WLFW_HOST_CAP:
        s->host_cap_ok = (v = wqmi_find(m, len, 0x10, &vl)) && vl == 1 && v[0] == 0;
        wqmi_result(&w, 0, 0);
        break;
    case WLFW_MSA_INFO: {
        s->msa_info_ok = (v = wqmi_find(m, len, 0x01, &vl)) && vl == 8 && rd64(v) == 0x8df00000ull
                      && (v = wqmi_find(m, len, 0x02, &vl)) && vl == 4 && rd32(v) == 0x100000;
        wqmi_result(&w, 0, 0);
        uint8_t reg[1 + 2 * 13] = {2};
        const uint64_t a0 = s->bad_region ? 0x8e000000ull : 0x8df00000ull, a1 = 0x8df80000ull;
        memcpy(reg + 1, &a0, 8); const uint32_t sz = 0x80000; memcpy(reg + 9, &sz, 4); reg[13] = 1;
        memcpy(reg + 14, &a1, 8); memcpy(reg + 22, &sz, 4); reg[26] = 0;
        wqmi_tlv(&w, 0x03, reg, sizeof reg);
        break;
    }
    case WLFW_MSA_READY:
        wqmi_result(&w, 0, 0);
        reply(s, node, sport, b, wqmi_end(&w));
        if (s->early_msa_ind) ind(s, node, sport, WLFW_MSA_READY_IND);   // before the cap exchange
        return;
    case WLFW_CAP: {
        if (s->drop_cap) return;
        wqmi_result(&w, 0, 0);
        const uint32_t chip[2] = {0x30214, 0x4001}, board = 0x102, soc = 0x400e0000;
        wqmi_tlv(&w, 0x10, chip, 8);
        wqmi_u32(&w, 0x11, board);
        wqmi_u32(&w, 0x12, soc);
        uint8_t fwv[4 + 1 + 16] = {0x34, 0x12, 0, 0, 16};
        memcpy(fwv + 5, "2021-11-25 23:51", 16);
        wqmi_tlv(&w, 0x13, fwv, sizeof fwv);
        wqmi_tlv(&w, 0x14, "WLAN.HL.2.0-00000-QCAHLSWMTPLZ-1", 32);
        reply(s, node, sport, b, wqmi_end(&w));
        if (!s->early_msa_ind) ind(s, node, sport, WLFW_MSA_READY_IND);   // after the cap response, as on SDM845
        return;
    }
    case WLFW_BDF_DOWNLOAD: {
        const uint8_t *valid = wqmi_find(m, len, 0x01, &vl);
        const uint8_t *fid = wqmi_find(m, len, 0x10, &vl);
        const uint8_t *tot = wqmi_find(m, len, 0x11, &vl);
        const uint8_t *seg = wqmi_find(m, len, 0x12, &vl);
        uint16_t dl = 0;
        const uint8_t *data = wqmi_find(m, len, 0x13, &dl);
        const uint8_t *end = wqmi_find(m, len, 0x14, &vl);
        CHECK(valid && valid[0] == 1 && fid && rd32(fid) == 0 && tot && seg && rd32(seg) == s->bdf_segs && data && end);
        if (!data || !tot || !end) return;
        const uint16_t n = (uint16_t)(data[0] | data[1] << 8);
        CHECK(n + 2 == dl && n <= WLFW_MAX_DATA && s->bdf_len + n <= sizeof s->bdf);
        memcpy(s->bdf + s->bdf_len, data + 2, n);
        s->bdf_len += n;
        s->bdf_total = rd32(tot);
        ++s->bdf_segs;
        CHECK(end[0] == (s->bdf_len == s->bdf_total));
        if (end[0] && s->bdf_end_fail) wqmi_result(&w, 1, 1);   // the CRC check fails on the last segment
        else wqmi_result(&w, 0, 0);
        break;
    }
    case WLFW_CAL_REPORT:
        s->cal_ok = (v = wqmi_find(m, len, 0x01, &vl)) && vl == 1 && v[0] == 0 && !wqmi_find(m, len, 0x10, &vl);
        wqmi_result(&w, 0, 0);
        reply(s, node, sport, b, wqmi_end(&w));
        ind(s, node, sport, WLFW_FW_READY_IND);
        return;
    case WLFW_WLAN_CFG: {
        // tgt_cfg: 12 x {pipe_num, pipe_dir, nentries, nbytes_max, flags}; svc_cfg: 21 x {service, dir, pipe}; shadow: 12 x {id, offset}
        const uint8_t *t = wqmi_find(m, len, 0x11, &vl);
        int ok = t && t[0] == 12 && vl == 1 + 12 * 20 && !wqmi_find(m, len, 0x10, &vl);
        for (unsigned i = 0; ok && i < 12; ++i) {
            const uint8_t *e = t + 1 + i * 20;
            ok = rd32(e) == wsnoc_target_ce_config[i].pipe_num && rd32(e + 4) == wsnoc_target_ce_config[i].pipe_dir
              && rd32(e + 8) == wsnoc_target_ce_config[i].nentries && rd32(e + 12) == wsnoc_target_ce_config[i].nbytes_max
              && rd32(e + 16) == wsnoc_target_ce_config[i].flags;
        }
        CHECK(ok && rd32(t + 1 + 4 * 20 + 16) == 8 && rd32(t + 1 + 4 * 20 + 8) == 256);   // CE4: DIS_INTR, 256 entries
        const uint8_t *sv = wqmi_find(m, len, 0x12, &vl);
        ok = ok && sv && sv[0] == 21 && vl == 1 + 21 * 12 && rd32(sv + 1) == 0x104 && rd32(sv + 1 + 4) == 2 && rd32(sv + 1 + 8) == 3;
        const uint8_t *sh = wqmi_find(m, len, 0x13, &vl);
        ok = ok && sh && sh[0] == 12 && vl == 1 + 12 * 4 && sh[1] == 0 && sh[3] == 0x3c && sh[1 + 5 * 4] == 1 && sh[3 + 5 * 4] == 0x40;
        s->cfg_ok = ok;
        wqmi_result(&w, 0, 0);
        break;
    }
    case WLFW_WLAN_MODE:
        s->mode_ok = (v = wqmi_find(m, len, 0x01, &vl)) && vl == 4 && (s->mode = rd32(v), 1)
                  && (v = wqmi_find(m, len, 0x10, &vl)) && vl == 1 && v[0] == 0;
        wqmi_result(&w, 0, 0);
        break;
    default:
        wqmi_result(&w, 1, WQMI_ERR_NOT_SUPPORTED);
        break;
    }
    reply(s, node, sport, b, wqmi_end(&w));
}

// ---- the board data: image/bdwlan.<id> on the phone (cnss naming: bdwlan.bin, .bNN below 0xff, .NNN above) ----
static uint8_t g_board[65536];
static uint32_t g_board_len;
static uint32_t g_board_asked;
static int board_cb(void *ctx, uint32_t board_id, uint32_t chip_id, const uint8_t **data, uint32_t *len)
{
    (void)ctx; (void)chip_id;
    g_board_asked = board_id;
    if (!g_board_len) return -1;
    *data = g_board; *len = g_board_len;
    return 0;
}
static int assigned_count;
static wlfw_mem_region assigned[2];
static int assign_cb(void *ctx, const wlfw_mem_region *r, unsigned n)
{
    (void)ctx;
    assigned_count = (int)n;
    memcpy(assigned, r, n * sizeof *r);
    return 0;
}
static int fail_assign(void *ctx, const wlfw_mem_region *r, unsigned n) {(void)ctx; (void)r; (void)n; return -1;}

static int fat_read_img(void *ctx, uint64_t block, uint32_t count, void *dst)
{
    FILE *f = ctx;
    if (fseek(f, (long)(block * 4096), SEEK_SET)) return -1;
    return fread(dst, 4096, count, f) == count ? 0 : -1;
}

typedef struct {wqrtr ap, mss; q_t to_mss, to_ap; srv_t s; wwlfw c;} rig_t;
static void pump(rig_t *g, uint64_t *now)
{
    for (int i = 0; i < 2000; ++i) {
        const int n = q_deliver(&g->to_mss, &g->mss) + q_deliver(&g->to_ap, &g->ap);
        *now += 5;
        wwlfw_poll(&g->c, *now);
        if (!n && !g->to_mss.n && !g->to_ap.n && g->c.state != WWLFW_MSA_DELAY) return;
    }
}
static void rig_up(rig_t *g, const wwlfw_ops *ops)
{
    memset(g, 0, sizeof *g);
    wqrtr_init(&g->ap, q_xmit, &g->to_mss, w_alloc, w_free, 0);
    wqrtr_init(&g->mss, q_xmit, &g->to_ap, w_alloc, w_free, 0);
    g->mss.local_nid = 0; g->mss.hello_once = 1;
    g->s.r = &g->mss;
    g->s.port = wqrtr_bind(&g->mss, 0, srv_rx, &g->s);
    wqrtr_publish(&g->mss, g->s.port, WWLFW_SERVICE, 1, 0);
    CHECK(wwlfw_init(&g->c, &g->ap, ops) == 0);
    wqrtr_hello(&g->mss);
    uint64_t now = 0;
    pump(g, &now);
}
static void rig_down(rig_t *g)
{
    wqrtr_reset(&g->ap); wqrtr_reset(&g->mss);
    q_deliver(&g->to_mss, &g->mss); q_deliver(&g->to_ap, &g->ap);
    wqrtr_reset(&g->ap); wqrtr_reset(&g->mss);
}
static wwlfw_ops default_ops(void)
{
    wwlfw_ops o = {assign_cb, board_cb, 0, 0x8df00000ull, 0x100000, 1};
    return o;
}

static void t_wlfw_full(void)
{
    // board data
    g_board_len = 0;
    if (t_modem_img) {
        FILE *f = fopen(t_modem_img, "rb");
        fseek(f, 0, SEEK_END); const long size = ftell(f);
        static wfat fs;
        CHECK(wfat_mount(&fs, fat_read_img, f, (uint64_t)size / 4096) == 0);
        wfat_file wf;
        CHECK(wfat_open(&fs, "image/bdwlan.102", &wf) == 0);
        const long n = wfat_read(&fs, &wf, 0, g_board, sizeof g_board);
        CHECK(n == 19152);
        g_board_len = n > 0 ? (uint32_t)n : 0;
        fclose(f);
        if (t_board2) {
            size_t blen = 0;
            uint8_t *b2 = t_slurp(t_board2, &blen);
            char names[3][64];
            const unsigned nn = wboard_candidate_names(names, 3, 0x102, 0xff);
            const uint8_t *bd = NULL; uint32_t bl = 0;
            CHECK(b2 && wboard_select(b2, blen, names, nn, &bd, &bl) >= 0);
            CHECK(bd && bl == g_board_len && !memcmp(bd, g_board, bl));
            if (bd && bl == g_board_len && !memcmp(bd, g_board, bl))
                printf("  wlfw: the phone's bdwlan.102 is identical to board-2.bin's qmi-board-id=102 entry\n");
            free(b2);
        }
    }
    if (!g_board_len) {g_board_len = 19152; for (unsigned i = 0; i < g_board_len; ++i) g_board[i] = (uint8_t)(i * 7);}
    // the happy path, MSA_READY_IND after the cap response (SDM845) and before it
    for (int early = 0; early < 2; ++early) {
        static rig_t g;
        wwlfw_ops ops = default_ops();
        rig_up(&g, &ops);
        g.s.early_msa_ind = early;
        g.s.bdf_end_fail = 1;
        assigned_count = 0;
        const wqr_server *srv = wqrtr_lookup(&g.ap, WWLFW_SERVICE, WWLFW_INSTANCE);
        CHECK(srv != NULL);
        if (!srv) {rig_down(&g); continue;}
        uint64_t now = 1000;
        CHECK(wwlfw_server(&g.c, srv->node, srv->port, now) == 0 && g.c.state == WWLFW_IND_REGISTER);
        pump(&g, &now);
        CHECK(g.c.state == WWLFW_FW_READY && g.c.fw_ready);
        CHECK(g.s.ind_ok && g.s.host_cap_ok && g.s.msa_info_ok && g.s.cal_ok);
        CHECK(assigned_count == 2 && assigned[0].addr == 0x8df00000ull && assigned[0].secure == 1 && assigned[1].size == 0x80000);
        CHECK(g.c.cap.chip_id == 0x30214 && g.c.cap.board_id == 0x102 && g_board_asked == 0x102 && g.c.cap.fw_version == 0x1234);
        CHECK(!strcmp(g.c.cap.fw_timestamp, "2021-11-25 23:51") && !strncmp(g.c.cap.build_id, "WLAN.HL.2.0", 11));
        CHECK(g.s.bdf_segs == (g_board_len + WLFW_MAX_DATA - 1) / WLFW_MAX_DATA && g.s.bdf_len == g_board_len
              && g.s.bdf_total == g_board_len && !memcmp(g.s.bdf, g_board, g_board_len));
        CHECK(g.s.requests[WLFW_MSA_READY] == 1 && g.s.requests[WLFW_CAP] == 1 && g.s.requests[WLFW_CAL_REPORT] == 1);
        // enable: WLAN_CFG with the snoc tables, then WLAN_MODE mission
        CHECK(wwlfw_enable(&g.c, WLFW_MODE_MISSION) == 0);
        pump(&g, &now);
        CHECK(g.c.state == WWLFW_ENABLED && g.s.cfg_ok && g.s.mode_ok && g.s.mode == WLFW_MODE_MISSION);
        CHECK(wwlfw_enable(&g.c, 0) == WQR_ERR_BUSY);
        rig_down(&g);
    }
}

static void t_wlfw_paths(void)
{
    static rig_t g;
    uint64_t now;
    const wqr_server *srv;
    wwlfw_ops ops = default_ops();
    // firmware already up: ind_register's fw_status says so, nothing else is sent
    rig_up(&g, &ops);
    g.s.fw_ready_in_status = 1;
    srv = wqrtr_lookup(&g.ap, WWLFW_SERVICE, 0);
    now = 0;
    wwlfw_server(&g.c, srv->node, srv->port, now);
    pump(&g, &now);
    CHECK(g.c.state == WWLFW_FW_READY && g.s.requests[WLFW_HOST_CAP] == 0);
    rig_down(&g);
    // an MSA region outside the carve-out fails in msa_info, before any permission change
    rig_up(&g, &ops);
    g.s.bad_region = 1;
    assigned_count = -1;
    srv = wqrtr_lookup(&g.ap, WWLFW_SERVICE, 0);
    now = 0;
    wwlfw_server(&g.c, srv->node, srv->port, now);
    pump(&g, &now);
    CHECK(g.c.state == WWLFW_FAILED && g.c.failed_in == WWLFW_MSA_INFO && assigned_count == -1);
    rig_down(&g);
    // the permission hand-over fails: stop before msa_ready
    ops.msa_assign = fail_assign;
    rig_up(&g, &ops);
    srv = wqrtr_lookup(&g.ap, WWLFW_SERVICE, 0);
    now = 0;
    wwlfw_server(&g.c, srv->node, srv->port, now);
    pump(&g, &now);
    CHECK(g.c.state == WWLFW_FAILED && g.c.failed_in == WWLFW_MSA_DELAY && g.s.requests[WLFW_MSA_READY] == 0);
    rig_down(&g);
    // no answer to cap: the 30 s timeout
    ops = default_ops();
    rig_up(&g, &ops);
    g.s.drop_cap = 1;
    srv = wqrtr_lookup(&g.ap, WWLFW_SERVICE, 0);
    now = 0;
    wwlfw_server(&g.c, srv->node, srv->port, now);
    pump(&g, &now);
    CHECK(g.c.state == WWLFW_CAP);
    wwlfw_poll(&g.c, now + WWLFW_TIMEOUT_MS - 100);
    CHECK(g.c.state == WWLFW_CAP);
    wwlfw_poll(&g.c, now + WWLFW_TIMEOUT_MS + 100);
    CHECK(g.c.state == WWLFW_FAILED && g.c.failed_in == WWLFW_CAP);
    // the server restarts: a fresh handshake is allowed after a failure, and a gone server resets
    wwlfw_server_gone(&g.c);
    CHECK(g.c.state == WWLFW_IDLE);
    g.s.drop_cap = 0;
    wwlfw_server(&g.c, srv->node, srv->port, now);
    pump(&g, &now);
    CHECK(g.c.state == WWLFW_FW_READY);
    // no board data: fail at the download, not later
    rig_down(&g);
    const uint32_t keep = g_board_len;
    g_board_len = 0;
    rig_up(&g, &ops);
    srv = wqrtr_lookup(&g.ap, WWLFW_SERVICE, 0);
    now = 0;
    wwlfw_server(&g.c, srv->node, srv->port, now);
    pump(&g, &now);
    CHECK(g.c.state == WWLFW_FAILED && g.s.requests[WLFW_BDF_DOWNLOAD] == 0);
    g_board_len = keep;
    rig_down(&g);
    CHECK(!strcmp(wwlfw_state_name(WWLFW_ENABLED), "enabled"));
}

void t_wlfw(void)
{
    RUN(t_wlfw_full);
    RUN(t_wlfw_paths);
}
