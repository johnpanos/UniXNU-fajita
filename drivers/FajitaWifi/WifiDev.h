// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WCN3990 device above wlfw FW_READY (layer 6/7 glue): the bring-up order of ath10k_core_start / ath10k_snoc on the
// freestanding layers, after the QMI wlan_cfg + wlan_mode (WifiWlfw) have enabled the target:
//   CE init (rings, rri) -> post rx (hif_start) -> HTC READY -> connect HTT_DATA_MSG -> connect WMI_CONTROL -> HTC start
//   -> WMI SERVICE_READY (host memory chunks) -> WMI INIT -> READY (MAC) -> HTT setup (version, frag bank, rx ring, aggr)
//   -> regdomain + scan channel list -> vdev create (station) -> RUNNING: scans, connect, data.
// Owns wce, whtc, wwmi, wmlme and whtt; all device memory comes from one DMA arena (wdma), which must be uncached for the
// CPU and visible to the target at arena_iova (the SMMU / bypass is the caller's). Poll-driven and single-threaded:
// wdev_poll does the CE work and the timeouts. Host-tested against the CE/HTC target model (tests/t_dev.c).
#ifndef FAJITA_WIFI_DEV_H
#define FAJITA_WIFI_DEV_H
#include <stddef.h>
#include <stdint.h>
#include "WifiCe.h"
#include "WifiHtc.h"
#include "WifiWmi.h"
#include "WifiHtt.h"
#include "WifiMlme.h"
#ifdef __cplusplus
extern "C" {
#endif

#define WDEV_STEP_TIMEOUT_MS 5000u    // each bring-up wait (ath10k: 1..3 s per step)
#define WDEV_DST_LIMIT 64u            // destination ring cap (rx buffers per pipe) to keep the arena small
#define WDEV_MAX_TX 128u              // HTT pending tx ids
#define WDEV_RX_RING 256u             // HTT rx ring (127 buffers posted)

enum {
    WDEV_OFF = 0, WDEV_HTC_WAIT, WDEV_CONNECT_HTT, WDEV_CONNECT_WMI, WDEV_SVC_READY_WAIT, WDEV_READY_WAIT, WDEV_HTT_WAIT,
    WDEV_RUNNING, WDEV_FAILED
};

typedef struct {
    void *ctx;
    uint32_t (*now_ms)(void *ctx);
    void (*random)(void *ctx, uint8_t *out, uint32_t n);
    // a received data frame (Ethernet II) that is not EAPOL; valid during the call
    void (*rx)(void *ctx, const uint8_t *eth, uint32_t len);
    // the MLME state changed (WMLME_*), e.g. link up/down for the network interface
    void (*link)(void *ctx, unsigned mlme_state, unsigned reason);
    // a bring-up step for the kernel log (the state entered, or a failure with its rc); may be NULL
    void (*log)(void *ctx, const char *step, int rc, uint32_t val);
    // ordinary (CPU-only) memory: CE transfer contexts, HTC's queued copies
    void *(*alloc)(size_t size, void *ctx);
    void (*free)(void *p, size_t size, void *ctx);
} wdev_ops;

typedef struct wdev {
    wdev_ops ops;
    wce_io io;
    wdma dma;
    wce ce;
    whtc htc;
    wwmi wmi;
    whtt htt;
    wmlme mlme;
    unsigned state, failed_in;
    int err;
    uint32_t step_at;
    unsigned htt_eid, wmi_eid;
    int mlme_up;
    uint8_t mac[6];
    // counters
    uint32_t polls, ce_work, wmi_send_fail, htt_tx_fail, eapol_drop, rx_eth;
    uint32_t steps_ms[WDEV_FAILED];   // when each state was entered (relative to wdev_start)
    uint32_t t0;
} wdev;

// Set up the CE pipes on the arena and post the rx buffers; the bring-up then runs from wdev_poll.
int wdev_start(wdev *d, const wdev_ops *ops, const wce_io *io, void *arena, uint64_t arena_iova, uint32_t arena_size);
void wdev_poll(wdev *d);
// After RUNNING: an active scan of every channel (results in d->mlme.bss). target: the SSID the table must keep (or NULL).
int wdev_scan(wdev *d, const char *target);
int wdev_connect(wdev *d, const char *ssid, const char *pass);
// An Ethernet frame to the AP (after CONNECTED: protected).
int wdev_tx(wdev *d, const uint8_t *eth, uint32_t len);
const char *wdev_state_name(unsigned s);

#ifdef __cplusplus
}
#endif
#endif
