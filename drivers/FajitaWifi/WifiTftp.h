// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AP's TFTP server over QRTR (service 4096 v1 i0), after linux-msm/tqftpserv (tqftpserv.c, translate.c): the modem
// fetches its firmware (wlanmdsp.mbn, ...) with RRQ + options (blksize, tsize, rsize, wsize, seek, timeoutms), one QRTR
// port per transfer. /readonly/... paths are served read-only from the phone's modem partition (image/<name>, through a
// file-system callback: the FAT reader on the phone); /readwrite/... is a small RAM store that is never written to
// storage. Freestanding, host-tested with recorded request sequences.
#ifndef FAJITA_WIFI_TFTP_H
#define FAJITA_WIFI_TFTP_H
#include <stddef.h>
#include <stdint.h>
#include "WifiRouter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WTFTP_SERVICE 4096u
#define WTFTP_VERSION 1u
#define WTFTP_INSTANCE 0u
#define WTFTP_MAX_CLIENTS 6
#define WTFTP_MAX_BLKSIZE 8192u        // offered at most; the GLINK fifo is 16 KiB (tqftpserv allows 65464)
#define WTFTP_RAM_FILES 4
#define WTFTP_RAM_MAX (64u << 10)

enum {WTFTP_OP_RRQ = 1, WTFTP_OP_WRQ, WTFTP_OP_DATA, WTFTP_OP_ACK, WTFTP_OP_ERROR, WTFTP_OP_OACK};
enum {WTFTP_E_UNDEF = 0, WTFTP_E_ENOENT = 1, WTFTP_E_EACCESS = 2, WTFTP_E_ENOSPACE = 3, WTFTP_E_EBADOP = 4,
      WTFTP_E_EOPTNEG = 8, WTFTP_E_END_OF_TRANSFER = 9};

// Read-only file source. `path` is relative to the partition root ("image/wlanmdsp.mbn").
typedef struct {
    int (*open)(void *ctx, const char *path, uint64_t *cookie, uint32_t *size);              // 0 or negative
    long (*read)(void *ctx, uint64_t cookie, uint32_t off, void *dst, uint32_t len);         // bytes, short at EOF
    void *ctx;
} wtftp_fs;

typedef struct {
    int used, writer;
    uint32_t port;                   // our per-transfer port
    uint32_t node, rport;            // the modem's
    uint64_t cookie;
    uint32_t size;
    int ram;                         // RAM store index, or -1 (a partition file)
    uint32_t blksize, rsize, wsize, timeoutms;
    uint64_t seek;
    // reader: blocks [next, end) of the current window still to send (QRTR flow control can pause it)
    uint32_t next, end;
    // writer
    uint16_t blk_expected;
    uint32_t wpos;
    char path[96];
} wtftp_client;

typedef struct {
    char name[64];
    uint8_t *data;
    uint32_t len;
    int used;
} wtftp_ram;

typedef struct {
    wqrtr *r;
    uint32_t port;
    wtftp_fs fs;
    wtftp_client c[WTFTP_MAX_CLIENTS];
    wtftp_ram ram[WTFTP_RAM_FILES];
    uint8_t blk[4 + WTFTP_MAX_BLKSIZE];
    // counters and the last request, for the FW4 log row
    uint32_t rrq, wrq, errors, refused;
    uint32_t busy;                   // requests refused because all WTFTP_MAX_CLIENTS sessions (or QRTR ports) were in use
    uint32_t max_blksize;                        // 0 = WTFTP_MAX_BLKSIZE; lower for a peer whose receive intents are small
    char hist[32][96]; unsigned nhist; int cur;  // every request: "R|W path bBLK rRSIZE [Ecode]" (first 32), for the FW4 history rows
    uint32_t tx_pkts, tx_fail, data_in, other_in, last_in_op;   // packets sent / failed to send / DATA received / other client packets
    uint64_t bytes;
    char last[96];
} wtftp;

// Bind the server port and publish service 4096. Returns 0 or a negative error.
int wtftp_init(wtftp *t, wqrtr *r, const wtftp_fs *fs);
// Send what QRTR flow control held back (call after wqrtr_input/flush).
void wtftp_poll(wtftp *t);
// Name-service events (wire wqrtr.event through this): BYE / DEL_CLIENT end that client's transfers.
void wtftp_event(wtftp *t, int what, uint32_t node, uint32_t port);
// Map a request path to the partition path; 0 = read-only partition, 1 = RAM store, -1 = refused.
int wtftp_translate(const char *path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
