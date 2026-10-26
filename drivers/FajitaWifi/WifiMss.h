// SPDX-License-Identifier: BSD-2-Clause-Patent
// Modem (MSS) boot: the self-authenticating MBA path of mainline Linux drivers/remoteproc/qcom_q6v5_mss.c for
// "qcom,sdm845-mss-pil" (MSS_SDM845: need_mem_protection, has_alt_reset, no qaccept/ext-bhs/spare regs, no vq6) plus the
// parts of qcom_q6v5.c it calls (load-state toggle, prepare/unprepare, wait for start, stop request). Freestanding C over
// an ops table, so the sequence, its ordering constraints and every failure path's teardown are host-tested against a
// simulated modem (tests/t_mss.c). The kernel side (package/FajitaWifiModem.cpp) supplies the registers, RPMh, SCM, the
// carveouts, the firmware reader and the AOSS QMP / SMP2P links.
//
// What boot does, in Linux's order (q6v5_start):
//   rmtfs buffer HLOS -> HLOS+MSS (rmtfs_mem.c), mba.mbn copied into the mba carveout (q6v5_load), then q6v5_mba_load:
//   load_state "modem on" (AOSS QMP), RPMh cx/mx/mss to their top level, xo/prng proxy clocks, the reset clocks (iface,
//   snoc_axi), PDC + AOSS MSS_RESTART reset with RMB_MBA_ALT_RESET, the active clocks (bus, mem, gpll0_mss, mnoc_axi), mpss and
//   mba HLOS -> MSS, RMB_MBA_IMAGE, the QDSP6SS sleep clock and boot FSM, PBL status, MBA "XPU unlocked";
//   then q6v5_mpss_load: modem.mdt metadata to a DMA buffer handed to the MSS, META_DATA_READY, "auth success", the segments of
//   modem.bNN into the mpss carveout with CODE_START/CODE_LENGTH, mpss to the MSS only, "auth complete"; then the SMP2P
//   "ready" bit, the mba carveout back to HLOS, and the SMP2P "handover" bit which releases the proxy votes.
// Every failure path unwinds what was done, in Linux's order, plus what Linux leaves: the mpss carveout and the rmtfs buffer
// go back to HLOS too, and the RPMh levels return to the values they had before (Linux's genpd votes are additive; here the
// apps' own vote on the shared rails is a single register).
#ifndef FAJITA_WIFI_MSS_H
#define FAJITA_WIFI_MSS_H
#include <stddef.h>
#include <stdint.h>
#include "WifiMdt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register blocks: offsets are those of the Linux driver (relative to the block base, GCC and TCSR from their syscon base).
enum {
    WMSS_BLK_QDSP6,     // 0x04080000 (reg_base)
    WMSS_BLK_RMB,       // 0x04180000 (rmb_base)
    WMSS_BLK_GCC,       // 0x00100000 (gcc-sdm845)
    WMSS_BLK_AOSS_CC,   // 0x0c2a0000 (reset-qcom-aoss, MSS_RESTART at 0x10000)
    WMSS_BLK_PDC,       // 0x0b2e0000 (reset-qcom-pdc, sync reset at 0x100, MODEM bit 9)
    WMSS_BLK_TCSR,      // 0x01f60000 (tcsr_regs_1: halt regs 0x3000 q6, 0x5000 modem, 0x4000 nc)
    WMSS_NBLK
};
enum {WMSS_REGION_MBA, WMSS_REGION_MPSS, WMSS_REGION_MDATA};
enum {WMSS_ARC_MX, WMSS_ARC_CX, WMSS_ARC_MSS, WMSS_NARC};   // the vote order (cx's parent is mx)

#define WMSS_VMID_HLOS 0x3u
#define WMSS_VMID_MSS_MSA 0xFu
#define WMSS_PERM_RW 0x6u
#define WMSS_PERM_RWX 0x7u
typedef struct {uint32_t vmid, perm;} wmss_vmperm;

// QDSP6SS, RMB, GCC, reset and halt registers (qcom_q6v5_mss.c, gcc-sdm845.c, reset-qcom-*.c, FajitaWifiBoard.h)
#define WMSS_QDSP6SS_SLEEP 0x3c
#define WMSS_QDSP6SS_BOOT_CORE_START 0x400
#define WMSS_QDSP6SS_BOOT_CMD 0x404
#define WMSS_CBCR_CLKEN 1u
#define WMSS_CBCR_CLKOFF (1u << 31)
#define WMSS_RMB_MBA_IMAGE 0x00
#define WMSS_RMB_PBL_STATUS 0x04
#define WMSS_RMB_MBA_COMMAND 0x08
#define WMSS_RMB_MBA_STATUS 0x0c
#define WMSS_RMB_PMI_META_DATA 0x10
#define WMSS_RMB_PMI_CODE_START 0x14
#define WMSS_RMB_PMI_CODE_LENGTH 0x18
#define WMSS_RMB_MSS_STATUS 0x40
#define WMSS_RMB_ALT_RESET 0x44
#define WMSS_PBL_SUCCESS 1
#define WMSS_MBA_XPU_UNLOCKED 1
#define WMSS_MBA_XPU_UNLOCKED_SCRIBBLED 2
#define WMSS_MBA_META_AUTH_SUCCESS 3
#define WMSS_MBA_AUTH_COMPLETE 4
#define WMSS_CMD_META_DATA_READY 1u
#define WMSS_CMD_LOAD_READY 2u
#define WMSS_AOSS_MSS_RESTART 0x10000
#define WMSS_PDC_SYNC_RESET 0x100
#define WMSS_PDC_MODEM_BIT 9
#define WMSS_HALT_Q6 0x3000
#define WMSS_HALT_MODEM 0x5000
#define WMSS_HALT_NC 0x4000
#define WMSS_AXI_HALTREQ 0x0
#define WMSS_AXI_HALTACK 0x4
#define WMSS_AXI_IDLE 0x8
// SMP2P "slave-kernel" bits (sdm845.dtsi mss_pil interrupts-extended; wdog is a GIC SPI, not a bit).
#define WMSS_SMP2P_FATAL (1u << 0)
#define WMSS_SMP2P_READY (1u << 1)
#define WMSS_SMP2P_HANDOVER (1u << 2)
#define WMSS_SMP2P_STOP_ACK (1u << 3)
#define WMSS_SMP2P_SHUTDOWN_ACK (1u << 7)
#define WMSS_SMP2P_STOP_BIT (1u << 0)   // "master-kernel" bit 0 (qcom,smem-states "stop")

typedef struct wmss_ops {
    void *ctx;
    uint32_t (*rd)(void *ctx, unsigned blk, uint32_t off);
    void (*wr)(void *ctx, unsigned blk, uint32_t off, uint32_t v);
    uint64_t (*now_us)(void *ctx);
    void (*udelay)(void *ctx, uint32_t us);
    void (*msleep)(void *ctx, uint32_t ms);
    // RPMh ARC level of the rail (an index into cmd-db's level table, what rpmhpd sends): the current vote and the top one.
    int (*arc_get)(void *ctx, unsigned arc, uint32_t *cur, uint32_t *max);
    int (*arc_set)(void *ctx, unsigned arc, uint32_t level);
    // qcom_scm_assign_mem: move [addr, addr+size) from the VMIDs in *cur to `next`, update *cur. 0 on success.
    int (*assign)(void *ctx, uint64_t addr, uint64_t size, uint64_t *cur, const wmss_vmperm *next, unsigned n);
    // AOSS QMP "{class: image, res: load_state, name: modem, val: on|off}".
    int (*load_state)(void *ctx, int on);
    // The firmware files of the phone's modem partition ("image/mba.mbn", "image/modem.mdt", "image/modem.b05").
    int (*fw_size)(void *ctx, const char *name, uint32_t *size);
    long (*fw_read)(void *ctx, const char *name, uint32_t off, void *dst, uint32_t len);
    // The carveouts (CPU access to physical memory the OS does not manage); the metadata DMA buffer (below 4 GiB, contiguous).
    int (*region_write)(void *ctx, unsigned region, uint64_t off, const void *src, uint32_t len);
    int (*region_zero)(void *ctx, unsigned region, uint64_t off, uint64_t len);
    int (*mdata_alloc)(void *ctx, uint32_t size, uint64_t *phys);
    void (*mdata_free)(void *ctx);
    // SMP2P with the modem: its inbound bits (0 = valid, negative before the modem allocated its item); our outbound bits.
    int (*smp2p_bits)(void *ctx, uint32_t *v);
    int (*smp2p_set)(void *ctx, uint32_t mask, uint32_t v);
} wmss_ops;

// The boot steps, as recorded (FWM rows).
enum {
    WMSS_S_PREFLIGHT, WMSS_S_RMTFS, WMSS_S_MBA_COPY, WMSS_S_LOAD_STATE, WMSS_S_VOTE, WMSS_S_PROXY_CLK, WMSS_S_RESET_CLK,
    WMSS_S_RESET, WMSS_S_ACTIVE_CLK, WMSS_S_ASSIGN_MPSS, WMSS_S_ASSIGN_MBA, WMSS_S_Q6_RESET, WMSS_S_MBA_WAIT, WMSS_S_META_ALLOC,
    WMSS_S_META_ASSIGN, WMSS_S_META_AUTH, WMSS_S_META_RECLAIM, WMSS_S_SEGMENTS, WMSS_S_MPSS_SHARE, WMSS_S_MPSS_MSS,
    WMSS_S_AUTH, WMSS_S_READY, WMSS_S_MBA_RECLAIM, WMSS_S_HANDOVER, WMSS_S_HALT, WMSS_S_STOP, WMSS_S_UNWIND, WMSS_NSTEPS
};
const char *wmss_step_name(unsigned step);
typedef struct {unsigned step; int rc; uint32_t value, us;} wmss_step;
#define WMSS_MAX_TRACE 64u

typedef struct {
    uint64_t mba_phys, mba_size, mpss_phys, mpss_size, rmtfs_phys, rmtfs_size;   // rmtfs_size 0: not assigned
    const char *mba_name, *mdt_name;    // "image/mba.mbn", "image/modem.mdt" (the .bNN names are derived from the latter)
    uint32_t ready_ms, handover_ms;     // 5000 / 10000
} wmss_cfg;

typedef struct {
    const wmss_ops *ops;
    wmss_cfg cfg;
    // Ownership bitmaps of the carveouts (BIT(vmid)), starting at HLOS only.
    uint64_t mba_perm, mpss_perm, rmtfs_perm, mdata_perm;
    uint64_t mdata_phys; uint32_t mdata_size; int mdata_held;
    // What is currently on (the unwind undoes exactly this).
    int load_state_on, proxy_held, reset_clks_on, active_clks_on, reset_asserted_by_us, mba_loaded, q6_started, running;
    int rmtfs_assigned;
    uint32_t arc_prior[WMSS_NARC], arc_max[WMSS_NARC];
    int arc_voted[WMSS_NARC];
    uint32_t clk_prior[16];      // the enable word of each clock before we touched it
    unsigned clk_state;          // bit i: clock i enabled by us
    wmdt_image img;              // modem.mdt, parsed
    uint32_t mdt_bytes;
    uint32_t boot_addr;
    wmss_step trace[WMSS_MAX_TRACE]; unsigned ntrace;
    int rc, failed_step;
    uint64_t t0;                 // boot start, for the trace
    uint8_t buf[65536];          // firmware read buffer
} wmss;

void wmss_init(wmss *m, const wmss_ops *ops, const wmss_cfg *cfg);
// Everything that can be checked without touching the hardware: mba.mbn fits, modem.mdt parses (hash segment, metadata size),
// every segment lies in the mpss region, every segment file exists with its p_filesz. 0 or a negative errno.
int wmss_preflight(wmss *m);
// q6v5_start. 0: the modem is running (SMP2P ready), the mba carveout is back with HLOS; the handover (proxy votes released)
// may still be pending (m->proxy_held). On failure everything is unwound and m->failed_step/m->rc say where.
int wmss_boot(wmss *m);
// Poll for the SMP2P handover bit and release the proxy resources (qcom_msa_handover). 1 when released, 0 not yet, <0 error.
int wmss_poll_handover(wmss *m);
// q6v5_stop: request stop over SMP2P, wait for the stop ack, then q6v5_mba_reclaim (+ mpss and rmtfs back to HLOS).
int wmss_stop(wmss *m);
// The register survey for a first look: reads the registers boot uses, writes nothing. Fills out[] with {block, offset, value}.
typedef struct {unsigned blk; uint32_t off, val;} wmss_reg;
unsigned wmss_survey(wmss *m, wmss_reg *out, unsigned cap);

#ifdef __cplusplus
}
#endif
#endif
