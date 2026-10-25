// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WifiFat.h"
#include <string.h>

static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static uint32_t le32(const uint8_t *p) {return le16(p) | le16(p + 2) << 16;}

// Copy len bytes at partition byte offset `at` into dst, through the one-block cache.
static int bytes_at(wfat *fs, uint64_t at, void *dst, uint32_t len)
{
    uint8_t *out = (uint8_t *)dst;
    while (len) {
        const uint64_t block = at / 4096;
        const uint32_t in = (uint32_t)(at % 4096);
        if (block >= fs->blocks) return WFAT_ERR_RANGE;
        // Whole aligned blocks go straight to the caller.
        if (!in && len >= 4096) {
            uint32_t n = len / 4096;
            if (block + n > fs->blocks) n = (uint32_t)(fs->blocks - block);
            if (n > 32) n = 32;
            if (fs->read(fs->ctx, block, n, out)) return WFAT_ERR_IO;
            out += (size_t)n * 4096; at += (uint64_t)n * 4096; len -= n * 4096;
            continue;
        }
        if (fs->blk_no != block) {
            if (fs->read(fs->ctx, block, 1, fs->blk)) {fs->blk_no = ~0ull; return WFAT_ERR_IO;}
            fs->blk_no = block;
        }
        uint32_t n = 4096 - in;
        if (n > len) n = len;
        memcpy(out, fs->blk + in, n);
        out += n; at += n; len -= n;
    }
    return WFAT_OK;
}

int wfat_mount(wfat *fs, wfat_read_fn read, void *ctx, uint64_t blocks)
{
    memset(fs, 0, sizeof *fs);
    fs->read = read; fs->ctx = ctx; fs->blocks = blocks; fs->blk_no = ~0ull;
    uint8_t b[64];
    if (bytes_at(fs, 0, b, sizeof b)) return WFAT_ERR_IO;
    const uint32_t sector = le16(b + 11), per = b[13], reserved = le16(b + 14), fats = b[16], roots = le16(b + 17);
    uint32_t total = le16(b + 19);
    if (!total) total = le32(b + 32);
    const uint32_t fat_sectors = le16(b + 22);
    if ((sector != 512 && sector != 1024 && sector != 2048 && sector != 4096) || !per || (per & (per - 1)) || !reserved ||
        !fats || !roots || !fat_sectors || !total)
        return WFAT_ERR_FORMAT;   // FAT32 (fat_sectors 0) is not needed here
    fs->sector = sector;
    fs->cluster_bytes = sector * per;
    fs->root_entries = roots;
    fs->fat_byte = (uint64_t)reserved * sector;
    fs->root_byte = fs->fat_byte + (uint64_t)fats * fat_sectors * sector;
    const uint64_t root_bytes = ((uint64_t)roots * 32 + sector - 1) / sector * sector;
    fs->data_byte = fs->root_byte + root_bytes;
    const uint64_t data_sectors = (uint64_t)total - fs->data_byte / sector;
    fs->clusters = (uint32_t)(data_sectors / per);
    fs->fat_bits = fs->clusters < 4085 ? 12 : 16;
    if (fs->clusters >= 65525 || (uint64_t)total * sector > blocks * 4096) return WFAT_ERR_FORMAT;
    return WFAT_OK;
}

static int next_cluster(wfat *fs, uint32_t c, uint32_t *next)
{
    uint8_t e[2];
    if (fs->fat_bits == 16) {
        if (bytes_at(fs, fs->fat_byte + (uint64_t)c * 2, e, 2)) return WFAT_ERR_IO;
        *next = le16(e);
        if (*next >= 0xfff8) *next = 0;
    } else {
        if (bytes_at(fs, fs->fat_byte + c + c / 2, e, 2)) return WFAT_ERR_IO;
        uint32_t v = le16(e);
        v = c & 1 ? v >> 4 : v & 0xfff;
        *next = v >= 0xff8 ? 0 : v;
    }
    if (*next == 1 || (*next && (*next < 2 || *next >= fs->clusters + 2))) return WFAT_ERR_CHAIN;
    return WFAT_OK;
}

// "modem.b05" -> "MODEM   B05"
static int name83(const char *s, size_t len, char out[11])
{
    memset(out, ' ', 11);
    size_t i = 0, o = 0;
    for (; i < len && s[i] != '.'; ++i) {if (o >= 8) return 0; out[o++] = s[i];}
    if (i < len) {
        ++i;
        for (o = 8; i < len; ++i) {if (o >= 11) return 0; out[o++] = s[i];}
    }
    for (o = 0; o < 11; ++o) if (out[o] >= 'a' && out[o] <= 'z') out[o] = (char)(out[o] - 32);
    return 1;
}

// Byte offset of the n-th 32-byte entry of a directory (root when first_cluster is 0); 0 when past its end.
static int dir_entry(wfat *fs, uint32_t first_cluster, uint32_t n, uint64_t *at)
{
    if (!first_cluster) {
        if (n >= fs->root_entries) return WFAT_ERR_NOENT;
        *at = fs->root_byte + (uint64_t)n * 32;
        return WFAT_OK;
    }
    const uint32_t per = fs->cluster_bytes / 32;
    uint32_t c = first_cluster;
    for (uint32_t k = n / per; k; --k) {
        uint32_t next;
        int r = next_cluster(fs, c, &next);
        if (r) return r;
        if (!next) return WFAT_ERR_NOENT;
        c = next;
    }
    *at = fs->data_byte + (uint64_t)(c - 2) * fs->cluster_bytes + (uint64_t)(n % per) * 32;
    return WFAT_OK;
}

static int lookup(wfat *fs, uint32_t dir_cluster, const char *name, size_t len, wfat_file *out)
{
    char want[11];
    if (!name83(name, len, want)) return WFAT_ERR_NOENT;
    for (uint32_t n = 0; n < 65536; ++n) {
        uint64_t at;
        int r = dir_entry(fs, dir_cluster, n, &at);
        if (r == WFAT_ERR_NOENT) return r;
        if (r) return r;
        uint8_t e[32];
        if (bytes_at(fs, at, e, 32)) return WFAT_ERR_IO;
        if (e[0] == 0) return WFAT_ERR_NOENT;
        if (e[0] == 0xe5 || (e[11] & 0x0f) == 0x0f || (e[11] & 0x08)) continue;   // deleted, LFN, volume label
        if (memcmp(e, want, 11)) continue;
        out->first_cluster = le16(e + 26);
        out->size = le32(e + 28);
        out->dir = (e[11] & 0x10) != 0;
        return WFAT_OK;
    }
    return WFAT_ERR_NOENT;
}

int wfat_open(wfat *fs, const char *path, wfat_file *out)
{
    uint32_t dir = 0;
    const char *p = path;
    for (;;) {
        while (*p == '/') ++p;
        size_t len = 0;
        while (p[len] && p[len] != '/') ++len;
        if (!len) return WFAT_ERR_NOENT;
        int r = lookup(fs, dir, p, len, out);
        if (r) return r;
        p += len;
        while (*p == '/') ++p;
        if (!*p) return WFAT_OK;
        if (!out->dir) return WFAT_ERR_NOENT;
        dir = out->first_cluster;
    }
}

long wfat_read(wfat *fs, const wfat_file *f, uint32_t off, void *dst, uint32_t len)
{
    if (f->dir) return WFAT_ERR_NOENT;
    if (off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;
    uint32_t c = f->first_cluster, done = 0;
    for (uint32_t k = off / fs->cluster_bytes; k; --k) {
        uint32_t next;
        int r = next_cluster(fs, c, &next);
        if (r) return r;
        if (!next) return WFAT_ERR_CHAIN;
        c = next;
    }
    uint32_t in = off % fs->cluster_bytes;
    while (done < len) {
        if (c < 2 || c >= fs->clusters + 2) return WFAT_ERR_CHAIN;
        // Extend over physically contiguous clusters so large files read in big pieces.
        uint32_t run = 1, last = c;
        while ((uint64_t)run * fs->cluster_bytes - in < len - done) {
            uint32_t next;
            int r = next_cluster(fs, last, &next);
            if (r) return r;
            if (next != last + 1) break;
            last = next; ++run;
        }
        uint64_t span = (uint64_t)run * fs->cluster_bytes - in;
        uint32_t n = span < len - done ? (uint32_t)span : len - done;
        int r = bytes_at(fs, fs->data_byte + (uint64_t)(c - 2) * fs->cluster_bytes + in, (uint8_t *)dst + done, n);
        if (r) return r;
        done += n;
        in = 0;
        if (done < len) {
            uint32_t next;
            r = next_cluster(fs, last, &next);
            if (r) return r;
            if (!next) return WFAT_ERR_CHAIN;
            c = next;
        }
    }
    return (long)done;
}
