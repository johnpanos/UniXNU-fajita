// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the fajita Wi-Fi stack above a booted modem: SMEM/SMP2P, GLINK over SMEM, QRTR and the AP services
// (pd-mapper, TFTP, rmtfs), the wlfw client, the ath10k CE/HTC/WMI path, the WPA2 supplicant and the AOSS QMP link-up. Built and run by
// scripts/check-fajita-wifi-host.py (ASan/UBSan). Usage: test_stack [modem-partition.img [efs-backup-dir [board-2.bin [smem-snapshot.bin]]]]
#include "tcheck.h"
#include <stdlib.h>
#include <string.h>

int tfails, tchecks;
const char *t_modem_img, *t_efs_dir, *t_board2, *t_smem_img;

uint8_t *t_slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = n; return b;
}

static void *t_allocs[4096];
static unsigned t_nallocs;
void *t_talloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (t_nallocs < 4096) t_allocs[t_nallocs++] = p;
    return p;
}
void t_tfree_all(void)
{
    while (t_nallocs) free(t_allocs[--t_nallocs]);
}

#define T_LAYERS(X) X(smem) X(glink) X(qrtr) X(wlfw) X(ce) X(wpa) X(qmp) X(mss) X(wmi) X(mlme) X(htt) X(dev) X(sae) X(smmu)
#define DECL(n) void t_##n(void);
T_LAYERS(DECL)

int main(int argc, char **argv)
{
    t_modem_img = argc > 1 && argv[1][0] ? argv[1] : NULL;
    t_efs_dir = argc > 2 && argv[2][0] ? argv[2] : NULL;
    t_board2 = argc > 3 && argv[3][0] ? argv[3] : NULL;
    t_smem_img = argc > 4 && argv[4][0] ? argv[4] : NULL;
#define CALL(n) t_##n();
    T_LAYERS(CALL)
    printf("%s: fajita Wi-Fi stack host tests, %d checks, %d failures\n", tfails ? "FAIL" : "PASS", tchecks, tfails);
    return tfails != 0;
}
