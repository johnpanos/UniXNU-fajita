// SPDX-License-Identifier: BSD-2-Clause-Patent
// The wlfw QMI client (service 0x45 v1 on the modem's wlan PD), after drivers/net/wireless/ath/ath10k/qmi.c (Linux v6.16):
// on server arrival ind_register, host_cap (SDM845 8-bit quirk), msa_info, [20 ms], the MSA permission hand-over (a
// callback: memory assignment is the modem-boot side's), msa_ready, cap; on MSA_READY_IND the board data download
// (6144-byte segments; a failed CRC on the last segment is tolerated as ath10k does) and cal_report; on FW_READY_IND the
// firmware is ready and wwlfw_enable sends wlan_cfg (the snoc CE tables) + wlan_mode. Non-blocking: requests go out over
// QRTR and responses advance the state; wwlfw_poll drives the delays and the 30 s timeouts. Host-tested against an
// encoded-message fixture server (tests/t_wlfw.c).
#ifndef FAJITA_WIFI_WLFW_H
#define FAJITA_WIFI_WLFW_H
#include <stddef.h>
#include <stdint.h>
#include "WifiRouter.h"
#include "WifiQmi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WWLFW_SERVICE 0x45u
#define WWLFW_INSTANCE 1u                 // version 1, instance 0 (qmi_add_lookup(WLFW_SERVICE_ID_V01, 1, 0))
#define WWLFW_TIMEOUT_MS 30000u           // ATH10K_QMI_TIMEOUT
#define WWLFW_MSA_DELAY_MS 20u            // the SDM845 XPU hack before the permission update

enum wwlfw_state {
    WWLFW_IDLE = 0, WWLFW_IND_REGISTER, WWLFW_HOST_CAP, WWLFW_MSA_INFO, WWLFW_MSA_DELAY, WWLFW_MSA_READY, WWLFW_CAP,
    WWLFW_WAIT_MSA_IND, WWLFW_BDF, WWLFW_CAL_REPORT, WWLFW_WAIT_FW_READY, WWLFW_FW_READY, WWLFW_WLAN_CFG,
    WWLFW_WLAN_MODE, WWLFW_ENABLED, WWLFW_FAILED
};

typedef struct {
    // Hand the MSA regions to the modem (ath10k_qmi_setup_msa_permissions: HLOS -> MSS_MSA + WLAN for each region).
    // 0 on success. NULL: skipped (logged by the caller) - only valid if the boot side already assigned them.
    int (*msa_assign)(void *ctx, const wlfw_mem_region *regions, unsigned count);
    // The board data for this board id (ath10k: "bus=snoc,qmi-board-id=%x"; on the phone image/bdwlan.%03x).
    // Returns 0 and points *data/*len at it (valid until the download ends).
    int (*board)(void *ctx, uint32_t board_id, uint32_t chip_id, const uint8_t **data, uint32_t *len);
    void *ctx;
    uint64_t msa_phys;                    // the WLAN MSA carve-out (wlan_msa 0x8df00000) and its size
    uint32_t msa_size;
    int host_cap_8bit;                    // qcom,snoc-host-cap-8bit-quirk (fajita: 1)
} wwlfw_ops;

typedef struct {
    wqrtr *r;
    uint32_t port, node, sport;           // our client port; the server
    wwlfw_ops ops;
    enum wwlfw_state state, failed_in;
    uint16_t txn, wait_txn, wait_msg;
    uint64_t deadline, delay_until, now;
    int msa_ind, fw_ready_ind, fw_ready;  // indications seen (an indication may arrive before its turn)
    wlfw_msa_info_resp msa;
    wlfw_cap_resp cap;
    const uint8_t *bd;
    uint32_t bd_len, bd_off, bd_seg;
    uint16_t last_result, last_error;
    int msa_assigned;
    uint32_t enable_mode;
    uint32_t requests, responses, indications;
    uint8_t buf[WLFW_MAX_DATA + 256];
} wwlfw;

int wwlfw_init(wwlfw *c, wqrtr *r, const wwlfw_ops *ops);
// The wlfw server appeared (QRTR NEW_SERVER for 0x45): start the handshake.
int wwlfw_server(wwlfw *c, uint32_t node, uint32_t port, uint64_t now_ms);
// The server went away (DEL_SERVER/BYE): back to idle.
void wwlfw_server_gone(wwlfw *c);
// Timers (the MSA delay, timeouts). Call often with the current time.
void wwlfw_poll(wwlfw *c, uint64_t now_ms);
// After FW_READY: send WLAN_CFG (the snoc CE tables) then WLAN_MODE(mode). 0 when sent.
int wwlfw_enable(wwlfw *c, uint32_t mode);
const char *wwlfw_state_name(enum wwlfw_state s);

#ifdef __cplusplus
}
#endif
#endif
