/* SPDX-License-Identifier: MIT */
/* Adreno 630 (SDM845) power-up: GPU clocks and CX, the legacy GMU (HFI v1) cold start, GX on, and the GPU OOB requests.
 *
 * A line-by-line port of the Linux msm driver paths an A630 takes (v6.16: drivers/gpu/drm/msm/adreno/a6xx_gmu.c,
 * a6xx_hfi.c, drivers/clk/qcom/gpucc-sdm845.c, gcc-sdm845.c, gdsc.c, drivers/soc/qcom/cmd-db.c), with the clock and power
 * frameworks replaced by the register writes they end in. Freestanding C: every register access goes through A630Io, so the
 * host tests (tests/) run the same code against a register model and a simulated GMU firmware.
 *
 * Register blocks and their bases on SDM845 (sdm845.dtsi):
 *   GCC    0x00100000 (byte offsets of gcc-sdm845.c)       GPUCC   0x05090000 (byte offsets of gpucc-sdm845.c)
 *   GMU    0x0506a000 (dword offsets of a6xx_gmu.xml)       RSCC    GMU + 0x23000 (a6xx_gmu_init: gmu->mmio + 0x23000)
 *   PDC    0x0b280000 (gmu_pdc, A6XX_PDC dword offsets)     PDCSEQ  0x0b480000 (gmu_pdc_seq)
 *   GPU    0x05000000 (dword offsets of a6xx.xml)
 * A630Io takes byte offsets for every block; the helpers below scale dword offsets. */
#ifndef FAJITA_A630_GMU_H
#define FAJITA_A630_GMU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum A630Block { A630BlockGcc, A630BlockGpuCc, A630BlockGmu, A630BlockRscc, A630BlockPdc, A630BlockPdcSeq, A630BlockGpu,
                 A630BlockCount };
enum { A630GmuRsccOffset = 0x23000 };
typedef struct {
    void *cookie;
    uint32_t (*read32)(void *, unsigned block, uint32_t byte_offset);
    void (*write32)(void *, unsigned block, uint32_t byte_offset, uint32_t value);
    void (*barrier)(void *);                 /* dsb sy: orders the writes before a poll or a doorbell */
    void (*delay_us)(void *, unsigned us);
} A630Io;

typedef enum {
    A630Ok = 0,
    A630ErrArgument = 1,
    A630ErrClock = 2,         /* a branch never reported running */
    A630ErrGdsc = 3,          /* the CX GDSC never reported power on */
    A630ErrRsc = 4,           /* GMU_RSCC_CONTROL_ACK or RSCC_SEQ_BUSY_DRV0 timed out (a6xx_rpmh_start) */
    A630ErrFirmware = 5,      /* image too large / empty */
    A630ErrFwInit = 6,        /* GMU_CM3_FW_INIT_RESULT never showed the reset value (a6xx_gmu_start) */
    A630ErrOob = 7,           /* an OOB request was never acknowledged */
    A630ErrHfiStart = 8,      /* GMU_HFI_CTRL_STATUS bit 0 never set (a6xx_gmu_hfi_start) */
    A630ErrHfiSend = 9,       /* the command queue had no room */
    A630ErrHfiTimeout = 10,   /* no MSGQ interrupt for a reply */
    A630ErrHfiReply = 11,     /* a malformed reply, or the firmware returned an error for the command */
    A630ErrCmdDb = 12,        /* the command DB is missing, malformed or lacks a level table */
    A630ErrVotes = 13,        /* an OPP level has no matching ARC level */
    A630ErrSptprac = 14,      /* SPTPRAC never reported on (Linux only logs it; recorded here, not fatal) */
    A630ErrGx = 15,           /* GX did not report on after GPU_SET */
} A630Status;

/* --- Command DB and RPMh ARC votes (a6xx_gmu_rpmh_arc_votes_init) --- */
typedef struct {const uint8_t *data; size_t bytes; uint32_t address;} A630CmdDbEntry;
/* cmd_db_read_aux_data over a CPU copy of the whole aop-cmd-db region (0x85fe0000, 0x20000 bytes). */
A630Status A630CmdDbFind(const uint8_t *db, size_t db_bytes, const char *id, A630CmdDbEntry *out);

enum { A630MaxGpuLevels = 16, A630MaxGmuLevels = 4 };
typedef struct {
    unsigned gpu_count, gmu_count;              /* including the "off" level 0 */
    uint32_t gpu_khz[A630MaxGpuLevels], gmu_khz[A630MaxGmuLevels];
    uint32_t gx_votes[A630MaxGpuLevels], cx_votes[A630MaxGmuLevels];
} A630PerfTable;
/* The sdm845.dtsi OPP tables (gpu_opp_table 257..710 MHz, gmu_opp_table 200/400 MHz) with an "off" level first, as
 * a6xx_gmu_build_freq_table builds them, and their RPMh levels. */
void A630DefaultLevels(A630PerfTable *t, uint16_t gpu_levels[A630MaxGpuLevels], uint16_t gmu_levels[A630MaxGmuLevels]);
/* votes[i] = (pri[pindex] << 16) | (sindex << 8) | pindex for each level, primary gfx.lvl / cx.lvl, secondary mx.lvl. */
A630Status A630ArcVotes(const A630CmdDbEntry *primary, const A630CmdDbEntry *secondary, const uint16_t *levels, unsigned count,
                        uint32_t *votes);
A630Status A630BuildPerfTable(const uint8_t *db, size_t db_bytes, A630PerfTable *t);

/* --- Clocks and CX (gpucc_sdm845 probe, gdsc_enable, the GMU's clk_bulk_prepare_enable) --- */
typedef struct {
    uint32_t gcc_cfg_ahb, gcc_memnoc, gcc_axi, gcc_gpll0, gcc_misc;   /* after */
    uint32_t cx_before, cx_after, cxo, rcg_cfg, cx_gmu, gx_gdscr;
    unsigned polls;
} A630ClockReport;
/* gcc_gpu_cfg_ahb (always on, checked), gcc_gpu_memnoc_gfx, gcc_ddrss_gpu_axi, the GPLL0 GPU source and divider votes, the
 * GPU MISC GPLL0 active-input disable (gcc_sdm845_probe), gpu_cx_gdsc on, gpu_cc_cxo. */
A630Status A630PowerCx(const A630Io *io, A630ClockReport *r);
/* gpu_cc_gmu_clk_src at 200 MHz (GPLL0_OUT_MAIN_DIV / 1.5, a6xx_gmu_resume's clk_set_rate) and gpu_cc_cx_gmu_clk with the
 * probe's wake/sleep fields. Needs A630PowerCx first. */
A630Status A630GmuClocks(const A630Io *io, A630ClockReport *r);

/* --- GMU memory, HFI v1 --- */
typedef struct {
    volatile uint32_t *hfi;      /* CPU view of the 16 KiB HFI block, Normal non-cacheable (the GMU maps it so) */
    uint32_t hfi_iova, debug_iova, debug_bytes, log_iova, log_bytes;
} A630GmuMemory;
typedef struct {
    uint32_t seq;                     /* last sequence number sent */
    uint32_t last_id, last_header, last_ret, last_error, replies, skipped, fw_errors, first_fw_error;
    uint32_t fw_version;              /* the FW_VERSION reply's first payload word */
} A630Hfi;
/* a6xx_hfi_init: the queue table, command queue (id 0) and response queue (legacy id 4), 4 KiB each at +4K and +8K. */
void A630HfiInit(const A630GmuMemory *m, A630Hfi *h);

typedef struct {
    uint32_t chip_id;                 /* 0x06030001 or 0x06030002 */
    const uint8_t *firmware;          /* a630_gmu.bin: a flat ITCM image of at most 0x8000 bytes */
    size_t firmware_bytes;
    const A630PerfTable *perf;
    unsigned boot_level;              /* gx_votes index for the boot vote (Linux: the highest, perf->gpu_count - 1) */
} A630GmuConfig;
typedef struct {
    unsigned step;                    /* A630GmuStep reached */
    A630Status status;
    uint32_t fw_init_result, hfi_status, sptprac_status, gx_status, rpmh_power_state, ao_status, g2h_info, fw_version;
    uint32_t rsc_ack, seq_busy, oob_info, core_fw_version;
    unsigned polls;
} A630GmuReport;
enum A630GmuStep {
    A630StepRpmhInit = 1, A630StepRpmhStart, A630StepFwLoad, A630StepFwStart, A630StepGmuStart, A630StepRailOn,
    A630StepSptprac, A630StepHfiStart, A630StepHfiInit, A630StepHfiVersion, A630StepHfiPerf, A630StepHfiBw,
    A630StepHfiTest, A630StepFreq, A630StepGpuSet, A630StepDone
};
/* a6xx_gmu_rpmh_init: the RSC and PDC sequences of a pre-650 GPU (PDC not in AOP, pdc_address_offset 0x30080), then
 * a6xx_rpmh_stop. Once per boot, before the first start. */
A630Status A630RpmhInit(const A630Io *io, A630GmuReport *r);
/* a6xx_gmu_resume for a cold boot (GMU_GENERAL_7 still 0): AO interrupts, a6xx_gmu_fw_start (TCM retention, RSC on, ITCM
 * load, boot config, HFI table, fence range, CM3 config, chip id, log, power config, CM3 release, GX rail vote + BOOT_SLUMBER
 * OOB, SPTPRAC, HFI start), a6xx_hfi_start_v1 (GMU_INIT, FW_VERSION, perf table v1, the A630 bandwidth table, TEST), the
 * GMU2HOST interrupt mask and the initial frequency (a6xx_gmu_set_freq legacy path). */
A630Status A630GmuColdStart(const A630Io *io, const A630GmuMemory *m, A630Hfi *h, const A630GmuConfig *c, A630GmuReport *r);
/* Legacy OOB requests (a6xx_gmu_set_oob / clear_oob, gmu->legacy bits). */
enum A630Oob { A630OobGpuSet = 0, A630OobPerfcounter = 1, A630OobBootSlumber = 2, A630OobDcvs = 3 };
A630Status A630SetOob(const A630Io *io, unsigned oob, uint32_t *info);
void A630ClearOob(const A630Io *io, unsigned oob);
/* GMU_SPTPRAC_PWR_CLK_STATUS: GX on (a6xx_gmu_gx_is_on) and SPTPRAC on (a6xx_gmu_sptprac_is_on). */
int A630GxOn(const A630Io *io);
int A630SptpracOn(const A630Io *io);
/* a6xx_gmu_set_freq, legacy: DCVS perf index through the DCVS_SET OOB; returns GMU_DCVS_RETURN in *ret. */
A630Status A630SetFreq(const A630Io *io, unsigned perf_index, uint32_t *ret);

/* HFI building blocks, also used by the tests: send one command and wait for its acknowledgement (a6xx_hfi_send_msg). */
A630Status A630HfiSend(const A630Io *io, const A630GmuMemory *m, A630Hfi *h, unsigned id, uint32_t *words, unsigned dwords,
                       uint32_t *payload, unsigned payload_words);

#ifdef __cplusplus
}
#endif
#endif
