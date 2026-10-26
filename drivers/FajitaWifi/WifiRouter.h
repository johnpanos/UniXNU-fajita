// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AP's QRTR node on one link (the modem's IPCRTR GLINK channel): packet routing after net/qrtr/af_qrtr.c and the
// name service after net/qrtr/ns.c (Linux v6.16). Local ports with receive callbacks, our servers announced on HELLO,
// the remote's servers tracked from NEW_SERVER/DEL_SERVER/BYE, DEL_CLIENT forwarded to the services, confirm_rx /
// RESUME_TX flow control both ways, and a TX queue for when the link is full. Freestanding, single-threaded (the caller
// serialises). Host-tested in tests/t_qrtr.c.
#ifndef FAJITA_WIFI_ROUTER_H
#define FAJITA_WIFI_ROUTER_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WQR_LOCAL_NID 1u            // qrtr_local_nid
#define WQR_NID_AUTO 0xffffffffu
#define WQR_PORT_CTRL 0xfffffffeu
#define WQR_MAX_PORTS 16
#define WQR_MAX_LOCAL_SRV 8
// The phone's modem announces 32+ services (33 with wlfw): the old 32-entry table silently dropped wlfw's NEW_SERVER.
#define WQR_MAX_REMOTE_SRV 128
#define WQR_MAX_FLOWS 16
#define WQR_TXQ 64
#define WQR_FLOW_HIGH 10u
#define WQR_FLOW_LOW 5u
#define WQR_MAX_PKT 0x10000u

enum {WQR_OK = 0, WQR_ERR_AGAIN = -11, WQR_ERR_INVAL = -22, WQR_ERR_NOMEM = -12, WQR_ERR_NOENT = -2, WQR_ERR_BUSY = -16};

struct wqrtr;
// A DATA packet for a local port.
typedef void (*wqr_rx_fn)(struct wqrtr *r, uint32_t port, uint32_t src_node, uint32_t src_port, const uint8_t *data,
                          size_t len, void *ctx);
// Name-service events: a remote server came (added=1) or went (0); a remote client went (DEL_CLIENT, service=0).
typedef void (*wqr_event_fn)(struct wqrtr *r, int what, uint32_t service, uint32_t instance, uint32_t node, uint32_t port,
                             void *ctx);
enum {WQR_EV_HELLO = 1, WQR_EV_NEW_SERVER = 2, WQR_EV_DEL_SERVER = 3, WQR_EV_DEL_CLIENT = 4, WQR_EV_BYE = 5};

typedef struct {uint32_t service, instance, node, port; int used;} wqr_server;

typedef struct wqrtr {
    // The link: transmit one complete QRTR packet; WQR_ERR_AGAIN (or any negative) keeps it queued.
    int (*xmit)(void *ctx, const uint8_t *pkt, size_t len);
    void *xmit_ctx;
    void *(*alloc)(size_t size, void *ctx);
    void (*free)(void *p, size_t size, void *ctx);
    void *alloc_ctx;
    wqr_event_fn event;
    void *event_ctx;
    uint32_t local_nid, remote_nid;
    int hello_seen;
    int hello_once;               // the simulated modem: a HELLO after ours is the answer, not echoed (Linux's ns answers every one)
    int hello_sent;
    struct {uint32_t port; wqr_rx_fn rx; void *ctx; int used;} ports[WQR_MAX_PORTS];
    uint32_t next_port;
    wqr_server local[WQR_MAX_LOCAL_SRV];
    wqr_server remote[WQR_MAX_REMOTE_SRV];
    struct {uint32_t node, port, pending; int used;} flows[WQR_MAX_FLOWS];
    struct {uint8_t *p; size_t len;} txq[WQR_TXQ];
    unsigned txq_head, txq_count;
    uint32_t rx_pkts, tx_pkts, dropped, bad;
    uint32_t flows_full, txq_full; // sends without a flow slot (no flow control) / packets refused by a full TX queue
    uint32_t remote_full;         // NEW_SERVERs that found the remote table full (the event is still delivered)
    struct {uint32_t src_node, src_port, dst_port, size; uint8_t head[12];} drop_log[8];   // the first packets to unbound ports
    unsigned ndrop_log;
} wqrtr;

void wqrtr_init(wqrtr *r, int (*xmit)(void *, const uint8_t *, size_t), void *xmit_ctx,
                void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *alloc_ctx);
// One received packet from the link (v1 or v2 header).
int wqrtr_input(wqrtr *r, const uint8_t *pkt, size_t len);
// Retry queued packets (after the link drained). Returns how many are still queued.
unsigned wqrtr_flush(wqrtr *r);
// Bind a local port (0 = pick an ephemeral one). Returns the port number, or 0 when full.
uint32_t wqrtr_bind(wqrtr *r, uint32_t port, wqr_rx_fn rx, void *ctx);
// Close a port: its servers are withdrawn (DEL_SERVER) and DEL_CLIENT is broadcast, as qrtr_port_remove does.
void wqrtr_unbind(wqrtr *r, uint32_t port);
// Publish a server on a bound port (qrtr_publish: instance word = instance << 8 | version); announced now if the link
// said HELLO, otherwise on HELLO.
int wqrtr_publish(wqrtr *r, uint32_t port, uint32_t service, uint32_t version, uint32_t instance);
// Send DATA from a local port. WQR_ERR_BUSY when the destination's flow window is full (wait for RESUME_TX).
int wqrtr_send(wqrtr *r, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data, size_t len);
// A remote server for `service` (instance word 0 = any). NULL if none announced.
const wqr_server *wqrtr_lookup(const wqrtr *r, uint32_t service, uint32_t instance);
// Say HELLO first (Linux waits for the remote's; offered for a link that expects ours).
int wqrtr_hello(wqrtr *r);
// Drop queued packets (link reset).
void wqrtr_reset(wqrtr *r);

#ifdef __cplusplus
}
#endif
#endif
