// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WLAN stage-1 context on apps_smmu. See WifiSmmu.h. Descriptor formats: ARMv8 VMSAv8-64, 4 KiB granule (the SMMUv2 AArch64
// context format); register fields as Linux drivers/iommu/arm/arm-smmu programs them (arm_smmu_write_context_bank).
#include "WifiSmmu.h"

static void put64(uint8_t *p, uint64_t v) {for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> 8 * i);}
static uint64_t get64(const uint8_t *p) {uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v;}

#define DESC_VALID 1ull
#define DESC_TABLE 3ull                     // bits 1:0 = 0b11 at level 1: next-level table
#define DESC_BLOCK 1ull                     // bits 1:0 = 0b01 at level 2: 2 MiB block
#define DESC_ATTR0 (0ull << 2)              // AttrIndx 0 (MAIR0 byte 0: Normal non-cacheable)
#define DESC_AP_RW (1ull << 6)              // AP[2:1] = 01: read/write
#define DESC_SH_OUTER (2ull << 8)
#define DESC_AF (1ull << 10)
#define OA_MASK 0x000000ffffe00000ull       // block output address bits 39:21 (40-bit PA)
#define TABLE_MASK 0x000000fffffff000ull

int wsmmu_build(uint8_t *tables, uint64_t table_pa, uint64_t arena_pa, uint64_t size, wsmmu_ctx *out)
{
    if (!tables || !out || (table_pa & 0xfff) || table_pa >> 40 || !size) return WSMMU_ERR_INVAL;
    if ((arena_pa | size) & (WSMMU_BLOCK - 1)) return WSMMU_ERR_INVAL;
    if (arena_pa + size > (1ull << 32) || arena_pa + size < arena_pa) return WSMMU_ERR_INVAL;   // T0SZ 32: a 4 GiB input space
    if ((arena_pa >> 30) != ((arena_pa + size - 1) >> 30)) return WSMMU_ERR_INVAL;             // one L2 table: inside one 1 GiB
    for (unsigned i = 0; i < WSMMU_TABLE_BYTES; ++i) tables[i] = 0;
    uint8_t *l1 = tables, *l2 = tables + 4096;
    const uint64_t l2_pa = table_pa + 4096;
    put64(l1 + 8 * (arena_pa >> 30), (l2_pa & TABLE_MASK) | DESC_TABLE);
    unsigned n = 0;
    for (uint64_t a = arena_pa; a < arena_pa + size; a += WSMMU_BLOCK, ++n)
        put64(l2 + 8 * ((a >> 21) & 0x1ff), (a & OA_MASK) | DESC_AF | DESC_SH_OUTER | DESC_AP_RW | DESC_ATTR0 | DESC_BLOCK);
    out->blocks = n;
    out->ttbr0 = (table_pa & TABLE_MASK) | (uint64_t)WSMMU_ASID << 48;
    // TCR (AArch64 format): T0SZ 32, non-cacheable non-shareable walks (the CPU cleans the tables to PoC), 4 KiB, TTBR1 off (EPD1)
    out->tcr = 32u | (0u << 8) | (0u << 10) | (0u << 12) | (0u << 14) | (1u << 23);
    // TCR2: PASize 40 bits (2), SEP upstream (7 << 15), 8-bit ASID
    out->tcr2 = 2u | (7u << 15);
    out->mair0 = 0x44u | (0x04u << 8);      // attr0 Normal NC, attr1 Device-nGnRE
    out->mair1 = 0;
    out->sctlr = 0x1u | 0x2u | 0x4u | 0x20u;   // M, TRE, AFE, CFRE (fault reporting in FSR; no context interrupt)
    out->cbar = 1u << 16;                   // TYPE: stage 1 translation, stage 2 bypass
    out->cba2r = 1u;                        // VA64
    return WSMMU_OK;
}

int wsmmu_walk(const uint8_t *tables, uint64_t table_pa, uint64_t iova, uint64_t *pa)
{
    if (iova >> 32) return -1;                                   // beyond T0SZ 32
    const uint64_t d1 = get64(tables + 8 * (iova >> 30));
    if ((d1 & 3) != DESC_TABLE) return -1;
    const uint64_t l2_pa = d1 & TABLE_MASK;
    if (l2_pa != table_pa + 4096) return -1;                     // only our own L2 table exists
    const uint64_t d2 = get64(tables + 4096 + 8 * ((iova >> 21) & 0x1ff));
    if ((d2 & 3) != DESC_BLOCK || !(d2 & DESC_AF)) return -1;
    if (pa) *pa = (d2 & OA_MASK) | (iova & (WSMMU_BLOCK - 1));
    return 0;
}
