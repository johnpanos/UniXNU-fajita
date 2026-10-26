// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 firmware model for the host tests, on the copy-engine simulator (wsim): the target side of HTC (READY,
// connect responses, SETUP_COMPLETE_EX, credit trailers), and hooks the WMI/HTT layers of the tests fill in.
#ifndef FAJITA_WIFI_WFW_H
#define FAJITA_WIFI_WFW_H
#include <stdint.h>
#include "wsim.h"

#define WFW_EID_WMI 1u
#define WFW_EID_HTT 2u
#define WFW_CTRL_PIPE 2u     // RSVD_CTRL IN
#define WFW_WMI_PIPE 2u      // WMI_CONTROL IN
#define WFW_HTT_PIPE 1u      // HTT_DATA_MSG IN

typedef struct wfw wfw;
struct wfw {
    wsim *sim;
    uint16_t credit_size, credit_count;
    int setup_complete, connects, auto_credit;
    uint16_t conn_service[8], conn_flags[8];
    uint8_t next_eid;
    uint8_t seq_seen[8];
    uint32_t seq_errors, flag_errors, ctrl_msgs, wmi_cmds, htt_msgs, credit_returns, unknown;
    // hooks: a WMI command (payload after the HTC header) and an HTT host->target message (CE4 or HTC on the HTT ep)
    void (*wmi)(wfw *f, const uint8_t *cmd, uint32_t len);
    void (*htt)(wfw *f, const uint8_t *msg, uint32_t len, unsigned ce);
    void *arg;
    uint8_t last[4096];
    uint32_t last_len;
};

void wfw_init(wfw *f, wsim *s);
int wfw_ready(wfw *f);
// target -> host HTC message on eid (pipe chosen by eid); trailer_credits > 0 appends a credit report for WMI.
int wfw_send(wfw *f, unsigned pipe, uint8_t eid, const void *payload, uint32_t len, uint8_t trailer_credits);
int wfw_credit(wfw *f, uint8_t credits);   // a zero-length HTC packet on eid 0 carrying only the credit trailer
#endif
