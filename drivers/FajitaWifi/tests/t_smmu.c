// SPDX-License-Identifier: BSD-2-Clause-Patent
// WifiSmmu: the WLAN stage-1 tables map exactly the arena (IOVA = PA), everything else faults; descriptor bits and register values.
#include "tcheck.h"
#include "../WifiSmmu.h"
#include <string.h>

static uint8_t g_tab[WSMMU_TABLE_BYTES];

static void t_smmu_map(void)
{
    wsmmu_ctx c;
    const uint64_t tpa = 0xa4321000ull, arena = 0xf6000000ull, size = 0x800000ull;
    CHECK(wsmmu_build(g_tab, tpa, arena, size, &c) == 0 && c.blocks == 4);
    uint64_t pa = 0;
    CHECK(wsmmu_walk(g_tab, tpa, arena, &pa) == 0 && pa == arena);
    CHECK(wsmmu_walk(g_tab, tpa, arena + 0x123456, &pa) == 0 && pa == arena + 0x123456);
    CHECK(wsmmu_walk(g_tab, tpa, arena + size - 4, &pa) == 0 && pa == arena + size - 4);
    // outside the arena: no descriptor
    CHECK(wsmmu_walk(g_tab, tpa, arena - 4, &pa) == -1);
    CHECK(wsmmu_walk(g_tab, tpa, arena + size, &pa) == -1);
    CHECK(wsmmu_walk(g_tab, tpa, 0x80000000ull, &pa) == -1);      // kernel RAM, another GiB
    CHECK(wsmmu_walk(g_tab, tpa, tpa, &pa) == -1);                // the tables themselves are not reachable
    CHECK(wsmmu_walk(g_tab, tpa, 0x100000000ull, &pa) == -1);     // beyond the 4 GiB input space
    // exactly 1 L1 and 4 L2 entries are set
    unsigned set = 0;
    for (unsigned i = 0; i < WSMMU_TABLE_BYTES; i += 8) {uint64_t v; memcpy(&v, g_tab + i, 8); set += v != 0;}
    CHECK(set == 5);
    // the block descriptor: valid block, AF, outer shareable, RW, AttrIndx 0
    uint64_t d;
    memcpy(&d, g_tab + 4096 + 8 * ((arena >> 21) & 0x1ff), 8);
    CHECK(d == (arena | 0x400 | 0x200 | 0x40 | 0x1));
    memcpy(&d, g_tab + 8 * 3, 8);
    CHECK(d == ((tpa + 4096) | 3));
    CHECK(c.ttbr0 == (tpa | (uint64_t)WSMMU_ASID << 48) && (c.tcr & 0x3f) == 32 && (c.tcr & (1u << 23)) && (c.tcr2 & 7) == 2);
    CHECK((c.sctlr & 1) && c.mair0 == 0x0444 && c.cbar == 0x10000 && c.cba2r == 1);
}

static void t_smmu_refuse(void)
{
    wsmmu_ctx c;
    CHECK(wsmmu_build(g_tab, 0xa4321800ull, 0xf6000000ull, 0x800000ull, &c) == WSMMU_ERR_INVAL);   // table not page aligned
    CHECK(wsmmu_build(g_tab, 0xa4321000ull, 0xf6100000ull, 0x800000ull, &c) == WSMMU_ERR_INVAL);  // arena not 2 MiB aligned
    CHECK(wsmmu_build(g_tab, 0xa4321000ull, 0xbfe00000ull, 0x400000ull, &c) == WSMMU_ERR_INVAL);  // crosses a GiB
    CHECK(wsmmu_build(g_tab, 0xa4321000ull, 0x1f6000000ull, 0x800000ull, &c) == WSMMU_ERR_INVAL); // above 4 GiB
}

void t_smmu(void)
{
    RUN(t_smmu_map);
    RUN(t_smmu_refuse);
}
