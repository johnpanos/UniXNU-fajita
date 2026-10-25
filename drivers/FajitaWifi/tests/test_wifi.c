// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the fajita Wi-Fi protocol code: mbn/ELF parsing (against the downloaded firmware when given),
// QMI TLV encode/decode and the wlfw messages, QRTR/GLINK framing, servreg + rmtfs, and board-2.bin selection.
// Build and run via scripts/check-fajita-wifi-host.py. Pure logic, no hardware.
#include "../WifiMdt.h"
#include "../WifiQmi.h"
#include "../WifiQrtr.h"
#include "../WifiBoard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = n; return b;
}

static void test_qmi_tlv(void)
{
    uint8_t buf[256];
    wqmi_writer w;
    wqmi_begin(&w, buf, sizeof buf, WQMI_REQUEST, 0x1234, 0x0020);
    wqmi_u8(&w, 0x10, 1);
    wqmi_u32(&w, 0x15, 0x4b4e454c);
    wqmi_u64(&w, 0x20, 0x1122334455667788ull);
    long n = wqmi_end(&w);
    CHECK(n > 0);
    wqmi_hdr h;
    CHECK(wqmi_parse_hdr(buf, n, &h) == 0);
    CHECK(h.type == WQMI_REQUEST && h.txn == 0x1234 && h.msg_id == 0x0020);
    uint16_t vlen = 0;
    const uint8_t *v = wqmi_find(buf, n, 0x15, &vlen);
    CHECK(v && vlen == 4);
    CHECK(v && (uint32_t)(v[0] | v[1]<<8 | v[2]<<16 | (uint32_t)v[3]<<24) == 0x4b4e454c);
    v = wqmi_find(buf, n, 0x20, &vlen);
    CHECK(v && vlen == 8 && v[0] == 0x88 && v[7] == 0x11);
    CHECK(wqmi_find(buf, n, 0x99, &vlen) == NULL);
    // round-trip through the body-length field
    CHECK(h.len + WQMI_HDR_BYTES == (uint16_t)n);
}

static void test_wlfw_messages(void)
{
    uint8_t buf[8192];
    long n = wlfw_ind_register_req(buf, sizeof buf, 1);
    CHECK(n > 0);
    wqmi_hdr h; CHECK(wqmi_parse_hdr(buf, n, &h) == 0 && h.msg_id == WLFW_IND_REGISTER);

    n = wlfw_msa_info_req(buf, sizeof buf, 2, 0x8df00000ull, 0x100000);
    CHECK(n > 0);
    uint16_t vl; const uint8_t *v = wqmi_find(buf, n, 0x01, &vl);
    CHECK(v && vl == 8);

    // craft an msa_info response: result TLV + region array
    wqmi_writer w; wqmi_begin(&w, buf, sizeof buf, WQMI_RESPONSE, 2, WLFW_MSA_INFO);
    wqmi_result(&w, WQMI_RESULT_SUCCESS, WQMI_ERR_NONE);
    uint8_t region[1 + 2*13];
    region[0] = 2;
    memset(region + 1, 0, 2*13);
    uint64_t a = 0x8df00000ull; memcpy(region + 1, &a, 8);
    uint32_t s = 0x80000; memcpy(region + 1 + 8, &s, 4); region[1 + 12] = 1;
    uint64_t a2 = 0x8df80000ull; memcpy(region + 1 + 13, &a2, 8);
    uint32_t s2 = 0x80000; memcpy(region + 1 + 13 + 8, &s2, 4); region[1 + 13 + 12] = 0;
    wqmi_tlv(&w, 0x03, region, sizeof region);
    n = wqmi_end(&w);
    wlfw_msa_info_resp resp;
    CHECK(wlfw_parse_msa_info_resp(buf, n, &resp) == 0);
    CHECK(resp.result == WQMI_RESULT_SUCCESS && resp.count == 2);
    CHECK(resp.region[0].addr == 0x8df00000ull && resp.region[0].secure == 1);
    CHECK(resp.region[1].addr == 0x8df80000ull && resp.region[1].secure == 0);

    // wlan_cfg with a couple CE entries
    wlfw_ce_tgt tgt[2] = {{0,3,32,2048,0},{1,2,32,2048,0}};
    wlfw_ce_svc svc[1] = {{0x300,2,4}};
    wlfw_shadow sh[1] = {{0, 0x100}};
    n = wlfw_wlan_cfg_req(buf, sizeof buf, 3, tgt, 2, svc, 1, sh, 1);
    CHECK(n > 0);
    CHECK(wqmi_parse_hdr(buf, n, &h) == 0 && h.msg_id == WLFW_WLAN_CFG);
    v = wqmi_find(buf, n, 0x11, &vl);
    CHECK(v && v[0] == 2 && vl == 1 + 2*20);

    // bdf download segment
    uint8_t board[64]; for (int i = 0; i < 64; ++i) board[i] = (uint8_t)i;
    n = wlfw_bdf_req(buf, sizeof buf, 4, 64, 0, board, 64, 1);
    CHECK(n > 0 && wqmi_parse_hdr(buf, n, &h) == 0 && h.msg_id == WLFW_BDF_DOWNLOAD);
    v = wqmi_find(buf, n, 0x13, &vl);
    CHECK(v && vl == 2 + 64 && v[0] == 64 && v[1] == 0 && v[2] == 0 && v[2 + 1] == 1);
}

static void test_qrtr_glink(void)
{
    uint8_t buf[256];
    uint8_t ctrl[20];
    qrtr_ctrl_build(ctrl, QRTR_TYPE_NEW_SERVER, WQMI_SVC_WLFW, WQMI_INSTANCE(1, 0), 1, 5);
    long n = qrtr_build(buf, sizeof buf, QRTR_TYPE_DATA, 1, 10, 2, WQMI_SVC_WLFW, 0, ctrl, 20);
    CHECK(n == 52);
    qrtr_hdr_v1 h; const uint8_t *pl; size_t pll;
    CHECK(qrtr_parse(buf, n, &h, &pl, &pll) == 0);
    CHECK(h.type == QRTR_TYPE_DATA && h.src_node == 1 && h.src_port == 10 && h.dst_node == 2 && pll == 20);
    qrtr_ctrl_pkt cp;
    CHECK(qrtr_ctrl_parse(pl, pll, &cp) == 0);
    CHECK(cp.cmd == QRTR_TYPE_NEW_SERVER && cp.service == WQMI_SVC_WLFW && cp.port == 5);

    CHECK(qrtr_build(buf, sizeof buf, QRTR_TYPE_DATA, 1, 10, 2, 3, 0, ctrl, 3) == -1);   // unaligned payload

    uint8_t g[64];
    long gl = glink_open(g, sizeof g, 5, "IPCRTR");
    CHECK(gl == 16);   // 8 hdr + 7 name -> align 16
    glink_msg_hdr gh;
    CHECK(glink_parse_hdr(g, gl, &gh) == 0 && gh.cmd == GLINK_CMD_OPEN && gh.param1 == 5 && gh.param2 == 7);
    CHECK(memcmp(g + 8, "IPCRTR", 7) == 0);

    uint8_t th[16];
    glink_tx_data_hdr(th, 0, 5, 7, 20, 0);
    CHECK(glink_parse_hdr(th, 16, &gh) == 0 && gh.cmd == GLINK_CMD_TX_DATA && gh.param1 == 5);
}

static void test_servreg_rmtfs(void)
{
    uint8_t req[64], resp[512];
    // build a GET_DOMAIN_LIST request for "wlan/fw"
    wqmi_writer w; wqmi_begin(&w, req, sizeof req, WQMI_REQUEST, 7, SERVREG_GET_DOMAIN_LIST);
    const char *name = "wlan/fw";
    uint8_t sv[1 + 7]; sv[0] = 7; memcpy(sv + 1, name, 7);
    wqmi_tlv(&w, 0x01, sv, 1 + 7);   // string: u8 len + chars (level-1 string uses TLV length only; our server ignores body)
    long rn = wqmi_end(&w);
    wqmi_domain doms[4];
    unsigned nd = servreg_sdm845_lookup("wlan/fw", doms, 4);
    CHECK(nd == 1 && !strcmp(doms[0].name, "msm/modem/wlan_pd") && doms[0].instance == 180);
    long sn = servreg_answer(req, rn, resp, sizeof resp, doms, nd);
    CHECK(sn > 0);
    wqmi_hdr h; CHECK(wqmi_parse_hdr(resp, sn, &h) == 0 && h.type == WQMI_RESPONSE && h.msg_id == SERVREG_GET_DOMAIN_LIST);
    uint16_t result, error; CHECK(wqmi_get_result(resp, sn, &result, &error) == 0 && result == WQMI_RESULT_SUCCESS);
    uint16_t vl; const uint8_t *v = wqmi_find(resp, sn, 0x12, &vl);
    CHECK(v && v[0] == 1);   // one domain

    unsigned servreg_n = servreg_sdm845_lookup("tms/servreg", doms, 4);
    CHECK(servreg_n == 2);   // root_pd and wlan_pd both provide it

    // rmtfs OPEN
    wqmi_writer rw; wqmi_begin(&rw, req, sizeof req, WQMI_REQUEST, 9, RMTFS_OPEN);
    const char *p = "/boot/modem_fs1";
    uint8_t ps[1 + 15]; ps[0] = 15; memcpy(ps + 1, p, 15);
    wqmi_tlv(&rw, 0x01, p, 15);
    rn = wqmi_end(&rw);
    rmtfs_req rr;
    CHECK(rmtfs_parse_req(req, rn, &rr) == 0 && rr.msg_id == RMTFS_OPEN && !strcmp(rr.path, "/boot/modem_fs1"));
    long on = rmtfs_resp(resp, sizeof resp, &rr, WQMI_RESULT_SUCCESS, WQMI_ERR_NONE, 1, 0);
    CHECK(on > 0);
    v = wqmi_find(resp, on, 0x10, &vl); CHECK(v && vl == 4 && v[0] == 1);
}

static void test_mbn(const char *dir)
{
    char path[512];
    const char *files[] = {"mba.mbn", "modem.mbn", "wlanmdsp.mbn"};
    for (int i = 0; i < 3; ++i) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        size_t len; uint8_t *f = slurp(path, &len);
        if (!f) { printf("  (skip %s: not found)\n", files[i]); continue; }
        wmdt_image img;
        int rc = wmdt_parse(f, len, &img);
        CHECK(rc == WMDT_OK);
        if (rc == WMDT_OK) {
            long ms = wmdt_metadata_size(&img);
            uint8_t *meta = malloc(ms);
            CHECK(wmdt_metadata(f, len, &img, meta, ms) == ms);
            // every loadable segment fits a region spanning min..max
            uint64_t region = img.min_addr, bytes = (uint64_t)img.max_addr - img.min_addr;
            int loads = 0;
            for (unsigned k = 0; k < img.phnum; ++k) {
                if (!wmdt_segment_loads(&img.ph[k])) continue;
                uint64_t off;
                CHECK(wmdt_segment_offset(&img, k, region, bytes, &off) == WMDT_OK);
                ++loads;
            }
            printf("  %s: %u phdrs, %d loadable, hash=%d, entry=%#x, span %#x..%#x, meta %ld B%s\n",
                   files[i], img.phnum, loads, img.hash_index, img.entry, img.min_addr, img.max_addr, ms,
                   img.relocatable ? " (relocatable)" : "");
            free(meta);
        }
        free(f);
    }
}

static void test_board(const char *path)
{
    size_t len; uint8_t *f = slurp(path, &len);
    if (!f) { printf("  (skip board-2.bin: not found)\n"); return; }
    char names[3][64];
    unsigned nn = wboard_candidate_names(names, 3, 0x102, 0xff);
    CHECK(nn == 1 && !strcmp(names[0], "bus=snoc,qmi-board-id=102"));
    const uint8_t *data; uint32_t dlen;
    int m = wboard_select(f, len, names, nn, &data, &dlen);
    CHECK(m == 0);
    if (m >= 0) printf("  board-2.bin: matched '%s', %u bytes of board data\n", names[m], dlen);
    // a board id that is not present
    nn = wboard_candidate_names(names, 3, 0xdead, 0xff);
    CHECK(wboard_select(f, len, names, nn, &data, &dlen) == -1);
    free(f);
}

int main(int argc, char **argv)
{
    test_qmi_tlv();
    test_wlfw_messages();
    test_qrtr_glink();
    test_servreg_rmtfs();
    const char *fwdir = argc > 1 ? argv[1] : NULL;
    const char *board = argc > 2 ? argv[2] : NULL;
    if (fwdir) { printf("firmware (%s):\n", fwdir); test_mbn(fwdir); }
    if (board) { printf("board data:\n"); test_board(board); }
    if (fails) { printf("FAIL: %d checks failed\n", fails); return 1; }
    printf("PASS: fajita Wi-Fi host tests (protocol + parsing)%s\n", fwdir ? " + firmware" : " (no firmware dir given)");
    return 0;
}
