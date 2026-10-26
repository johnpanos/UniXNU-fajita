// SPDX-License-Identifier: BSD-2-Clause-Patent
// QRTR router + the AP services (pd-mapper, TFTP, rmtfs) against a simulated modem: a second router (node 0) on the far
// end of an in-memory link, issuing the requests the modem sends. With the dumped modem partition (t_modem_img) the
// pd-mapper loads the real .jsn files and TFTP serves the real wlanmdsp.mbn through the FAT reader; with the verified
// EFS backup (t_efs_dir) rmtfs serves the real modemst1/2 sectors.
#include "tcheck.h"
#include "../WifiRouter.h"
#include "../WifiPdmap.h"
#include "../WifiTftp.h"
#include "../WifiRmtfs.h"
#include "../WifiQmi.h"
#include "../WifiFat.h"
#include <stdlib.h>
#include <string.h>

static long live;
static void *q_alloc(size_t n, void *c) {(void)c; ++live; return malloc(n);}
static void q_free(void *p, size_t n, void *c) {(void)c; (void)n; --live; free(p);}

// The link: packets from one router queue up for the other.
typedef struct {uint8_t *p[512]; size_t len[512]; unsigned head, n; int full;} lane;
static int lane_xmit(void *ctx, const uint8_t *pkt, size_t len)
{
    lane *l = ctx;
    if (l->full || l->n == 512) return -11;
    const unsigned i = (l->head + l->n) % 512;
    l->p[i] = malloc(len); memcpy(l->p[i], pkt, len); l->len[i] = len; ++l->n;
    return 0;
}
typedef struct {wqrtr ap, mss; lane to_mss, to_ap;} link_t;
static int deliver(lane *l, wqrtr *r)
{
    int n = 0;
    while (l->n) {
        const unsigned i = l->head;
        wqrtr_input(r, l->p[i], l->len[i]);
        free(l->p[i]);
        l->head = (i + 1) % 512; --l->n; ++n;
    }
    return n;
}
static void pump(link_t *k, void (*poll)(void *), void *pc)
{
    for (int i = 0; i < 10000; ++i) {
        int n = deliver(&k->to_mss, &k->mss) + deliver(&k->to_ap, &k->ap);
        wqrtr_flush(&k->ap); wqrtr_flush(&k->mss);
        if (poll) poll(pc);
        if (!n && !k->to_mss.n && !k->to_ap.n) return;
    }
    CHECK(!"link did not settle");
}
static void link_up(link_t *k)
{
    memset(k, 0, sizeof *k);
    wqrtr_init(&k->ap, lane_xmit, &k->to_mss, q_alloc, q_free, 0);
    wqrtr_init(&k->mss, lane_xmit, &k->to_ap, q_alloc, q_free, 0);
    k->mss.local_nid = 0;   // the modem is node 0 on SDM845
    k->mss.hello_once = 1;
}
static void link_down(link_t *k)
{
    wqrtr_reset(&k->ap); wqrtr_reset(&k->mss);
    deliver(&k->to_mss, &k->mss); deliver(&k->to_ap, &k->ap);
    wqrtr_reset(&k->ap); wqrtr_reset(&k->mss);
}

typedef struct {uint8_t buf[70000]; size_t len; uint32_t node, port; int count;} box;
static void box_rx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *d, size_t n, void *ctx)
{
    (void)r; (void)port;
    box *b = ctx;
    if (n <= sizeof b->buf) memcpy(b->buf, d, n);
    b->len = n; b->node = node; b->port = sport; ++b->count;
}
static int evs[8];
static void count_ev(wqrtr *r, int what, uint32_t s, uint32_t i, uint32_t n, uint32_t p, void *c)
{
    (void)r; (void)s; (void)i; (void)n; (void)p; (void)c;
    ++evs[what & 7];
}

static void t_router(void)
{
    link_t k;
    live = 0;
    link_up(&k);
    memset(evs, 0, sizeof evs);
    k.ap.event = count_ev;
    box apb = {0}, mb = {0};
    const uint32_t ap_port = wqrtr_bind(&k.ap, 0, box_rx, &apb);
    CHECK(ap_port >= 0x4000 && wqrtr_bind(&k.ap, ap_port, box_rx, &apb) == 0);
    CHECK(wqrtr_publish(&k.ap, ap_port, 4096, 1, 0) == 0);
    CHECK(k.to_mss.n == 0);   // nothing announced before HELLO
    // The modem: its own servers (wlfw 0x45 v1) and a HELLO to us.
    const uint32_t wlfw_port = wqrtr_bind(&k.mss, 0x10, box_rx, &mb);
    CHECK(wqrtr_publish(&k.mss, wlfw_port, 0x45, 1, 0) == 0);
    CHECK(wqrtr_hello(&k.mss) == 0);
    pump(&k, 0, 0);
    CHECK(k.ap.remote_nid == 0 && k.ap.hello_seen && evs[WQR_EV_HELLO] == 1);
    CHECK(k.mss.hello_seen);   // our HELLO answered theirs
    const wqr_server *srv = wqrtr_lookup(&k.mss, 4096, 0);
    CHECK(srv && srv->node == 1 && srv->port == ap_port && srv->instance == 1);   // instance word 0 << 8 | 1
    srv = wqrtr_lookup(&k.ap, 0x45, 1);
    CHECK(srv && srv->node == 0 && srv->port == 0x10 && evs[WQR_EV_NEW_SERVER] == 1);
    CHECK(wqrtr_lookup(&k.ap, 0x45, 0x0201) == NULL && wqrtr_lookup(&k.ap, 0x45, 0) == srv);
    // Data both ways, odd lengths padded to 4.
    CHECK(wqrtr_send(&k.ap, ap_port, 0, 0x10, "hello", 5) == 0);
    pump(&k, 0, 0);
    CHECK(mb.count == 1 && mb.len == 5 && !memcmp(mb.buf, "hello", 5) && mb.node == 1 && mb.port == ap_port);
    CHECK(wqrtr_send(&k.mss, 0x10, 1, ap_port, "abc", 3) == 0);
    pump(&k, 0, 0);
    CHECK(apb.count == 1 && apb.len == 3);
    // Flow control: the 5th packet asks for confirm_rx; without RESUME_TX the 11th is refused.
    lane hold = {0};
    k.ap.xmit_ctx = &hold;
    for (int i = 0; i < 9; ++i) CHECK(wqrtr_send(&k.ap, ap_port, 0, 0x10, "x", 1) == 0);
    CHECK(wqrtr_send(&k.ap, ap_port, 0, 0x10, "x", 1) == WQR_ERR_BUSY);   // 1 + 9 pending = 10
    CHECK(hold.n == 9 && hold.p[3][16] == 1 && hold.p[2][16] == 0);       // confirm_rx on the 5th (index 3: 1 sent before)
    k.ap.xmit_ctx = &k.to_mss;
    for (unsigned i = 0; i < hold.n; ++i) {wqrtr_input(&k.mss, hold.p[i], hold.len[i]); free(hold.p[i]);}
    pump(&k, 0, 0);   // the modem answered the confirm_rx packet with RESUME_TX
    CHECK(wqrtr_send(&k.ap, ap_port, 0, 0x10, "x", 1) == 0);
    pump(&k, 0, 0);
    // A full link: packets wait in the queue, then go.
    k.to_mss.full = 1;
    CHECK(wqrtr_send(&k.ap, ap_port, 0, 0x10, "q", 1) == 0 && k.ap.txq_count == 1);
    k.to_mss.full = 0;
    CHECK(wqrtr_flush(&k.ap) == 0);
    pump(&k, 0, 0);
    // A v2 header from the modem (16 bytes, u16 node/port).
    {
        uint8_t v2[24] = {3, 1, 0, 0, 4, 0, 0, 0, 0, 0, 0x10, 0, 1, 0, 0, 0, 'v', '2', '!', '!'};
        v2[14] = (uint8_t)ap_port; v2[15] = (uint8_t)(ap_port >> 8);
        CHECK(wqrtr_input(&k.ap, v2, 20) == 0 && apb.len == 4 && !memcmp(apb.buf, "v2!!", 4));
    }
    // Malformed packets are refused and counted.
    {
        uint8_t bad[32] = {1};
        const uint32_t b0 = k.ap.bad;
        CHECK(wqrtr_input(&k.ap, bad, 30) == WQR_ERR_INVAL);
        bad[4] = 4; bad[20] = 8; bad[28] = 5;   // NEW_SERVER to a data port with a short body
        CHECK(wqrtr_input(&k.ap, bad, 32) == WQR_ERR_INVAL);
        bad[0] = 9;
        CHECK(wqrtr_input(&k.ap, bad, 32) == WQR_ERR_INVAL && k.ap.bad == b0 + 3);
    }
    // The modem's client goes away: DEL_CLIENT drops its server here.
    wqrtr_unbind(&k.mss, wlfw_port);
    pump(&k, 0, 0);
    CHECK(wqrtr_lookup(&k.ap, 0x45, 0) == NULL && evs[WQR_EV_DEL_CLIENT] == 1 && evs[WQR_EV_DEL_SERVER] >= 1);
    // Unbinding our port withdraws our server at the modem.
    wqrtr_unbind(&k.ap, ap_port);
    pump(&k, 0, 0);
    CHECK(wqrtr_lookup(&k.mss, 4096, 0) == NULL);
    link_down(&k);
    CHECK(live == 0);
}

static void t_router_bye(void)
{
    link_t k;
    live = 0;
    link_up(&k);
    memset(evs, 0, sizeof evs);
    k.ap.event = count_ev;
    const uint32_t p1 = wqrtr_bind(&k.mss, 0, 0, 0), p2 = wqrtr_bind(&k.mss, 0, 0, 0);
    wqrtr_publish(&k.mss, p1, 0x45, 1, 0);
    wqrtr_publish(&k.mss, p2, 0x42, 1, 0);
    wqrtr_hello(&k.mss);
    pump(&k, 0, 0);
    CHECK(wqrtr_lookup(&k.ap, 0x42, 0) && wqrtr_lookup(&k.ap, 0x45, 0));
    uint8_t bye[32 + 20] = {0};
    bye[0] = 1; bye[4] = 3; bye[8] = 0; bye[20] = 20;
    memset(bye + 24, 0xff, 4); bye[28] = 0xfe; bye[29] = 0xff; bye[30] = 0xff; bye[31] = 0xff;
    bye[32] = 3;
    CHECK(wqrtr_input(&k.ap, bye, sizeof bye) == 0);
    CHECK(!wqrtr_lookup(&k.ap, 0x42, 0) && !wqrtr_lookup(&k.ap, 0x45, 0) && evs[WQR_EV_BYE] == 1 && evs[WQR_EV_DEL_SERVER] == 2);
    link_down(&k);
    CHECK(live == 0);
}

// ---- pd-mapper ----
static const char kJsnWlan[] =
    "{\n \"sr_version\": {\"major\": 1, \"minor\": 1, \"patch\": 1},\n"
    " \"sr_domain\": {\"soc\": \"msm\", \"domain\": \"modem\", \"subdomain\": \"wlan_pd\", \"qmi_instance_id\": 180},\n"
    " \"sr_service\": [\n"
    "  {\"provider\": \"kernel\", \"service\": \"elf_loader\", \"service_data_valid\": 0, \"service_data\": 0},\n"
    "  {\"provider\": \"tms\", \"service\": \"servreg\", \"service_data_valid\": 0, \"service_data\": 0},\n"
    "  {\"provider\": \"wlan\", \"service\": \"fw\", \"service_data_valid\": 0, \"service_data\": 0}\n ]\n}\n";
static const char kJsnRoot[] =
    "{\"sr_version\":{\"major\":1,\"minor\":1,\"patch\":1},\"sr_domain\":{\"soc\":\"msm\",\"domain\":\"modem\","
    "\"subdomain\":\"root_pd\",\"qmi_instance_id\":180},\"sr_service\":[{\"provider\":\"tms\",\"service\":\"servreg\","
    "\"service_data_valid\":0,\"service_data\":0}]}";

static long domain_req(uint8_t *buf, size_t cap, uint16_t txn, const char *name, int with_offset, uint32_t offset)
{
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WPDM_GET_DOMAIN_LIST);
    wqmi_tlv(&w, 0x01, name, strlen(name));
    if (with_offset) wqmi_u32(&w, 0x10, offset);
    return wqmi_end(&w);
}

// Decode a GET_DOMAIN_LIST response into "name:instance;" entries.
static int decode_domains(const uint8_t *m, size_t n, char *out, size_t cap, unsigned *total)
{
    uint16_t res = 9, err = 9, vl;
    if (wqmi_get_result(m, n, &res, &err) || res) return -1;
    const uint8_t *t = wqmi_find(m, n, 0x10, &vl);
    *total = t && vl == 2 ? (unsigned)(t[0] | t[1] << 8) : 0;
    out[0] = 0;
    const uint8_t *l = wqmi_find(m, n, 0x12, &vl);
    if (!l) return 0;
    size_t at = 1;
    for (unsigned i = 0; i < l[0]; ++i) {
        const unsigned nl = l[at++];
        char name[80]; memcpy(name, l + at, nl); name[nl] = 0; at += nl;
        const uint32_t inst = l[at] | l[at + 1] << 8 | l[at + 2] << 16 | (uint32_t)l[at + 3] << 24;
        at += 9;
        snprintf(out + strlen(out), cap - strlen(out), "%s:%u;", name, inst);
    }
    return at == vl ? (int)l[0] : -1;
}

static int fat_read_img(void *ctx, uint64_t block, uint32_t count, void *dst)
{
    FILE *f = ctx;
    if (fseek(f, (long)(block * 4096), SEEK_SET)) return -1;
    return fread(dst, 4096, count, f) == count ? 0 : -1;
}

static void t_pdmap(void)
{
    static wpdm p;
    wpdm_init(&p);
    CHECK(wpdm_add_jsn(&p, kJsnRoot, strlen(kJsnRoot)) == 1);
    CHECK(wpdm_add_jsn(&p, kJsnWlan, strlen(kJsnWlan)) == 3);
    CHECK(p.count == 4 && !strcmp(p.map[1].service, "kernel/elf_loader") && !strcmp(p.map[1].domain, "msm/modem/wlan_pd"));
    // Malformed files add nothing.
    CHECK(wpdm_add_jsn(&p, "{\"sr_domain\": {}}", 17) == -1);
    CHECK(wpdm_add_jsn(&p, kJsnWlan, strlen(kJsnWlan) - 5) == -1);
    CHECK(wpdm_add_jsn(&p, "{\"sr_domain\": {\"soc\": \"m\\u0041\"}}", 33) == -1);
    CHECK(p.count == 4);
    uint8_t req[128], resp[2048];
    char got[512];
    unsigned total = 0;
    long n = domain_req(req, sizeof req, 7, "tms/servreg", 0, 0);
    long rn = wpdm_handle(&p, req, n, resp, sizeof resp);
    wqmi_hdr h;
    CHECK(rn > 0 && wqmi_parse_hdr(resp, rn, &h) == 0 && h.type == WQMI_RESPONSE && h.txn == 7 && h.msg_id == 0x21);
    CHECK(decode_domains(resp, rn, got, sizeof got, &total) == 2 && total == 2);
    CHECK(!strcmp(got, "msm/modem/root_pd:180;msm/modem/wlan_pd:180;"));
    n = domain_req(req, sizeof req, 8, "wlan/fw", 0, 0);
    rn = wpdm_handle(&p, req, n, resp, sizeof resp);
    CHECK(decode_domains(resp, rn, got, sizeof got, &total) == 1 && !strcmp(got, "msm/modem/wlan_pd:180;"));
    // Paging with domain_offset.
    n = domain_req(req, sizeof req, 9, "tms/servreg", 1, 1);
    rn = wpdm_handle(&p, req, n, resp, sizeof resp);
    CHECK(decode_domains(resp, rn, got, sizeof got, &total) == 1 && total == 2 && !strcmp(got, "msm/modem/wlan_pd:180;"));
    // Unknown service: success, zero domains, no list TLV (pd-mapper).
    n = domain_req(req, sizeof req, 10, "audio/x", 0, 0);
    rn = wpdm_handle(&p, req, n, resp, sizeof resp);
    CHECK(decode_domains(resp, rn, got, sizeof got, &total) == 0 && total == 0);
    // A request without the name TLV is malformed.
    {
        wqmi_writer w; wqmi_begin(&w, req, sizeof req, WQMI_REQUEST, 11, 0x21); n = wqmi_end(&w);
        rn = wpdm_handle(&p, req, n, resp, sizeof resp);
        uint16_t res, err;
        CHECK(rn > 0 && wqmi_get_result(resp, rn, &res, &err) == 0 && res == 1 && err == 2);
    }
    // The existing sdm845 table agrees with what the .jsn files say.
    wqmi_domain d[4];
    CHECK(servreg_sdm845_lookup("tms/servreg", d, 4) == 2 && !strcmp(d[0].name, "msm/modem/root_pd"));
    // The real files from the phone's modem partition.
    if (t_modem_img) {
        FILE *f = fopen(t_modem_img, "rb");
        static wfat fs;
        fseek(f, 0, SEEK_END); const long size = ftell(f);
        CHECK(wfat_mount(&fs, fat_read_img, f, (uint64_t)size / 4096) == 0);
        static wpdm real;
        wpdm_init(&real);
        const char *names[] = {"image/modemr.jsn", "image/modemuw.jsn"};
        for (int i = 0; i < 2; ++i) {
            wfat_file wf;
            char buf[2048];
            CHECK(wfat_open(&fs, names[i], &wf) == 0);
            const long got_n = wfat_read(&fs, &wf, 0, buf, sizeof buf);
            CHECK(got_n > 0 && wpdm_add_jsn(&real, buf, (size_t)got_n) > 0);
        }
        CHECK(real.count == 4);
        for (unsigned i = 0; i < 4; ++i)
            CHECK(!strcmp(real.map[i].service, p.map[i].service) && !strcmp(real.map[i].domain, p.map[i].domain)
                  && real.map[i].instance == p.map[i].instance);
        printf("  pd-mapper: %u services from modemr.jsn + modemuw.jsn match the synthetic table\n", real.count);
        fclose(f);
    }
}

// ---- TFTP ----
typedef struct {const char *name; const uint8_t *data; uint32_t len;} memfile;
static memfile mfiles[4];
static int mem_open(void *ctx, const char *path, uint64_t *cookie, uint32_t *size)
{
    (void)ctx;
    for (unsigned i = 0; i < 4; ++i)
        if (mfiles[i].name && !strcmp(mfiles[i].name, path)) {*cookie = i; *size = mfiles[i].len; return 0;}
    return -1;
}
static long mem_read(void *ctx, uint64_t cookie, uint32_t off, void *dst, uint32_t len)
{
    (void)ctx;
    const memfile *m = &mfiles[cookie];
    if (off >= m->len) return 0;
    const uint32_t n = m->len - off < len ? m->len - off : len;
    memcpy(dst, m->data + off, n);
    return n;
}

// FAT-backed file source (as the phone uses), cookie = first_cluster | size << 32.
static wfat g_fat;
static int fat_open(void *ctx, const char *path, uint64_t *cookie, uint32_t *size)
{
    (void)ctx;
    wfat_file f;
    if (wfat_open(&g_fat, path, &f) || f.dir) return -1;
    *cookie = f.first_cluster | (uint64_t)f.size << 32;
    *size = f.size;
    return 0;
}
static long fat_read(void *ctx, uint64_t cookie, uint32_t off, void *dst, uint32_t len)
{
    (void)ctx;
    wfat_file f = {(uint32_t)cookie, (uint32_t)(cookie >> 32), 0};
    return wfat_read(&g_fat, &f, off, dst, len);
}

static void tftp_poll(void *c) {wtftp_poll(c);}
static void tftp_ev(wqrtr *r, int what, uint32_t s, uint32_t i, uint32_t n, uint32_t p, void *c)
{
    (void)r; (void)s; (void)i;
    wtftp_event(c, what, n, p);
}

static size_t opt(uint8_t *b, size_t at, const char *k, const char *v)
{
    memcpy(b + at, k, strlen(k) + 1); at += strlen(k) + 1;
    memcpy(b + at, v, strlen(v) + 1); return at + strlen(v) + 1;
}

// A collecting client: every DATA block is appended at its block offset.
typedef struct {uint8_t *out; uint32_t blksize, max_block, bytes, packets, short_seen; uint32_t xport; int err;} collector;
static void collect_rx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *d, size_t n, void *ctx)
{
    (void)r; (void)port; (void)node;
    collector *c = ctx;
    c->xport = sport;
    ++c->packets;
    if (n < 4) return;
    const unsigned op = d[0] << 8 | d[1], blk = d[2] << 8 | d[3];
    if (op == WTFTP_OP_ERROR) {c->err = d[3] ? d[3] : 100; return;}
    if (op == WTFTP_OP_OACK) return;
    if (op != WTFTP_OP_DATA || !blk) {c->err = 200; return;}
    memcpy(c->out + (size_t)(blk - 1) * c->blksize, d + 4, n - 4);
    c->bytes += (uint32_t)(n - 4);
    if (blk > c->max_block) c->max_block = blk;
    if (n - 4 < c->blksize) c->short_seen = 1;
}

// Read [seek, seek + rsize) of `path` the way the modem does: RRQ+options, ACK 0, then ACK each window's last block,
// END_OF_TRANSFER once rsize bytes are in. Returns bytes, or -error.
static long modem_read(link_t *k, wtftp *t, const char *path, uint32_t blksize, uint32_t rsize, uint32_t seek,
                       uint32_t wsize, uint8_t *out, uint32_t *blk_used)
{
    collector c = {out, blksize, 0, 0, 0, 0, 0, 0};
    const uint32_t port = wqrtr_bind(&k->mss, 0, collect_rx, &c);
    uint8_t rrq[256];
    size_t at = 2;
    rrq[0] = 0; rrq[1] = 1;
    memcpy(rrq + at, path, strlen(path) + 1); at += strlen(path) + 1;
    memcpy(rrq + at, "octet", 6); at += 6;
    char v[16];
    snprintf(v, sizeof v, "%u", blksize); at = opt(rrq, at, "blksize", v);
    snprintf(v, sizeof v, "%u", rsize); at = opt(rrq, at, "rsize", v);
    snprintf(v, sizeof v, "%u", seek); at = opt(rrq, at, "seek", v);
    snprintf(v, sizeof v, "%u", wsize); at = opt(rrq, at, "wsize", v);
    const wqr_server *srv = wqrtr_lookup(&k->mss, WTFTP_SERVICE, 0);
    long ret = -100;
    if (!srv) goto out;
    wqrtr_send(&k->mss, port, srv->node, srv->port, rrq, at);
    pump(k, tftp_poll, t);
    if (c.err) {ret = -c.err; goto out;}
    if (c.packets != 1) {ret = -101; goto out;}
    // the OACK's blksize
    {
        uint8_t ack[4] = {0, WTFTP_OP_ACK, 0, 0};
        c.blksize = blksize > WTFTP_MAX_BLKSIZE ? WTFTP_MAX_BLKSIZE : blksize;
        if (blk_used) *blk_used = c.blksize;
        uint32_t last = 0;
        for (int guard = 0; guard < 100000; ++guard) {
            ack[2] = (uint8_t)(last >> 8); ack[3] = (uint8_t)last;
            wqrtr_send(&k->mss, port, 1, c.xport, ack, 4);
            pump(k, tftp_poll, t);
            if (c.err) {ret = -c.err; goto out;}
            if (c.bytes >= rsize || c.short_seen) break;
            if (c.max_block == last) {ret = -102; goto out;}
            last = c.max_block;
        }
        uint8_t eot[5] = {0, WTFTP_OP_ERROR, 0, WTFTP_E_END_OF_TRANSFER, 0};
        wqrtr_send(&k->mss, port, 1, c.xport, eot, 5);
        pump(k, tftp_poll, t);
        ret = c.bytes;
    }
out:
    wqrtr_unbind(&k->mss, port);
    pump(k, tftp_poll, t);
    return ret;
}

static long simple_req(link_t *k, wtftp *t, int op, const char *path, const char *mode, const char *const *opts,
                       uint8_t *reply, size_t *reply_len)
{
    box b = {0};
    const uint32_t port = wqrtr_bind(&k->mss, 0, box_rx, &b);
    uint8_t rq[256];
    size_t at = 2;
    rq[0] = 0; rq[1] = (uint8_t)op;
    memcpy(rq + at, path, strlen(path) + 1); at += strlen(path) + 1;
    memcpy(rq + at, mode, strlen(mode) + 1); at += strlen(mode) + 1;
    for (; opts && *opts; opts += 2) at = opt(rq, at, opts[0], opts[1]);
    const wqr_server *srv = wqrtr_lookup(&k->mss, WTFTP_SERVICE, 0);
    wqrtr_send(&k->mss, port, srv->node, srv->port, rq, at);
    pump(k, tftp_poll, t);
    if (reply) {memcpy(reply, b.buf, b.len); *reply_len = b.len;}
    wqrtr_unbind(&k->mss, port);
    pump(k, tftp_poll, t);
    return b.count ? (long)b.port : -1;
}

static void t_tftp(void)
{
    link_t k;
    live = 0;
    link_up(&k);
    static uint8_t img[100000], out[200000];
    for (unsigned i = 0; i < sizeof img; ++i) img[i] = (uint8_t)(i * 131 + (i >> 8));
    mfiles[0] = (memfile){"image/wlanmdsp.mbn", img, sizeof img};
    mfiles[1] = (memfile){"image/modem_pr/mcfg.mbn", img, 1000};
    static wtftp t;
    wtftp_fs fs = {mem_open, mem_read, 0};
    CHECK(wtftp_init(&t, &k.ap, &fs) == 0);
    k.ap.event = tftp_ev; k.ap.event_ctx = &t;
    wqrtr_hello(&k.mss);
    pump(&k, 0, 0);
    CHECK(wqrtr_lookup(&k.mss, WTFTP_SERVICE, 1) != NULL);
    // Path translation (translate.c).
    char p[96];
    CHECK(wtftp_translate("/readonly/firmware/image/wlanmdsp.mbn", p, sizeof p) == 0 && !strcmp(p, "image/wlanmdsp.mbn"));
    CHECK(wtftp_translate("/readonly/vendor/firmware_mnt/image/a.b", p, sizeof p) == 0 && !strcmp(p, "image/a.b"));
    CHECK(wtftp_translate("/readonly/vendor/firmware/x", p, sizeof p) == 0 && !strcmp(p, "image/x"));
    CHECK(wtftp_translate("/readonly/firmware/modem_pr/mcfg.mbn", p, sizeof p) == 0 && !strcmp(p, "image/modem_pr/mcfg.mbn"));
    CHECK(wtftp_translate("/readwrite/server_check.txt", p, sizeof p) == 1 && !strcmp(p, "server_check.txt"));
    CHECK(wtftp_translate("/readonly/firmware/image/../x", p, sizeof p) == -1);
    CHECK(wtftp_translate("/readonly//firmware/image/x", p, sizeof p) == -1);
    CHECK(wtftp_translate("/etc/passwd", p, sizeof p) == -1);
    // The whole file in rsize windows at increasing seek, as the modem reads wlanmdsp.mbn.
    uint32_t used = 0;
    const uint32_t window = 4 * 7680;
    long total = 0;
    for (uint32_t seek = 0; seek < sizeof img; seek += window) {
        const uint32_t want = sizeof img - seek < window ? sizeof img - seek : window;
        const long n = modem_read(&k, &t, "/readonly/firmware/image/wlanmdsp.mbn", 7680, want, seek, 2, out + seek, &used);
        CHECK(n == (long)want);
        if (n != (long)want) break;
        total += n;
    }
    CHECK(total == (long)sizeof img && !memcmp(out, img, sizeof img) && used == 7680);
    CHECK(t.rrq == (sizeof img + window - 1) / window && t.bytes >= sizeof img);
    for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) CHECK(!t.c[i].used);   // every transfer closed
    // blksize above our cap is answered with the cap; wsize 12 exceeds the QRTR window and is paused, then resumed.
    memset(out, 0, sizeof out);
    long n = modem_read(&k, &t, "/readonly/firmware/image/wlanmdsp.mbn", 16384, 98304, 0, 12, out, &used);
    CHECK(n == 98304 && used == WTFTP_MAX_BLKSIZE && !memcmp(out, img, 98304));
    // Errors: unknown file (ENOENT), traversal (EACCESS), netascii (EBADOP), bad option (EOPTNEG), write to firmware.
    static uint8_t rep[70000]; size_t rl = 0;
    const char *const o1[] = {"blksize", "7", 0};
    CHECK(simple_req(&k, &t, 1, "/readonly/firmware/image/nope.bin", "octet", 0, rep, &rl) > 0 && rep[1] == 5 && rep[3] == 1);
    CHECK(simple_req(&k, &t, 1, "/readonly/firmware/image/../../x", "octet", 0, rep, &rl) > 0 && rep[1] == 5 && rep[3] == 2);
    CHECK(simple_req(&k, &t, 1, "/readonly/firmware/image/wlanmdsp.mbn", "netascii", 0, rep, &rl) > 0 && rep[3] == 4);
    CHECK(simple_req(&k, &t, 1, "/readonly/firmware/image/wlanmdsp.mbn", "octet", o1, rep, &rl) > 0 && rep[3] == 8);
    CHECK(simple_req(&k, &t, 2, "/readonly/firmware/image/wlanmdsp.mbn", "octet", 0, rep, &rl) > 0 && rep[3] == 2);
    CHECK(t.refused >= 2);
    // No options: block 1 straight away (tqftpserv).
    CHECK(simple_req(&k, &t, 1, "/readonly/firmware/image/modem_pr/mcfg.mbn", "OCTET", 0, rep, &rl) > 0 && rep[1] == 3
          && rep[3] == 1 && rl == 4 + 512 && !memcmp(rep + 4, img, 512));
    // tsize is reported in the OACK, each option as name NUL value NUL (tqftpserv.c tftp_send_oack).
    const char *const o2[] = {"blksize", "1024", "tsize", "0", 0};
    CHECK(simple_req(&k, &t, 1, "/readonly/firmware/image/wlanmdsp.mbn", "octet", o2, rep, &rl) > 0 && rep[1] == 6);
    {
        static const char want[] = "\0\6blksize\0" "1024\0timeoutms\0" "1000\0tsize\0" "100000\0wsize\0" "1";
        CHECK(rl == sizeof want && !memcmp(rep, want, sizeof want));
    }
    // /readwrite/: a write lands in the RAM store, and reads back.
    {
        collector c = {out, 512, 0, 0, 0, 0, 0, 0};
        const uint32_t port = wqrtr_bind(&k.mss, 0, collect_rx, &c);
        uint8_t rq[128] = {0, 2};
        size_t at = 2;
        memcpy(rq + at, "/readwrite/server_check.txt", 28); at += 28;
        memcpy(rq + at, "octet", 6); at += 6;
        at = opt(rq, at, "blksize", "512");
        const wqr_server *srv = wqrtr_lookup(&k.mss, WTFTP_SERVICE, 0);
        wqrtr_send(&k.mss, port, srv->node, srv->port, rq, at);
        pump(&k, tftp_poll, &t);
        CHECK(c.packets == 1 && !c.err);   // OACK
        uint8_t d[4 + 600] = {0, 3, 0, 1};
        memset(d + 4, 'A', 512);
        wqrtr_send(&k.mss, port, 1, c.xport, d, 4 + 512);
        pump(&k, tftp_poll, &t);
        d[3] = 2; memcpy(d + 4, "tail", 4);
        wqrtr_send(&k.mss, port, 1, c.xport, d, 8);
        pump(&k, tftp_poll, &t);
        wqrtr_unbind(&k.mss, port);
        pump(&k, tftp_poll, &t);
        CHECK(t.ram[0].used && t.ram[0].len == 516 && !memcmp(t.ram[0].data + 512, "tail", 4));
        memset(out, 0, 600);
        n = modem_read(&k, &t, "/readwrite/server_check.txt", 512, 516, 0, 4, out, &used);
        CHECK(n == 516 && out[0] == 'A' && !memcmp(out + 512, "tail", 4));
    }
    // DEL_CLIENT from the modem ends a transfer it abandoned.
    {
        collector c = {out, 512, 0, 0, 0, 0, 0, 0};
        const uint32_t port = wqrtr_bind(&k.mss, 0, collect_rx, &c);
        uint8_t rq[128] = {0, 1};
        size_t at = 2;
        memcpy(rq + at, "/readonly/firmware/image/wlanmdsp.mbn", 38); at += 38;
        memcpy(rq + at, "octet", 6); at += 6;
        at = opt(rq, at, "rsize", "4096");
        const wqr_server *srv = wqrtr_lookup(&k.mss, WTFTP_SERVICE, 0);
        k.ap.event = 0;
        wqrtr_send(&k.mss, port, srv->node, srv->port, rq, at);
        pump(&k, tftp_poll, &t);
        int open = 0;
        for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) open += t.c[i].used;
        CHECK(open == 1);
        wtftp_event(&t, WQR_EV_DEL_CLIENT, 0, port);
        open = 0;
        for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) open += t.c[i].used;
        CHECK(open == 0);
        wqrtr_unbind(&k.mss, port);
        pump(&k, tftp_poll, &t);
    }
    {   // a write request echoes the size the modem announced (tqftpserv's OACK carries the parsed tsize), not the file's
        const char *const o3[] = {"blksize", "512", "tsize", "5", 0};
        CHECK(simple_req(&k, &t, 2, "/readwrite/tsize.txt", "octet", o3, rep, &rl) > 0 && rep[1] == 6);
        static const char want[] = "\0\6blksize\0" "512\0timeoutms\0" "1000\0tsize\0" "5\0wsize\0" "1";
        CHECK(rl == sizeof want && !memcmp(rep, want, sizeof want));
    }
    // The real wlanmdsp.mbn from the phone's own modem partition, through the FAT reader, against a direct read.
    if (t_modem_img) {
        FILE *f = fopen(t_modem_img, "rb");
        fseek(f, 0, SEEK_END); const long size = ftell(f);
        CHECK(wfat_mount(&g_fat, fat_read_img, f, (uint64_t)size / 4096) == 0);
        wtftp_fs ffs = {fat_open, fat_read, 0};
        t.fs = ffs;
        wfat_file wf;
        CHECK(wfat_open(&g_fat, "image/wlanmdsp.mbn", &wf) == 0);
        uint8_t *direct = malloc(wf.size), *served = malloc(wf.size + 65536);
        CHECK(wfat_read(&g_fat, &wf, 0, direct, wf.size) == (long)wf.size);
        long got = 0;
        const uint32_t win = 64 * 1024;
        for (uint32_t seek = 0; seek < wf.size; seek += win) {
            const uint32_t want = wf.size - seek < win ? wf.size - seek : win;
            const long r = modem_read(&k, &t, "/readonly/firmware/image/wlanmdsp.mbn", 7680, want, seek, 8, served + seek, &used);
            if (r != (long)want) {CHECK(r == (long)want); break;}
            got += r;
        }
        CHECK(got == (long)wf.size && !memcmp(direct, served, wf.size));
        printf("  tftp: served the real wlanmdsp.mbn (%u bytes) in %u rsize windows, identical to the FAT read\n",
               wf.size, (wf.size + win - 1) / win);
        free(direct); free(served);
        fclose(f);
    }
    link_down(&k);
    for (unsigned i = 0; i < WTFTP_RAM_FILES; ++i) if (t.ram[i].used) q_free(t.ram[i].data, 0, 0);
    CHECK(live == 0);
}

// ---- rmtfs ----
typedef struct {const char *dir;} efs;
static long efs_read(void *ctx, const char *name, uint64_t off, void *dst, uint32_t len)
{
    efs *e = ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.img", e->dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET)) {fclose(f); return 0;}
    const size_t n = fread(dst, 1, len, f);
    fclose(f);
    return (long)n;
}
static long long efs_size(void *ctx, const char *name)
{
    efs *e = ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.img", e->dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long long n = ftell(f);
    fclose(f);
    return n;
}
// A synthetic partition set for when the backup is absent.
static uint8_t synth[2][65536];
static long syn_read(void *ctx, const char *name, uint64_t off, void *dst, uint32_t len)
{
    (void)ctx;
    const int i = !strcmp(name, "modemst1") ? 0 : !strcmp(name, "modemst2") ? 1 : -1;
    if (i < 0) return -1;
    if (off >= sizeof synth[i]) return 0;
    const uint32_t n = sizeof synth[i] - off < len ? (uint32_t)(sizeof synth[i] - off) : len;
    memcpy(dst, synth[i] + off, n);
    return n;
}
static long long syn_size(void *ctx, const char *name)
{
    (void)ctx;
    return !strcmp(name, "modemst1") || !strcmp(name, "modemst2") ? (long long)sizeof synth[0] : -1;
}

static long rm_open(uint8_t *b, size_t cap, uint16_t txn, const char *path)
{
    wqmi_writer w; wqmi_begin(&w, b, cap, WQMI_REQUEST, txn, 1); wqmi_tlv(&w, 1, path, strlen(path)); return wqmi_end(&w);
}
static long rm_iovec(uint8_t *b, size_t cap, uint16_t txn, uint32_t caller, uint8_t dir, const uint32_t (*iov)[3], unsigned n)
{
    wqmi_writer w; wqmi_begin(&w, b, cap, WQMI_REQUEST, txn, 3);
    wqmi_u32(&w, 1, caller); wqmi_u8(&w, 2, dir);
    uint8_t v[1 + 12 * 16]; v[0] = (uint8_t)n;
    for (unsigned i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) memcpy(v + 1 + i * 12 + k * 4, &iov[i][k], 4);
    wqmi_tlv(&w, 3, v, 1 + 12 * n); wqmi_u8(&w, 4, 0);
    return wqmi_end(&w);
}
static int rm_result(const uint8_t *b, long n, uint16_t *err)
{
    uint16_t res = 9; *err = 9;
    if (n <= 0 || wqmi_get_result(b, (size_t)n, &res, err)) return -1;
    return res;
}

static void t_rmtfs(void)
{
    static uint8_t shared[0x10000];   // stands in for the rmtfs_mem window
    const uint64_t phys = 0xf5b01000ull;
    static wrmtfs s;
    efs e = {t_efs_dir};
    for (unsigned i = 0; i < sizeof synth[0]; ++i) {synth[0][i] = (uint8_t)(i ^ (i >> 9)); synth[1][i] = (uint8_t)~i;}
    if (t_efs_dir && efs_size(&e, "modemst1") > 0) wrmtfs_init(&s, 0, efs_read, efs_size, &e, shared, phys, sizeof shared);
    else wrmtfs_init(&s, 0, syn_read, syn_size, 0, shared, phys, sizeof shared);
    CHECK(!strcmp(wrmtfs_partition("/boot/modem_fs1"), "modemst1") && !strcmp(wrmtfs_partition("/boot/modem_fsg"), "fsg"));
    CHECK(wrmtfs_partition("/boot/other") == NULL && !strcmp(wrmtfs_partition("/oppo/oem_partion"), "oem_stanvbk"));
    uint8_t rq[512], rp[256];
    uint16_t err;
    long n = rm_open(rq, sizeof rq, 1, "/boot/modem_fs1");
    long rn = wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp);
    uint16_t vl;
    CHECK(rm_result(rp, rn, &err) == 0);
    const uint8_t *v = wqmi_find(rp, rn, 0x10, &vl);
    CHECK(v && vl == 4 && v[0] == 0);
    n = rm_open(rq, sizeof rq, 2, "/boot/modem_fs2");
    rn = wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp);
    v = wqmi_find(rp, rn, 0x10, &vl);
    CHECK(rm_result(rp, rn, &err) == 0 && v && v[0] == 1);
    n = rm_open(rq, sizeof rq, 3, "/boot/modem_fs1");   // the same node reopening gets the same caller
    rn = wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp);
    v = wqmi_find(rp, rn, 0x10, &vl);
    CHECK(v && v[0] == 0);
    n = rm_open(rq, sizeof rq, 4, "/boot/nope");
    CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 1 && err == 1);
    // ALLOC_BUFF: the shared buffer's physical address.
    {
        wqmi_writer w; wqmi_begin(&w, rq, sizeof rq, WQMI_REQUEST, 5, 4); wqmi_u32(&w, 1, 0); wqmi_u32(&w, 2, 0x8000);
        n = wqmi_end(&w);
        rn = wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp);
        v = wqmi_find(rp, rn, 0x10, &vl);
        uint64_t addr = 0; if (v) memcpy(&addr, v, 8);
        CHECK(rm_result(rp, rn, &err) == 0 && vl == 8 && addr == phys);
        wqmi_begin(&w, rq, sizeof rq, WQMI_REQUEST, 6, 4); wqmi_u32(&w, 1, 0); wqmi_u32(&w, 2, 0x20000);
        n = wqmi_end(&w);
        CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 1);   // too large
    }
    // Read sectors 0..7 of modemst1 to buf+0 and sectors 100..103 of modemst2 to buf+0x2000.
    memset(shared, 0xee, sizeof shared);
    const uint32_t iov[2][3] = {{0, (uint32_t)phys, 8}, {100, (uint32_t)phys + 0x2000, 4}};
    n = rm_iovec(rq, sizeof rq, 7, 0, 0, iov, 1);
    CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 0);
    n = rm_iovec(rq, sizeof rq, 8, 1, 0, iov + 1, 1);
    CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 0);
    uint8_t want[4096];
    const void *rctx = s.ctx;
    CHECK(s.read((void *)rctx, "modemst1", 0, want, 4096) == 4096 && !memcmp(shared, want, 4096));
    CHECK(s.read((void *)rctx, "modemst2", 100 * 512, want, 2048) == 2048 && !memcmp(shared + 0x2000, want, 2048));
    CHECK(shared[4096] == 0xee && s.sectors_read == 12);
    // Writes are refused and counted; the buffer is untouched, the partition is never touched (read-only callbacks).
    memset(shared, 0x77, 512);
    n = rm_iovec(rq, sizeof rq, 9, 0, 1, iov, 1);
    CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 1 && err == 1 && s.writes_refused == 1);
    // Out of the buffer, an unknown caller, another node's caller, a malformed iovec.
    const uint32_t outside[1][3] = {{0, (uint32_t)phys + 0x10000 - 256, 1}};
    n = rm_iovec(rq, sizeof rq, 10, 0, 0, outside, 1);
    CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 1);
    n = rm_iovec(rq, sizeof rq, 11, 7, 0, iov, 1);
    CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 1);
    n = rm_iovec(rq, sizeof rq, 12, 0, 0, iov, 1);
    CHECK(rm_result(rp, wrmtfs_handle(&s, 5, rq, n, rp, sizeof rp), &err) == 1);
    {
        // count byte says 2 entries but only one is present
        long m = rm_iovec(rq, sizeof rq, 14, 0, 0, iov, 1);
        for (long i = 7; i + 3 < m; ) {
            const uint8_t type = rq[i]; const uint16_t l = rq[i + 1] | rq[i + 2] << 8;
            if (type == 3) {rq[i + 3] = 2; break;}
            i += 3 + l;
        }
        CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, m, rp, sizeof rp), &err) == 1 && err == 2);
    }
    // GET_DEV_ERROR, CLOSE.
    {
        wqmi_writer w; wqmi_begin(&w, rq, sizeof rq, WQMI_REQUEST, 15, 5); wqmi_u32(&w, 1, 0); n = wqmi_end(&w);
        rn = wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp);
        v = wqmi_find(rp, rn, 0x10, &vl);
        CHECK(rm_result(rp, rn, &err) == 0 && v && vl == 1 && v[0] == 0);
        wqmi_begin(&w, rq, sizeof rq, WQMI_REQUEST, 16, 2); wqmi_u32(&w, 1, 1); n = wqmi_end(&w);
        CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 0);
        CHECK(rm_result(rp, wrmtfs_handle(&s, 0, rq, n, rp, sizeof rp), &err) == 1);   // already closed
    }
    if (t_efs_dir && s.read == efs_read) printf("  rmtfs: served modemst1/modemst2 sectors from %s, writes refused\n", t_efs_dir);
    // Over QRTR: published as service 14, answered to the asking port.
    link_t k;
    live = 0;
    link_up(&k);
    static wrmtfs s2;
    CHECK(wrmtfs_init(&s2, &k.ap, syn_read, syn_size, 0, shared, phys, sizeof shared) == 0);
    wqrtr_hello(&k.mss);
    pump(&k, 0, 0);
    const wqr_server *srv = wqrtr_lookup(&k.mss, WRMTFS_SERVICE, 1);
    CHECK(srv != NULL);
    box b = {0};
    const uint32_t port = wqrtr_bind(&k.mss, 0, box_rx, &b);
    n = rm_open(rq, sizeof rq, 21, "/boot/modem_fs2");
    wqrtr_send(&k.mss, port, srv->node, srv->port, rq, (size_t)n);
    pump(&k, 0, 0);
    wqmi_hdr h;
    CHECK(b.count == 1 && wqmi_parse_hdr(b.buf, b.len, &h) == 0 && h.txn == 21 && rm_result(b.buf, (long)b.len, &err) == 0);
    link_down(&k);
    CHECK(live == 0);
}

// The phone's modem announces 32 services before wlfw (round 8 log): wlfw's NEW_SERVER is the 33rd and must not be lost.
static uint32_t last_new_svc;
static void last_ev(wqrtr *r, int what, uint32_t s, uint32_t i, uint32_t n, uint32_t p, void *c)
{
    (void)r; (void)i; (void)n; (void)p; (void)c;
    ++evs[what & 7];
    if (what == WQR_EV_NEW_SERVER) last_new_svc = s;
}
static void new_server_pkt(wqrtr *ap, uint32_t svc, uint32_t inst, uint32_t port)
{
    uint8_t p[52] = {0};
    const uint32_t w[13] = {1, 4, 0, 0xfffffffeu, 0, 20, 1, 0xfffffffeu, 4, svc, inst, 0, port};
    for (int i = 0; i < 13; ++i) for (int b = 0; b < 4; ++b) p[4 * i + b] = (uint8_t)(w[i] >> 8 * b);
    CHECK(wqrtr_input(ap, p, sizeof p) == 0);
}
static void t_router_many_servers(void)
{
    link_t k;
    live = 0;
    link_up(&k);
    memset(evs, 0, sizeof evs);
    k.ap.event = last_ev;
    CHECK(wqrtr_hello(&k.mss) == 0);
    pump(&k, 0, 0);
    CHECK(k.ap.hello_seen);
    for (uint32_t i = 0; i < 32; ++i) new_server_pkt(&k.ap, 0x100 + i, 0x101, 0x20 + i);
    new_server_pkt(&k.ap, 0x45, 1, 0x60);   // the 33rd: wlfw
    CHECK(evs[WQR_EV_NEW_SERVER] == 33 && last_new_svc == 0x45);
    const wqr_server *srv = wqrtr_lookup(&k.ap, 0x45, 0);
    CHECK(srv && srv->port == 0x60 && srv->node == 0 && k.ap.remote_full == 0);
    // Past the table: the event still reaches the services.
    for (uint32_t i = 0; i < WQR_MAX_REMOTE_SRV; ++i) new_server_pkt(&k.ap, 0x400 + i, 1, 0x100 + i);
    CHECK(k.ap.remote_full == 33 && last_new_svc == 0x400 + WQR_MAX_REMOTE_SRV - 1);
    link_down(&k);
    CHECK(live == 0);
}

void t_qrtr(void)
{
    RUN(t_router);
    RUN(t_router_bye);
    RUN(t_router_many_servers);
    RUN(t_pdmap);
    RUN(t_tftp);
    RUN(t_rmtfs);
}
