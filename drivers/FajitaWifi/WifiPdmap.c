// SPDX-License-Identifier: BSD-2-Clause-Patent
// pd-mapper (servreg locator server), after linux-msm/pd-mapper. See WifiPdmap.h.
#include "WifiPdmap.h"
#include "WifiQmi.h"

static size_t slen(const char *s) {size_t n = 0; while (s[n]) ++n; return n;}
static int seq(const char *a, const char *b) {while (*a && *a == *b) {++a; ++b;} return *a == *b;}

void wpdm_init(wpdm *p)
{
    p->count = 0;
    p->requests = p->pfr = p->names_more = p->list_cut = 0;
}

// ---- a minimal JSON reader: enough for the .jsn schema (objects, arrays, strings, integers, true/false/null) ----
typedef struct {
    const char *s, *end;
    int err, depth;
    // what we collect
    char soc[24], domain[24], subdomain[24];
    int have_instance;
    uint32_t instance;
    unsigned nsvc;
    char provider[8][24], service[8][24];
} jrd;

static void ws(jrd *j) {while (j->s < j->end && (*j->s == ' ' || *j->s == '\n' || *j->s == '\r' || *j->s == '\t')) ++j->s;}
static int lit(jrd *j, char c) {ws(j); if (j->s < j->end && *j->s == c) {++j->s; return 1;} return 0;}

// A string into out (truncation is an error; escapes other than \" \\ \/ are refused).
static int jstr(jrd *j, char *out, size_t cap)
{
    ws(j);
    if (j->s >= j->end || *j->s != '"') return -1;
    ++j->s;
    size_t n = 0;
    while (j->s < j->end && *j->s != '"') {
        char c = *j->s++;
        if (c == '\\') {
            if (j->s >= j->end) return -1;
            c = *j->s++;
            if (c != '"' && c != '\\' && c != '/') return -1;
        }
        if ((unsigned char)c < 0x20) return -1;
        if (out) {if (n + 1 >= cap) return -1; out[n] = c;}
        ++n;
    }
    if (j->s >= j->end) return -1;
    ++j->s;
    if (out) out[n] = 0;
    return 0;
}

static int jnum(jrd *j, uint32_t *v)
{
    ws(j);
    uint64_t n = 0;
    int digits = 0;
    if (j->s < j->end && *j->s == '-') return -1;
    while (j->s < j->end && *j->s >= '0' && *j->s <= '9') {
        n = n * 10 + (uint64_t)(*j->s++ - '0');
        if (n > 0xffffffffu) return -1;
        ++digits;
    }
    if (!digits) return -1;
    if (j->s < j->end && (*j->s == '.' || *j->s == 'e' || *j->s == 'E')) return -1;
    if (v) *v = (uint32_t)n;
    return 0;
}

enum {CTX_NONE, CTX_DOMAIN, CTX_SERVICES, CTX_SERVICE};
static int jvalue(jrd *j, int ctx, const char *key);

static int jobject(jrd *j, int ctx)
{
    if (++j->depth > 8) return -1;
    if (!lit(j, '{')) return -1;
    if (lit(j, '}')) {--j->depth; return 0;}
    for (;;) {
        char key[24];
        if (jstr(j, key, sizeof key)) return -1;
        if (!lit(j, ':')) return -1;
        int sub = CTX_NONE;
        if (ctx == CTX_NONE && j->depth == 1 && seq(key, "sr_domain")) sub = CTX_DOMAIN;
        if (ctx == CTX_NONE && j->depth == 1 && seq(key, "sr_service")) sub = CTX_SERVICES;
        if (sub != CTX_NONE) {if (jvalue(j, sub, key)) return -1;}
        else if (jvalue(j, ctx, key)) return -1;
        if (lit(j, ',')) continue;
        if (lit(j, '}')) break;
        return -1;
    }
    --j->depth;
    return 0;
}

static int jvalue(jrd *j, int ctx, const char *key)
{
    ws(j);
    if (j->s >= j->end) return -1;
    const char c = *j->s;
    if (c == '{') {
        if (ctx == CTX_SERVICES) {
            if (j->nsvc >= 8) return -1;
            j->provider[j->nsvc][0] = j->service[j->nsvc][0] = 0;
            if (jobject(j, CTX_SERVICE)) return -1;
            ++j->nsvc;
            return 0;
        }
        return jobject(j, ctx == CTX_DOMAIN ? CTX_DOMAIN : CTX_NONE);
    }
    if (c == '[') {
        ++j->s;
        if (lit(j, ']')) return 0;
        for (;;) {
            if (jvalue(j, ctx == CTX_SERVICES ? CTX_SERVICES : CTX_NONE, 0)) return -1;
            if (lit(j, ',')) continue;
            if (lit(j, ']')) return 0;
            return -1;
        }
    }
    if (c == '"') {
        char *dst = 0;
        size_t cap = 24;
        if (ctx == CTX_DOMAIN && key) {
            if (seq(key, "soc")) dst = j->soc;
            else if (seq(key, "domain")) dst = j->domain;
            else if (seq(key, "subdomain")) dst = j->subdomain;
        } else if (ctx == CTX_SERVICE && key) {
            if (seq(key, "provider")) dst = j->provider[j->nsvc];
            else if (seq(key, "service")) dst = j->service[j->nsvc];
        }
        return jstr(j, dst, cap);
    }
    if (c >= '0' && c <= '9') {
        uint32_t v;
        if (jnum(j, &v)) return -1;
        if (ctx == CTX_DOMAIN && key && seq(key, "qmi_instance_id")) {j->instance = v; j->have_instance = 1;}
        return 0;
    }
    static const char *const words[] = {"true", "false", "null"};
    for (unsigned w = 0; w < 3; ++w) {
        const size_t n = slen(words[w]);
        if ((size_t)(j->end - j->s) >= n) {
            size_t k = 0;
            while (k < n && j->s[k] == words[w][k]) ++k;
            if (k == n) {j->s += n; return 0;}
        }
    }
    return -1;
}

static int join(char *out, size_t cap, const char *const parts[], unsigned n)
{
    size_t at = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (i) {if (at + 1 >= cap) return -1; out[at++] = '/';}
        for (const char *s = parts[i]; *s; ++s) {if (at + 1 >= cap) return -1; out[at++] = *s;}
    }
    out[at] = 0;
    return 0;
}

int wpdm_add_jsn(wpdm *p, const char *json, size_t len)
{
    jrd j;
    j.s = json; j.end = json + len; j.err = 0; j.depth = 0;
    j.soc[0] = j.domain[0] = j.subdomain[0] = 0;
    j.have_instance = 0; j.instance = 0; j.nsvc = 0;
    if (jobject(&j, CTX_NONE)) return -1;
    ws(&j);
    while (j.s < j.end && *j.s == 0) ++j.s;   // tolerate a NUL-padded file tail
    if (j.s != j.end) return -1;
    if (!j.soc[0] || !j.domain[0] || !j.subdomain[0] || !j.have_instance || !j.nsvc) return -1;
    if (p->count + j.nsvc > WPDM_MAX_MAPS) return -1;
    for (unsigned i = 0; i < j.nsvc; ++i) {
        if (!j.provider[i][0] || !j.service[i][0]) return -1;
        wpdm_map *m = &p->map[p->count + i];
        const char *svc[] = {j.provider[i], j.service[i]}, *dom[] = {j.soc, j.domain, j.subdomain};
        if (join(m->service, sizeof m->service, svc, 2) || join(m->domain, sizeof m->domain, dom, 3)) return -1;
        m->instance = j.instance;
    }
    p->count += j.nsvc;
    return (int)j.nsvc;
}

// GET_DOMAIN_LIST response (servreg_loc.qmi): result, total_domains (0x10), db_revision (0x11), domain_list (0x12:
// u8 count, then entries of u8-length-prefixed name, u32 instance, u8 service_data_valid, u32 service_data).
long wpdm_handle(wpdm *p, const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    wqmi_hdr h;
    if (wqmi_parse_hdr(req, len, &h) || h.type != WQMI_REQUEST) return -1;
    wqmi_writer w;
    if (h.msg_id == WPDM_PFR) {
        ++p->pfr;
        wqmi_begin(&w, out, cap, WQMI_RESPONSE, h.txn, WPDM_PFR);
        wqmi_result(&w, WQMI_RESULT_SUCCESS, WQMI_ERR_NONE);
        return wqmi_end(&w);
    }
    if (h.msg_id != WPDM_GET_DOMAIN_LIST) return -1;
    ++p->requests;
    wqmi_begin(&w, out, cap, WQMI_RESPONSE, h.txn, WPDM_GET_DOMAIN_LIST);
    uint16_t vlen = 0;
    const uint8_t *v = wqmi_find(req, len, 0x01, &vlen);
    if (!v || !vlen || vlen >= WPDM_NAME) {
        wqmi_result(&w, WQMI_RESULT_FAILURE, 2 /* QMI_ERR_MALFORMED_MSG (servreg_loc.qmi) */);
        return wqmi_end(&w);
    }
    char name[WPDM_NAME];
    for (uint16_t i = 0; i < vlen; ++i) name[i] = (char)v[i];
    name[vlen] = 0;
    uint32_t offset = 0;
    const uint8_t *o = wqmi_find(req, len, 0x10, &vlen);
    if (o && vlen == 4) offset = (uint32_t)o[0] | (uint32_t)o[1] << 8 | (uint32_t)o[2] << 16 | (uint32_t)o[3] << 24;
    unsigned total = 0;
    for (unsigned i = 0; i < p->count; ++i) total += seq(p->map[i].service, name);
    wqmi_result(&w, WQMI_RESULT_SUCCESS, WQMI_ERR_NONE);
    wqmi_u16(&w, 0x10, (uint16_t)total);
    wqmi_u16(&w, 0x11, 1);
    uint8_t list[1 + 32 * (1 + WPDM_NAME + 9)];
    size_t at = 1;
    unsigned n = 0, seen = 0;
    for (unsigned i = 0; i < p->count && n < 32; ++i) {
        if (!seq(p->map[i].service, name)) continue;
        if (seen++ < offset) continue;
        const size_t dl = slen(p->map[i].domain);
        list[at++] = (uint8_t)dl;
        for (size_t k = 0; k < dl; ++k) list[at++] = (uint8_t)p->map[i].domain[k];
        const uint32_t inst = p->map[i].instance;
        for (int k = 0; k < 4; ++k) list[at++] = (uint8_t)(inst >> 8 * k);
        list[at++] = 0;
        for (int k = 0; k < 4; ++k) list[at++] = 0;
        ++n;
    }
    list[0] = (uint8_t)n;
    if (n == 32 && total > offset + n) ++p->list_cut;
    if (p->nnames >= 6) ++p->names_more;
    if (p->nnames < 6) {
        char *d = p->names[p->nnames++];
        size_t k = 0;
        for (size_t i = 0; name[i] && k < 36; ++i) d[k++] = name[i];
        d[k++] = '@'; d[k++] = (char)('0' + offset % 10); d[k++] = '='; d[k++] = (char)('0' + n % 10); d[k++] = '/'; d[k++] = (char)('0' + total % 10); d[k] = 0;
    }
    if (n) wqmi_tlv(&w, 0x12, list, at);
    return wqmi_end(&w);
}
