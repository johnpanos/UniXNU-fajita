// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only FAT12/16 reader for the phone's own firmware partition (modem_a/modem_b: FAT16, 4096-byte sectors, the
// image/ directory with mba.mbn, modem.mdt + modem.bNN, wlanmdsp.mbn, bdwlan.NNN). Freestanding C over a block-read
// callback; nothing is ever written. Host-tested against a dumped partition (scripts/check-fajita-wifi-host.py --modem).
#ifndef FAJITA_WIFI_FAT_H
#define FAJITA_WIFI_FAT_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read `count` 4096-byte blocks of the partition starting at block `block` into dst; 0 on success.
typedef int (*wfat_read_fn)(void *ctx, uint64_t block, uint32_t count, void *dst);

typedef struct {
    wfat_read_fn read;
    void *ctx;
    uint64_t blocks;                 // partition size in 4096-byte blocks
    uint32_t sector, cluster_bytes;  // bytes per sector, bytes per cluster
    uint64_t fat_byte, root_byte, data_byte;   // byte offsets of FAT #1, the root directory, cluster 2
    uint32_t root_entries, clusters, fat_bits;
    uint8_t blk[4096];               // one-block cache
    uint64_t blk_no;                 // cached block, ~0 when empty
} wfat;

typedef struct {
    uint32_t first_cluster, size;
    int dir;
} wfat_file;

enum {WFAT_OK = 0, WFAT_ERR_IO = -1, WFAT_ERR_FORMAT = -2, WFAT_ERR_NOENT = -3, WFAT_ERR_RANGE = -4, WFAT_ERR_CHAIN = -5};

int wfat_mount(wfat *fs, wfat_read_fn read, void *ctx, uint64_t blocks);
// Path with '/' separators, 8.3 names, case-insensitive ("image/modem.b05").
int wfat_open(wfat *fs, const char *path, wfat_file *out);
// Read len bytes at off of the file into dst. Returns bytes read (short at EOF) or a negative WFAT_ERR.
long wfat_read(wfat *fs, const wfat_file *f, uint32_t off, void *dst, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif
