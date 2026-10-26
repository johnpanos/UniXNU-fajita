// SPDX-License-Identifier: BSD-2-Clause-Patent
// The rmtfs QMI server (service 14 v1 i0), after linux-msm/rmtfs (rmtfs.c, storage.c, sharedmem.c): the modem opens its
// EFS partitions by path (/boot/modem_fs1 -> modemst1, ...), gets the shared rmtfs buffer's physical address, and moves
// 512-byte sectors between the partition and that buffer with RW_IOVEC. READ-ONLY: reads come from the named-partition
// reader (NativeUfs on the phone, the verified .cache/efs-backup images in the host tests); every write is refused with
// QMI_RMTFS_ERR_INTERNAL and counted, and nothing is ever written to storage. Freestanding, host-tested.
#ifndef FAJITA_WIFI_RMTFS_H
#define FAJITA_WIFI_RMTFS_H
#include <stddef.h>
#include <stdint.h>
#include "WifiRouter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WRMTFS_SERVICE 14u
#define WRMTFS_VERSION 1u
#define WRMTFS_INSTANCE 0u
#define WRMTFS_SECTOR 512u
#define WRMTFS_MAX_CALLERS 10
#define WRMTFS_ERR_INTERNAL 1u
#define WRMTFS_ERR_MALFORMED 2u

// Read `len` bytes at byte offset `off` of partition `name`. Returns bytes read (short or 0 past the end), negative on error.
typedef long (*wrmtfs_read_fn)(void *ctx, const char *name, uint64_t off, void *dst, uint32_t len);
// The partition's size in bytes, or negative if it does not exist.
typedef long long (*wrmtfs_size_fn)(void *ctx, const char *name);

typedef struct {
    wqrtr *r;
    uint32_t port;
    wrmtfs_read_fn read;
    wrmtfs_size_fn size;
    void *ctx;
    volatile uint8_t *buf;    // the mapped rmtfs shared memory
    uint64_t buf_phys;        // its physical address (FW_RMTFS_PHYS), what the modem is told
    uint32_t buf_size;
    struct {int used; uint32_t node; const char *part; const char *path;} caller[WRMTFS_MAX_CALLERS];
    // counters for the FW4 row
    uint32_t opens, iovecs, sectors_read, writes_refused, errors;
    char last_path[32];
} wrmtfs;

int wrmtfs_init(wrmtfs *s, wqrtr *r, wrmtfs_read_fn read, wrmtfs_size_fn size, void *ctx, volatile uint8_t *buf,
                uint64_t buf_phys, uint32_t buf_size);
// Answer one request (also used directly by the tests). Returns the response length, or -1 if not a request.
long wrmtfs_handle(wrmtfs *s, uint32_t node, const uint8_t *req, size_t len, uint8_t *out, size_t cap);
// The partition a modem path maps to (storage.c partition_table), NULL if unknown.
const char *wrmtfs_partition(const char *path);
// BYE from the modem: its callers are forgotten.
void wrmtfs_event(wrmtfs *s, int what, uint32_t node);

#ifdef __cplusplus
}
#endif
#endif
