// SPDX-License-Identifier: BSD-2-Clause-Patent
// wlfw QMI client, after ath10k qmi.c (Linux v6.16). See WifiWlfw.h.
#include "WifiWlfw.h"
#include "WifiSnoc.h"

const char *wwlfw_state_name(enum wwlfw_state s)
{
    static const char *const n[] = {"idle", "ind_register", "host_cap", "msa_info", "msa_delay", "msa_ready", "cap",
                                    "wait_msa_ind", "bdf", "cal_report", "wait_fw_ready", "fw_ready", "wlan_cfg",
                                    "wlan_mode", "enabled", "failed"};
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

static void fail(wwlfw *c)
{
    c->failed_in = c->state;
    c->state = WWLFW_FAILED;
}

static int send(wwlfw *c, long n, uint16_t msg, enum wwlfw_state next)
{
    if (n <= 0) {fail(c); return -1;}
    c->wait_txn = c->txn;
    c->wait_msg = msg;
    c->state = next;
    c->deadline = c->now + WWLFW_TIMEOUT_MS;
    ++c->requests;
    if (wqrtr_send(c->r, c->port, c->node, c->sport, c->buf, (size_t)n) < 0) {fail(c); return -1;}
    return 0;
}

static uint16_t next_txn(wwlfw *c) {if (!++c->txn) c->txn = 1; return c->txn;}

static int host_cap(wwlfw *c)
{
    long n;
    const uint16_t t = next_txn(c);
    if (c->ops.host_cap_8bit) {
        n = wlfw_host_cap_8bit_req(c->buf, sizeof c->buf, t);
    } else {   // wlfw_host_cap_req_msg_v01_ei: daemon_support as u32 at 0x10
        wqmi_writer w;
        wqmi_begin(&w, c->buf, sizeof c->buf, WQMI_REQUEST, t, WLFW_HOST_CAP);
        wqmi_u32(&w, 0x10, 0);
        n = wqmi_end(&w);
    }
    return send(c, n, WLFW_HOST_CAP, WWLFW_HOST_CAP);
}

static int bdf_segment(wwlfw *c)
{
    const uint32_t left = c->bd_len - c->bd_off;
    const uint16_t len = (uint16_t)(left > WLFW_MAX_DATA ? WLFW_MAX_DATA : left);
    const int end = left <= WLFW_MAX_DATA;
    const long n = wlfw_bdf_req(c->buf, sizeof c->buf, next_txn(c), c->bd_len, c->bd_seg, c->bd + c->bd_off, len, end);
    return send(c, n, WLFW_BDF_DOWNLOAD, WWLFW_BDF);
}

static void start_bdf(wwlfw *c)
{
    // ath10k_qmi_fetch_board_file: board id from the cap response (0xff when absent), chip id likewise.
    const uint32_t board = c->cap.board_valid ? c->cap.board_id : 0xff, chip = c->cap.chip_valid ? c->cap.chip_id : 0xff;
    if (!c->ops.board || c->ops.board(c->ops.ctx, board, chip, &c->bd, &c->bd_len) || !c->bd || !c->bd_len) {fail(c); return;}
    c->bd_off = 0;
    c->bd_seg = 0;
    bdf_segment(c);
}

static void after_cal(wwlfw *c)
{
    c->state = c->fw_ready_ind ? WWLFW_FW_READY : WWLFW_WAIT_FW_READY;
    if (c->state == WWLFW_FW_READY) c->fw_ready = 1;
}

static void response(wwlfw *c, const uint8_t *m, size_t len)
{
    uint16_t res = 1, err = 0;
    if (wqmi_get_result(m, len, &res, &err)) {fail(c); return;}
    c->last_result = res;
    c->last_error = err;
    switch (c->state) {
    case WWLFW_IND_REGISTER: {
        wlfw_ind_register_resp r;
        if (wlfw_parse_ind_register_resp(m, len, &r) || r.result != WQMI_RESULT_SUCCESS) {fail(c); return;}
        if (r.fw_status_valid && (r.fw_status & WLFW_FW_READY_BIT)) {c->fw_ready = 1; c->state = WWLFW_FW_READY; return;}
        host_cap(c);
        return;
    }
    case WWLFW_HOST_CAP:
        if (res != WQMI_RESULT_SUCCESS) {fail(c); return;}
        send(c, wlfw_msa_info_req(c->buf, sizeof c->buf, next_txn(c), c->ops.msa_phys, c->ops.msa_size), WLFW_MSA_INFO,
             WWLFW_MSA_INFO);
        return;
    case WWLFW_MSA_INFO: {
        if (wlfw_parse_msa_info_resp(m, len, &c->msa) || c->msa.result != WQMI_RESULT_SUCCESS) {fail(c); return;}
        // ath10k_qmi_msa_mem_info_send_sync_msg: every region inside the MSA carve-out
        const uint64_t lo = c->ops.msa_phys, hi = c->ops.msa_phys + c->ops.msa_size;
        for (unsigned i = 0; i < c->msa.count; ++i) {
            const wlfw_mem_region *g = &c->msa.region[i];
            if (g->size > c->ops.msa_size || g->addr > hi || g->addr < lo || g->addr + g->size > hi) {fail(c); return;}
        }
        c->state = WWLFW_MSA_DELAY;
        c->delay_until = c->now + WWLFW_MSA_DELAY_MS;
        return;
    }
    case WWLFW_MSA_READY:
        if (res != WQMI_RESULT_SUCCESS) {fail(c); return;}
        send(c, wlfw_empty_req(c->buf, sizeof c->buf, next_txn(c), WLFW_CAP), WLFW_CAP, WWLFW_CAP);
        return;
    case WWLFW_CAP:
        if (wlfw_parse_cap_resp(m, len, &c->cap) || c->cap.result != WQMI_RESULT_SUCCESS) {fail(c); return;}
        if (c->msa_ind) start_bdf(c);
        else c->state = WWLFW_WAIT_MSA_IND;
        return;
    case WWLFW_BDF: {
        const uint32_t left = c->bd_len - c->bd_off;
        const int end = left <= WLFW_MAX_DATA;
        // end = 1 makes the firmware CRC-check the BDF; ath10k accepts a failure there (released BDFs fail it).
        if (res != WQMI_RESULT_SUCCESS && !(end && res == 1)) {fail(c); return;}
        c->bd_off += end ? left : WLFW_MAX_DATA;
        ++c->bd_seg;
        if (!end) {bdf_segment(c); return;}
        send(c, wlfw_cal_report_req(c->buf, sizeof c->buf, next_txn(c)), WLFW_CAL_REPORT, WWLFW_CAL_REPORT);
        return;
    }
    case WWLFW_CAL_REPORT:
        if (res != WQMI_RESULT_SUCCESS) {fail(c); return;}
        after_cal(c);
        return;
    case WWLFW_WLAN_CFG:
        if (res != WQMI_RESULT_SUCCESS) {fail(c); return;}
        send(c, wlfw_wlan_mode_req(c->buf, sizeof c->buf, next_txn(c), c->enable_mode),
             WLFW_WLAN_MODE, WWLFW_WLAN_MODE);
        return;
    case WWLFW_WLAN_MODE:
        if (res != WQMI_RESULT_SUCCESS) {fail(c); return;}
        c->state = WWLFW_ENABLED;
        return;
    default:
        return;
    }
}

static void client_rx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *b, size_t len, void *ctx)
{
    (void)r; (void)port;
    wwlfw *c = (wwlfw *)ctx;
    if (node != c->node || sport != c->sport) return;
    wqmi_hdr h;
    if (wqmi_parse_hdr(b, len, &h)) return;
    if (h.type == WQMI_INDICATION) {
        ++c->indications;
        if (h.msg_id == WLFW_MSA_READY_IND) {
            c->msa_ind = 1;
            if (c->state == WWLFW_WAIT_MSA_IND) start_bdf(c);
        } else if (h.msg_id == WLFW_FW_READY_IND) {
            c->fw_ready_ind = 1;
            if (c->state == WWLFW_WAIT_FW_READY) {c->state = WWLFW_FW_READY; c->fw_ready = 1;}
        }
        return;
    }
    if (h.type != WQMI_RESPONSE || h.txn != c->wait_txn || h.msg_id != c->wait_msg) return;
    ++c->responses;
    c->wait_msg = 0xffff;
    response(c, b, len);
}

int wwlfw_init(wwlfw *c, wqrtr *r, const wwlfw_ops *ops)
{
    c->r = r;
    c->ops = *ops;
    c->state = c->failed_in = WWLFW_IDLE;
    c->txn = 0;
    c->wait_msg = 0xffff;
    c->node = c->sport = 0;
    c->msa_ind = c->fw_ready_ind = c->fw_ready = c->msa_assigned = 0;
    c->requests = c->responses = c->indications = 0;
    c->now = 0;
    c->port = wqrtr_bind(r, 0, client_rx, c);
    return c->port ? 0 : WQR_ERR_NOMEM;
}

int wwlfw_server(wwlfw *c, uint32_t node, uint32_t port, uint64_t now_ms)
{
    if (c->state != WWLFW_IDLE && c->state != WWLFW_FAILED) return WQR_ERR_BUSY;
    c->node = node;
    c->sport = port;
    c->now = now_ms;
    c->msa_ind = c->fw_ready_ind = c->fw_ready = c->msa_assigned = 0;
    c->cap.board_valid = c->cap.chip_valid = 0;
    // ath10k_qmi_event_server_arrive
    return send(c, wlfw_ind_register_req(c->buf, sizeof c->buf, next_txn(c)), WLFW_IND_REGISTER, WWLFW_IND_REGISTER);
}

void wwlfw_server_gone(wwlfw *c)
{
    c->state = WWLFW_IDLE;
    c->node = c->sport = 0;
    c->wait_msg = 0xffff;
}

void wwlfw_poll(wwlfw *c, uint64_t now_ms)
{
    c->now = now_ms;
    if (c->state == WWLFW_MSA_DELAY && now_ms >= c->delay_until) {
        // ath10k_qmi_setup_msa_permissions, then msa_ready
        if (c->ops.msa_assign) {
            if (c->ops.msa_assign(c->ops.ctx, c->msa.region, c->msa.count)) {fail(c); return;}
            c->msa_assigned = 1;
        }
        send(c, wlfw_empty_req(c->buf, sizeof c->buf, next_txn(c), WLFW_MSA_READY), WLFW_MSA_READY, WWLFW_MSA_READY);
        return;
    }
    if (c->wait_msg != 0xffff && now_ms > c->deadline) fail(c);
}

int wwlfw_enable(wwlfw *c, uint32_t mode)
{
    if (c->state != WWLFW_FW_READY) return WQR_ERR_BUSY;
    c->enable_mode = mode;
    const long n = wlfw_wlan_cfg_req(c->buf, sizeof c->buf, next_txn(c), wsnoc_target_ce_config, WSNOC_CE_COUNT,
                                     wsnoc_service_map, WSNOC_N_SERVICE_MAP, wsnoc_shadow_map, WSNOC_N_SHADOW);
    return send(c, n, WLFW_WLAN_CFG, WWLFW_WLAN_CFG);
}
