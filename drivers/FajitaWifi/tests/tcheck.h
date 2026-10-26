// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared check macro and fixtures for the stack host tests (test_stack.c and the t_*.c layer tests).
#ifndef FAJITA_WIFI_TCHECK_H
#define FAJITA_WIFI_TCHECK_H
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
extern int tfails, tchecks;
#define CHECK(cond) do { ++tchecks; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++tfails; } } while (0)
#define RUN(fn) do { int f0 = tfails; fn(); printf("%s %s\n", tfails == f0 ? "ok  " : "FAIL", #fn); } while (0)
// Firmware/partition files given on the command line (NULL when absent): argv[1] = a modem partition image.
extern const char *t_modem_img;
extern const char *t_efs_dir;
extern const char *t_board2;   // argv[3]: board-2.bin
extern const char *t_smem_img; // argv[4]: a SMEM snapshot from the phone (kcmd wifi snap; smem-snap.py)
uint8_t *t_slurp(const char *path, size_t *len);
// Test allocations, released together by t_tfree_all() (the kernel side never frees its ring state).
void *t_talloc(size_t n);
void t_tfree_all(void);
// A synthetic partitioned SMEM image (version 12): a global partition and an APPS<->MODEM private partition.
#define T_SMEM_SIZE (2u << 20)
void t_smem_image(uint8_t *mem, int heap_only);
#endif
