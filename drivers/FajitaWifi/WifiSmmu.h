// SPDX-License-Identifier: BSD-2-Clause-Patent
// The apps_smmu stage-1 context for the WLAN streams (SIDs 0x40, 0x41): ARM SMMUv2 context bank in AArch64 format, 4 KiB granule,
// T0SZ 32 (a 4 GiB input space, levels 1-2), one L1 table and one L2 table whose 2 MiB block entries map only the DMA arena,
// IOVA = PA (the device code programs physical addresses), Normal non-cacheable (MAIR index 0 = 0x44, as the CPU maps the arena
// write-combined). Anything outside the arena has no descriptor and faults (CB_FSR.TF) instead of reaching memory. The tables live
// outside the arena, so the device cannot rewrite them. Freestanding: this builds the tables and the register values; the kernel
// glue (package/FajitaWifiDev.cpp) does the MMIO. Host-tested with a software walk of the result (tests/t_smmu.c).
#ifndef FAJITA_WIFI_SMMU_H
#define FAJITA_WIFI_SMMU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define WSMMU_TABLE_BYTES 8192u           // L1 (4 KiB) + L2 (4 KiB), 4 KiB aligned
#define WSMMU_BLOCK (2u << 20)
#define WSMMU_ASID 0x40u
// context bank register offsets (from the bank's page)
#define WSMMU_CB_SCTLR 0x000u
#define WSMMU_CB_TCR2 0x010u
#define WSMMU_CB_TTBR0 0x020u
#define WSMMU_CB_TCR 0x030u
#define WSMMU_CB_MAIR0 0x038u
#define WSMMU_CB_MAIR1 0x03cu
#define WSMMU_CB_FSR 0x058u
#define WSMMU_CB_FAR 0x060u
#define WSMMU_CB_FSYNR0 0x068u
#define WSMMU_CB_TLBIALL 0x618u
#define WSMMU_CB_TLBSYNC 0x7f0u
#define WSMMU_CB_TLBSTATUS 0x7f4u
#define WSMMU_FSR_FAULTS 0x800001feu       // MULTI | TF AFF PF EF TLBMCF TLBLKF ASF UUT (not FORMAT 10:9, not SS)
enum {WSMMU_OK = 0, WSMMU_ERR_INVAL = -22};

typedef struct {
    uint64_t ttbr0;                        // L1 table PA | ASID << 48
    uint32_t tcr, tcr2, mair0, mair1, sctlr;
    uint32_t cbar, cba2r;                  // global register 1 values for the bank
    unsigned blocks;                       // 2 MiB blocks mapped
} wsmmu_ctx;

// Build the tables into tables[] (WSMMU_TABLE_BYTES, CPU view; table_pa = its physical address, 4 KiB aligned, < 2^40) for the
// arena [arena_pa, arena_pa + size): 2 MiB aligned, below 4 GiB. Fills the register values. 0 or WSMMU_ERR_INVAL.
int wsmmu_build(uint8_t *tables, uint64_t table_pa, uint64_t arena_pa, uint64_t size, wsmmu_ctx *out);
// A software walk of the tables (what the SMMU would do): 0 and *pa for a mapped IOVA, -1 for a translation fault.
int wsmmu_walk(const uint8_t *tables, uint64_t table_pa, uint64_t iova, uint64_t *pa);

#ifdef __cplusplus
}
#endif
#endif
