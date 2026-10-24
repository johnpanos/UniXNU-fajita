/* SPDX-License-Identifier: MIT */
/* Adreno 630 hw_init, CP init and submit framing; see A630Gpu.h. */
#define A630_WANT_TABLES 1
#include "A630Gpu.h"
#include "A630Regs.h"

static void wr(const A630Io *io, uint32_t reg, uint32_t v) {io->write32(io->cookie, A630BlockGpu, reg * 4, v);}
static void wr64(const A630Io *io, uint32_t reg, uint64_t v) {wr(io, reg, (uint32_t)v); wr(io, reg + 1, (uint32_t)(v >> 32));}
static uint32_t rd(const A630Io *io, uint32_t reg) {return io->read32(io->cookie, A630BlockGpu, reg * 4);}
static uint32_t gmu(const A630Io *io, uint32_t reg) {return io->read32(io->cookie, A630BlockGmu, reg * 4);}
static void gmuw(const A630Io *io, uint32_t reg, uint32_t v) {io->write32(io->cookie, A630BlockGmu, reg * 4, v);}
static void gmurmw(const A630Io *io, uint32_t reg, uint32_t clear, uint32_t set) {gmuw(io, reg, (gmu(io, reg) & ~clear) | set);}

enum {
    kClockCntlOn = 0x8aa8aa02,      /* a6xx_set_hwcg: adreno_is_a630 */
    kGmuCgcMode = 0x00020202,       /* a630 .gmu_cgc_mode */
    kCgcDelay = 0x10111, kCgcHyst = 0x5555,
    kIntMask = (1u << 1) | (1u << 6) | (1u << 9) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 20) | (1u << 22) |
               (1u << 23) | (1u << 24) | (1u << 25),   /* A6XX_INT_MASK */
};

static void setHwcg(const A630Io *io, A630HwInitReport *r)
{
    gmuw(io, A630_GPU_GMU_AO_GMU_CGC_MODE_CNTL, kGmuCgcMode);
    gmuw(io, A630_GPU_GMU_AO_GMU_CGC_DELAY_CNTL, kCgcDelay);
    gmuw(io, A630_GPU_GMU_AO_GMU_CGC_HYST_CNTL, kCgcHyst);
    const uint32_t val = rd(io, A630_RBBM_CLOCK_CNTL);
    r->clock_cntl_before = val;
    if (val == kClockCntlOn) return;   /* Don't re-program the registers if they are already correct */
    /* Disable SP clock before programming HWCG registers */
    gmurmw(io, A630_GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL, 1, 0);
    for (unsigned i = 0; i < A630_HWCG_COUNT; ++i) wr(io, A630Hwcg[i][0], A630Hwcg[i][1]);
    /* Enable SP clock */
    gmurmw(io, A630_GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL, 0, 1);
    wr(io, A630_RBBM_CLOCK_CNTL, kClockCntlOn);
}

static void setUbwc(const A630Io *io)
{
    /* a6xx_calc_ubwc_config defaults (A630 takes none of the overrides) and a6xx_set_ubwc_config. */
    const uint32_t rgb565_predicator = 0, uavflagprd_inv = 0, min_acc_len = 0, ubwc_swizzle = 0x6, macrotile_mode = 0,
                   highest_bank_bit = 15, amsbc = 0;
    const uint32_t hbb = highest_bank_bit - 13, hbb_hi = hbb >> 2, hbb_lo = hbb & 3;
    const uint32_t ubwc_mode = ubwc_swizzle & 1, level2_swizzling_dis = !(ubwc_swizzle & 2);
    wr(io, A630_RB_NC_MODE_CNTL, level2_swizzling_dis << 12 | rgb565_predicator << 11 | hbb_hi << 10 | amsbc << 4 |
       min_acc_len << 3 | hbb_lo << 1 | ubwc_mode);
    wr(io, A630_TPL1_NC_MODE_CNTL, level2_swizzling_dis << 6 | hbb_hi << 4 | min_acc_len << 3 | hbb_lo << 1 | ubwc_mode);
    wr(io, A630_SP_NC_MODE_CNTL, level2_swizzling_dis << 12 | hbb_hi << 10 | uavflagprd_inv << 4 | min_acc_len << 3 |
       hbb_lo << 1 | ubwc_mode);
    wr(io, A630_UCHE_MODE_CNTL, min_acc_len << 23 | hbb_lo << 21);
    wr(io, A630_RBBM_NC_MODE_CNTL, macrotile_mode);
}

static void setCpProtect(const A630Io *io)
{
    /* ACCESS_PROT_EN | ACCESS_FAULT_ON_VIOL_EN | LAST_SPAN_INF_RANGE */
    wr(io, A630_CP_PROTECT_CNTL, (1u << 0) | (1u << 1) | (1u << 3));
    unsigned i;
    for (i = 0; i < A630_PROTECT_COUNT - 1; i++)
        if (A630Protect[i]) wr(io, A630_CP_PROTECT + i, A630Protect[i]);
    /* last CP_PROTECT to have "infinite" length on the last entry */
    wr(io, A630_CP_PROTECT + A630_PROTECT_COUNT_MAX - 1, A630Protect[i]);
}

void A630HwInit(const A630Io *io, const A630HwInitConfig *c, A630HwInitReport *r)
{
    r->step = 1;
    /* (no GBIF on A630) */
    wr(io, A630_RBBM_SECVID_TSB_CNTL, 0);
    /* Disable the trusted memory range */
    wr64(io, A630_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
    wr(io, A630_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
    /* Turn on 64 bit addressing for all blocks */
    wr(io, A630_CP_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_VSC_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_GRAS_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_RB_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_PC_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_HLSQ_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_VFD_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_VPC_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_UCHE_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_SP_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_TPL1_ADDR_MODE_CNTL, 0x1);
    wr(io, A630_RBBM_SECVID_TSB_ADDR_MODE_CNTL, 0x1);
    /* enable hardware clockgating */
    r->step = 2;
    setHwcg(io, r);
    /* VBIF start */
    r->step = 3;
    wr(io, A630_RBBM_VBIF_CLIENT_QOS_CNTL, 0x3);
    wr(io, A630_VBIF_GATE_OFF_WRREQ_EN, 0x00000009);
    /* Make all blocks contribute to the GPU BUSY perf counter */
    wr(io, A630_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
    /* Disable L2 bypass in the UCHE */
    wr64(io, A630_UCHE_WRITE_RANGE_MAX, c->uche_trap_base + 0xfc0);
    wr64(io, A630_UCHE_TRAP_BASE, c->uche_trap_base);
    wr64(io, A630_UCHE_WRITE_THRU_BASE, c->uche_trap_base);
    /* Set the GMEM VA range [gmem_base, gmem_base + gmem - 1] */
    wr64(io, A630_UCHE_GMEM_RANGE_MIN, c->gmem_base);
    wr64(io, A630_UCHE_GMEM_RANGE_MAX, c->gmem_base + c->gmem_bytes - 1);
    wr(io, A630_UCHE_FILTER_CNTL, 0x804);
    wr(io, A630_UCHE_CACHE_WAYS, 0x4);
    wr(io, A630_CP_ROQ_THRESHOLDS_2, 0x010000c0);
    wr(io, A630_CP_ROQ_THRESHOLDS_1, 0x8040362c);
    /* Setting the mem pool size */
    wr(io, A630_CP_MEM_POOL_SIZE, 128);
    /* Set the default primFifo threshold values */
    wr(io, A630_PC_DBG_ECO_CNTL, 0x00180000);
    /* Set the AHB default slave response to "ERROR" */
    wr(io, A630_CP_AHB_CNTL, 0x1);
    /* Turn on performance counters */
    wr(io, A630_RBBM_PERFCTR_CNTL, 0x1);
    /* Select CP0 to always count cycles (PERF_CP_ALWAYS_COUNT = 0) */
    wr(io, A630_CP_PERFCTR_CP_SEL + 0, 0);
    r->step = 4;
    setUbwc(io);
    /* Enable fault detection */
    wr(io, A630_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0x1fffff);
    wr(io, A630_UCHE_CLIENT_PF, (1u << 7) | 0x1);
    /* Set up the CX GMU counter 0 to count busy ticks */
    gmuw(io, A630_GPU_GMU_AO_GPU_CX_BUSY_MASK, 0xff000000);
    /* Enable the power counter */
    gmurmw(io, A630_GMU_CX_GMU_POWER_COUNTER_SELECT_0, 0xff, 1u << 5);
    gmuw(io, A630_GMU_CX_GMU_POWER_COUNTER_ENABLE, 1);
    /* Protect registers from the CP */
    r->step = 5;
    setCpProtect(io);
    /* Enable interrupts */
    wr(io, A630_RBBM_INT_0_MASK, kIntMask);
    r->step = 6;
    wr64(io, A630_CP_SQE_INSTR_BASE, c->sqe_iova);
    /* Set the ringbuffer address */
    wr64(io, A630_CP_RB_BASE, c->ring_iova);
    /* No hardware APRIV: MSM_GPU_RB_CNTL_DEFAULT (BUFSZ ilog2(size / 8), BLKSZ ilog2(32 / 8)) | AXXX_CP_RB_CNTL_NO_UPDATE */
    {
        uint32_t bufsz = 0;
        for (uint32_t v = c->ring_bytes / 8; v > 1; v >>= 1) ++bufsz;
        wr(io, A630_CP_RB_CNTL, (bufsz & 0x3f) | (2u << 8) | (1u << 27));
    }
    /* Configure the RPTR shadow (WHERE_AM_I writes it) */
    wr64(io, A630_CP_RB_RPTR_ADDR, c->rptr_shadow_iova);
    /* Enable the SQE_to start the CP engine */
    r->step = 7;
    wr(io, A630_CP_SQE_CNTL, 1);
    if (io->barrier) io->barrier(io->cookie);
    r->rbbm_status = rd(io, A630_RBBM_STATUS);
    r->int_status = rd(io, A630_RBBM_INT_0_STATUS);
    r->cp_hw_fault = rd(io, A630_CP_HW_FAULT);
    r->sqe_cntl = rd(io, A630_CP_SQE_CNTL);
    r->step = 8;
}

int A630SqeHasWhereAmI(const uint32_t *buf, size_t words)
{
    /* adreno_fw_create_bo drops the first dword, so the BO's buf[0] is the file's dword 1. */
    if (words < 4) return 0;
    const uint32_t *b = buf + 1;
    return (((b[0] & 0xf) == 0xa) && (b[2] & 0xf) >= 1) || (b[0] & 0xfff) >= 0x190;
}

static uint32_t parity(uint32_t val)
{
    return (0x9669 >> (0xf & (val ^ (val >> 4) ^ (val >> 8) ^ (val >> 12) ^ (val >> 16) ^ (val >> 20) ^ (val >> 24) ^
                              (val >> 28)))) & 1;
}
uint32_t A630Pkt4(uint32_t reg, uint32_t count)
{
    return (4u << 28) | count | (parity(count) << 7) | ((reg & 0x3ffff) << 8) | (parity(reg) << 27);
}
uint32_t A630Pkt7(uint32_t opcode, uint32_t count)
{
    return (7u << 28) | count | (parity(count) << 15) | ((opcode & 0x7f) << 16) | (parity(opcode) << 23);
}

unsigned A630CpInitWords(uint32_t *o)
{
    o[0] = A630Pkt7(A630CpMeInit, 8);
    o[1] = 0x0000002f;
    o[2] = 0x00000003;   /* Enable multiple hardware contexts */
    o[3] = 0x20000000;   /* Enable error detection */
    o[4] = 0;            /* Don't enable header dump */
    o[5] = 0;
    o[6] = 0;            /* No workarounds enabled */
    o[7] = 0;            /* Pad rest of the cmds with 0's */
    o[8] = 0;
    return 9;
}
unsigned A630WhereAmIWords(uint64_t shadow, uint32_t *o)
{
    o[0] = A630Pkt7(A630CpWhereAmI, 2);
    o[1] = (uint32_t)shadow;
    o[2] = (uint32_t)(shadow >> 32);
    return 3;
}
unsigned A630FenceWords(uint32_t seqno, uint64_t fence, uint32_t *o)
{
    o[0] = A630Pkt4(A630_CP_SCRATCH + 2, 1);
    o[1] = seqno;
    o[2] = A630Pkt7(A630CpEventWrite, 4);
    o[3] = A630EventCacheFlushTs | A630EventIrq;
    o[4] = (uint32_t)fence;
    o[5] = (uint32_t)(fence >> 32);
    o[6] = seqno;
    return 7;
}
unsigned A630SubmitPrologueWords(uint32_t *o)
{
    o[0] = A630Pkt7(A630CpEventWrite, 1);
    o[1] = A630EventCcuInvalidateDepth;
    o[2] = A630Pkt7(A630CpEventWrite, 1);
    o[3] = A630EventCcuInvalidateColor;
    return 4;
}
unsigned A630IbWords(uint64_t iova, uint32_t dwords, uint32_t *o)
{
    o[0] = A630Pkt7(A630CpIndirectBuffer, 3);
    o[1] = (uint32_t)iova;
    o[2] = (uint32_t)(iova >> 32);
    o[3] = dwords & 0xfffff;
    return 4;
}
