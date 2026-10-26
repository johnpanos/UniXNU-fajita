// SPDX-License-Identifier: BSD-2-Clause-Patent
// Station MLME for the WCN3990 (layer 5c): the 802.11 management side that mac80211 + cfg80211 + ath10k's mac.c do on Linux,
// reduced to one open-system-authenticated WPA2-PSK (CCMP) infrastructure BSS. Scan results from the management frames the
// target delivers (beacons and probe responses), BSS selection, then the ath10k connect sequence on top of WifiWmi:
//   vdev start on the AP's channel -> peer create -> Authentication (open system) -> Association (legacy rates, RSNE) ->
//   peer assoc -> power save off -> vdev up(aid, bssid) -> the 4-way handshake (WifiWpa, EAPOL over the data path) ->
//   install the pairwise and group keys -> peer authorize.
// Freestanding, non-blocking and event driven: wmlme_wmi_event takes the WifiWmi events, wmlme_eapol_rx the EAPOL frames
// from the data path, wmlme_poll the timers. Host-tested against a simulated AP and target (tests/t_mlme.c).
#ifndef FAJITA_WIFI_MLME_H
#define FAJITA_WIFI_MLME_H
#include <stddef.h>
#include <stdint.h>
#include "WifiWmi.h"
#include "WifiWpa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WMLME_MAX_BSS 32u
#define WMLME_MAX_RATES 12u
#define WMLME_RSN_MAX 64u
#define WMLME_MGMT_BUFS 4u
#define WMLME_MGMT_BYTES 512u
#define WMLME_TIMEOUT_MS 1000u        // authentication / association / key steps
#define WMLME_RETRIES 3u

enum {WMLME_OK = 0, WMLME_ERR_INVAL = -22, WMLME_ERR_NOENT = -2, WMLME_ERR_BUSY = -16, WMLME_ERR_PROTO = -71, WMLME_ERR_NOTSUP = -95};

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[32]; uint8_t ssid_len;
    uint32_t freq; int32_t rssi;            // dBm (snr - 95)
    uint16_t capab, beacon_int;
    uint8_t rates[WMLME_MAX_RATES]; uint8_t n_rates;   // supported + extended, as sent (basic flag bit kept)
    uint8_t rsn[WMLME_RSN_MAX]; uint8_t rsn_len;       // the whole RSN element (id, length, body); 0 = open
    int rsn_ok;                             // CCMP pairwise + group, PSK, no required PMF
    int ht;                                 // advertises HT (the connect still uses legacy rates)
    uint32_t seen_ms; int probe_resp;
} wmlme_bss;

enum {
    WMLME_IDLE = 0, WMLME_SCANNING, WMLME_SCAN_DONE, WMLME_VDEV_START, WMLME_AUTH, WMLME_ASSOC, WMLME_ASSOCIATED, WMLME_KEYS,
    WMLME_CONNECTED, WMLME_FAILED
};
enum {WMLME_REASON_NONE = 0, WMLME_REASON_NO_BSS, WMLME_REASON_VDEV_START, WMLME_REASON_AUTH, WMLME_REASON_ASSOC, WMLME_REASON_KEYS,
      WMLME_REASON_SEND, WMLME_REASON_KICKOUT, WMLME_REASON_DEAUTH, WMLME_REASON_UNSUPPORTED};

typedef struct {
    void *ctx;
    // An EAPOL frame (starting at the EAPOL header) to the AP, over the data path as an ordinary data frame.
    int (*eapol_tx)(void *ctx, const uint8_t dst[6], const uint8_t *eapol, uint32_t len);
    void (*random)(void *ctx, uint8_t *out, uint32_t n);
    uint32_t (*now_ms)(void *ctx);
    // DMA memory the target reads management frames from (WMLME_MGMT_BYTES each, WMLME_MGMT_BUFS of them, reused round robin).
    void *(*dma_alloc)(void *ctx, uint32_t size, uint64_t *iova);
    // A state change (WMLME_*), e.g. to log it or to raise the link in the network interface.
    void (*state)(void *ctx, unsigned state, unsigned reason);
} wmlme_ops;

typedef struct wmlme {
    wmlme_ops ops;
    wwmi *wmi;
    uint8_t mac[6];
    uint32_t vdev_id;
    int vdev_created;
    unsigned state, reason;
    wmlme_bss bss[WMLME_MAX_BSS]; unsigned nbss;
    // the connection
    uint8_t ssid[32]; uint8_t ssid_len;
    uint8_t pmk[WWPA_PMK_LEN]; int have_pmk;
    wmlme_bss target;                       // the BSS being joined
    uint32_t aid, step_at, retries;
    uint16_t seq;
    uint8_t own_rsne[24];
    wwpa_supp supp;
    int keys_sent, ptk_installed, authorized, vdev_started;
    uint32_t key_events;
    // management buffers
    struct {uint8_t *cpu; uint64_t iova;} mbuf[WMLME_MGMT_BUFS]; unsigned mnext, mdesc;
    // counters
    uint32_t scan_frames, scan_new, mgmt_bad, auth_tx, assoc_tx, eapol_rx, eapol_tx, ev_scan_done;
    int scan_running;
} wmlme;

void wmlme_init(wmlme *m, const wmlme_ops *ops, wwmi *wmi, const uint8_t mac[6], uint32_t vdev_id);
// The 2.4 and 5 GHz channel table (non-DFS channels) the glue sends with wwmi_scan_chan_list, and the scan channel list.
unsigned wmlme_channels(wwmi_chan *out, unsigned cap);
// Create the vdev and apply the ath10k_add_interface parameters (nss, wake policy ...). Call after READY.
int wmlme_start(wmlme *m);
// Active scan of every channel (wildcard SSID) or of the channels of `freqs` (n > 0). Results accumulate in m->bss.
int wmlme_scan(wmlme *m, const uint32_t *freqs, unsigned n);
// Connect to `ssid` with a passphrase (8..63 characters) or a 64-hex-digit PSK, using the best BSS in the scan table. The PBKDF2 runs
// here. WMLME_ERR_NOENT when the SSID is not in the table (scan first).
int wmlme_connect(wmlme *m, const char *ssid, const char *pass);
int wmlme_disconnect(wmlme *m);
// Feed: WifiWmi events (the wwmi_ops.event callback), EAPOL frames from the data path, and the clock.
void wmlme_wmi_event(wmlme *m, const wwmi_event *ev);
void wmlme_eapol_rx(wmlme *m, const uint8_t src[6], const uint8_t *eapol, uint32_t len);
void wmlme_poll(wmlme *m);
const wmlme_bss *wmlme_find(const wmlme *m, const char *ssid);
const char *wmlme_state_name(unsigned state);

// Frame helpers (exposed for the tests and the data path)
// Parse a beacon / probe response (the frame from the 802.11 header on) into b; 0 or WMLME_ERR_PROTO.
int wmlme_parse_beacon(const uint8_t *f, uint32_t len, wmlme_bss *b);
// The RSN element checks: CCMP pairwise and group, PSK AKM, PMF not required. Returns 1 if usable.
int wmlme_rsn_usable(const uint8_t *rsn, unsigned len);

#ifdef __cplusplus
}
#endif
#endif
