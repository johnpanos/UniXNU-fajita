// SPDX-License-Identifier: BSD-2-Clause-Patent
// Station MLME: scan table, authentication, association, key installation. See WifiMlme.h.
#include "WifiMlme.h"
#include "WifiAth10kIds.h"

#define FC_ASSOC_REQ 0x00
#define FC_ASSOC_RESP 0x10
#define FC_PROBE_RESP 0x50
#define FC_BEACON 0x80
#define FC_DISASSOC 0xa0
#define FC_AUTH 0xb0
#define FC_DEAUTH 0xc0
#define CAP_ESS 0x0001u
#define CAP_PRIVACY 0x0010u
#define CAP_SHORT_PREAMBLE 0x0020u
#define CAP_SHORT_SLOT 0x0400u
#define IE_SSID 0
#define IE_RATES 1
#define IE_RSN 48
#define IE_EXT_RATES 50
#define IE_HT_CAP 45

static void zero(void *p, size_t n) {uint8_t *b = p; while (n--) *b++ = 0;}
static void copy(void *d, const void *s, size_t n) {uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++;}
static int eq(const void *a, const void *b, size_t n) {const uint8_t *x = a, *y = b; while (n--) if (*x++ != *y++) return 0; return 1;}
static uint32_t g16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static void p16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static size_t slen(const char *s) {size_t n = 0; while (s[n]) ++n; return n;}

const char *wmlme_state_name(unsigned s)
{
    static const char *const n[] = {"idle", "scanning", "scan-done", "vdev-start", "auth", "assoc", "associated", "keys", "connected", "failed"};
    return s < sizeof n / sizeof n[0] ? n[s] : "?";
}

static void set_state(wmlme *m, unsigned s, unsigned reason)
{
    m->state = s;
    m->reason = reason;
    if (m->ops.state) m->ops.state(m->ops.ctx, s, reason);
}
static void arm(wmlme *m) {m->step_at = m->ops.now_ms(m->ops.ctx) + WMLME_TIMEOUT_MS;}
static void fail(wmlme *m, unsigned reason)
{
    if (reason == WMLME_REASON_AUTH || reason == WMLME_REASON_ASSOC) {copy(m->avoid, m->target.bssid, 6); m->have_avoid = 1;}
    else if (reason == WMLME_REASON_NONE) m->have_avoid = 0;
    wwpa_supp_clear(&m->supp);
    set_state(m, WMLME_FAILED, reason);
}

void wmlme_init(wmlme *m, const wmlme_ops *ops, wwmi *wmi, const uint8_t mac[6], uint32_t vdev_id)
{
    zero(m, sizeof *m);
    m->ops = *ops;
    m->wmi = wmi;
    copy(m->mac, mac, 6);
    m->vdev_id = vdev_id;
    // our RSN element: group CCMP, pairwise CCMP, AKM PSK, no capabilities
    static const uint8_t rsne[22] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
    copy(m->own_rsne, rsne, sizeof rsne);
}

void wmlme_seed_seq(wmlme *m)
{
    uint8_t r[2] = {0, 0};
    if (m->ops.random) m->ops.random(m->ops.ctx, r, 2);
    m->seq = (uint16_t)((r[0] | r[1] << 8) & 0xfff);
    m->seq_random = 1;
}

// ---- channels ----
unsigned wmlme_channels(wwmi_chan *out, unsigned cap)
{
    static const uint16_t g[] = {2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462};
    static const uint16_t a[] = {5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825};
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof g / sizeof g[0] && n < cap; ++i, ++n) {
        zero(&out[n], sizeof out[n]);
        out[n].freq = out[n].center1 = g[i]; out[n].flags = WA_WMI_CHAN_FLAG_ALLOW_HT; out[n].mode = WA_MODE_11G;
        out[n].max_power = out[n].reg_power = 40;
    }
    for (unsigned i = 0; i < sizeof a / sizeof a[0] && n < cap; ++i, ++n) {
        zero(&out[n], sizeof out[n]);
        out[n].freq = out[n].center1 = a[i]; out[n].flags = WA_WMI_CHAN_FLAG_ALLOW_HT; out[n].mode = WA_MODE_11A;
        out[n].max_power = out[n].reg_power = 40;
    }
    return n;
}

// ---- information elements ----
static const uint8_t *ie_find(const uint8_t *ies, uint32_t len, uint8_t id)
{
    while (len >= 2) {
        const uint32_t l = ies[1];
        if (2 + l > len) return 0;
        if (ies[0] == id) return ies;
        ies += 2 + l; len -= 2 + l;
    }
    return 0;
}

unsigned wmlme_rsn_akm(const uint8_t *rsn, unsigned len, unsigned *pmf)
{
    if (pmf) *pmf = 0;
    if (len < 2 || rsn[0] != IE_RSN || rsn[1] + 2u > len) return 0;
    const uint8_t *p = rsn + 2, *end = rsn + 2 + rsn[1];
    if (end - p < 2 || g16(p) != 1) return 0;
    p += 2;
    static const uint8_t oui[3] = {0x00, 0x0f, 0xac};
    if (end - p < 4 || !eq(p, oui, 3) || p[3] != 4) return 0;      // group cipher CCMP
    p += 4;
    if (end - p < 2) return 0;
    unsigned n = g16(p);
    p += 2;
    int ccmp = 0;
    if (end - p < (long)(4 * n)) return 0;
    for (unsigned i = 0; i < n; ++i, p += 4) ccmp |= eq(p, oui, 3) && p[3] == 4;
    if (!ccmp) return 0;
    if (end - p < 2) return 0;
    n = g16(p);
    p += 2;
    int psk = 0, sae = 0;
    if (end - p < (long)(4 * n)) return 0;
    for (unsigned i = 0; i < n; ++i, p += 4) {psk |= eq(p, oui, 3) && p[3] == 2; sae |= eq(p, oui, 3) && p[3] == 8;}
    unsigned caps = end - p >= 2 ? g16(p) : 0;
    const unsigned mfp = caps & 0x40 ? 2u : caps & 0x80 ? 1u : 0u;   // MFPR (bit 6) / MFPC (bit 7)
    if (pmf) *pmf = mfp;
    return sae ? WWPA_AKM_SAE : psk ? WWPA_AKM_PSK : 0;
}

int wmlme_rsn_usable(const uint8_t *rsn, unsigned len) {return wmlme_rsn_akm(rsn, len, 0) != 0;}

int wmlme_parse_beacon(const uint8_t *f, uint32_t len, wmlme_bss *b)
{
    if (len < 24 + 12) return WMLME_ERR_PROTO;
    const unsigned sub = f[0] & 0xfc;
    if (sub != FC_BEACON && sub != FC_PROBE_RESP) return WMLME_ERR_PROTO;
    zero(b, sizeof *b);
    copy(b->bssid, f + 16, 6);
    b->beacon_int = (uint16_t)g16(f + 24 + 8);
    b->capab = (uint16_t)g16(f + 24 + 10);
    b->probe_resp = sub == FC_PROBE_RESP;
    const uint8_t *ies = f + 36;
    const uint32_t ilen = len - 36;
    const uint8_t *s = ie_find(ies, ilen, IE_SSID);
    if (!s || s[1] > 32) return WMLME_ERR_PROTO;
    b->ssid_len = s[1];
    copy(b->ssid, s + 2, s[1]);
    const uint8_t *r = ie_find(ies, ilen, IE_RATES);
    if (r) for (unsigned i = 0; i < r[1] && b->n_rates < WMLME_MAX_RATES; ++i) b->rates[b->n_rates++] = r[2 + i];
    const uint8_t *x = ie_find(ies, ilen, IE_EXT_RATES);
    if (x) for (unsigned i = 0; i < x[1] && b->n_rates < WMLME_MAX_RATES; ++i) b->rates[b->n_rates++] = x[2 + i];
    const uint8_t *rsn = ie_find(ies, ilen, IE_RSN);
    if (rsn && 2u + rsn[1] <= WMLME_RSN_MAX) {b->rsn_len = (uint8_t)(2 + rsn[1]); copy(b->rsn, rsn, b->rsn_len); unsigned pmf = 0; b->akm = (uint8_t)wmlme_rsn_akm(b->rsn, b->rsn_len, &pmf); b->pmf = (uint8_t)pmf; b->rsn_ok = b->akm != 0;}
    b->ht = ie_find(ies, ilen, IE_HT_CAP) != 0;
    const uint8_t *rx = ie_find(ies, ilen, 244);   // RSN extension: SAE H2E (bit 5), SAE-PK (bit 6)
    if (rx) {b->rsnxe_len = rx[1]; b->rsnxe0 = rx[1] ? rx[2] : 0;}
    for (unsigned i = 0; i < b->n_rates; ++i) if (b->rates[i] == (0x80 | 123)) b->h2e_only = 1;
    return 0;
}

// ---- scan ----
int wmlme_start(wmlme *m)
{
    int rc = wwmi_vdev_create(m->wmi, m->vdev_id, WA_WMI_VDEV_TYPE_STA, WA_WMI_TLV_VDEV_SUBTYPE_NONE, m->mac);
    if (rc) return rc;
    m->vdev_created = 1;
    // ath10k_update_vif_offload and ath10k_add_interface: native wifi encapsulation, spatial streams from the chains
    rc = wwmi_vdev_set_param(m->wmi, m->vdev_id, WA_WMI_TLV_VDEV_PARAM_TX_ENCAP_TYPE, WA_ATH10K_HW_TXRX_NATIVE_WIFI);
    if (rc) return rc;
    rc = wwmi_vdev_set_param(m->wmi, m->vdev_id, WA_WMI_TLV_VDEV_PARAM_NSS, m->wmi->num_rf_chains ? m->wmi->num_rf_chains : 1);
    if (rc) return rc;
    rc = wwmi_set_psmode(m->wmi, m->vdev_id, WA_WMI_STA_PS_MODE_DISABLED);
    return rc;
}

int wmlme_scan(wmlme *m, const uint32_t *freqs, unsigned n)
{
    if (m->state != WMLME_IDLE && m->state != WMLME_SCAN_DONE && m->state != WMLME_FAILED) return WMLME_ERR_BUSY;
    wwmi_scan_arg a;
    wwmi_scan_init(&a);
    a.vdev_id = m->vdev_id;
    a.scan_id = 0;
    a.n_ssids = 1;            // the wildcard SSID: active scan
    a.ssid_len[0] = 0;
    if (n) {
        if (n > WWMI_MAX_SCAN_CHANS) return WMLME_ERR_INVAL;
        for (unsigned i = 0; i < n; ++i) a.channels[i] = freqs[i];
        a.n_channels = n;
    } else {
        wwmi_chan ch[WWMI_MAX_SCAN_CHANS];
        const unsigned c = wmlme_channels(ch, WWMI_MAX_SCAN_CHANS);
        for (unsigned i = 0; i < c; ++i) a.channels[i] = ch[i].freq;
        a.n_channels = c;
    }
    m->nbss = 0;
    m->scan_running = 1;
    set_state(m, WMLME_SCANNING, 0);
    const int rc = wwmi_start_scan(m->wmi, &a);
    if (rc) {m->scan_running = 0; set_state(m, WMLME_IDLE, WMLME_REASON_SEND);}
    return rc;
}

static void scan_frame(wmlme *m, const wwmi_event *ev)
{
    wmlme_bss b;
    if (ev->d != WA_WMI_RX_STATUS_OK) {++m->mgmt_bad; return;}
    if (wmlme_parse_beacon(ev->data, ev->len, &b)) return;
    ++m->scan_frames;
    b.freq = ev->a;
    b.rssi = (int32_t)ev->b - 95;
    b.seen_ms = m->ops.now_ms(m->ops.ctx);
    b.n_beacons = (uint16_t)!b.probe_resp; b.n_probe_resps = (uint16_t)!!b.probe_resp;
    for (unsigned i = 0; i < m->nbss; ++i) {
        if (!eq(m->bss[i].bssid, b.bssid, 6)) continue;
        const uint16_t nb = m->bss[i].n_beacons, np = m->bss[i].n_probe_resps;
        // a probe response beats a beacon (the hidden-SSID beacon has an empty SSID); keep the stronger signal
        if (b.ssid_len || !m->bss[i].ssid_len) m->bss[i] = b;
        else m->bss[i].rssi = b.rssi;
        if (nb < 0xffff) m->bss[i].n_beacons = (uint16_t)(nb + !b.probe_resp);
        if (np < 0xffff) m->bss[i].n_probe_resps = (uint16_t)(np + !!b.probe_resp);
        return;
    }
    if (m->nbss < WMLME_MAX_BSS) {m->bss[m->nbss++] = b; ++m->scan_new; return;}
    // Full: never lose the network we want; otherwise keep the strongest. Replace the weakest entry that is not the target.
    ++m->scan_full;
    const int want = m->ssid_len && b.ssid_len == m->ssid_len && eq(b.ssid, m->ssid, m->ssid_len);
    int victim = -1;
    for (unsigned i = 0; i < m->nbss; ++i) {
        const wmlme_bss *q = &m->bss[i];
        if (m->ssid_len && q->ssid_len == m->ssid_len && eq(q->ssid, m->ssid, m->ssid_len)) continue;
        if (victim < 0 || q->rssi < m->bss[victim].rssi) victim = (int)i;
    }
    if (victim >= 0 && (want || m->bss[victim].rssi < b.rssi)) {m->bss[victim] = b; ++m->scan_evicted; ++m->scan_new;}
}

void wmlme_set_target(wmlme *m, const char *ssid)
{
    size_t n = 0;
    while (ssid && ssid[n] && n < sizeof m->ssid) ++n;
    m->ssid_len = (uint8_t)n; copy(m->ssid, ssid, n);
}

// Rank of a BSS under the band preference: higher is better (signal in dBm, plus a tier for the preferred band).
static int bss_rank(const wmlme *m, const wmlme_bss *b)
{
    const int five = b->freq > 3000, r = b->rssi;
    switch (m->band_pref) {
    case WMLME_BAND_AUTO: return five && r >= WMLME_5G_GOOD_DBM ? 1000 + r : r;
    case WMLME_BAND_2G: return five ? r : 1000 + r;
    case WMLME_BAND_5G: return five ? 1000 + r : r;
    default: return r;
    }
}

const wmlme_bss *wmlme_find_other(const wmlme *m, const char *ssid, const uint8_t skip[6])
{
    const size_t l = slen(ssid);
    const wmlme_bss *best = 0;
    for (unsigned i = 0; i < m->nbss; ++i) {
        const wmlme_bss *b = &m->bss[i];
        if (b->ssid_len != l || !eq(b->ssid, ssid, l) || (skip && eq(b->bssid, skip, 6))) continue;
        if (!best || (b->rsn_ok && !best->rsn_ok) || (b->rsn_ok == best->rsn_ok && bss_rank(m, b) > bss_rank(m, best))) best = b;
    }
    return best;
}

const wmlme_bss *wmlme_find(const wmlme *m, const char *ssid) {return wmlme_find_other(m, ssid, 0);}

void wmlme_count(const wmlme *m, const char *ssid, unsigned *n2g, unsigned *n5g)
{
    const size_t l = slen(ssid);
    *n2g = *n5g = 0;
    for (unsigned i = 0; i < m->nbss; ++i)
        if (m->bss[i].ssid_len == l && eq(m->bss[i].ssid, ssid, l)) ++*(m->bss[i].freq > 3000 ? n5g : n2g);
}

// ---- management frames ----
static uint8_t *mbuf_next(wmlme *m, uint64_t *iova)
{
    const unsigned i = m->mnext++ % WMLME_MGMT_BUFS;
    if (!m->mbuf[i].cpu) {
        m->mbuf[i].cpu = m->ops.dma_alloc ? m->ops.dma_alloc(m->ops.ctx, WMLME_MGMT_BYTES, &m->mbuf[i].iova) : 0;
        if (!m->mbuf[i].cpu) return 0;
    }
    *iova = m->mbuf[i].iova;
    return m->mbuf[i].cpu;
}
static int mgmt_send(wmlme *m, uint8_t fc0, const uint8_t *body, uint32_t blen)
{
    uint64_t iova;
    uint8_t *f = mbuf_next(m, &iova);
    if (!f || 24 + blen > WMLME_MGMT_BYTES) return WWMI_ERR_NOMEM;
    zero(f, 24);
    f[0] = fc0;
    copy(f + 4, m->target.bssid, 6);      // addr1: the AP
    copy(f + 10, m->mac, 6);              // addr2: us
    copy(f + 16, m->target.bssid, 6);     // addr3: BSSID
    p16(f + 22, (uint32_t)(m->seq++ & 0xfff) << 4);
    copy(f + 24, body, blen);
    ++m->mtx_sent;
    return wwmi_mgmt_tx(m->wmi, m->vdev_id, ++m->mdesc & 0xffff, f, 24 + blen, iova);
}

static int send_auth(wmlme *m)
{
    ++m->auth_tx;
    if (m->akm != WWPA_AKM_SAE) {
        uint8_t b[6] = {0, 0, 1, 0, 0, 0};    // open system, sequence 1, status success
        return mgmt_send(m, FC_AUTH, b, sizeof b);
    }
    // SAE (algorithm 3): Commit (sequence 1, with the anti-clogging token if the AP asked) or Confirm (sequence 2)
    uint8_t b[6 + WSAE_COMMIT_LEN + WSAE_TOKEN_MAX];
    b[0] = 3; b[1] = 0; b[2] = (uint8_t)(m->sae_step ? 2 : 1); b[3] = 0; b[4] = 0; b[5] = 0;
    const long n = m->sae_step ? wsae_write_confirm(&m->sae, b + 6, sizeof b - 6)
                               : wsae_write_commit(&m->sae, m->sae_token, m->sae_token_len, b + 6, sizeof b - 6);
    if (n < 0) return (int)n;
    if (m->sae_step) ++m->sae_confirms; else ++m->sae_commits;
    return mgmt_send(m, FC_AUTH, b, (uint32_t)(6 + n));
}

static int send_assoc(wmlme *m)
{
    uint8_t b[160];
    unsigned at = 0;
    uint16_t cap = CAP_ESS | CAP_PRIVACY | (m->target.capab & (CAP_SHORT_PREAMBLE | CAP_SHORT_SLOT));
    p16(b, cap); p16(b + 2, 10);          // capabilities, listen interval
    at = 4;
    b[at++] = IE_SSID; b[at++] = m->target.ssid_len; copy(b + at, m->target.ssid, m->target.ssid_len); at += m->target.ssid_len;
    const unsigned nr = m->target.n_rates < 8 ? m->target.n_rates : 8;
    b[at++] = IE_RATES; b[at++] = (uint8_t)nr; copy(b + at, m->target.rates, nr); at += nr;
    if (m->target.n_rates > 8) {
        b[at++] = IE_EXT_RATES; b[at++] = (uint8_t)(m->target.n_rates - 8); copy(b + at, m->target.rates + 8, m->target.n_rates - 8); at += m->target.n_rates - 8;
    }
    copy(b + at, m->own_rsne, 22); at += 22;
    ++m->assoc_tx;
    return mgmt_send(m, FC_ASSOC_REQ, b, at);
}

// ---- connect ----
static int hexval(char c) {return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;}

int wmlme_connect(wmlme *m, const char *ssid, const char *pass)
{
    if (m->state == WMLME_SCANNING || (m->state >= WMLME_VDEV_START && m->state <= WMLME_CONNECTED)) return WMLME_ERR_BUSY;
    const wmlme_bss *b = wmlme_find(m, ssid);
    if (b && m->have_avoid && eq(b->bssid, m->avoid, 6)) {   // the choice did not answer last time: the next best BSS of the SSID, if usable
        const wmlme_bss *o = wmlme_find_other(m, ssid, m->avoid);
        if (o && o->rsn_ok) b = o;
    }
    if (!b) {set_state(m, WMLME_FAILED, WMLME_REASON_NO_BSS); return WMLME_ERR_NOENT;}
    if (!b->rsn_ok) {set_state(m, WMLME_FAILED, WMLME_REASON_UNSUPPORTED); return WMLME_ERR_NOTSUP;}
    const size_t pl = slen(pass);
    m->have_pmk = 0;
    m->akm = b->akm; m->pmf = b->pmf != 0;   // PMF whenever the AP is capable: we are
    m->sae_step = 0; m->sae_token_len = 0; m->sae_status = 0;
    {   // our RSN element: group CCMP, pairwise CCMP, the AKM, MFPC (+ MFPR when the AP requires it)
        static const uint8_t base[22] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
        copy(m->own_rsne, base, sizeof base);
        m->own_rsne[19] = (uint8_t)m->akm;
        m->own_rsne[20] = (uint8_t)(b->pmf ? 0x80 | (b->pmf == 2 ? 0x40 : 0) : 0);
    }
    if (m->akm == WWPA_AKM_SAE) {
        // SAE: the PWE now (the AP's MAC is known); the PMK comes out of the exchange
        if (!pl || wsae_init(&m->sae, m->mac, b->bssid, (const uint8_t *)pass, pl, m->ops.random, m->ops.ctx)) return WMLME_ERR_INVAL;
    } else if (pl == 64) {
        for (unsigned i = 0; i < 32; ++i) {
            const int h = hexval(pass[2 * i]), l = hexval(pass[2 * i + 1]);
            if (h < 0 || l < 0) return WMLME_ERR_INVAL;
            m->pmk[i] = (uint8_t)(h << 4 | l);
        }
    } else if (wwpa_psk(pass, pl, (const uint8_t *)ssid, slen(ssid), m->pmk)) {
        return WMLME_ERR_INVAL;
    }
    m->have_pmk = m->akm != WWPA_AKM_SAE;
    m->target = *b;
    m->ssid_len = b->ssid_len; copy(m->ssid, b->ssid, b->ssid_len);
    m->retries = 0; m->keys_sent = m->ptk_installed = m->authorized = 0; m->key_events = 0; m->vdev_started = 0;
    if (m->have_pmk) wwpa_supp_init(&m->supp, m->pmk, b->bssid, m->mac, m->own_rsne, 22, b->rsn, b->rsn_len);
    // vdev start on the AP's channel, as ath10k_mac_op_assign_vif_chanctx does for a station
    wwmi_vdev_start_arg a;
    zero(&a, sizeof a);
    a.vdev_id = m->vdev_id;
    a.chan.freq = a.chan.center1 = b->freq;
    a.chan.mode = b->freq < 3000 ? WA_MODE_11G : WA_MODE_11A;
    a.chan.max_power = a.chan.reg_power = 40;
    set_state(m, WMLME_VDEV_START, 0);
    arm(m);
    const int rc = wwmi_vdev_start(m->wmi, &a, 0);
    if (rc) fail(m, WMLME_REASON_SEND);
    return rc;
}

int wmlme_disconnect(wmlme *m)
{
    if (m->state < WMLME_VDEV_START || m->state > WMLME_FAILED) return 0;
    if (m->state >= WMLME_ASSOCIATED && m->state <= WMLME_CONNECTED) wwmi_vdev_down(m->wmi, m->vdev_id);
    if (m->vdev_started) {
        wwmi_peer_delete(m->wmi, m->vdev_id, m->target.bssid);
        wwmi_vdev_stop(m->wmi, m->vdev_id);
    }
    m->vdev_started = 0;
    wwpa_supp_clear(&m->supp);
    set_state(m, WMLME_IDLE, 0);
    return 0;
}

static void legacy_rates(const wmlme_bss *b, uint8_t *out, uint32_t *n)
{
    *n = 0;
    for (unsigned i = 0; i < b->n_rates && *n < 32; ++i) {
        const uint8_t r = b->rates[i] & 0x7f;
        const int cck = r == 2 || r == 4 || r == 11 || r == 22;     // ath10k_mac_bitrate_is_cck
        if (b->freq > 3000 && cck) continue;
        out[(*n)++] = (uint8_t)(r | (cck ? 0x80 : 0));
    }
}

static void assoc_done(wmlme *m, uint32_t aid)
{
    m->aid = aid;
    wwmi_peer_assoc_arg pa;
    zero(&pa, sizeof pa);
    copy(pa.addr, m->target.bssid, 6);
    pa.vdev_id = m->vdev_id; pa.aid = aid;
    pa.flags = WA_WMI_TLV_PEER_AUTH | WA_WMI_TLV_PEER_NEED_PTK_4_WAY | (m->pmf ? WA_WMI_TLV_PEER_PMF : 0);   // 802.11w in the firmware
    pa.caps = CAP_ESS | CAP_PRIVACY | (m->target.capab & (CAP_SHORT_PREAMBLE | CAP_SHORT_SLOT));
    pa.listen_intval = 1; pa.nss = 1;
    pa.phymode = m->target.freq < 3000 ? WA_MODE_11G : WA_MODE_11A;
    legacy_rates(&m->target, pa.legacy_rates, &pa.n_legacy);
    int rc = wwmi_peer_assoc(m->wmi, &pa);
    if (!rc) rc = wwmi_vdev_up(m->wmi, m->vdev_id, aid, m->target.bssid);
    if (!rc) rc = wwmi_set_psmode(m->wmi, m->vdev_id, WA_WMI_STA_PS_MODE_DISABLED);
    if (rc) {fail(m, WMLME_REASON_SEND); return;}
    set_state(m, WMLME_ASSOCIATED, 0);
    arm(m);
}

static void mgmt_frame(wmlme *m, const wwmi_event *ev)
{
    const uint8_t *f = ev->data;
    const uint32_t len = ev->len;
    if (ev->d != WA_WMI_RX_STATUS_OK || len < 24) {++m->mgmt_bad; return;}
    const unsigned sub = f[0] & 0xfc;
    if (sub == FC_BEACON || sub == FC_PROBE_RESP) {scan_frame(m, ev); return;}
    if (!eq(f + 16, m->target.bssid, 6) || !eq(f + 4, m->mac, 6)) return;       // not our AP, not for us
    ++m->ap_rx[sub >> 4];
    if (sub == 0xd0 && len > 24) m->ap_last_cat = f[24];                        // action (SA Query = 8)
    if ((sub == FC_DEAUTH || sub == FC_DISASSOC) && len >= 26) m->ap_last_reason = (uint16_t)g16(f + 24);
    if (sub == FC_AUTH && m->state == WMLME_AUTH && m->akm == WWPA_AKM_SAE) {
        if (len < 24 + 6 || g16(f + 24) != 3) return;
        const unsigned seq = g16(f + 26), status = g16(f + 28);
        const uint8_t *body = f + 30;
        const uint32_t blen = len - 30;
        m->sae_status = status;
        if (seq == 1 && status == 76) {   // anti-clogging token required: group || token; commit again with it
            if (blen < 2 || blen - 2 > WSAE_TOKEN_MAX || m->sae_step) {fail(m, WMLME_REASON_AUTH); return;}
            m->sae_token_len = blen - 2;
            copy(m->sae_token, body + 2, m->sae_token_len);
            ++m->sae_tokens;
            m->retries = 0;
            arm(m);
            if (send_auth(m)) fail(m, WMLME_REASON_SEND);
            return;
        }
        if (status != 0) {fail(m, WMLME_REASON_AUTH); return;}
        if (seq == 1) {   // the AP's Commit: validate, derive the keys, send our Confirm
            if (m->sae_step) return;   // a retransmitted commit after ours was processed
            if (wsae_process_commit(&m->sae, body, blen)) {fail(m, WMLME_REASON_AUTH); return;}
            m->sae_step = 1;
            m->retries = 0;
            arm(m);
            if (send_auth(m)) fail(m, WMLME_REASON_SEND);
            return;
        }
        if (seq != 2 || !m->sae_step) return;
        if (wsae_check_confirm(&m->sae, body, blen)) {fail(m, WMLME_REASON_AUTH); return;}
        copy(m->pmk, m->sae.pmk, WWPA_PMK_LEN);
        m->have_pmk = 1;
        wsae_clear(&m->sae);
        wwpa_supp_init(&m->supp, m->pmk, m->target.bssid, m->mac, m->own_rsne, 22, m->target.rsn, m->target.rsn_len);
        wwpa_supp_set_akm(&m->supp, WWPA_AKM_SAE);
        m->retries = 0;
        set_state(m, WMLME_ASSOC, 0);
        arm(m);
        if (send_assoc(m)) fail(m, WMLME_REASON_SEND);
    } else if (sub == FC_AUTH && m->state == WMLME_AUTH) {
        if (len < 24 + 6 || g16(f + 24) != 0 || g16(f + 26) != 2) return;
        if (g16(f + 28) != 0) {fail(m, WMLME_REASON_AUTH); return;}
        m->retries = 0;
        set_state(m, WMLME_ASSOC, 0);
        arm(m);
        if (send_assoc(m)) fail(m, WMLME_REASON_SEND);
    } else if (sub == FC_ASSOC_RESP && m->state == WMLME_ASSOC) {
        if (len < 24 + 6) return;
        if (g16(f + 26) != 0) {fail(m, WMLME_REASON_ASSOC); return;}
        {   // the rates the AP accepted us with (supported + extended) are the peer's legacy rate set
            const uint8_t *ies = f + 30;
            const uint32_t ilen = len - 30;
            const uint8_t *r = ie_find(ies, ilen, IE_RATES), *x = ie_find(ies, ilen, IE_EXT_RATES);
            unsigned n = 0;
            uint8_t rates[WMLME_MAX_RATES];
            if (r) for (unsigned i = 0; i < r[1] && n < WMLME_MAX_RATES; ++i) rates[n++] = r[2 + i];
            if (x) for (unsigned i = 0; i < x[1] && n < WMLME_MAX_RATES; ++i) rates[n++] = x[2 + i];
            if (n) {copy(m->target.rates, rates, n); m->target.n_rates = (uint8_t)n;}
        }
        assoc_done(m, g16(f + 28) & 0x3fff);
    } else if ((sub == FC_DEAUTH || sub == FC_DISASSOC) && m->state >= WMLME_AUTH && m->state <= WMLME_CONNECTED) {
        fail(m, WMLME_REASON_DEAUTH);
    }
}

void wmlme_wmi_event(wmlme *m, const wwmi_event *ev)
{
    switch (ev->type) {
    case WWMI_EV_SCAN:
        if (ev->d != m->vdev_id) break;
        if (ev->a & (WA_WMI_SCAN_EVENT_COMPLETED | WA_WMI_SCAN_EVENT_START_FAILED)) {
            m->scan_running = 0;
            ++m->ev_scan_done;
            if (m->state == WMLME_SCANNING) set_state(m, WMLME_SCAN_DONE, 0);
        }
        break;
    case WWMI_EV_MGMT_RX: mgmt_frame(m, ev); break;
    case WWMI_EV_VDEV_START_RESP:
        if (m->state != WMLME_VDEV_START || ev->a != m->vdev_id) break;
        if (ev->b != 0) {fail(m, WMLME_REASON_VDEV_START); break;}
        m->vdev_started = 1;
        if (wwmi_peer_create(m->wmi, m->vdev_id, m->target.bssid, WA_WMI_TLV_PEER_TYPE_BSS) || send_auth(m)) {fail(m, WMLME_REASON_SEND); break;}
        set_state(m, WMLME_AUTH, 0);
        m->retries = 0;
        arm(m);
        break;
    case WWMI_EV_MGMT_TX_COMPL:
        ++m->mtx_status[ev->b < 4 ? ev->b : 4];
        m->mtx_last = ev->b;
        break;
    case WWMI_EV_INSTALL_KEY_COMPLETE:
        ++m->key_events;
        break;
    case WWMI_EV_PEER_KICKOUT:
        if (m->state >= WMLME_ASSOCIATED && m->state <= WMLME_CONNECTED) fail(m, WMLME_REASON_KICKOUT);
        break;
    default: break;
    }
}

// ---- the 4-way handshake ----
void wmlme_eapol_rx(wmlme *m, const uint8_t src[6], const uint8_t *eapol, uint32_t len)
{
    if ((m->state != WMLME_ASSOCIATED && m->state != WMLME_KEYS && m->state != WMLME_CONNECTED) || !eq(src, m->target.bssid, 6)) return;
    ++m->eapol_rx;
    uint8_t snonce[WWPA_NONCE_LEN], out[400];
    size_t olen = 0;
    m->ops.random(m->ops.ctx, snonce, sizeof snonce);
    const int r = wwpa_supp_rx(&m->supp, eapol, len, snonce, out, sizeof out, &olen);
    zero(snonce, sizeof snonce);
    if (r < 0) return;       // dropped: replay, MIC, RSNE mismatch ... (the AP will retry or time out)
    if (m->state == WMLME_ASSOCIATED) {set_state(m, WMLME_KEYS, 0); arm(m);}
    if (olen) {
        ++m->eapol_tx;
        if (m->ops.eapol_tx(m->ops.ctx, m->target.bssid, out, (uint32_t)olen)) {fail(m, WMLME_REASON_SEND); return;}
    }
    if (r == WWPA_DONE && !m->keys_sent) {
        // M4 is out: pairwise key for the AP's peer, then the group key (ath10k_set_key: macaddr = the BSSID for a station's GTK)
        wwmi_key_arg k;
        zero(&k, sizeof k);
        k.vdev_id = m->vdev_id; k.macaddr = m->target.bssid; k.key_idx = 0; k.key_flags = WA_WMI_KEY_PAIRWISE;
        k.key_cipher = WA_WMI_CIPHER_AES_CCM; k.key_len = 16; k.key = m->supp.ptk.tk;
        int rc = wwmi_vdev_install_key(m->wmi, &k);
        if (!rc) {
            m->ptk_installed = 1;
            if (m->supp.gtk_len == 16) {
                k.key_idx = m->supp.gtk_idx; k.key_flags = WA_WMI_KEY_GROUP; k.key = m->supp.gtk;
                rc = wwmi_vdev_install_key(m->wmi, &k);
            }
        }
        if (rc) {fail(m, WMLME_REASON_KEYS); return;}
        m->keys_sent = 1;
        arm(m);
    }
}

void wmlme_poll(wmlme *m)
{
    const uint32_t now = m->ops.now_ms(m->ops.ctx);
    if (m->state == WMLME_KEYS && m->keys_sent && !m->authorized && (m->key_events >= 2 || now >= m->step_at)) {
        // both keys are in (the target reported them, or the wait ran out as ath10k's 3 s one would): open the port
        if (wwmi_peer_set_param(m->wmi, m->vdev_id, m->target.bssid, WA_WMI_PEER_AUTHORIZE, 1)) {fail(m, WMLME_REASON_SEND); return;}
        m->authorized = 1;
        set_state(m, WMLME_CONNECTED, 0);
        return;
    }
    if (m->state < WMLME_VDEV_START || m->state > WMLME_KEYS || now < m->step_at) return;
    if (m->state == WMLME_AUTH || m->state == WMLME_ASSOC) {
        if (++m->retries > WMLME_RETRIES) {fail(m, m->state == WMLME_AUTH ? WMLME_REASON_AUTH : WMLME_REASON_ASSOC); return;}
        arm(m);
        if ((m->state == WMLME_AUTH ? send_auth(m) : send_assoc(m))) fail(m, WMLME_REASON_SEND);
        return;
    }
    // vdev start, association-to-M1 or the handshake did not complete in time
    fail(m, m->state == WMLME_VDEV_START ? WMLME_REASON_VDEV_START : WMLME_REASON_KEYS);
}
