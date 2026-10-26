// SPDX-License-Identifier: BSD-2-Clause-Patent
// TFTP server over QRTR, after linux-msm/tqftpserv. See WifiTftp.h.
#include "WifiTftp.h"

#define MIN_BLKSIZE 8u
#define MAX_BLKSIZE 65464u
#define MAX_WSIZE 65535u
#define MAX_RSIZE (100u * 1024 * 1024)
#define MAX_TIMEOUTMS 255000u

static size_t slen(const char *s) {size_t n = 0; while (s[n]) ++n; return n;}
static size_t snlen(const char *s, size_t max) {size_t n = 0; while (n < max && s[n]) ++n; return n;}
static int prefix(const char *s, const char *p) {while (*p) if (*s++ != *p++) return 0; return 1;}
static int ieq(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int seq(const char *a, const char *b) {while (*a && *a == *b) {++a; ++b;} return *a == *b;}
static void scpy(char *d, const char *s, size_t cap) {size_t i = 0; for (; i + 1 < cap && s[i]; ++i) d[i] = s[i]; d[i] = 0;}
static void copy(uint8_t *d, const uint8_t *s, size_t n) {while (n--) *d++ = *s++;}

// strtoll(value, 10) with tqftpserv's checks: digits only, in [lo, hi].
static int parse_num(const char *v, uint64_t lo, uint64_t hi, uint64_t *out)
{
    uint64_t n = 0;
    if (!*v) return -1;
    for (; *v; ++v) {
        if (*v < '0' || *v > '9') return -1;
        n = n * 10 + (uint64_t)(*v - '0');
        if (n > 0xffffffffffffull) return -1;
    }
    if (n < lo || n > hi) return -1;
    *out = n;
    return 0;
}
static size_t utoa(char *out, uint64_t v)
{
    char tmp[24];
    size_t n = 0;
    do {tmp[n++] = (char)('0' + v % 10); v /= 10;} while (v);
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

// sanitize_path: one leading '/', no empty components, no "..".
static int sanitize(const char *path)
{
    const char *p = path;
    if (*p == '/') ++p;
    while (*p) {
        if (*p == '/') return -1;
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return -1;
        while (*p && *p != '/') ++p;
        if (*p) ++p;
    }
    return 0;
}

int wtftp_translate(const char *path, char *out, size_t cap)
{
    if (sanitize(path)) return -1;
    static const struct {const char *pre; size_t strip; int kind;} map[] = {
        {"/readonly/firmware/image/", 25, 0},
        {"/readonly/firmware/modem_pr", 19, 0},
        {"/readwrite/", 11, 1},
        {"/readonly/vendor/firmware_mnt/image/", 36, 0},
        {"/readonly/vendor/firmware/", 26, 0},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i) {
        if (!prefix(path, map[i].pre)) continue;
        const char *rest = path + map[i].strip;
        if (!*rest) return -1;
        if (map[i].kind == 1) {
            if (slen(rest) + 1 > cap) return -1;
            scpy(out, rest, cap);
            return 1;
        }
        // translate_readonly: <remoteproc firmware dir>/<rest>; the partition's image/ directory is that dir.
        if (slen(rest) + 7 > cap) return -1;
        scpy(out, "image/", cap);
        scpy(out + 6, rest, cap - 6);
        return 0;
    }
    return -1;
}

static void send_raw(wtftp *t, uint32_t from, uint32_t node, uint32_t port, const uint8_t *b, size_t n)
{
    if (wqrtr_send(t->r, from, node, port, b, n) < 0) ++t->errors;
}

static void send_error(wtftp *t, uint32_t from, uint32_t node, uint32_t port, unsigned code, const char *msg)
{
    uint8_t b[96];
    const size_t ml = slen(msg) < 80 ? slen(msg) : 80;
    b[0] = 0; b[1] = WTFTP_OP_ERROR; b[2] = (uint8_t)(code >> 8); b[3] = (uint8_t)code;
    copy(b + 4, (const uint8_t *)msg, ml);
    b[4 + ml] = 0;
    send_raw(t, from, node, port, b, 5 + ml);
    ++t->errors;
}

// tftp_send_error_to: an error before a transfer exists, from a temporary port.
static void send_error_to(wtftp *t, uint32_t node, uint32_t port, unsigned code, const char *msg)
{
    const uint32_t tmp = wqrtr_bind(t->r, 0, 0, 0);
    if (!tmp) return;
    send_error(t, tmp, node, port, code, msg);
    wqrtr_unbind(t->r, tmp);
}

static void close_client(wtftp *t, wtftp_client *c)
{
    if (!c->used) return;
    wqrtr_unbind(t->r, c->port);
    c->used = 0;
}

static wtftp_client *client_of_port(wtftp *t, uint32_t port)
{
    for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) if (t->c[i].used && t->c[i].port == port) return &t->c[i];
    return 0;
}

static long file_read(wtftp *t, wtftp_client *c, uint64_t off, uint8_t *dst, uint32_t len)
{
    if (c->ram >= 0) {
        const wtftp_ram *f = &t->ram[c->ram];
        if (off >= f->len) return 0;
        const uint32_t n = (uint32_t)(f->len - off < len ? f->len - off : len);
        copy(dst, f->data + off, n);
        return n;
    }
    if (off >= c->size) return 0;
    if (!t->fs.read) return -1;
    return t->fs.read(t->fs.ctx, c->cookie, (uint32_t)off, dst, len);
}

// tftp_send_data. Returns 1 sent, 0 held by flow control, -1 read error (sent ERROR), -2 rsize/short read refused.
static int send_data(wtftp *t, wtftp_client *c, uint32_t block, uint64_t offset, uint32_t response_size)
{
    uint8_t *b = t->blk;
    b[0] = 0; b[1] = WTFTP_OP_DATA; b[2] = (uint8_t)(block >> 8); b[3] = (uint8_t)block;
    const long n = file_read(t, c, offset, b + 4, c->blksize);
    if (n < 0) {send_error(t, c->port, c->node, c->rport, WTFTP_E_UNDEF, "Read error"); return -1;}
    size_t send_len = 4 + (size_t)n;
    if (response_size) {
        if (4 + (size_t)response_size > send_len) return -2;
        send_len = 4 + response_size;
    }
    const int r = wqrtr_send(t->r, c->port, c->node, c->rport, b, send_len);
    if (r == WQR_ERR_BUSY) return 0;
    if (r < 0) {++t->errors; return -2;}
    t->bytes += send_len - 4;
    return 1;
}

// The rest of handle_reader's window loop; resumable when QRTR flow control holds a block back.
static void pump(wtftp *t, wtftp_client *c)
{
    while (c->used && !c->writer && c->next < c->end) {
        const uint32_t block = c->next;
        const uint64_t offset = c->seek + (uint64_t)block * c->blksize;
        uint32_t response_size = 0;
        const int last = (uint64_t)(block + 1) * c->blksize > c->rsize;
        if (last) response_size = c->rsize % c->blksize;
        const int r = send_data(t, c, (block + 1) & 0xffff, offset, response_size);
        if (r == 0) return;           // flow control: resume in wtftp_poll
        if (r < 0) {c->end = c->next; return;}
        ++c->next;
        if (last) {c->end = c->next; return;}
    }
}

void wtftp_poll(wtftp *t)
{
    for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) if (t->c[i].used) pump(t, &t->c[i]);
}

static void client_rx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *b, size_t len, void *ctx);

static void send_oack(wtftp *t, wtftp_client *c, int tsize_valid, int rsize_valid, int seek_valid)
{
    uint8_t b[256];
    size_t at = 0;
    b[at++] = 0; b[at++] = WTFTP_OP_OACK;
    // tftp_send_oack, byte for byte (including the extra NUL after the blksize value).
    copy(b + at, (const uint8_t *)"blksize", 8); at += 8;
    at += utoa((char *)b + at, c->blksize) + 1;
    b[at++] = 0;
    copy(b + at, (const uint8_t *)"timeoutms", 10); at += 10;
    at += utoa((char *)b + at, c->timeoutms) + 1;
    if (tsize_valid) {copy(b + at, (const uint8_t *)"tsize", 6); at += 6; at += utoa((char *)b + at, c->size) + 1;}
    copy(b + at, (const uint8_t *)"wsize", 6); at += 6;
    at += utoa((char *)b + at, c->wsize) + 1;
    if (rsize_valid) {copy(b + at, (const uint8_t *)"rsize", 6); at += 6; at += utoa((char *)b + at, c->rsize) + 1;}
    if (seek_valid) {copy(b + at, (const uint8_t *)"seek", 5); at += 5; at += utoa((char *)b + at, c->seek) + 1;}
    send_raw(t, c->port, c->node, c->rport, b, at);
}

static int ram_file(wtftp *t, const char *name, int create)
{
    int free_slot = -1;
    for (int i = 0; i < WTFTP_RAM_FILES; ++i) {
        if (t->ram[i].used && seq(t->ram[i].name, name)) return i;
        if (!t->ram[i].used && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    wtftp_ram *f = &t->ram[free_slot];
    f->data = (uint8_t *)t->r->alloc(WTFTP_RAM_MAX, t->r->alloc_ctx);
    if (!f->data) return -1;
    f->used = 1; f->len = 0;
    scpy(f->name, name, sizeof f->name);
    return free_slot;
}

// handle_rrq / handle_wrq
static void request(wtftp *t, int write, const uint8_t *buf, size_t len, uint32_t node, uint32_t rport)
{
    const char *p = (const char *)buf + 2, *end = (const char *)buf + len;
    const size_t flen = snlen(p, (size_t)(end - p));
    if (flen == (size_t)(end - p)) {++t->errors; return;}   // filename not NUL-terminated
    const char *filename = p;
    p += flen + 1;
    if (p >= end) {++t->errors; return;}
    const size_t mlen = snlen(p, (size_t)(end - p));
    if (mlen == (size_t)(end - p)) {++t->errors; return;}
    const char *mode = p;
    p += mlen + 1;
    scpy(t->last, filename, sizeof t->last);
    if (!ieq(mode, "octet")) {send_error_to(t, node, rport, WTFTP_E_EBADOP, "Only octet mode supported"); return;}
    if (sanitize(filename)) {++t->refused; send_error_to(t, node, rport, WTFTP_E_EACCESS, "Access violation"); return;}
    char path[96];
    const int kind = wtftp_translate(filename, path, sizeof path);
    // options
    uint64_t blksize = 512, timeoutms = 1000, rsize = 0, wsize = 1, v;
    int tsize_req = 0, seek_set = 0, oack = 0;
    uint64_t seek = 0;
    if (p < end) {
        oack = 1;
        while (p < end) {
            const char *opt = p;
            const size_t ol = snlen(opt, (size_t)(end - p));
            if (ol == (size_t)(end - p)) goto bad_opt;
            p += ol + 1;
            if (p >= end) goto bad_opt;
            const char *val = p;
            const size_t vl = snlen(val, (size_t)(end - p));
            if (vl == (size_t)(end - p)) goto bad_opt;
            p += vl + 1;
            if (seq(opt, "blksize")) {if (parse_num(val, MIN_BLKSIZE, MAX_BLKSIZE, &blksize)) goto bad_opt;}
            else if (seq(opt, "timeoutms")) {if (parse_num(val, 1, MAX_TIMEOUTMS, &timeoutms)) goto bad_opt;}
            else if (seq(opt, "tsize")) {if (parse_num(val, 0, ~0ull, &v)) goto bad_opt; tsize_req = 1;}
            else if (seq(opt, "rsize")) {if (parse_num(val, 1, MAX_RSIZE, &rsize)) goto bad_opt;}
            else if (seq(opt, "wsize")) {if (parse_num(val, 1, MAX_WSIZE, &wsize)) goto bad_opt;}
            else if (seq(opt, "seek")) {if (parse_num(val, 0, ~0ull, &seek)) goto bad_opt; seek_set = 1;}
            // unknown options are ignored, as tqftpserv does
        }
    }
    if (blksize > WTFTP_MAX_BLKSIZE) blksize = WTFTP_MAX_BLKSIZE;   // the OACK tells the modem the smaller size
    if (write) ++t->wrq; else ++t->rrq;
    if (kind < 0) {
        ++t->refused;
        send_error_to(t, node, rport, write ? WTFTP_E_EACCESS : WTFTP_E_ENOENT, write ? "Access violation" : "file not found");
        return;
    }
    if (write && kind == 0) {   // the partition is never written
        ++t->refused;
        send_error_to(t, node, rport, WTFTP_E_EACCESS, "Access violation");
        return;
    }
    wtftp_client *c = 0;
    for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) if (!t->c[i].used) {c = &t->c[i]; break;}
    if (!c) {send_error_to(t, node, rport, WTFTP_E_UNDEF, "Resources temporarily unavailable"); return;}
    c->port = wqrtr_bind(t->r, 0, client_rx, t);
    if (!c->port) {send_error_to(t, node, rport, WTFTP_E_UNDEF, "Resources temporarily unavailable"); return;}
    c->used = 1; c->writer = write; c->node = node; c->rport = rport;
    c->blksize = (uint32_t)blksize; c->rsize = (uint32_t)rsize; c->wsize = (uint32_t)wsize;
    c->timeoutms = (uint32_t)timeoutms; c->seek = seek_set ? seek : 0;
    c->next = c->end = 0; c->blk_expected = 1; c->wpos = 0; c->ram = -1; c->size = 0;
    scpy(c->path, path, sizeof c->path);
    if (kind == 1) {
        c->ram = ram_file(t, path, write);
        if (c->ram < 0) {
            send_error(t, c->port, node, rport, write ? WTFTP_E_ENOSPACE : WTFTP_E_ENOENT, write ? "Disk full" : "file not found");
            close_client(t, c);
            return;
        }
        c->size = t->ram[c->ram].len;
    } else {
        if (!t->fs.open || t->fs.open(t->fs.ctx, path, &c->cookie, &c->size)) {
            send_error(t, c->port, node, rport, WTFTP_E_ENOENT, "file not found");
            close_client(t, c);
            return;
        }
    }
    if (oack) {
        send_oack(t, c, tsize_req, rsize != 0, seek_set);
    } else if (write) {
        uint8_t ack[4] = {0, WTFTP_OP_ACK, 0, 0};   // RFC 1350 (tqftpserv sends DATA here, which fails its own read)
        send_raw(t, c->port, node, rport, ack, 4);
    } else {
        c->next = 0; c->end = 1;
        uint32_t keep = c->rsize;
        c->rsize = 0xffffffffu;   // no rsize: block 1 is a full block
        pump(t, c);
        c->rsize = keep;
    }
    return;
bad_opt:
    send_error_to(t, node, rport, WTFTP_E_EOPTNEG, "Option negotiation failed");
}

static void client_rx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *b, size_t len, void *ctx)
{
    (void)r;
    wtftp *t = (wtftp *)ctx;
    wtftp_client *c = client_of_port(t, port);
    if (!c || node != c->node || sport != c->rport) return;   // spoofed / stale
    if (len < 4) {
        if (c->writer) send_error(t, c->port, c->node, c->rport, WTFTP_E_EBADOP, "Malformed packet");
        close_client(t, c);
        return;
    }
    const unsigned op = (unsigned)b[0] << 8 | b[1], blk = (unsigned)b[2] << 8 | b[3];
    if (!c->writer) {
        // handle_reader
        if (op == WTFTP_OP_ERROR) {close_client(t, c); return;}   // incl. END_OF_TRANSFER
        if (op != WTFTP_OP_ACK) {send_error(t, c->port, c->node, c->rport, WTFTP_E_EBADOP, "Expected ACK opcode"); close_client(t, c); return;}
        if ((uint64_t)blk * c->blksize > c->rsize) {close_client(t, c); return;}
        c->next = blk;
        c->end = blk + c->wsize;
        pump(t, c);
        return;
    }
    // handle_writer
    if (op != WTFTP_OP_DATA) {send_error(t, c->port, c->node, c->rport, WTFTP_E_EBADOP, "Expected DATA opcode"); close_client(t, c); return;}
    const size_t payload = len - 4;
    if (payload > c->blksize) {send_error(t, c->port, c->node, c->rport, WTFTP_E_EBADOP, "Malformed packet"); close_client(t, c); return;}
    if (blk != c->blk_expected) {
        send_error(t, c->port, c->node, c->rport, WTFTP_E_EBADOP, "Block number out of sequence");
        uint16_t e = c->blk_expected;
        if (e % c->wsize == 0) e = (uint16_t)(e - (c->wsize + 1));
        else e = (uint16_t)(e - ((e % c->wsize) - 1));
        c->blk_expected = e;
        close_client(t, c);   // handle_writer returns -1: the transfer is closed
        return;
    }
    ++c->blk_expected;
    wtftp_ram *f = &t->ram[c->ram];
    if ((uint64_t)c->wpos + payload > WTFTP_RAM_MAX) {
        send_error(t, c->port, c->node, c->rport, WTFTP_E_ENOSPACE, "Disk full or write error");
        close_client(t, c);
        return;
    }
    copy(f->data + c->wpos, b + 4, payload);
    c->wpos += (uint32_t)payload;
    if (c->wpos > f->len) f->len = c->wpos;
    t->bytes += payload;
    if (blk % c->wsize == 0 || payload < c->blksize) {
        uint8_t ack[4] = {0, WTFTP_OP_ACK, b[2], b[3]};
        send_raw(t, c->port, c->node, c->rport, ack, 4);
    }
    if (payload != c->blksize) close_client(t, c);
}

static void server_rx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *b, size_t len, void *ctx)
{
    (void)r; (void)port;
    wtftp *t = (wtftp *)ctx;
    if (len < 2) return;
    const unsigned op = (unsigned)b[0] << 8 | b[1];
    if (op == WTFTP_OP_RRQ) request(t, 0, b, len, node, sport);
    else if (op == WTFTP_OP_WRQ) request(t, 1, b, len, node, sport);
    else ++t->errors;
}

int wtftp_init(wtftp *t, wqrtr *r, const wtftp_fs *fs)
{
    for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) t->c[i].used = 0;
    for (unsigned i = 0; i < WTFTP_RAM_FILES; ++i) {t->ram[i].used = 0; t->ram[i].data = 0;}
    t->r = r;
    t->fs = *fs;
    t->rrq = t->wrq = t->errors = t->refused = 0;
    t->bytes = 0;
    t->last[0] = 0;
    t->port = wqrtr_bind(r, 0, server_rx, t);
    if (!t->port) return WQR_ERR_NOMEM;
    return wqrtr_publish(r, t->port, WTFTP_SERVICE, WTFTP_VERSION, WTFTP_INSTANCE);
}

void wtftp_event(wtftp *t, int what, uint32_t node, uint32_t port)
{
    for (unsigned i = 0; i < WTFTP_MAX_CLIENTS; ++i) {
        wtftp_client *c = &t->c[i];
        if (!c->used || c->node != node) continue;
        if (what == WQR_EV_BYE || (what == WQR_EV_DEL_CLIENT && c->rport == port)) close_client(t, c);
    }
}
