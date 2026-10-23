/* SPDX-License-Identifier: MIT */
/* Adreno 630 GPU bring-up after GX is on: a6xx_gpu.c hw_init (the A630 branches), a6xx_cp_init, and the a6xx_submit framing
 * of a command stream (the CP packets around the indirect buffers and the timestamp fence). Freestanding C over A630Io. */
#ifndef FAJITA_A630_GPU_H
#define FAJITA_A630_GPU_H
#include "A630Gmu.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t sqe_iova, ring_iova, rptr_shadow_iova;   /* the SQE microcode (without its first dword), ring 0, the WHERE_AM_I shadow */
    uint32_t ring_bytes;                              /* 32 KiB: MSM_GPU_RINGBUFFER_SZ */
    uint64_t uche_trap_base;                          /* 0x1fffffffff000 (a6xx_gpu_init) */
    uint64_t gmem_base;                               /* UCHE_GMEM_RANGE_MIN: SZ_1M on A630 */
    uint32_t gmem_bytes;                              /* SZ_1M */
} A630HwInitConfig;
typedef struct {
    unsigned step;
    uint32_t clock_cntl_before, rbbm_status, int_status, cp_hw_fault, sqe_cntl;
} A630HwInitReport;
/* hw_init from the GBIF/SECVID writes through CP_SQE_CNTL = 1, for an A630: 64-bit addressing of every block, hwcg
 * (a630_hwcg, RBBM_CLOCK_CNTL 0x8aa8aa02, GMU CGC 0x00020202/0x10111/0x5555 with SPTPRAC clock control around it), VBIF QoS,
 * VBIF_GATE_OFF_WRREQ_EN 9, UCHE ranges, ROQ thresholds, mem pool, prim FIFO 0x00180000, AHB error response, perf counters,
 * UBWC (a6xx_calc_ubwc_config defaults: hbb 15, swizzle 6), hang detect (1<<30)|0x1fffff, UCHE_CLIENT_PF, CX busy mask
 * and power counter, CP protect (a630_protect), A6XX_INT_MASK, SQE base, ring base, RB_CNTL with NO_UPDATE (no hw APRIV),
 * the RPTR shadow address, SQE start. OOB GPU_SET must be held. */
void A630HwInit(const A630Io *io, const A630HwInitConfig *c, A630HwInitReport *r);
/* a6xx_ucode_check_version for a630_sqe.fw: true when the image (with its first dword) has WHERE_AM_I. */
int A630SqeHasWhereAmI(const uint32_t *image, size_t words);

/* PM4 packet headers (adreno_pm4.xml TYPE4/TYPE7 with the odd-parity bits, as OUT_PKT4/OUT_PKT7 build them). */
uint32_t A630Pkt4(uint32_t reg, uint32_t count);
uint32_t A630Pkt7(uint32_t opcode, uint32_t count);
enum {
    A630CpNop = 0x10, A630CpWaitForIdle = 0x26, A630CpMemWrite = 0x3d, A630CpIndirectBuffer = 0x3f, A630CpEventWrite = 0x46,
    A630CpMeInit = 0x48, A630CpWhereAmI = 0x62, A630CpSetSecureMode = 0x66, A630CpRegToMem = 0x3e, A630CpWaitMemWrites = 0x12,
    A630CpMemcpy = 0x75,
    A630EventCacheFlushTs = 4, A630EventCcuInvalidateDepth = 24, A630EventCcuInvalidateColor = 25, A630EventCacheInvalidate = 49,
    A630EventIrq = 1u << 31,
};
/* a6xx_cp_init: CP_ME_INIT with its 8 dwords. Writes 9 words, returns 9. */
unsigned A630CpInitWords(uint32_t *out);
/* CP_WHERE_AM_I to the RPTR shadow (update_shadow_rptr, has_whereami without APRIV): 3 words. */
unsigned A630WhereAmIWords(uint64_t shadow_iova, uint32_t *out);
/* The a6xx_submit fence: CP_SCRATCH(2) = seqno, CP_EVENT_WRITE CACHE_FLUSH_TS | IRQ to fence_iova (8 words). */
unsigned A630FenceWords(uint32_t seqno, uint64_t fence_iova, uint32_t *out);
/* a6xx_submit's prologue: CCU depth and color invalidate (4 words). */
unsigned A630SubmitPrologueWords(uint32_t *out);
/* CP_INDIRECT_BUFFER (4 words); dwords is the IB size in dwords (IB_SIZE bits 0..19). */
unsigned A630IbWords(uint64_t iova, uint32_t dwords, uint32_t *out);

#ifdef __cplusplus
}
#endif
#endif
