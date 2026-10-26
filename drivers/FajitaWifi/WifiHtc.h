// SPDX-License-Identifier: BSD-2-Clause-Patent
// HTC over the WCN3990 copy engines (ath10k htc.c/htc.h, Linux v6.16), non-blocking: the READY message, service
// connects (CONNECT_SERVICE / _RESP), SETUP_COMPLETE_EX, the 8-byte HTC header, rx trailers (credit reports) and tx
// credits. WCN3990: use_fw_tx_credits = false, so HTC grants one credit in total, to the WMI control endpoint (the only
// flow-controlled one); WMI commands queue here until the target returns the credit. CE5 carries HTT without HTC
// processing (ath10k_snoc_htt_rx_cb strips the header and calls the HTT handler): whtc routes it to htt_direct.
#ifndef FAJITA_WIFI_HTC_H
#define FAJITA_WIFI_HTC_H
#include <stddef.h>
#include <stdint.h>
#include "WifiCe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WHTC_HDR_LEN 8u
#define WHTC_EP_COUNT 8u
#define WHTC_MAX_CTRL 256u          // ATH10K_HTC_MAX_CTRL_MSG_LEN
#define WHTC_MAX_LEN 4096u          // ATH10K_HTC_MAX_LEN
#define WHTC_QUEUE 32u              // queued flow-controlled messages
#define WHTC_HTT_PIPE 5u            // target->host HTT without HTC (CE5)
// message ids
#define WHTC_MSG_READY 1u
#define WHTC_MSG_CONNECT 2u
#define WHTC_MSG_CONNECT_RESP 3u
#define WHTC_MSG_SETUP_COMPLETE 4u
#define WHTC_MSG_SETUP_COMPLETE_EX 5u
#define WHTC_MSG_SUSPEND_COMPLETE 6u
// header flags
#define WHTC_FLAG_NEED_CREDIT_UPDATE 0x01u
#define WHTC_FLAG_TRAILER_PRESENT 0x02u
#define WHTC_CONN_DISABLE_CREDIT_FLOW 0x0008u
#define WHTC_CONN_RECV_ALLOC_LSB 8u
#define WHTC_RECORD_CREDITS 1u

enum {WHTC_OK = 0, WHTC_ERR_AGAIN = -11, WHTC_ERR_PROTO = -71, WHTC_ERR_INVAL = -22, WHTC_ERR_NOMEM = -12};
enum {WHTC_WAIT_READY = 0, WHTC_READY, WHTC_FAILED};

struct whtc;
typedef void (*whtc_rx_fn)(struct whtc *h, unsigned eid, uint8_t *data, uint32_t len, void *arg);

typedef struct {
    uint16_t service;               // 0 = unused
    uint8_t ul, dl;
    uint16_t max_msg;
    int flow;                       // tx credit flow control
    int credits;
    uint16_t credit_size;
    uint8_t seq;
    whtc_rx_fn rx;
    void *arg;
    uint32_t rx_count, tx_count;
} whtc_ep;

typedef struct {uint8_t eid; uint32_t len; uint8_t *data;} whtc_qent;

typedef struct whtc {
    wce *ce;
    int state;
    uint16_t total_credits, credit_size, fw_credit_count;
    uint8_t max_endpoints;
    whtc_ep ep[WHTC_EP_COUNT];
    // the outstanding connect (one at a time, as ath10k waits for each)
    uint16_t conn_service;
    int conn_pending, conn_status, conn_eid;
    whtc_rx_fn conn_rx;
    void *conn_arg;
    // flow-controlled tx queue
    whtc_qent q[WHTC_QUEUE];
    unsigned qhead, qcount;
    void (*htt_direct)(struct whtc *h, uint8_t *data, uint32_t len, void *arg);   // CE5, header stripped
    void *htt_arg;
    void *(*alloc)(size_t size, void *ctx);
    void (*free)(void *p, size_t size, void *ctx);
    void *alloc_ctx;
    uint32_t rx_bad, unsolicited, credit_reports, tx_queued;
} whtc;

// Takes over ce->rx/ce->tx_done. EP0 (RSVD_CTRL, ul/dl from the service map) is set up as ath10k does.
int whtc_init(whtc *h, wce *ce, void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *ctx);
// Request a service connection; 0 = request sent. whtc_connect_result: 1 connected (*eid), 0 pending, <0 failed.
int whtc_connect(whtc *h, uint16_t service, whtc_rx_fn rx, void *arg);
int whtc_connect_result(whtc *h, uint16_t service, unsigned *eid);
// ath10k_htc_start: SETUP_COMPLETE_EX (no pktlog service on snoc: HTT_LOG_MSG has no ul pipe).
int whtc_start(whtc *h);
// Send payload on an endpoint (the header is added). Flow-controlled endpoints queue when out of credits.
int whtc_send(whtc *h, unsigned eid, const void *payload, uint32_t len);
// Drain the credit queue (called from whtc after credit reports; safe to call any time).
void whtc_kick(whtc *h);
// Build an HTC header (for the HTT TX_FRM path, which bypasses HTC).
void whtc_hdr(uint8_t *hdr, uint8_t eid, uint16_t len, uint8_t flags, uint8_t seq);

#ifdef __cplusplus
}
#endif
#endif
