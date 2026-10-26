// SPDX-License-Identifier: BSD-2-Clause-Patent
// WMI-TLV for the WCN3990 (layer 5b/5c), after ath10k wmi-tlv.c / wmi.c (Linux v6.16): the command encoders and event decoders
// of the station path, freestanding and non-blocking. Commands go out through ops->send (the HTC WMI endpoint); events come in
// through wwmi_rx (the HTC rx callback of the WMI endpoint, payload after the HTC header). Constants are the generated
// WifiAth10kIds.h (checked against Linux's structs by tests/t_wmi.c with ath10k_layout.h).
//
// Boot order driven by the glue (ath10k_core_start): wait for SERVICE_READY (wwmi_rx parses it, allocates the firmware's
// host memory chunks through ops->dma_alloc), wwmi_init_cmd, wait for READY (mac address), then pdev params, the channel list,
// a vdev; later scan, vdev start, peer create, management frames, peer assoc, vdev up, keys.
#ifndef FAJITA_WIFI_WMI_H
#define FAJITA_WIFI_WMI_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WWMI_MAX_CHUNKS 16u
#define WWMI_MAX_VDEVS 4u
#define WWMI_MAX_PEERS 33u
#define WWMI_SERVICE_WORDS 64u
#define WWMI_MAX_SCAN_CHANS 40u
#define WWMI_MGMT_INLINE 64u          // WMI_TLV_MGMT_TX_FRAME_MAX_LEN: the rest of the frame is read from its DMA address
#define WWMI_CMD_MAX 2560u

enum {WWMI_OK = 0, WWMI_ERR_INVAL = -22, WWMI_ERR_PROTO = -71, WWMI_ERR_NOMEM = -12, WWMI_ERR_NOSPC = -28, WWMI_ERR_OPNOTSUPP = -95};

enum {
    WWMI_EV_SERVICE_READY = 1, WWMI_EV_SERVICE_AVAILABLE, WWMI_EV_READY, WWMI_EV_SCAN, WWMI_EV_MGMT_RX, WWMI_EV_MGMT_TX_COMPL,
    WWMI_EV_VDEV_START_RESP, WWMI_EV_VDEV_STOPPED, WWMI_EV_VDEV_DELETE_RESP, WWMI_EV_PEER_DELETE_RESP, WWMI_EV_PEER_KICKOUT,
    WWMI_EV_INSTALL_KEY_COMPLETE, WWMI_EV_CHAN_INFO, WWMI_EV_OTHER
};

// One decoded event. a..d depend on the type (see wwmi_rx). data/len: the management frame of MGMT_RX.
typedef struct {
    unsigned type;
    uint32_t id;                    // the WMI event id
    uint32_t a, b, c, d;
    uint8_t mac[6];
    const uint8_t *data;
    uint32_t len;
} wwmi_event;

typedef struct {
    void *ctx;
    // Send one WMI command (4-byte command id + TLVs) to the target; 0 when sent or queued (credits), negative on failure.
    int (*send)(void *ctx, const void *buf, uint32_t len);
    // A zeroed DMA block the target can read and write (an IOVA in *iova), for the host memory chunks.
    void *(*dma_alloc)(void *ctx, uint32_t size, uint64_t *iova);
    void (*event)(void *ctx, const wwmi_event *ev);
} wwmi_ops;

typedef struct {uint32_t req_id; uint64_t iova; uint32_t len;} wwmi_chunk;

typedef struct {
    uint32_t freq, center1, center2, flags, min_power, max_power, reg_power, reg_class, antenna_max, mode;
} wwmi_chan;

typedef struct {
    uint32_t vdev_id, scan_id, scan_req_id, priority, notify_events, dwell_active, dwell_passive, min_rest, max_rest;
    uint32_t repeat_probe, probe_spacing, idle_time, max_scan_time, probe_delay, ctrl_flags, burst_duration;
    uint32_t n_channels, channels[WWMI_MAX_SCAN_CHANS];
    unsigned n_ssids; uint8_t ssid[2][32]; uint8_t ssid_len[2];
    unsigned n_bssids; uint8_t bssid[2][6];
    const uint8_t *ie; uint32_t ie_len;
} wwmi_scan_arg;

typedef struct {
    uint32_t vdev_id, bcn_intval, dtim_period, flags, bcn_tx_rate, bcn_tx_power, disable_hw_ack;
    const uint8_t *ssid; uint32_t ssid_len;
    wwmi_chan chan;
} wwmi_vdev_start_arg;

typedef struct {
    uint8_t addr[6];
    uint32_t vdev_id, reassoc, aid, flags, caps, listen_intval, ht_caps, max_mpdu, mpdu_density, rate_caps, nss, vht_caps, phymode;
    uint32_t ht_info[2];
    uint8_t legacy_rates[32]; uint32_t n_legacy;
    uint8_t ht_rates[32]; uint32_t n_ht;
    uint32_t vht_rx_max, vht_rx_mcs, vht_tx_max, vht_tx_mcs;
} wwmi_peer_assoc_arg;

typedef struct {
    uint32_t vdev_id; const uint8_t *macaddr; uint32_t key_idx, key_flags, key_cipher, key_len, txmic_len, rxmic_len; const uint8_t *key;
} wwmi_key_arg;

typedef struct wwmi {
    wwmi_ops ops;
    // service ready
    int service_ready, ready, service_avail;
    uint32_t svc_main[WWMI_SERVICE_WORDS]; uint32_t svc_main_bytes;
    uint32_t svc_ext[16]; uint32_t svc_ext_len;
    uint32_t fw_version0, fw_version1, fw_build, ht_cap, vht_cap, vht_supp_mcs, num_rf_chains, phy_capab, sys_cap, min_tx_power, max_tx_power;
    uint32_t eeprom_rd, low_2g, high_2g, low_5g, high_5g, num_mem_reqs;
    wwmi_chunk chunk[WWMI_MAX_CHUNKS]; unsigned nchunks;
    uint32_t num_msdu_desc;         // resource config: the HTT's pending tx limit
    uint8_t mac[6]; uint32_t ready_status;
    // counters
    uint32_t cmds, cmd_fail, events, bad_events, unknown_events;
    uint32_t last_unknown_id;
    uint8_t cmd[WWMI_CMD_MAX];
} wwmi;

void wwmi_init(wwmi *w, const wwmi_ops *ops);
// One event from the target: the WMI payload (command header + TLVs). Returns 0 or a negative WWMI_ERR on a malformed event.
int wwmi_rx(wwmi *w, const uint8_t *data, uint32_t len);
// ath10k's WMI_SERVICE_IS_ENABLED (4 services per word of the main bitmap, the rest in bits 4.. of its words) and the extension.
int wwmi_service(const wwmi *w, uint32_t id);

// ---- commands (each sends one WMI command; 0 or the negative error of ops->send / WWMI_ERR_*) ----
int wwmi_cmd_init(wwmi *w);
int wwmi_pdev_set_param(wwmi *w, uint32_t param, uint32_t value);
int wwmi_pdev_set_regdomain(wwmi *w, uint32_t rd, uint32_t rd2g, uint32_t rd5g, uint32_t ctl2g, uint32_t ctl5g);
int wwmi_scan_chan_list(wwmi *w, const wwmi_chan *ch, unsigned n);
void wwmi_scan_init(wwmi_scan_arg *a);   // ath10k_wmi_start_scan_init
int wwmi_start_scan(wwmi *w, const wwmi_scan_arg *a);
int wwmi_stop_scan(wwmi *w, uint32_t req_id, uint32_t scan_id, uint32_t vdev_id, int one);
int wwmi_vdev_create(wwmi *w, uint32_t vdev_id, uint32_t type, uint32_t subtype, const uint8_t mac[6]);
int wwmi_vdev_delete(wwmi *w, uint32_t vdev_id);
int wwmi_vdev_start(wwmi *w, const wwmi_vdev_start_arg *a, int restart);
int wwmi_vdev_stop(wwmi *w, uint32_t vdev_id);
int wwmi_vdev_up(wwmi *w, uint32_t vdev_id, uint32_t aid, const uint8_t bssid[6]);
int wwmi_vdev_down(wwmi *w, uint32_t vdev_id);
int wwmi_vdev_set_param(wwmi *w, uint32_t vdev_id, uint32_t param, uint32_t value);
int wwmi_vdev_install_key(wwmi *w, const wwmi_key_arg *a);
int wwmi_peer_create(wwmi *w, uint32_t vdev_id, const uint8_t addr[6], uint32_t type);
int wwmi_peer_delete(wwmi *w, uint32_t vdev_id, const uint8_t addr[6]);
int wwmi_peer_set_param(wwmi *w, uint32_t vdev_id, const uint8_t addr[6], uint32_t param, uint32_t value);
int wwmi_peer_assoc(wwmi *w, const wwmi_peer_assoc_arg *a);
int wwmi_set_psmode(wwmi *w, uint32_t vdev_id, uint32_t mode);
// frame at paddr (the whole frame, in DMA memory); the first min(len, 64) bytes also go inline.
int wwmi_mgmt_tx(wwmi *w, uint32_t vdev_id, uint32_t desc_id, const uint8_t *frame, uint32_t len, uint64_t paddr);

#ifdef __cplusplus
}
#endif
#endif
