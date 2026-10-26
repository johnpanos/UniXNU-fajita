// SPDX-License-Identifier: BSD-2-Clause-Patent
// Layer 5c: the station MLME (WifiMlme.c) over WifiWmi against a simulated target and a simulated WPA2-PSK access point.
// The target answers the WMI commands the way the WCN3990 firmware does (scan events with management frames, vdev start
// response, management frame transmission to the AP); the AP is an authenticator built from the same WifiWpa primitives
// (PTK derivation, AES key wrap, EAPOL-Key MICs) so the whole connect sequence is checked end to end, including the keys that
// reach the target.
#include "tcheck.h"
#include <string.h>
#include "../WifiMlme.h"
#include "../WifiAth10kIds.h"

#define QMAX 64
typedef struct {uint8_t d[600]; uint32_t len; int is_eapol;} qent;

typedef struct {
    wwmi w;
    wmlme m;
    uint32_t now;
    // arena for the management frame buffers
    uint8_t arena[4096]; uint32_t arena_used;
    // target -> host events and AP -> host EAPOL, delivered by pump()
    qent q[QMAX]; unsigned qh, qt;
    // the commands the host sent
    uint32_t cmds[256]; uint8_t cmd_copy[256][160]; uint32_t cmd_len[256]; unsigned ncmd;
    // the access point
    uint8_t bssid[6], other[6];
    const char *ssid, *pass;
    uint16_t freq;
    int ap_rsn_flags;           // 0 plain WPA2-PSK/CCMP; 1 PMF required; 2 TKIP group
    int ap_ignore_auth, ap_auth_status, ap_assoc_status, ap_no_m1, ap_resend_m3, ap_drop_key_complete;
    uint8_t pmk[32], anonce[32], snonce[32], gtk[16];
    wwpa_ptk ptk; int have_ptk; uint64_t replay;
    unsigned ap_auth_rx, ap_assoc_rx, m2_ok, m4_ok, m2_bad;
    uint8_t assoc_body[200]; uint32_t assoc_len;
    uint8_t auth_body[8];
    // counters
    unsigned eapol_sent, key_cmds, ev_states[16]; unsigned last_state, last_reason;
} sim;

static const uint8_t kMac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kAp[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60}, kOther[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x61};

static void w32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static void w16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static uint32_t r32(const uint8_t *p) {return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t r16(const uint8_t *p) {return p[0] | p[1] << 8;}
static uint8_t *tlv(uint8_t *p, uint32_t tag, uint32_t len) {w16(p, len); w16(p + 2, tag); memset(p + 4, 0, len); return p + 4;}

static void enqueue(sim *s, const uint8_t *d, uint32_t len, int is_eapol)
{
    if (((s->qt + 1) % QMAX) == s->qh) {CHECK(!"event queue full"); return;}
    memcpy(s->q[s->qt].d, d, len);
    s->q[s->qt].len = len;
    s->q[s->qt].is_eapol = is_eapol;
    s->qt = (s->qt + 1) % QMAX;
}

// ---- target events ----
static void ev_scan(sim *s, uint32_t type, uint32_t freq)
{
    uint8_t e[40];
    w32(e, WA_WMI_TLV_SCAN_EVENTID);
    uint8_t *p = tlv(e + 4, WA_WMI_TLV_TAG_STRUCT_SCAN_EVENT, 24);
    w32(p, type); w32(p + 8, freq); w32(p + 12, 0xa001); w32(p + 16, 0xa000); w32(p + 20, 0);
    enqueue(s, e, 4 + 4 + 24, 0);
}
static void ev_mgmt(sim *s, const uint8_t *frame, uint32_t len, uint32_t freq, uint32_t snr)
{
    uint8_t e[600];
    w32(e, WA_WMI_TLV_MGMT_RX_EVENTID);
    uint8_t *h = tlv(e + 4, WA_WMI_TLV_TAG_STRUCT_MGMT_RX_HDR, 40);
    w32(h, freq); w32(h + 4, snr); w32(h + 16, len);
    const uint32_t padded = (len + 3) & ~3u;
    uint8_t *b = tlv(e + 4 + 44, WA_WMI_TLV_TAG_ARRAY_BYTE, padded);
    memcpy(b, frame, len);
    enqueue(s, e, 4 + 44 + 4 + padded, 0);
}
static void ev_simple(sim *s, uint32_t id, uint32_t tag, const uint32_t *words, unsigned n)
{
    uint8_t e[64];
    w32(e, id);
    uint8_t *p = tlv(e + 4, tag, n * 4);
    for (unsigned i = 0; i < n; ++i) w32(p + 4 * i, words[i]);
    enqueue(s, e, 4 + 4 + n * 4, 0);
}

// ---- 802.11 frames from the AP ----
static unsigned put_ie(uint8_t *p, uint8_t id, const void *d, unsigned n) {p[0] = id; p[1] = (uint8_t)n; memcpy(p + 2, d, n); return 2 + n;}
static uint32_t beacon(sim *s, uint8_t *f, const uint8_t *bssid, const char *ssid, int rsn, int rsn_flags, int probe)
{
    memset(f, 0, 24);
    f[0] = probe ? 0x50 : 0x80;
    memset(f + 4, 0xff, 6);
    memcpy(f + 10, bssid, 6); memcpy(f + 16, bssid, 6);
    w16(f + 24 + 8, 100); w16(f + 24 + 10, 0x0431);
    uint32_t at = 36;
    at += put_ie(f + at, 0, ssid, (unsigned)strlen(ssid));
    static const uint8_t rates[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    at += put_ie(f + at, 1, rates, 8);
    const uint8_t ch = 6;
    at += put_ie(f + at, 3, &ch, 1);
    static const uint8_t ext[4] = {0x30, 0x48, 0x60, 0x6c};
    at += put_ie(f + at, 50, ext, 4);
    if (rsn) {
        uint8_t r[24] = {1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
        if (rsn_flags == 2) r[5] = 2;                  // group cipher TKIP
        if (rsn_flags == 1) r[18] = 0xc0;              // MFPR | MFPC
        at += put_ie(f + at, 48, r, 20);
    }
    (void)s;
    return at;
}
static void ap_beacons(sim *s, uint32_t freq)
{
    uint8_t f[300];
    uint32_t n = beacon(s, f, s->bssid, s->ssid, 1, s->ap_rsn_flags, 0);
    ev_mgmt(s, f, n, freq, 55);                               // our network, snr 55 -> -40 dBm
    n = beacon(s, f, kOther, s->ssid, 1, s->ap_rsn_flags, 1);               // the same SSID from a weaker AP, as a probe response
    ev_mgmt(s, f, n, freq, 30);
    n = beacon(s, f, (const uint8_t[]){0x30, 1, 2, 3, 4, 5}, "OtherNet", 0, 0, 0);
    ev_mgmt(s, f, n, freq, 45);
    n = beacon(s, f, s->bssid, "", 1, 0, 0);                  // a hidden-SSID style beacon of the same AP: must not wipe the SSID
    ev_mgmt(s, f, n, freq, 50);
}

// ---- the authenticator ----
static void ap_send_eapol(sim *s, const wwpa_key *k)
{
    qent e;
    const long n = wwpa_key_build(k, e.d, sizeof e.d);
    CHECK(n > 0);
    if (n <= 0) return;
    e.len = (uint32_t)n;
    enqueue(s, e.d, e.len, 1);
    ++s->eapol_sent;
}
static void ap_m1(sim *s)
{
    wwpa_key k;
    memset(&k, 0, sizeof k);
    k.eapol_version = 2; k.info = WWPA_KI_VER_HMAC_SHA1_AES | WWPA_KI_PAIRWISE | WWPA_KI_ACK; k.key_len = 16; k.replay = ++s->replay;
    memcpy(k.nonce, s->anonce, 32);
    ap_send_eapol(s, &k);
}
static void ap_m3(sim *s, uint64_t replay)
{
    uint8_t kd[64];
    unsigned at = 0;
    uint8_t rsne[22] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
    memcpy(kd, rsne, 22); at = 22;
    kd[at++] = 0xdd; kd[at++] = 0x16; kd[at++] = 0x00; kd[at++] = 0x0f; kd[at++] = 0xac; kd[at++] = 0x01;
    kd[at++] = 0x01 | 0x04; kd[at++] = 0;              // key id 1, tx
    memcpy(kd + at, s->gtk, 16); at += 16;
    kd[at++] = 0xdd;
    while (at % 8) kd[at++] = 0;
    uint8_t wrapped[80];
    CHECK(wwpa_aes_wrap(s->ptk.kek, 16, kd, at, wrapped) == 0);
    wwpa_key k;
    memset(&k, 0, sizeof k);
    k.eapol_version = 2;
    k.info = WWPA_KI_VER_HMAC_SHA1_AES | WWPA_KI_PAIRWISE | WWPA_KI_INSTALL | WWPA_KI_ACK | WWPA_KI_MIC | WWPA_KI_SECURE | WWPA_KI_ENC_DATA;
    k.key_len = 16; k.replay = replay;
    memcpy(k.nonce, s->anonce, 32);
    k.data = wrapped; k.data_len = (uint16_t)(at + 8);
    qent e;
    const long n = wwpa_key_build(&k, e.d, sizeof e.d);
    CHECK(n > 0);
    wwpa_key_sign(s->ptk.kck, e.d, (size_t)n);
    e.len = (uint32_t)n;
    enqueue(s, e.d, e.len, 1);
    ++s->eapol_sent;
}
static void ap_eapol_from_sta(sim *s, const uint8_t *f, uint32_t len)
{
    wwpa_key k;
    if (wwpa_key_parse(f, len, &k)) return;
    if (!s->have_ptk) {                                   // M2
        memcpy(s->snonce, k.nonce, 32);
        CHECK(wwpa_ptk_derive(s->pmk, s->bssid, kMac, s->anonce, s->snonce, 16, &s->ptk) == 0);
        s->have_ptk = 1;
        if (wwpa_key_verify(s->ptk.kck, f, len)) {++s->m2_bad; s->have_ptk = 0; return;}
        // the station's RSNE in M2 is the one it associated with
        ++s->m2_ok;
        ap_m3(s, ++s->replay);
        return;
    }
    if (!wwpa_key_verify(s->ptk.kck, f, len) && (k.info & WWPA_KI_SECURE)) {          // M4
        ++s->m4_ok;
        if (s->ap_resend_m3) ap_m3(s, ++s->replay);   // an AP that retransmits M3 after M4 was lost
    }
}

// ---- the target ----
static int tsend(void *c, const void *buf, uint32_t len)
{
    sim *s = c;
    const uint8_t *d = buf;
    const uint32_t id = r32(d) & 0xffffff;
    if (s->ncmd < 256) {
        s->cmds[s->ncmd] = id;
        s->cmd_len[s->ncmd] = len < 160 ? len : 160;
        memcpy(s->cmd_copy[s->ncmd], d, s->cmd_len[s->ncmd]);
        ++s->ncmd;
    }
    if (id == WA_WMI_TLV_START_SCAN_CMDID) {
        const uint8_t *cmd = d + 4 + 4;
        const uint32_t nch = r32(cmd + 64);
        const uint8_t *arr = d + 4 + 4 + 100 + 4;
        ev_scan(s, WA_WMI_SCAN_EVENT_STARTED, 0);
        for (uint32_t i = 0; i < nch; ++i)
            if (r32(arr + 4 * i) == s->freq) ap_beacons(s, s->freq);
        ev_scan(s, WA_WMI_SCAN_EVENT_COMPLETED, 0);
    } else if (id == WA_WMI_TLV_VDEV_START_REQUEST_CMDID) {
        const uint32_t w[4] = {0, 1, 0, 0};
        ev_simple(s, WA_WMI_TLV_VDEV_START_RESP_EVENTID, WA_WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT, w, 4);
    } else if (id == WA_WMI_TLV_MGMT_TX_SEND_CMD) {
        const uint8_t *c28 = d + 4 + 4;
        const uint32_t flen = r32(c28 + 20);
        const uint64_t paddr = (uint64_t)r32(c28 + 12) | (uint64_t)r32(c28 + 16) << 32;
        const uint8_t *f = s->arena + (paddr - 0x1c00100000ull);
        CHECK(paddr >= 0x1c00100000ull && paddr + flen <= 0x1c00100000ull + sizeof s->arena);
        // the frame is addressed to the AP from us
        const unsigned sub = f[0] & 0xfc;
        CHECK(!memcmp(f + 4, s->bssid, 6) && !memcmp(f + 10, kMac, 6) && !memcmp(f + 16, s->bssid, 6));
        uint8_t resp[100];
        if (sub == 0xb0) {
            ++s->ap_auth_rx;
            memcpy(s->auth_body, f + 24, 6);
            if (!s->ap_ignore_auth) {
                memset(resp, 0, 24);
                resp[0] = 0xb0; memcpy(resp + 4, kMac, 6); memcpy(resp + 10, s->bssid, 6); memcpy(resp + 16, s->bssid, 6);
                w16(resp + 24, 0); w16(resp + 26, 2); w16(resp + 28, s->ap_auth_status);
                ev_mgmt(s, resp, 30, s->freq, 55);
            }
        } else if (sub == 0x00) {
            ++s->ap_assoc_rx;
            s->assoc_len = flen - 24 < sizeof s->assoc_body ? flen - 24 : (uint32_t)sizeof s->assoc_body;
            memcpy(s->assoc_body, f + 24, s->assoc_len);
            memset(resp, 0, 24);
            resp[0] = 0x10; memcpy(resp + 4, kMac, 6); memcpy(resp + 10, s->bssid, 6); memcpy(resp + 16, s->bssid, 6);
            w16(resp + 24, 0x0431); w16(resp + 26, s->ap_assoc_status); w16(resp + 28, 0xc001);   // AID 1 with the two top bits set
            static const uint8_t rates[4] = {0x82, 0x84, 0x8b, 0x96};
            resp[30] = 1; resp[31] = 4; memcpy(resp + 32, rates, 4);
            ev_mgmt(s, resp, 36, s->freq, 55);
        }
    } else if (id == WA_WMI_TLV_VDEV_UP_CMDID) {
        if (!s->ap_no_m1) ap_m1(s);      // the AP starts the handshake as soon as the station is associated
    } else if (id == WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID) {
        ++s->key_cmds;
        if (!s->ap_drop_key_complete) {
            uint8_t e[8];
            w32(e, WA_WMI_TLV_VDEV_INSTALL_KEY_COMPLETE_EVENTID);
            enqueue(s, e, 4, 0);
        }
    }
    return 0;
}
static void *tdma(void *c, uint32_t size, uint64_t *iova)
{
    sim *s = c;
    if (s->arena_used + size > sizeof s->arena) return NULL;
    void *p = s->arena + s->arena_used;
    *iova = 0x1c00100000ull + s->arena_used;
    s->arena_used += size;
    return p;
}
static void tevent(void *c, const wwmi_event *e) {sim *s = c; wmlme_wmi_event(&s->m, e);}
static int teapol_tx(void *c, const uint8_t dst[6], const uint8_t *f, uint32_t len)
{
    sim *s = c;
    CHECK(!memcmp(dst, s->bssid, 6));
    ap_eapol_from_sta(s, f, len);
    return 0;
}
static void trandom(void *c, uint8_t *out, uint32_t n) {(void)c; for (uint32_t i = 0; i < n; ++i) out[i] = (uint8_t)(0x40 + i);}
static uint32_t tnow(void *c) {return ((sim *)c)->now;}
static void tstate(void *c, unsigned st, unsigned reason)
{
    sim *s = c;
    if (st < 16) ++s->ev_states[st];
    s->last_state = st; s->last_reason = reason;
}

static void pump(sim *s)
{
    for (unsigned guard = 0; s->qh != s->qt && guard < 1000; ++guard) {
        qent e = s->q[s->qh];
        s->qh = (s->qh + 1) % QMAX;
        if (e.is_eapol) wmlme_eapol_rx(&s->m, s->bssid, e.d, e.len);
        else wwmi_rx(&s->w, e.d, e.len);
        wmlme_poll(&s->m);
    }
}
static void advance(sim *s, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {s->now += 50; wmlme_poll(&s->m); pump(s);}
}

static sim *new_sim(const char *ssid, const char *pass)
{
    sim *s = t_talloc(sizeof *s);
    memset(s, 0, sizeof *s);
    memcpy(s->bssid, kAp, 6); memcpy(s->other, kOther, 6);
    s->ssid = ssid; s->pass = pass; s->freq = 2437;
    s->now = 1000;
    for (unsigned i = 0; i < 32; ++i) s->anonce[i] = (uint8_t)(0xe0 + i);
    for (unsigned i = 0; i < 16; ++i) s->gtk[i] = (uint8_t)(0x70 + i);
    CHECK(wwpa_psk(pass, strlen(pass), (const uint8_t *)ssid, strlen(ssid), s->pmk) == 0);
    const wwmi_ops wo = {s, tsend, tdma, tevent};
    wwmi_init(&s->w, &wo);
    s->w.num_rf_chains = 2;
    const wmlme_ops mo = {s, teapol_tx, trandom, tnow, tdma, tstate};
    wmlme_init(&s->m, &mo, &s->w, kMac, 0);
    return s;
}
static unsigned count_cmd(const sim *s, uint32_t id) {unsigned n = 0; for (unsigned i = 0; i < s->ncmd; ++i) n += s->cmds[i] == id; return n;}
static const uint8_t *nth_cmd(const sim *s, uint32_t id, unsigned n)
{
    for (unsigned i = 0; i < s->ncmd; ++i) if (s->cmds[i] == id && n-- == 0) return s->cmd_copy[i];
    return NULL;
}

static void t_mlme_beacons(void)
{
    uint8_t f[300];
    sim *s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_bss b;
    uint32_t n = beacon(s, f, kAp, "KittyCorner", 1, 0, 0);
    CHECK(wmlme_parse_beacon(f, n, &b) == 0 && b.ssid_len == 11 && !memcmp(b.ssid, "KittyCorner", 11) && !memcmp(b.bssid, kAp, 6));
    CHECK(b.beacon_int == 100 && b.capab == 0x0431 && b.n_rates == 12 && b.rsn_len == 22 && b.rsn_ok && !b.ht && !b.probe_resp);
    n = beacon(s, f, kAp, "KittyCorner", 1, 1, 1);
    CHECK(wmlme_parse_beacon(f, n, &b) == 0 && b.probe_resp && !b.rsn_ok);                 // PMF required
    n = beacon(s, f, kAp, "KittyCorner", 1, 2, 0);
    CHECK(wmlme_parse_beacon(f, n, &b) == 0 && !b.rsn_ok);                                  // TKIP group
    n = beacon(s, f, kAp, "OpenNet", 0, 0, 0);
    CHECK(wmlme_parse_beacon(f, n, &b) == 0 && b.rsn_len == 0 && !b.rsn_ok);
    CHECK(wmlme_parse_beacon(f, 20, &b) == WMLME_ERR_PROTO && wmlme_parse_beacon(f, 40, &b) == WMLME_ERR_PROTO);
    f[0] = 0x40;
    CHECK(wmlme_parse_beacon(f, n, &b) == WMLME_ERR_PROTO);                                  // not a beacon
    // an IE that runs past the frame
    n = beacon(s, f, kAp, "x", 1, 0, 0);
    f[37] = 200;
    CHECK(wmlme_parse_beacon(f, n, &b) == WMLME_ERR_PROTO);
    t_tfree_all();
}

static void t_mlme_scan(void)
{
    sim *s = new_sim("KittyCorner", "hunter22hunter");
    wwmi_chan ch[WWMI_MAX_SCAN_CHANS];
    const unsigned nch = wmlme_channels(ch, WWMI_MAX_SCAN_CHANS);
    CHECK(nch == 20 && ch[0].freq == 2412 && ch[10].freq == 2462 && ch[11].freq == 5180 && ch[19].freq == 5825 && ch[0].mode == WA_MODE_11G && ch[11].mode == WA_MODE_11A);
    CHECK(wmlme_start(&s->m) == 0);
    CHECK(count_cmd(s, WA_WMI_TLV_VDEV_CREATE_CMDID) == 1 && count_cmd(s, WA_WMI_TLV_VDEV_SET_PARAM_CMDID) == 2 && count_cmd(s, WA_WMI_TLV_STA_POWERSAVE_MODE_CMDID) == 1);
    CHECK(r32(nth_cmd(s, WA_WMI_TLV_VDEV_SET_PARAM_CMDID, 1) + 4 + 4 + 4) == WA_WMI_TLV_VDEV_PARAM_NSS && r32(nth_cmd(s, WA_WMI_TLV_VDEV_SET_PARAM_CMDID, 1) + 4 + 4 + 8) == 2);
    CHECK(wmlme_scan(&s->m, NULL, 0) == 0 && s->m.state == WMLME_SCANNING);
    CHECK(wmlme_scan(&s->m, NULL, 0) == WMLME_ERR_BUSY);
    pump(s);
    CHECK(s->m.state == WMLME_SCAN_DONE && !s->m.scan_running && s->m.nbss == 3);
    const wmlme_bss *b = wmlme_find(&s->m, "KittyCorner");
    CHECK(b && !memcmp(b->bssid, kAp, 6) && b->rssi == -45 && b->freq == 2437 && b->rsn_ok && b->ssid_len == 11);   // the stronger AP; the empty-SSID beacon did not wipe it
    CHECK(wmlme_find(&s->m, "OtherNet") && wmlme_find(&s->m, "OtherNet")->rssi == -50 && !wmlme_find(&s->m, "OtherNet")->rsn_ok);
    CHECK(!wmlme_find(&s->m, "Nope"));
    // a scan of one channel that has nothing on it
    uint32_t f1 = 5180;
    CHECK(wmlme_scan(&s->m, &f1, 1) == 0);
    pump(s);
    CHECK(s->m.state == WMLME_SCAN_DONE && s->m.nbss == 0);
    // a damaged frame is counted and ignored
    t_tfree_all();
}

static void check_connected_commands(sim *s)
{
    // vdev start on the AP's channel, legacy mode, no SSID, flags 0
    const uint8_t *vs = nth_cmd(s, WA_WMI_TLV_VDEV_START_REQUEST_CMDID, 0);
    CHECK(vs != NULL);
    if (vs) {
        const uint8_t *c = vs + 8, *ch = vs + 8 + 72 + 4;
        CHECK(r32(c) == 0 && r32(c + 16) == 0 && r32(c + 20) == 0);
        CHECK(r32(ch) == 2437 && (r32(ch + 12) & 0x3f) == WA_MODE_11G);
    }
    const uint8_t *pc = nth_cmd(s, WA_WMI_TLV_PEER_CREATE_CMDID, 0);
    CHECK(pc && !memcmp(pc + 8 + 4, kAp, 6) && r32(pc + 8 + 12) == WA_WMI_TLV_PEER_TYPE_BSS);
    CHECK(s->ap_auth_rx == 1 && !memcmp(s->auth_body, (const uint8_t[]){0, 0, 1, 0, 0, 0}, 6));
    // the association request: capabilities (ESS | privacy | short slot | short preamble), listen interval, SSID, 8 + 4 rates, our RSNE
    CHECK(s->ap_assoc_rx == 1 && r16(s->assoc_body) == 0x0431 && r16(s->assoc_body + 2) == 10);
    CHECK(s->assoc_body[4] == 0 && s->assoc_body[5] == 11 && !memcmp(s->assoc_body + 6, "KittyCorner", 11));
    CHECK(s->assoc_body[17] == 1 && s->assoc_body[18] == 8 && s->assoc_body[27] == 50 && s->assoc_body[28] == 4);
    static const uint8_t rsne[22] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
    CHECK(s->assoc_len == 4 + 13 + 10 + 6 + 22 && !memcmp(s->assoc_body + s->assoc_len - 22, rsne, 22));
    // peer assoc: AID without the two top bits, flags auth | need ptk 4-way, the rates with CCK marked, 11g
    const uint8_t *pa = nth_cmd(s, WA_WMI_TLV_PEER_ASSOC_CMDID, 0);
    CHECK(pa != NULL);
    if (pa) {
        const uint8_t *c = pa + 8;
        CHECK(!memcmp(c, kAp, 6) && r32(c + 12) == 1 && r32(c + 16) == 1 && r32(c + 20) == (WA_WMI_TLV_PEER_AUTH | WA_WMI_TLV_PEER_NEED_PTK_4_WAY));
        CHECK(r32(c + 24) == 0x0431 && r32(c + 56) == WA_MODE_11G && r32(c + 68) == 4 && r32(c + 72) == 0);
        const uint8_t *rates = pa + 8 + 76 + 4;
        CHECK(rates[0] == 0x82 && rates[1] == 0x84 && rates[2] == 0x8b && rates[3] == 0x96);   // the AP's four rates in the response; CCK flagged
    }
    const uint8_t *up = nth_cmd(s, WA_WMI_TLV_VDEV_UP_CMDID, 0);
    CHECK(up && r32(up + 8 + 4) == 1 && !memcmp(up + 8 + 8, kAp, 6));
    CHECK(count_cmd(s, WA_WMI_TLV_STA_POWERSAVE_MODE_CMDID) >= 1);
}

static void t_mlme_connect(void)
{
    sim *s = new_sim("KittyCorner", "hunter22hunter");
    CHECK(wmlme_start(&s->m) == 0);
    CHECK(wmlme_connect(&s->m, "KittyCorner", "hunter22hunter") == WMLME_ERR_NOENT && s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_NO_BSS);
    CHECK(wmlme_scan(&s->m, NULL, 0) == 0);
    pump(s);
    CHECK(wmlme_connect(&s->m, "KittyCorner", "short") == WMLME_ERR_INVAL);
    CHECK(wmlme_connect(&s->m, "KittyCorner", "hunter22hunter") == 0);
    pump(s);
    CHECK(s->m.state == WMLME_CONNECTED && s->m.aid == 1);
    check_connected_commands(s);
    // the handshake: M1, M2 ok at the AP, M3, M4 ok
    CHECK(s->m2_ok == 1 && s->m2_bad == 0 && s->m4_ok == 1 && s->m.eapol_rx == 2 && s->m.eapol_tx == 2);
    // the keys that reached the target: pairwise = the PTK's TK the AP derived, then the group key id 1
    CHECK(s->key_cmds == 2 && count_cmd(s, WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID) == 2);
    const uint8_t *k1 = nth_cmd(s, WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID, 0), *k2 = nth_cmd(s, WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID, 1);
    CHECK(k1 && k2);
    if (k1 && k2) {
        const uint8_t *c1 = k1 + 8, *c2 = k2 + 8;
        CHECK(!memcmp(c1 + 4, kAp, 6) && r32(c1 + 12) == 0 && r32(c1 + 16) == WA_WMI_KEY_PAIRWISE && r32(c1 + 20) == WA_WMI_CIPHER_AES_CCM && r32(c1 + 80) == 16);
        CHECK(!memcmp(c1 + 92 + 4, s->ptk.tk, 16));
        CHECK(!memcmp(c2 + 4, kAp, 6) && r32(c2 + 12) == 1 && r32(c2 + 16) == WA_WMI_KEY_GROUP && r32(c2 + 20) == WA_WMI_CIPHER_AES_CCM);
        CHECK(!memcmp(c2 + 92 + 4, s->gtk, 16));
    }
    // peer authorize, after the keys
    const uint8_t *au = nth_cmd(s, WA_WMI_TLV_PEER_SET_PARAM_CMDID, 0);
    CHECK(au && r32(au + 8 + 12) == WA_WMI_PEER_AUTHORIZE && r32(au + 8 + 16) == 1 && s->m.authorized);
    // the order of the whole sequence
    static const uint32_t order[] = {WA_WMI_TLV_VDEV_CREATE_CMDID, WA_WMI_TLV_START_SCAN_CMDID, WA_WMI_TLV_VDEV_START_REQUEST_CMDID,
                                     WA_WMI_TLV_PEER_CREATE_CMDID, WA_WMI_TLV_MGMT_TX_SEND_CMD, WA_WMI_TLV_MGMT_TX_SEND_CMD, WA_WMI_TLV_PEER_ASSOC_CMDID,
                                     WA_WMI_TLV_VDEV_UP_CMDID, WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID, WA_WMI_TLV_VDEV_INSTALL_KEY_CMDID,
                                     WA_WMI_TLV_PEER_SET_PARAM_CMDID};
    unsigned at = 0;
    for (unsigned i = 0; i < s->ncmd && at < sizeof order / sizeof order[0]; ++i) if (s->cmds[i] == order[at]) ++at;
    CHECK(at == sizeof order / sizeof order[0]);
    // a retransmitted M3 after the connection: M4 again, the keys are not installed again (KRACK)
    s->ap_resend_m3 = 0;
    ap_m3(s, ++s->replay);
    pump(s);
    CHECK(s->key_cmds == 2 && s->m4_ok == 2 && s->m.state == WMLME_CONNECTED);
    // disconnect
    CHECK(wmlme_disconnect(&s->m) == 0 && s->m.state == WMLME_IDLE && count_cmd(s, WA_WMI_TLV_VDEV_DOWN_CMDID) == 1 &&
          count_cmd(s, WA_WMI_TLV_PEER_DELETE_CMDID) == 1 && count_cmd(s, WA_WMI_TLV_VDEV_STOP_CMDID) == 1);
    t_tfree_all();
}

static void t_mlme_failures(void)
{
    // the AP never answers authentication: three retries, then the failure
    sim *s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    s->ap_ignore_auth = 1;
    CHECK(wmlme_connect(&s->m, "KittyCorner", "hunter22hunter") == 0);
    pump(s);
    CHECK(s->m.state == WMLME_AUTH);
    advance(s, 5000);
    CHECK(s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_AUTH && s->ap_auth_rx == 1 + WMLME_RETRIES);
    // authentication refused
    s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    s->ap_auth_status = 13;
    wmlme_connect(&s->m, "KittyCorner", "hunter22hunter"); pump(s);
    CHECK(s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_AUTH && s->ap_assoc_rx == 0);
    // association refused (status 17: AP full)
    s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    s->ap_assoc_status = 17;
    wmlme_connect(&s->m, "KittyCorner", "hunter22hunter"); pump(s);
    CHECK(s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_ASSOC && count_cmd(s, WA_WMI_TLV_PEER_ASSOC_CMDID) == 0);
    // the wrong passphrase: the AP rejects M2 and never sends M3; the handshake times out, no key reaches the target
    s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    wmlme_connect(&s->m, "KittyCorner", "not-the-passphrase"); pump(s);
    CHECK(s->m.state == WMLME_KEYS && s->m2_bad == 1 && s->key_cmds == 0);
    advance(s, 2000);
    CHECK(s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_KEYS && s->key_cmds == 0 && count_cmd(s, WA_WMI_TLV_PEER_SET_PARAM_CMDID) == 0);
    // an AP that never starts the handshake
    s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    s->ap_no_m1 = 1;
    wmlme_connect(&s->m, "KittyCorner", "hunter22hunter"); pump(s);
    CHECK(s->m.state == WMLME_ASSOCIATED);
    advance(s, 2000);
    CHECK(s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_KEYS);
    // PMF required / TKIP group: refused up front
    s = new_sim("KittyCorner", "hunter22hunter");
    s->ap_rsn_flags = 1;
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    CHECK(wmlme_connect(&s->m, "KittyCorner", "hunter22hunter") == WMLME_ERR_NOTSUP && s->m.reason == WMLME_REASON_UNSUPPORTED);
    // key install events never come (a quiet target): the port opens after the wait, as ath10k's 3 s wait would end
    s = new_sim("KittyCorner", "hunter22hunter");
    s->ap_drop_key_complete = 1;
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    wmlme_connect(&s->m, "KittyCorner", "hunter22hunter"); pump(s);
    CHECK(s->m.state == WMLME_KEYS && s->key_cmds == 2);
    advance(s, 1500);
    CHECK(s->m.state == WMLME_CONNECTED);
    // kicked out by the target
    uint32_t w[1] = {0};
    (void)w;
    uint8_t e[24];
    w32(e, WA_WMI_TLV_PEER_STA_KICKOUT_EVENTID);
    uint8_t *k = tlv(e + 4, WA_WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT, 8);
    memcpy(k, kAp, 6);
    wwmi_rx(&s->w, e, 4 + 4 + 8);
    CHECK(s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_KICKOUT);
    // deauthentication from the AP; a deauth from another BSS is ignored
    s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    wmlme_connect(&s->m, "KittyCorner", "hunter22hunter"); pump(s);
    CHECK(s->m.state == WMLME_CONNECTED);
    uint8_t da[26];
    memset(da, 0, sizeof da);
    da[0] = 0xc0; memcpy(da + 4, kMac, 6); memcpy(da + 10, kOther, 6); memcpy(da + 16, kOther, 6); w16(da + 24, 3);
    ev_mgmt(s, da, 26, 2437, 50); pump(s);
    CHECK(s->m.state == WMLME_CONNECTED);
    memcpy(da + 10, kAp, 6); memcpy(da + 16, kAp, 6);
    ev_mgmt(s, da, 26, 2437, 50); pump(s);
    CHECK(s->m.state == WMLME_FAILED && s->m.reason == WMLME_REASON_DEAUTH);
    t_tfree_all();
}

static void t_mlme_hexpsk(void)
{
    // a 64-digit PSK is the PMK itself
    sim *s = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s->m); wmlme_scan(&s->m, NULL, 0); pump(s);
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", s->pmk[i]);
    CHECK(wmlme_connect(&s->m, "KittyCorner", hex) == 0);
    pump(s);
    CHECK(s->m.state == WMLME_CONNECTED && !memcmp(s->m.pmk, s->pmk, 32));
    hex[3] = 'g';
    sim *s2 = new_sim("KittyCorner", "hunter22hunter");
    wmlme_start(&s2->m); wmlme_scan(&s2->m, NULL, 0); pump(s2);
    CHECK(wmlme_connect(&s2->m, "KittyCorner", hex) == WMLME_ERR_INVAL);
    t_tfree_all();
}

void t_mlme(void)
{
    RUN(t_mlme_beacons);
    RUN(t_mlme_scan);
    RUN(t_mlme_connect);
    RUN(t_mlme_failures);
    RUN(t_mlme_hexpsk);
}
