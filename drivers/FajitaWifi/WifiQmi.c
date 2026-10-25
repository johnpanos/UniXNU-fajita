// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WifiQmi.h"
#include <string.h>

// ---- header + TLV writer ----
void wqmi_begin(wqmi_writer *w, uint8_t *buf, size_t cap, uint8_t type, uint16_t txn, uint16_t msg_id)
{
    w->buf = buf; w->cap = cap; w->at = 0; w->err = 0;
    if (cap < WQMI_HDR_BYTES) {w->err = 1; return;}
    buf[0] = type;
    buf[1] = (uint8_t)txn; buf[2] = (uint8_t)(txn >> 8);
    buf[3] = (uint8_t)msg_id; buf[4] = (uint8_t)(msg_id >> 8);
    buf[5] = 0; buf[6] = 0;             // length, patched by wqmi_end
    w->at = WQMI_HDR_BYTES;
}

void wqmi_tlv(wqmi_writer *w, uint8_t type, const void *value, size_t len)
{
    if (w->err) return;
    if (len > 0xffff || w->at + 3 + len > w->cap) {w->err = 1; return;}
    w->buf[w->at++] = type;
    w->buf[w->at++] = (uint8_t)len;
    w->buf[w->at++] = (uint8_t)(len >> 8);
    memcpy(w->buf + w->at, value, len);
    w->at += len;
}

void wqmi_u8(wqmi_writer *w, uint8_t type, uint8_t v) {wqmi_tlv(w, type, &v, 1);}
void wqmi_u16(wqmi_writer *w, uint8_t type, uint16_t v) {uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; wqmi_tlv(w, type, b, 2);}
void wqmi_u32(wqmi_writer *w, uint8_t type, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    wqmi_tlv(w, type, b, 4);
}
void wqmi_u64(wqmi_writer *w, uint8_t type, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i));
    wqmi_tlv(w, type, b, 8);
}
void wqmi_result(wqmi_writer *w, uint16_t result, uint16_t error)
{
    uint8_t b[4] = {(uint8_t)result, (uint8_t)(result >> 8), (uint8_t)error, (uint8_t)(error >> 8)};
    wqmi_tlv(w, 0x02, b, 4);
}

long wqmi_end(wqmi_writer *w)
{
    if (w->err) return -1;
    const size_t body = w->at - WQMI_HDR_BYTES;
    w->buf[5] = (uint8_t)body; w->buf[6] = (uint8_t)(body >> 8);
    return (long)w->at;
}

// ---- reader ----
static uint16_t rd16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t rd64(const uint8_t *p) {uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i); return v;}

int wqmi_parse_hdr(const uint8_t *msg, size_t len, wqmi_hdr *h)
{
    if (len < WQMI_HDR_BYTES) return -1;
    h->type = msg[0];
    h->txn = rd16(msg + 1);
    h->msg_id = rd16(msg + 3);
    h->len = rd16(msg + 5);
    if ((size_t)h->len + WQMI_HDR_BYTES != len) return -1;
    return 0;
}

const uint8_t *wqmi_find(const uint8_t *msg, size_t len, uint8_t type, uint16_t *vlen)
{
    if (len < WQMI_HDR_BYTES) return NULL;
    size_t at = WQMI_HDR_BYTES;
    while (at + 3 <= len) {
        const uint8_t t = msg[at];
        const uint16_t l = rd16(msg + at + 1);
        if (at + 3 + l > len) return NULL;
        if (t == type) {if (vlen) *vlen = l; return msg + at + 3;}
        at += 3 + l;
    }
    return NULL;
}

int wqmi_get_result(const uint8_t *msg, size_t len, uint16_t *result, uint16_t *error)
{
    uint16_t vlen = 0;
    const uint8_t *v = wqmi_find(msg, len, 0x02, &vlen);
    if (!v || vlen < 4) return -1;
    if (result) *result = rd16(v);
    if (error) *error = rd16(v + 2);
    return 0;
}

// ---- wlfw requests ----
long wlfw_ind_register_req(uint8_t *buf, size_t cap, uint16_t txn)
{
    // Only the set optional fields appear, each as one value TLV (the EI OPT_FLAG emits no bytes). ath10k sets
    // fw_ready_enable (0x10), msa_ready_enable (0x13) and client_id (0x15).
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WLFW_IND_REGISTER);
    wqmi_u8(&w, 0x10, 1);                              // fw_ready_enable
    wqmi_u8(&w, 0x13, 1);                              // msa_ready_enable
    wqmi_u32(&w, 0x15, WLFW_CLIENT_ID);                // client_id
    return wqmi_end(&w);
}

long wlfw_host_cap_8bit_req(uint8_t *buf, size_t cap, uint16_t txn)
{
    // The SDM845 8-bit quirk: daemon_support as a single u8 at TLV 0x10 (wlfw_host_cap_8bit_req_msg_v01_ei).
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WLFW_HOST_CAP);
    wqmi_u8(&w, 0x10, 0);                              // daemon_support = 0 (no userspace daemon)
    return wqmi_end(&w);
}

long wlfw_msa_info_req(uint8_t *buf, size_t cap, uint16_t txn, uint64_t msa_addr, uint32_t size)
{
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WLFW_MSA_INFO);
    wqmi_u64(&w, 0x01, msa_addr);
    wqmi_u32(&w, 0x02, size);
    return wqmi_end(&w);
}

long wlfw_empty_req(uint8_t *buf, size_t cap, uint16_t txn, uint16_t msg_id)
{
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, msg_id);
    return wqmi_end(&w);
}

long wlfw_bdf_req(uint8_t *buf, size_t cap, uint16_t txn, uint32_t total, uint32_t seg, const uint8_t *data, uint16_t len, int end)
{
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WLFW_BDF_DOWNLOAD);
    wqmi_u8(&w, 0x01, 1);                              // valid (required u8)
    wqmi_u32(&w, 0x10, 0);                             // file_id = 0 (QMI_WLFW_CAL_TEMP_IDX_0)
    wqmi_u32(&w, 0x11, total);                         // total_size
    wqmi_u32(&w, 0x12, seg);                           // seg_id
    // data (TLV 0x13): DATA_LEN u16 + bytes
    if (!w.err && len <= WLFW_MAX_DATA) {
        if (w.at + 3 + 2 + len <= w.cap) {
            w.buf[w.at++] = 0x13;
            const uint16_t tlvlen = (uint16_t)(2 + len);
            w.buf[w.at++] = (uint8_t)tlvlen; w.buf[w.at++] = (uint8_t)(tlvlen >> 8);
            w.buf[w.at++] = (uint8_t)len; w.buf[w.at++] = (uint8_t)(len >> 8);
            memcpy(w.buf + w.at, data, len); w.at += len;
        } else w.err = 1;
    } else w.err = 1;
    wqmi_u8(&w, 0x14, end ? 1 : 0);                    // end
    return wqmi_end(&w);
}

long wlfw_cal_report_req(uint8_t *buf, size_t cap, uint16_t txn)
{
    // No cal data on fajita: an empty meta list (DATA_LEN u8 = 0 under TLV 0x01).
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WLFW_CAL_REPORT);
    if (!w.err && w.at + 4 <= w.cap) {
        w.buf[w.at++] = 0x01; w.buf[w.at++] = 1; w.buf[w.at++] = 0; w.buf[w.at++] = 0;   // TLV 0x01, len 1, count 0
    } else w.err = 1;
    return wqmi_end(&w);
}

long wlfw_wlan_mode_req(uint8_t *buf, size_t cap, uint16_t txn, uint32_t mode)
{
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WLFW_WLAN_MODE);
    wqmi_u32(&w, 0x01, mode);
    wqmi_u8(&w, 0x10, 0);                              // hw_debug = 0
    return wqmi_end(&w);
}

long wlfw_wlan_cfg_req(uint8_t *buf, size_t cap, uint16_t txn, const wlfw_ce_tgt *tgt, unsigned ntgt, const wlfw_ce_svc *svc,
                       unsigned nsvc, const wlfw_shadow *shadow, unsigned nshadow)
{
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_REQUEST, txn, WLFW_WLAN_CFG);
    // host_version (TLV 0x10) omitted. tgt_cfg (TLV 0x11): DATA_LEN u8 then struct array of 5 u32 each.
    if (ntgt > WLFW_MAX_CE) ntgt = WLFW_MAX_CE;
    uint8_t tmp[WLFW_MAX_CE * 20 + 1];
    tmp[0] = (uint8_t)ntgt;
    for (unsigned i = 0; i < ntgt; ++i) {
        uint8_t *p = tmp + 1 + i * 20;
        const uint32_t f[5] = {tgt[i].pipe_num, tgt[i].pipe_dir, tgt[i].nentries, tgt[i].nbytes_max, tgt[i].flags};
        for (int k = 0; k < 5; ++k) {p[k*4] = (uint8_t)f[k]; p[k*4+1] = (uint8_t)(f[k]>>8); p[k*4+2] = (uint8_t)(f[k]>>16); p[k*4+3] = (uint8_t)(f[k]>>24);}
    }
    wqmi_tlv(&w, 0x11, tmp, 1 + ntgt * 20);
    if (nsvc > WLFW_MAX_SVC) nsvc = WLFW_MAX_SVC;
    uint8_t stmp[WLFW_MAX_SVC * 12 + 1];
    stmp[0] = (uint8_t)nsvc;
    for (unsigned i = 0; i < nsvc; ++i) {
        uint8_t *p = stmp + 1 + i * 12;
        const uint32_t f[3] = {svc[i].service_id, svc[i].pipe_dir, svc[i].pipe_num};
        for (int k = 0; k < 3; ++k) {p[k*4] = (uint8_t)f[k]; p[k*4+1] = (uint8_t)(f[k]>>8); p[k*4+2] = (uint8_t)(f[k]>>16); p[k*4+3] = (uint8_t)(f[k]>>24);}
    }
    wqmi_tlv(&w, 0x12, stmp, 1 + nsvc * 12);
    if (nshadow > WLFW_MAX_SHADOW) nshadow = WLFW_MAX_SHADOW;
    uint8_t htmp[WLFW_MAX_SHADOW * 4 + 1];
    htmp[0] = (uint8_t)nshadow;
    for (unsigned i = 0; i < nshadow; ++i) {
        uint8_t *p = htmp + 1 + i * 4;
        p[0] = (uint8_t)shadow[i].id; p[1] = (uint8_t)(shadow[i].id >> 8);
        p[2] = (uint8_t)shadow[i].offset; p[3] = (uint8_t)(shadow[i].offset >> 8);
    }
    wqmi_tlv(&w, 0x13, htmp, 1 + nshadow * 4);
    return wqmi_end(&w);
}

// ---- wlfw response parsers ----
int wlfw_parse_ind_register_resp(const uint8_t *msg, size_t len, wlfw_ind_register_resp *out)
{
    memset(out, 0, sizeof *out);
    if (wqmi_get_result(msg, len, &out->result, &out->error)) return -1;
    uint16_t vlen = 0;
    const uint8_t *v = wqmi_find(msg, len, 0x10, &vlen);
    if (v && vlen >= 8) {out->fw_status_valid = 1; out->fw_status = rd64(v);}
    return 0;
}

int wlfw_parse_msa_info_resp(const uint8_t *msg, size_t len, wlfw_msa_info_resp *out)
{
    memset(out, 0, sizeof *out);
    if (wqmi_get_result(msg, len, &out->result, &out->error)) return -1;
    uint16_t vlen = 0;
    const uint8_t *v = wqmi_find(msg, len, 0x03, &vlen);
    if (!v || vlen < 1) return 0;
    unsigned count = v[0];
    if (count > WLFW_MAX_MEM_REG) count = WLFW_MAX_MEM_REG;
    if ((size_t)1 + count * 13u > vlen) return -1;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *p = v + 1 + i * 13;
        out->region[i].addr = rd64(p);
        out->region[i].size = rd32(p + 8);
        out->region[i].secure = p[12];
    }
    out->count = count;
    return 0;
}

static void copy_str(char *dst, size_t dstcap, const uint8_t *v, uint16_t vlen)
{
    size_t n = vlen < dstcap - 1 ? vlen : dstcap - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

int wlfw_parse_cap_resp(const uint8_t *msg, size_t len, wlfw_cap_resp *out)
{
    memset(out, 0, sizeof *out);
    if (wqmi_get_result(msg, len, &out->result, &out->error)) return -1;
    uint16_t vlen = 0;
    const uint8_t *v;
    if ((v = wqmi_find(msg, len, 0x10, &vlen)) && vlen >= 8) {out->chip_valid = 1; out->chip_id = rd32(v); out->chip_family = rd32(v + 4);}
    if ((v = wqmi_find(msg, len, 0x11, &vlen)) && vlen >= 4) {out->board_valid = 1; out->board_id = rd32(v);}
    if ((v = wqmi_find(msg, len, 0x12, &vlen)) && vlen >= 4) {out->soc_valid = 1; out->soc_id = rd32(v);}
    if ((v = wqmi_find(msg, len, 0x13, &vlen)) && vlen >= 4) {out->fw_valid = 1; out->fw_version = rd32(v); copy_str(out->fw_timestamp, sizeof out->fw_timestamp, v + 4, vlen > 4 ? vlen - 4 : 0);}
    if ((v = wqmi_find(msg, len, 0x14, &vlen))) {out->build_valid = 1; copy_str(out->build_id, sizeof out->build_id, v, vlen);}
    return 0;
}

// ---- servreg locator server (pd-mapper) ----
unsigned servreg_sdm845_lookup(const char *service, wqmi_domain *out, unsigned cap)
{
    // qcom_pd_mapper.c sdm845_domains: the modem's root_pd (instance 180) and wlan_pd (also 180). The modem's wlfw lives in
    // wlan_pd, which provides kernel/elf_loader, tms/servreg and wlan/fw. GET_DOMAIN_LIST(name) returns the domains whose
    // service set contains `name`.
    static const struct {const char *svc; const char *domain; uint32_t instance;} table[] = {
        {"tms/servreg", "msm/modem/root_pd", 180},
        {"kernel/elf_loader", "msm/modem/wlan_pd", 180},
        {"tms/servreg", "msm/modem/wlan_pd", 180},
        {"wlan/fw", "msm/modem/wlan_pd", 180},
    };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof table / sizeof table[0] && n < cap; ++i) {
        if (!strcmp(service, table[i].svc)) {out[n].name = table[i].domain; out[n].instance = table[i].instance; ++n;}
    }
    return n;
}

long servreg_answer(const uint8_t *req, size_t len, uint8_t *buf, size_t cap, const wqmi_domain *domains, unsigned ndomains)
{
    wqmi_hdr h;
    if (wqmi_parse_hdr(req, len, &h) || h.msg_id != SERVREG_GET_DOMAIN_LIST) return -1;
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_RESPONSE, h.txn, SERVREG_GET_DOMAIN_LIST);
    wqmi_result(&w, WQMI_RESULT_SUCCESS, WQMI_ERR_NONE);
    wqmi_u16(&w, 0x10, (uint16_t)ndomains);   // total_domains
    wqmi_u16(&w, 0x11, 1);                     // db_revision
    // domain_list (TLV 0x12): DATA_LEN u8 then entries.
    // servreg_location_entry: string name (u8 len + chars), u32 instance, u8 service_data_valid, u32 service_data.
    if (ndomains) {
        {
            size_t lenpos = w.at;
            // we build the value in place: [count u8][entries...]
            if (w.at + 3 >= w.cap) {w.err = 1;}
            else {
                w.buf[w.at] = 0x12;      // type
                w.at += 3;               // leave 2 bytes for tlv length
                size_t vstart = w.at;
                w.buf[w.at++] = (uint8_t)ndomains;
                for (unsigned i = 0; i < ndomains && !w.err; ++i) {
                    size_t namelen = strlen(domains[i].name);
                    if (w.at + 1 + namelen + 4 + 1 + 4 > w.cap || namelen > 64) {w.err = 1; break;}
                    w.buf[w.at++] = (uint8_t)namelen;
                    memcpy(w.buf + w.at, domains[i].name, namelen); w.at += namelen;
                    const uint32_t inst = domains[i].instance;
                    w.buf[w.at++] = (uint8_t)inst; w.buf[w.at++] = (uint8_t)(inst>>8); w.buf[w.at++] = (uint8_t)(inst>>16); w.buf[w.at++] = (uint8_t)(inst>>24);
                    w.buf[w.at++] = 0;   // service_data_valid
                    w.buf[w.at++] = 0; w.buf[w.at++] = 0; w.buf[w.at++] = 0; w.buf[w.at++] = 0;   // service_data
                }
                const size_t vlen = w.at - vstart;
                w.buf[lenpos + 1] = (uint8_t)vlen; w.buf[lenpos + 2] = (uint8_t)(vlen >> 8);
            }
        }
    }
    return wqmi_end(&w);
}

// ---- rmtfs server ----
int rmtfs_parse_req(const uint8_t *msg, size_t len, rmtfs_req *out)
{
    memset(out, 0, sizeof *out);
    wqmi_hdr h;
    if (wqmi_parse_hdr(msg, len, &h) || h.type != WQMI_REQUEST) return -1;
    out->msg_id = h.msg_id; out->txn = h.txn;
    uint16_t vlen = 0;
    const uint8_t *v;
    switch (h.msg_id) {
    case RMTFS_OPEN:
        if ((v = wqmi_find(msg, len, 0x01, &vlen))) copy_str(out->path, sizeof out->path, v, vlen);
        return 0;
    case RMTFS_CLOSE:
    case RMTFS_GET_DEV_ERROR:
        if ((v = wqmi_find(msg, len, 0x01, &vlen)) && vlen >= 4) out->caller_id = rd32(v);
        return 0;
    case RMTFS_ALLOC_BUFF:
        if ((v = wqmi_find(msg, len, 0x01, &vlen)) && vlen >= 4) out->caller_id = rd32(v);
        if ((v = wqmi_find(msg, len, 0x02, &vlen)) && vlen >= 4) out->buff_size = rd32(v);
        return 0;
    case RMTFS_RW_IOVEC:
        if ((v = wqmi_find(msg, len, 0x01, &vlen)) && vlen >= 4) out->caller_id = rd32(v);
        if ((v = wqmi_find(msg, len, 0x02, &vlen)) && vlen >= 1) out->direction = v[0];
        if ((v = wqmi_find(msg, len, 0x03, &vlen)) && vlen >= 1) {
            unsigned n = v[0];
            if (n > 32) n = 32;
            if ((size_t)1 + n * 12u <= vlen) {
                for (unsigned i = 0; i < n; ++i) {
                    const uint8_t *p = v + 1 + i * 12;
                    out->iov[i].sector = rd32(p); out->iov[i].offset = rd32(p + 4); out->iov[i].count = rd32(p + 8);
                }
                out->niov = n;
            }
        }
        if ((v = wqmi_find(msg, len, 0x04, &vlen)) && vlen >= 1) out->force_sync = v[0];
        return 0;
    default:
        return -1;
    }
}

long rmtfs_resp(uint8_t *buf, size_t cap, const rmtfs_req *req, uint16_t result, uint16_t error, uint32_t caller_id,
                uint64_t buff_addr)
{
    wqmi_writer w;
    wqmi_begin(&w, buf, cap, WQMI_RESPONSE, req->txn, req->msg_id);
    wqmi_result(&w, result, error);
    if (req->msg_id == RMTFS_OPEN && result == WQMI_RESULT_SUCCESS) wqmi_u32(&w, 0x10, caller_id);
    if (req->msg_id == RMTFS_ALLOC_BUFF && result == WQMI_RESULT_SUCCESS) wqmi_u64(&w, 0x10, buff_addr);
    return wqmi_end(&w);
}
