// SPDX-License-Identifier: BSD-2-Clause-Patent
// rmtfs server, read-only, after linux-msm/rmtfs. See WifiRmtfs.h.
#include "WifiRmtfs.h"
#include "WifiQmi.h"

static int seq(const char *a, const char *b) {while (*a && *a == *b) {++a; ++b;} return *a == *b;}
static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

const char *wrmtfs_partition(const char *path)
{
    static const struct {const char *path, *part;} table[] = {
        {"/boot/modem_fs1", "modemst1"}, {"/boot/modem_fs2", "modemst2"}, {"/boot/modem_fsc", "fsc"},
        {"/boot/modem_fsg", "fsg"}, {"/boot/modem_study", "study"}, {"/boot/modem_tunning", "tunning"},
        {"/boot/modem_tng", "tunning"}, {"/oem/nvbk/static", "oem_stanvbk"}, {"/oem/nvbk/dynamic", "oem_dycnvbk"},
        {"/oppo/oem_partion", "oem_stanvbk"},
    };
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; ++i) if (seq(path, table[i].path)) return table[i].part;
    return 0;
}

static void serve(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *b, size_t len, void *ctx)
{
    wrmtfs *s = (wrmtfs *)ctx;
    uint8_t out[256];
    const long n = wrmtfs_handle(s, node, b, len, out, sizeof out);
    if (n > 0 && wqrtr_send(r, port, node, sport, out, (size_t)n) < 0) ++s->errors;
}

int wrmtfs_init(wrmtfs *s, wqrtr *r, wrmtfs_read_fn read, wrmtfs_size_fn size, void *ctx, volatile uint8_t *buf,
                uint64_t buf_phys, uint32_t buf_size)
{
    s->r = r; s->read = read; s->size = size; s->ctx = ctx;
    s->buf = buf; s->buf_phys = buf_phys; s->buf_size = buf_size;
    for (unsigned i = 0; i < WRMTFS_MAX_CALLERS; ++i) s->caller[i].used = 0;
    s->opens = s->iovecs = s->sectors_read = s->writes_refused = s->errors = s->callers_full = 0;
    s->last_path[0] = 0;
    s->port = 0;
    if (!r) return 0;   // handler-only use (tests)
    s->port = wqrtr_bind(r, 0, serve, s);
    if (!s->port) return WQR_ERR_NOMEM;
    return wqrtr_publish(r, s->port, WRMTFS_SERVICE, WRMTFS_VERSION, WRMTFS_INSTANCE);
}

void wrmtfs_event(wrmtfs *s, int what, uint32_t node)
{
    if (what != WQR_EV_BYE) return;
    for (unsigned i = 0; i < WRMTFS_MAX_CALLERS; ++i) if (s->caller[i].node == node) s->caller[i].used = 0;
}

static long respond(uint8_t *out, size_t cap, uint16_t txn, uint16_t id, uint16_t error, int has_u32, uint32_t u32,
                    int has_u64, uint64_t u64)
{
    wqmi_writer w;
    wqmi_begin(&w, out, cap, WQMI_RESPONSE, txn, id);
    wqmi_result(&w, error ? WQMI_RESULT_FAILURE : WQMI_RESULT_SUCCESS, error);
    if (has_u32) wqmi_u32(&w, 0x10, u32);
    if (has_u64) wqmi_u64(&w, 0x10, u64);
    return wqmi_end(&w);
}

static int caller_ok(const wrmtfs *s, uint32_t node, uint32_t id)
{
    return id < WRMTFS_MAX_CALLERS && s->caller[id].used && s->caller[id].node == node;
}

long wrmtfs_handle(wrmtfs *s, uint32_t node, const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    wqmi_hdr h;
    if (wqmi_parse_hdr(req, len, &h) || h.type != WQMI_REQUEST) return -1;
    uint16_t vl = 0;
    const uint8_t *v;
    switch (h.msg_id) {
    case 1: {   // OPEN
        v = wqmi_find(req, len, 0x01, &vl);
        if (!v || !vl || vl >= 64) {++s->errors; return respond(out, cap, h.txn, 1, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);}
        char path[64];
        for (uint16_t i = 0; i < vl; ++i) path[i] = (char)v[i];
        path[vl] = 0;
        for (unsigned i = 0; i < sizeof s->last_path - 1 && i <= vl; ++i) s->last_path[i] = path[i];
        s->last_path[sizeof s->last_path - 1] = 0;
        ++s->opens;
        const char *part = wrmtfs_partition(path);
        if (!part || !s->size || s->size(s->ctx, part) < 0) {++s->errors; return respond(out, cap, h.txn, 1, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);}
        // storage_open: the same node reopening a partition gets its caller id back
        int id = -1;
        for (unsigned i = 0; i < WRMTFS_MAX_CALLERS; ++i)
            if (s->caller[i].used && s->caller[i].node == node && seq(s->caller[i].part, part)) {id = (int)i; break;}
        for (unsigned i = 0; i < WRMTFS_MAX_CALLERS && id < 0; ++i) if (!s->caller[i].used) id = (int)i;
        if (id < 0) {++s->errors; ++s->callers_full; return respond(out, cap, h.txn, 1, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);}
        s->caller[id].used = 1; s->caller[id].node = node; s->caller[id].part = part;
        return respond(out, cap, h.txn, 1, 0, 1, (uint32_t)id, 0, 0);
    }
    case 2: {   // CLOSE
        v = wqmi_find(req, len, 0x01, &vl);
        if (!v || vl != 4) return respond(out, cap, h.txn, 2, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);
        const uint32_t id = rd32(v);
        if (!caller_ok(s, node, id)) return respond(out, cap, h.txn, 2, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);
        s->caller[id].used = 0;
        return respond(out, cap, h.txn, 2, 0, 0, 0, 0, 0);
    }
    case 3: {   // RW_IOVEC
        ++s->iovecs;
        const uint8_t *cid = wqmi_find(req, len, 0x01, &vl);
        if (!cid || vl != 4) {++s->errors; return respond(out, cap, h.txn, 3, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);}
        const uint8_t *dir = wqmi_find(req, len, 0x02, &vl);
        if (!dir || vl != 1) {++s->errors; return respond(out, cap, h.txn, 3, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);}
        uint16_t il = 0;
        const uint8_t *iov = wqmi_find(req, len, 0x03, &il);
        if (!iov || !il || il != 1u + iov[0] * 12u) {++s->errors; return respond(out, cap, h.txn, 3, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);}
        const uint32_t id = rd32(cid);
        if (!caller_ok(s, node, id)) {++s->errors; return respond(out, cap, h.txn, 3, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);}
        if (dir[0]) {   // write: refused, never reaches storage
            ++s->writes_refused;
            return respond(out, cap, h.txn, 3, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);
        }
        const char *part = s->caller[id].part;
        uint8_t sec[WRMTFS_SECTOR];
        for (unsigned e = 0; e < iov[0]; ++e) {
            const uint8_t *p = iov + 1 + e * 12;
            const uint64_t sector = rd32(p), phys = rd32(p + 4), count = rd32(p + 8);
            for (uint64_t j = 0; j < count; ++j) {
                const uint64_t at = phys + j * WRMTFS_SECTOR;
                if (at < s->buf_phys || at + WRMTFS_SECTOR > s->buf_phys + s->buf_size || !s->buf) {
                    ++s->errors;
                    return respond(out, cap, h.txn, 3, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);
                }
                long n = s->read(s->ctx, part, (sector + j) * WRMTFS_SECTOR, sec, WRMTFS_SECTOR);
                if (n < 0) {++s->errors; return respond(out, cap, h.txn, 3, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);}
                for (long k = n; k < (long)WRMTFS_SECTOR; ++k) sec[k] = 0;   // past the end: zeros
                volatile uint8_t *dst = s->buf + (at - s->buf_phys);
                if (((uintptr_t)dst & 3) == 0)
                    for (unsigned k = 0; k < WRMTFS_SECTOR; k += 4) *(volatile uint32_t *)(dst + k) = rd32(sec + k);
                else
                    for (unsigned k = 0; k < WRMTFS_SECTOR; ++k) dst[k] = sec[k];
                ++s->sectors_read;
            }
        }
        return respond(out, cap, h.txn, 3, 0, 0, 0, 0, 0);
    }
    case 4: {   // ALLOC_BUFF: the one shared buffer, if it is large enough (rmtfs_mem_alloc)
        const uint8_t *cid = wqmi_find(req, len, 0x01, &vl);
        if (!cid || vl != 4) return respond(out, cap, h.txn, 4, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);
        const uint8_t *sz = wqmi_find(req, len, 0x02, &vl);
        if (!sz || vl != 4) return respond(out, cap, h.txn, 4, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);
        if (rd32(sz) > s->buf_size || !s->buf) {++s->errors; return respond(out, cap, h.txn, 4, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);}
        return respond(out, cap, h.txn, 4, 0, 0, 0, 1, s->buf_phys);
    }
    case 5: {   // GET_DEV_ERROR: no device errors on a read-only backing
        v = wqmi_find(req, len, 0x01, &vl);
        if (!v || vl != 4) return respond(out, cap, h.txn, 5, WRMTFS_ERR_MALFORMED, 0, 0, 0, 0);
        if (!caller_ok(s, node, rd32(v))) return respond(out, cap, h.txn, 5, WRMTFS_ERR_INTERNAL, 0, 0, 0, 0);
        wqmi_writer w;
        wqmi_begin(&w, out, cap, WQMI_RESPONSE, h.txn, 5);
        wqmi_result(&w, WQMI_RESULT_SUCCESS, 0);
        wqmi_u8(&w, 0x10, 0);
        return wqmi_end(&w);
    }
    default:
        return -1;
    }
}
