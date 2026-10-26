// SPDX-License-Identifier: BSD-2-Clause-Patent
// GLINK over SMEM: the FIFO pair (drivers/rpmsg/qcom_glink_smem.c) and the native protocol (qcom_glink_native.c,
// Linux v6.16): version/feature negotiation, channel open/close by name, receive intents (advertise, request, rx_done
// with reuse), and chunked TX_DATA. Freestanding and poll-driven: the caller runs wglink_rx() on every GLINK interrupt
// (or periodically) under its own lock. FIFO accesses are aligned volatile word/byte accesses (safe on uncached memory).
// Host-tested by a loopback of two ends over one synthetic SMEM (tests/t_glink.c).
#ifndef FAJITA_WIFI_GLINK_H
#define FAJITA_WIFI_GLINK_H
#include <stddef.h>
#include <stdint.h>
#include "WifiSmem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WGLINK_SMEM_DESC 478u
#define WGLINK_SMEM_FIFO0 479u           // AP -> remote
#define WGLINK_SMEM_FIFO1 480u           // remote -> AP (the remote allocates it)
#define WGLINK_FIFO_BYTES (16u << 10)
#define WGLINK_MAX_CH 8
#define WGLINK_MAX_INTENTS 16
#define WGLINK_NAME 32
#define WGLINK_CHUNK (8u << 10)          // SZ_8K per TX_DATA chunk
#define WGLINK_FEATURES 1u               // GLINK_FEATURE_INTENT_REUSE (qcom_glink_smem_register)

enum {WGLINK_OK = 0, WGLINK_ERR_AGAIN = -11, WGLINK_ERR_INVAL = -22, WGLINK_ERR_NOMEM = -12, WGLINK_ERR_NOENT = -2,
      WGLINK_ERR_BUSY = -16, WGLINK_ERR_PROTO = -71, WGLINK_ERR_NOTCONN = -107};

typedef struct {
    volatile uint32_t *tail, *head;   // descriptor words
    volatile uint8_t *fifo;
    uint32_t length;
} wglink_pipe;

struct wglink;
// A complete message on a channel. The data is valid only during the call.
typedef void (*wglink_rx_fn)(struct wglink *g, int ch, const uint8_t *data, size_t len, void *ctx);
// A channel event: the remote opened (1), the channel became ready (2: both opens acked), the remote closed (3),
// our close was acked (4).
typedef void (*wglink_event_fn)(struct wglink *g, int ch, int event, void *ctx);
enum {WGLINK_EV_REMOTE_OPEN = 1, WGLINK_EV_READY = 2, WGLINK_EV_REMOTE_CLOSE = 3, WGLINK_EV_CLOSED = 4};

typedef struct {
    uint32_t id, size, offset;
    uint8_t *data;
    int reuse, used;                 // local: reuse = advertised as reusable; used = slot taken
} wglink_lintent;

typedef struct {
    uint32_t id, size;
    int in_use, used;
} wglink_rintent;

typedef struct {
    char name[WGLINK_NAME];
    int used;
    uint16_t lcid, rcid;             // 0 = none
    int open_sent, open_acked, ack_sent, ready, closing;
    int want;                        // opened locally (wglink_open)
    wglink_rx_fn rx;
    void *rx_ctx;
    unsigned intent_size, intent_count;   // what to advertise when ready
    wglink_lintent li[WGLINK_MAX_INTENTS];
    wglink_rintent ri[WGLINK_MAX_INTENTS];
    int filling;                     // index of the local intent being filled by a multi-chunk message, -1 none
    int intent_req_pending;          // RX_INTENT_REQ sent, no ack yet
    int intent_req_result;           // last ack: 1 granted, 0 refused, -1 none
    uint32_t next_liid;
} wglink_channel;

typedef struct wglink {
    wglink_pipe tx, rx;
    // rx fifo lookup when the remote allocates it after us (SMEM fifo 1); NULL for a fixed pipe
    wsmem *smem;
    unsigned remote_pid, rx_item;
    void (*kick)(void *ctx);
    void *kick_ctx;
    void *(*alloc)(size_t size, void *ctx);
    void (*free)(void *p, size_t size, void *ctx);
    void *alloc_ctx;
    wglink_event_fn event;
    void *event_ctx;
    uint32_t features;
    int version_acked;               // the remote acked our version with the same features
    int read_notify_sent;
    uint16_t next_lcid;
    wglink_channel ch[WGLINK_MAX_CH];
    // counters for logging
    uint32_t rx_cmds, tx_cmds, rx_msgs, tx_msgs, errors, last_error_cmd;
} wglink;

// The AP end over SMEM, after qcom_glink_smem_register: alloc/get the 32-byte descriptor (478) and our 16 KiB TX fifo
// (479); the RX fifo (480) is looked up lazily. Then sends VERSION.
int wglink_smem_init(wglink *g, wsmem *smem, unsigned remote_pid, void (*kick)(void *), void *kick_ctx,
                     void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *alloc_ctx);
// A generic end over two given pipes (the loopback's simulated remote). Sends VERSION.
int wglink_init(wglink *g, const wglink_pipe *tx, const wglink_pipe *rx, void (*kick)(void *), void *kick_ctx,
                void *(*alloc)(size_t, void *), void (*free_fn)(void *, size_t, void *), void *alloc_ctx);
// Open the named channel (qcom_glink_create_ept: create_local, or create_remote if the remote opened it first). The
// channel is usable once ready (event WGLINK_EV_READY), when `intent_count` intents of `intent_size` are advertised
// (Linux default 5 x 1 KiB). Returns the channel index.
int wglink_open(wglink *g, const char *name, wglink_rx_fn rx, void *rx_ctx, unsigned intent_size, unsigned intent_count);
int wglink_close(wglink *g, int ch);
int wglink_ready(const wglink *g, int ch);
int wglink_find(const wglink *g, const char *name);
// Send one message (chunked by 8 KiB). Needs a free remote intent of at least len bytes: without one it sends an
// RX_INTENT_REQ and returns WGLINK_ERR_AGAIN (retry after wglink_rx). AGAIN too when the TX fifo lacks room.
int wglink_send(wglink *g, int ch, const void *data, size_t len);
// Process everything in the RX fifo. Returns the number of commands handled, or a negative error on a protocol fault.
int wglink_rx(wglink *g);
size_t wglink_tx_avail(wglink *g);

#ifdef __cplusplus
}
#endif
#endif
