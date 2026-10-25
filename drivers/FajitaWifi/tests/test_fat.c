// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of WifiFat.c against a dumped firmware partition (ufspart.py backup modem_b): extracts the named files into
// OUTDIR so scripts/check-fajita-wifi-host.py can compare them with mtools' copies, and checks the MDT split layout.
//   test_fat IMAGE OUTDIR PATH...
#include "../WifiFat.h"
#include "../WifiMdt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *img;
static int rd(void *ctx, uint64_t block, uint32_t count, void *dst)
{
    (void)ctx;
    if (fseek(img, (long)(block * 4096), SEEK_SET)) return -1;
    return fread(dst, 4096, count, img) == count ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {fprintf(stderr, "usage: test_fat IMAGE OUTDIR PATH...\n"); return 2;}
    img = fopen(argv[1], "rb");
    if (!img) {perror(argv[1]); return 2;}
    fseek(img, 0, SEEK_END);
    const uint64_t blocks = (uint64_t)ftell(img) / 4096;
    static wfat fs;
    int r = wfat_mount(&fs, rd, NULL, blocks);
    if (r) {printf("FAIL mount %d\n", r); return 1;}
    printf("fat%u: sector %u cluster %u clusters %u\n", fs.fat_bits, fs.sector, fs.cluster_bytes, fs.clusters);
    int fails = 0;
    for (int i = 3; i < argc; ++i) {
        wfat_file f;
        r = wfat_open(&fs, argv[i], &f);
        if (r) {printf("FAIL open %s: %d\n", argv[i], r); ++fails; continue;}
        uint8_t *buf = malloc(f.size ? f.size : 1);
        // Read in odd-sized pieces to exercise the cluster walk and the block cache.
        uint32_t at = 0;
        while (at < f.size) {
            uint32_t n = f.size - at < 70001 ? f.size - at : 70001;
            long got = wfat_read(&fs, &f, at, buf + at, n);
            if (got != (long)n) {printf("FAIL read %s at %u: %ld\n", argv[i], at, got); ++fails; break;}
            at += n;
        }
        const char *base = strrchr(argv[i], '/');
        char out[4096];
        snprintf(out, sizeof out, "%s/%s", argv[2], base ? base + 1 : argv[i]);
        FILE *o = fopen(out, "wb");
        if (o) {fwrite(buf, 1, f.size, o); fclose(o);}
        if (strstr(argv[i], ".mdt")) {
            wmdt_image m;
            r = wmdt_parse(buf, f.size, &m);
            printf("  %s: mdt parse %d phnum %u hash %d meta %ld reloc %d span %x..%x\n", argv[i], r, m.phnum, m.hash_index,
                   wmdt_metadata_size(&m), m.relocatable, m.min_addr, m.max_addr);
            if (r) ++fails;
        }
        printf("  %s: %u bytes\n", argv[i], f.size);
        free(buf);
    }
    wfat_file none;
    if (wfat_open(&fs, "image/nosuch.bin", &none) != WFAT_ERR_NOENT) {printf("FAIL missing file found\n"); ++fails;}
    printf(fails ? "FAIL: fat\n" : "PASS: fat\n");
    return fails ? 1 : 0;
}
