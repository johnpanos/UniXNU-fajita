/* SPDX-License-Identifier: MIT */
/* Adreno 630 power-up and legacy GMU cold start; see A630Gmu.h. Linux v6.16 function names are given at each step. */
#include "A630Gmu.h"
#include "A630Regs.h"

/* ---- register helpers ------------------------------------------------------------------------------------------------ */
static uint32_t gcc(const A630Io *io, uint32_t off) {return io->read32(io->cookie, A630BlockGcc, off);}
static void gccw(const A630Io *io, uint32_t off, uint32_t v) {io->write32(io->cookie, A630BlockGcc, off, v);}
static uint32_t cc(const A630Io *io, uint32_t off) {return io->read32(io->cookie, A630BlockGpuCc, off);}
static void ccw(const A630Io *io, uint32_t off, uint32_t v) {io->write32(io->cookie, A630BlockGpuCc, off, v);}
static uint32_t gmu(const A630Io *io, uint32_t reg) {return io->read32(io->cookie, A630BlockGmu, reg * 4);}
static void gmuw(const A630Io *io, uint32_t reg, uint32_t v) {io->write32(io->cookie, A630BlockGmu, reg * 4, v);}
static void gmurmw(const A630Io *io, uint32_t reg, uint32_t clear, uint32_t set) {gmuw(io, reg, (gmu(io, reg) & ~clear) | set);}
static uint32_t rscc(const A630Io *io, uint32_t reg) {return io->read32(io->cookie, A630BlockRscc, reg * 4);}
static void rsccw(const A630Io *io, uint32_t reg, uint32_t v) {io->write32(io->cookie, A630BlockRscc, reg * 4, v);}
static void pdcw(const A630Io *io, uint32_t reg, uint32_t v) {io->write32(io->cookie, A630BlockPdc, reg * 4, v);}
static void pdcseqw(const A630Io *io, uint32_t reg, uint32_t v) {io->write32(io->cookie, A630BlockPdcSeq, reg * 4, v);}
static void barrier(const A630Io *io) {if (io->barrier) io->barrier(io->cookie);}
static void delay(const A630Io *io, unsigned us) {if (io->delay_us) io->delay_us(io->cookie, us);}

/* gmu_poll_timeout(gmu, reg, val, cond, interval_us, timeout_us): poll until the masked value matches. */
static int pollGmu(const A630Io *io, uint32_t reg, uint32_t mask, uint32_t want, unsigned interval, unsigned timeout,
                   uint32_t *last, unsigned *polls)
{
    for (unsigned waited = 0;; waited += interval) {
        uint32_t v = gmu(io, reg);
        if (last) *last = v;
        if (polls) ++*polls;
        if ((v & mask) == want) return 1;
        if (waited >= timeout) return 0;
        delay(io, interval);
    }
}
static int pollRscc(const A630Io *io, uint32_t reg, uint32_t mask, uint32_t want, unsigned interval, unsigned timeout,
                    uint32_t *last)
{
    for (unsigned waited = 0;; waited += interval) {
        uint32_t v = rscc(io, reg);
        if (last) *last = v;
        if ((v & mask) == want) return 1;
        if (waited >= timeout) return 0;
        delay(io, interval);
    }
}

/* ---- command DB (drivers/soc/qcom/cmd-db.c) -------------------------------------------------------------------------- */
static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;}

A630Status A630CmdDbFind(const uint8_t *db, size_t bytes, const char *id, A630CmdDbEntry *out)
{
    enum {kHeader = 8, kRscHdr = 16, kSlaves = 8, kData = 8 + 16 * 8 + 8, kEntry = 24};
    if (!db || !id || !out || bytes < kData) return A630ErrArgument;
    if (db[4] != 0xdb || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0c) return A630ErrCmdDb;
    for (unsigned i = 0; i < kSlaves; ++i) {
        const uint8_t *h = db + kHeader + kRscHdr * i;
        const unsigned slave = le16(h), header = le16(h + 2), data = le16(h + 4), count = le16(h + 6);
        if (!slave) break;   /* cmd_db_get_header: the table ends at the first zero slv_id */
        for (unsigned j = 0; j < count; ++j) {
            const size_t at = (size_t)kData + header + (size_t)j * kEntry;
            if (at + kEntry > bytes) return A630ErrCmdDb;
            const uint8_t *e = db + at;
            unsigned k = 0;
            for (; k < 8; ++k) {   /* strncmp(ent->id, id, sizeof(ent->id)) */
                if (e[k] != (uint8_t)id[k]) break;
                if (!id[k]) {k = 8; break;}
            }
            if (k != 8) continue;
            const size_t len = le16(e + 20), off = (size_t)kData + data + le16(e + 22);
            if (off > bytes || len > bytes - off) return A630ErrCmdDb;
            out->data = db + off;
            out->bytes = len;
            out->address = le32(e + 16);
            return A630Ok;
        }
    }
    return A630ErrCmdDb;
}

/* RPMH_REGULATOR_LEVEL_* (include/dt-bindings/power/qcom-rpmpd.h) of the sdm845.dtsi OPP tables. */
void A630DefaultLevels(A630PerfTable *t, uint16_t gpu_levels[A630MaxGpuLevels], uint16_t gmu_levels[A630MaxGmuLevels])
{
    static const uint32_t gpu_khz[] = {0, 257000, 342000, 414000, 520000, 596000, 675000, 710000};
    static const uint16_t gpu_lvl[] = {0, 64, 128, 192, 256, 320, 384, 416};   /* LOW_SVS SVS SVS_L1 NOM NOM_L1 TURBO TURBO_L1 */
    static const uint32_t gmu_khz[] = {0, 200000, 400000};
    static const uint16_t gmu_lvl[] = {0, 48, 128};                            /* MIN_SVS SVS */
    for (unsigned i = 0; i < A630MaxGpuLevels; ++i) {t->gpu_khz[i] = 0; t->gx_votes[i] = 0; gpu_levels[i] = 0;}
    for (unsigned i = 0; i < A630MaxGmuLevels; ++i) {t->gmu_khz[i] = 0; t->cx_votes[i] = 0; gmu_levels[i] = 0;}
    t->gpu_count = sizeof gpu_khz / sizeof *gpu_khz;
    t->gmu_count = sizeof gmu_khz / sizeof *gmu_khz;
    for (unsigned i = 0; i < t->gpu_count; ++i) {t->gpu_khz[i] = gpu_khz[i]; gpu_levels[i] = gpu_lvl[i];}
    for (unsigned i = 0; i < t->gmu_count; ++i) {t->gmu_khz[i] = gmu_khz[i]; gmu_levels[i] = gmu_lvl[i];}
}

A630Status A630ArcVotes(const A630CmdDbEntry *pri, const A630CmdDbEntry *sec, const uint16_t *levels, unsigned count,
                        uint32_t *votes)
{
    const unsigned pn = (unsigned)(pri->bytes / 2), sn = (unsigned)(sec->bytes / 2);
    if (!pn || !sn) return A630ErrCmdDb;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned level = levels[i];
        unsigned pindex = 0, sindex = 0, j;
        for (j = 0; j < pn; ++j)
            if (le16(pri->data + 2 * j) >= level) {pindex = j; break;}
        if (j == pn) return A630ErrVotes;
        for (j = 0; j < sn; ++j) {
            const unsigned s = le16(sec->data + 2 * j);
            if (s >= level) {sindex = j; break;}
            else if (s) sindex = j;
        }
        votes[i] = ((uint32_t)le16(pri->data + 2 * pindex) << 16) | (sindex << 8) | pindex;
    }
    return A630Ok;
}

A630Status A630BuildPerfTable(const uint8_t *db, size_t bytes, A630PerfTable *t)
{
    uint16_t gpu[A630MaxGpuLevels], gmul[A630MaxGmuLevels];
    A630CmdDbEntry gfx, cx, mx;
    A630DefaultLevels(t, gpu, gmul);
    if (A630CmdDbFind(db, bytes, "gfx.lvl", &gfx) || A630CmdDbFind(db, bytes, "cx.lvl", &cx)) return A630ErrCmdDb;
    /* a6xx_gmu_rpmh_arc_votes_init: gmxc.lvl first, else mx.lvl (SDM845 has no GMxC). */
    if (A630CmdDbFind(db, bytes, "gmxc.lvl", &mx) && A630CmdDbFind(db, bytes, "mx.lvl", &mx)) return A630ErrCmdDb;
    A630Status s = A630ArcVotes(&gfx, &mx, gpu, t->gpu_count, t->gx_votes);
    if (!s) s = A630ArcVotes(&cx, &mx, gmul, t->gmu_count, t->cx_votes);
    return s;
}

/* ---- clocks and CX ---------------------------------------------------------------------------------------------------- */
enum {
    kGccGpuCfgAhb = 0x71004, kGccGpuMemnocGfx = 0x7100c, kGccDdrssGpuAxi = 0x44038, kGccGpll0Vote = 0x52004,
    kGccGpuMisc = 0x71028,
    kCcCxGdscr = 0x106c, kCcGmuCmdRcgr = 0x1120, kCcGmuCfgRcgr = 0x1124, kCcCxGmuCbcr = 0x1098, kCcCxoCbcr = 0x109c,
    kCcGxGdscr = 0x100c,
    kBranchOff = 1u << 31, kBranchEnable = 1u << 0,
    kGdscPwrOn = 1u << 31, kGdscSwCollapse = 1u << 0,
    kRcgUpdate = 1u << 0, kRcgRootEn = 1u << 1, kRcgRootOff = 1u << 31,
};
static int branchOn(const A630Io *io, unsigned block, uint32_t off, unsigned *polls)
{
    /* clk_branch2 enable: set the enable bit, then wait for CLK_OFF to clear (BRANCH_HALT / BRANCH_VOTED, 200 us). */
    uint32_t v = io->read32(io->cookie, block, off);
    if (!(v & kBranchEnable)) io->write32(io->cookie, block, off, v | kBranchEnable);
    barrier(io);
    for (unsigned i = 0; i < 200; ++i) {
        if (polls) ++*polls;
        if (!(io->read32(io->cookie, block, off) & kBranchOff)) return 1;
        delay(io, 1);
    }
    return 0;
}

A630Status A630PowerCx(const A630Io *io, A630ClockReport *r)
{
    if (!io || !r) return A630ErrArgument;
    r->polls = 0;
    /* gcc_gpu_cfg_ahb_clk is CLK_IS_CRITICAL: the bootloader leaves it on (seen on fajita). Without it nothing in GPUCC or
     * the GPU can be read, so it is checked first. */
    if (!branchOn(io, A630BlockGcc, kGccGpuCfgAhb, &r->polls)) {r->gcc_cfg_ahb = gcc(io, kGccGpuCfgAhb); return A630ErrClock;}
    /* gcc_sdm845_probe: "Disable the GPLL0 active input to MMSS and GPU via MISC registers". */
    gccw(io, kGccGpuMisc, gcc(io, kGccGpuMisc) | 3);
    /* gcc_gpu_gpll0_clk_src / gcc_gpu_gpll0_div_clk_src: votes in 0x52004 bits 15 and 16, BRANCH_HALT_DELAY. */
    gccw(io, kGccGpll0Vote, gcc(io, kGccGpll0Vote) | (1u << 15) | (1u << 16));
    barrier(io);
    delay(io, 10);
    /* The adreno_smmu "bus" clock and the GMU "memnoc"/"axi" clocks (BRANCH_VOTED). */
    int ok = branchOn(io, A630BlockGcc, kGccGpuMemnocGfx, &r->polls);
    ok &= branchOn(io, A630BlockGcc, kGccDdrssGpuAxi, &r->polls);
    r->gcc_cfg_ahb = gcc(io, kGccGpuCfgAhb);
    r->gcc_memnoc = gcc(io, kGccGpuMemnocGfx);
    r->gcc_axi = gcc(io, kGccDdrssGpuAxi);
    r->gcc_gpll0 = gcc(io, kGccGpll0Vote);
    r->gcc_misc = gcc(io, kGccGpuMisc);
    if (!ok) return A630ErrClock;
    /* gpu_cx_gdsc (gdsc_init then gdsc_enable): HW_CONTROL and SW_OVERRIDE off, EN_REST_WAIT 2, EN_FEW_WAIT 8,
     * CLK_DIS_WAIT 8 (clk_dis_wait_val), then SW_COLLAPSE cleared and PWR_ON awaited. */
    r->cx_before = cc(io, kCcCxGdscr);
    uint32_t v = (r->cx_before & ~(0x2u | 0x4u | (0xfu << 20) | (0xfu << 16) | (0xfu << 12))) | (2u << 20) | (8u << 16) | (8u << 12);
    ccw(io, kCcCxGdscr, v);
    ccw(io, kCcCxGdscr, v & ~kGdscSwCollapse);
    barrier(io);
    unsigned i = 0;
    for (; i < 500 && !(cc(io, kCcCxGdscr) & kGdscPwrOn); ++i) delay(io, 1);
    r->polls += i;
    r->cx_after = cc(io, kCcCxGdscr);
    if (!(r->cx_after & kGdscPwrOn)) return A630ErrGdsc;
    delay(io, 1);   /* gdsc_enable: clocks of the domain must not run within 400 ns of power-up */
    /* gpu_cc_cxo_clk ("cxo" of the GMU). */
    ok = branchOn(io, A630BlockGpuCc, kCcCxoCbcr, &r->polls);
    r->cxo = cc(io, kCcCxoCbcr);
    r->gx_gdscr = cc(io, kCcGxGdscr);
    return ok ? A630Ok : A630ErrClock;
}

A630Status A630GmuClocks(const A630Io *io, A630ClockReport *r)
{
    if (!io || !r) return A630ErrArgument;
    /* gpu_cc_sdm845_probe: CX_GMU_CBCR wake and sleep fields 0xf. */
    uint32_t cbcr = cc(io, kCcCxGmuCbcr);
    ccw(io, kCcCxGmuCbcr, (cbcr & ~(0xffu << 4)) | (0xfu << 8) | (0xfu << 4));
    /* clk_rcg2_shared: 200 MHz = P_GPLL0_OUT_MAIN_DIV (parent map index 6) / 1.5 (hid 2 * 1.5 - 1 = 2). The RCG is off, so
     * the configuration is written under ROOT_EN (clk_rcg2_shared_enable -> set_force_enable, update_config). */
    ccw(io, kCcGmuCmdRcgr, cc(io, kCcGmuCmdRcgr) | kRcgRootEn);
    barrier(io);
    unsigned i = 0;
    for (; i < 500 && (cc(io, kCcGmuCmdRcgr) & kRcgRootOff); ++i) delay(io, 1);
    r->polls += i;
    uint32_t cfg = cc(io, kCcGmuCfgRcgr);
    cfg = (cfg & ~((0x7u << 8) | 0x1fu | (0x3u << 12))) | (6u << 8) | 2u;   /* SRC_SEL, SRC_DIV, MODE (no M/N) */
    ccw(io, kCcGmuCfgRcgr, cfg);
    ccw(io, kCcGmuCmdRcgr, cc(io, kCcGmuCmdRcgr) | kRcgUpdate);
    barrier(io);
    for (i = 0; i < 500 && (cc(io, kCcGmuCmdRcgr) & kRcgUpdate); ++i) delay(io, 1);
    r->polls += i;
    int updated = !(cc(io, kCcGmuCmdRcgr) & kRcgUpdate);
    /* gpu_cc_cx_gmu_clk ("gmu"); then the force enable is dropped, the branch keeps the root on. */
    int ok = branchOn(io, A630BlockGpuCc, kCcCxGmuCbcr, &r->polls);
    ccw(io, kCcGmuCmdRcgr, cc(io, kCcGmuCmdRcgr) & ~kRcgRootEn);
    r->rcg_cfg = cc(io, kCcGmuCfgRcgr);
    r->cx_gmu = cc(io, kCcCxGmuCbcr);
    return ok && updated ? A630Ok : A630ErrClock;
}

/* ---- RSC / PDC (a6xx_gmu_rpmh_init, a6xx_rpmh_start, a6xx_rpmh_stop) ------------------------------------------------- */
static A630Status rpmhStop(const A630Io *io, A630GmuReport *r)
{
    gmuw(io, A630_GMU_RSCC_CONTROL_REQ, 1);
    barrier(io);
    int ok = pollRscc(io, A630_RSCC_GPU_RSCC_RSC_STATUS0_DRV0, 1u << 16, 1u << 16, 100, 10000, &r->seq_busy);
    gmuw(io, A630_GMU_RSCC_CONTROL_REQ, 0);
    return ok ? A630Ok : A630ErrRsc;
}

A630Status A630RpmhInit(const A630Io *io, A630GmuReport *r)
{
    if (!io || !r) return A630ErrArgument;
    r->step = A630StepRpmhInit;
    const uint32_t pdc_address_offset = 0x30080;   /* a630: neither a618/a640 nor a619 */
    /* Disable SDE clock gating */
    rsccw(io, A630_RSCC_GPU_RSCC_RSC_STATUS0_DRV0, 1u << 24);
    /* Setup RSC PDC handshake for sleep and wakeup */
    rsccw(io, A630_RSCC_RSCC_PDC_SLAVE_ID_DRV0, 1);
    rsccw(io, A630_RSCC_RSCC_HIDDEN_TCS_CMD0_DATA, 0);
    rsccw(io, A630_RSCC_RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
    rsccw(io, A630_RSCC_RSCC_HIDDEN_TCS_CMD0_DATA + 2, 0);
    rsccw(io, A630_RSCC_RSCC_HIDDEN_TCS_CMD0_ADDR + 2, 0);
    rsccw(io, A630_RSCC_RSCC_HIDDEN_TCS_CMD0_DATA + 4, 0x80000000);
    rsccw(io, A630_RSCC_RSCC_HIDDEN_TCS_CMD0_ADDR + 4, 0);
    rsccw(io, A630_RSCC_RSCC_OVERRIDE_START_ADDR, 0);
    rsccw(io, A630_RSCC_RSCC_PDC_SEQ_START_ADDR, 0x4520);
    rsccw(io, A630_RSCC_RSCC_PDC_MATCH_VALUE_LO, 0x4510);
    rsccw(io, A630_RSCC_RSCC_PDC_MATCH_VALUE_HI, 0x4514);
    /* Load RSC sequencer uCode for sleep and wakeup */
    rsccw(io, A630_RSCC_RSCC_SEQ_MEM_0_DRV0, 0xa7a506a0);
    rsccw(io, A630_RSCC_RSCC_SEQ_MEM_0_DRV0 + 1, 0xa1e6a6e7);
    rsccw(io, A630_RSCC_RSCC_SEQ_MEM_0_DRV0 + 2, 0xa2e081e1);
    rsccw(io, A630_RSCC_RSCC_SEQ_MEM_0_DRV0 + 3, 0xe9a982e2);
    rsccw(io, A630_RSCC_RSCC_SEQ_MEM_0_DRV0 + 4, 0x0020e8a8);
    /* Load PDC sequencer uCode for power up and power down sequence */
    pdcseqw(io, A630_PDC_SEQ_MEM_0, 0xfebea1e1);
    pdcseqw(io, A630_PDC_SEQ_MEM_0 + 1, 0xa5a4a3a2);
    pdcseqw(io, A630_PDC_SEQ_MEM_0 + 2, 0x8382a6e0);
    pdcseqw(io, A630_PDC_SEQ_MEM_0 + 3, 0xbce3e284);
    pdcseqw(io, A630_PDC_SEQ_MEM_0 + 4, 0x002081fc);
    /* Set TCS commands used by PDC sequence for low power modes */
    pdcw(io, A630_PDC_GPU_TCS1_CMD_ENABLE_BANK, 7);
    pdcw(io, A630_PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK, 0);
    pdcw(io, A630_PDC_GPU_TCS1_CONTROL, 0);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_MSGID, 0x10108);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_ADDR, 0x30010);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_DATA, 1);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_MSGID + 4, 0x10108);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_ADDR + 4, 0x30000);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_DATA + 4, 0x0);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_MSGID + 8, 0x10108);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_ADDR + 8, pdc_address_offset);
    pdcw(io, A630_PDC_GPU_TCS1_CMD0_DATA + 8, 0x0);
    pdcw(io, A630_PDC_GPU_TCS3_CMD_ENABLE_BANK, 7);
    pdcw(io, A630_PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK, 0);
    pdcw(io, A630_PDC_GPU_TCS3_CONTROL, 0);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_MSGID, 0x10108);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_ADDR, 0x30010);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_DATA, 2);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_MSGID + 4, 0x10108);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_ADDR + 4, 0x30000);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_DATA + 4, 0x3);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_MSGID + 8, 0x10108);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_ADDR + 8, pdc_address_offset);
    pdcw(io, A630_PDC_GPU_TCS3_CMD0_DATA + 8, 0x3);
    /* Setup GPU PDC */
    pdcw(io, A630_PDC_GPU_SEQ_START_ADDR, 0);
    pdcw(io, A630_PDC_GPU_ENABLE_PDC, 0x80000001);
    /* ensure no writes happen before the uCode is fully written */
    barrier(io);
    A630Status s = rpmhStop(io, r);
    r->status = s;
    return s;
}

static A630Status rpmhStart(const A630Io *io, A630GmuReport *r)
{
    gmuw(io, A630_GMU_RSCC_CONTROL_REQ, 1u << 1);
    barrier(io);
    if (!pollGmu(io, A630_GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, 100, 10000, &r->rsc_ack, &r->polls)) return A630ErrRsc;
    if (!pollRscc(io, A630_RSCC_RSCC_SEQ_BUSY_DRV0, ~0u, 0, 100, 10000, &r->seq_busy)) return A630ErrRsc;
    gmuw(io, A630_GMU_RSCC_CONTROL_REQ, 0);
    return A630Ok;
}

/* ---- OOB (a6xx_gmu_oob_bits, legacy columns) ----------------------------------------------------------------------- */
static const uint8_t kOobSet[4] = {16, 17, 22, 23}, kOobAck[4] = {24, 25, 30, 31}, kOobClear[4] = {24, 25, 30, 31};
A630Status A630SetOob(const A630Io *io, unsigned oob, uint32_t *info)
{
    if (oob > 3) return A630ErrArgument;
    uint32_t last = 0;
    gmuw(io, A630_GMU_HOST2GMU_INTR_SET, 1u << kOobSet[oob]);
    barrier(io);
    int ok = pollGmu(io, A630_GMU_GMU2HOST_INTR_INFO, 1u << kOobAck[oob], 1u << kOobAck[oob], 100, 10000, &last, 0);
    if (info) *info = last;
    gmuw(io, A630_GMU_GMU2HOST_INTR_CLR, 1u << kOobAck[oob]);
    return ok ? A630Ok : A630ErrOob;
}
void A630ClearOob(const A630Io *io, unsigned oob)
{
    if (oob > 3) return;
    gmuw(io, A630_GMU_HOST2GMU_INTR_SET, 1u << kOobClear[oob]);
    barrier(io);
}
int A630GxOn(const A630Io *io) {return !(gmu(io, A630_GMU_SPTPRAC_PWR_CLK_STATUS) & ((1u << 6) | (1u << 7)));}
int A630SptpracOn(const A630Io *io) {return !(gmu(io, A630_GMU_SPTPRAC_PWR_CLK_STATUS) & ((1u << 2) | (1u << 4)));}

/* ---- HFI v1 (a6xx_hfi.c with gmu->legacy) ---------------------------------------------------------------------------- */
enum {
    kQueueTable = 0, kTableWords = 6, kHeaderWords = 12, kQueueWords = 1024,   /* SZ_4K >> 2 */
    kCmdQueue = 0x1000 / 4, kRspQueue = 0x2000 / 4,
    /* queue header word indices */
    qStatus = 0, qIova, qType, qSize, qMsgSize, qDropped, qRxWm, qTxWm, qRxReq, qTxReq, qRead, qWrite,
};
static volatile uint32_t *qhdr(const A630GmuMemory *m, unsigned q) {return m->hfi + kTableWords + kHeaderWords * q;}

void A630HfiInit(const A630GmuMemory *m, A630Hfi *h)
{
    for (unsigned i = 0; i < 0x4000 / 4; ++i) m->hfi[i] = 0;
    m->hfi[0] = 0;                                     /* version */
    m->hfi[1] = kTableWords + 2 * kHeaderWords;        /* size: table header + both queue headers, in dwords */
    m->hfi[2] = kTableWords;                           /* qhdr0_offset (sizeof(*table) >> 2) */
    m->hfi[3] = kHeaderWords;                          /* qhdr_size */
    m->hfi[4] = 2;                                     /* num_queues */
    m->hfi[5] = 2;                                     /* active_queues */
    const uint32_t offsets[2] = {0x1000, 0x2000}, ids[2] = {0, 4};   /* legacy: the response queue is id 4 */
    for (unsigned q = 0; q < 2; ++q) {
        volatile uint32_t *hd = qhdr(m, q);
        hd[qIova] = m->hfi_iova + offsets[q];
        hd[qType] = 10u << 8 | ids[q];
        hd[qStatus] = 1;
        hd[qSize] = kQueueWords;
        hd[qMsgSize] = 0;
        hd[qDropped] = 0;
        hd[qRxWm] = 1;
        hd[qTxWm] = 1;
        hd[qRxReq] = 1;
        hd[qTxReq] = 0;
        hd[qRead] = 0;
        hd[qWrite] = 0;
    }
    *h = (A630Hfi){0};
}

static A630Status queueWrite(const A630Io *io, const A630GmuMemory *m, const uint32_t *data, unsigned dwords)
{
    volatile uint32_t *hd = qhdr(m, 0), *q = m->hfi + kCmdQueue;
    uint32_t index = hd[qWrite], read = hd[qRead], size = hd[qSize];
    if (size != kQueueWords || index >= size || read >= size) return A630ErrHfiSend;
    const uint32_t space = (read - index - 1) & (size - 1);   /* CIRC_SPACE */
    if (space < dwords) {hd[qDropped] = hd[qDropped] + 1; return A630ErrHfiSend;}
    for (unsigned i = 0; i < dwords; ++i) {q[index] = data[i]; index = (index + 1) % size;}
    barrier(io);
    hd[qWrite] = index;
    barrier(io);
    gmuw(io, A630_GMU_HOST2GMU_INTR_SET, 0x01);
    return A630Ok;
}

/* a6xx_hfi_queue_read (legacy: no 4-dword alignment of the read index). Returns the packet size, 0 when empty, ~0u when
 * the header claims more than fits. */
static unsigned queueRead(const A630GmuMemory *m, uint32_t *data, unsigned dwords)
{
    volatile uint32_t *hd = qhdr(m, 1), *q = m->hfi + kRspQueue;
    uint32_t index = hd[qRead];
    if (index == hd[qWrite]) {hd[qRxReq] = 1; return 0;}
    if (index >= kQueueWords) return ~0u;
    const uint32_t hdr = q[index];
    const unsigned size = (hdr >> 8) & 0xff;
    if (!size || size > dwords) return ~0u;
    for (unsigned i = 0; i < size; ++i) {data[i] = q[index]; index = (index + 1) % kQueueWords;}
    hd[qRead] = index;
    return size;
}

static A630Status waitMsg(const A630Io *io)
{
    /* a6xx_hfi_wait_for_msg_interrupt: MSGQ (bit 0) of GMU2HOST_INTR_INFO, 100 us interval, 1 s. */
    if (!pollGmu(io, A630_GMU_GMU2HOST_INTR_INFO, 1, 1, 100, 1000000, 0, 0)) return A630ErrHfiTimeout;
    gmuw(io, A630_GMU_GMU2HOST_INTR_CLR, 1);
    return A630Ok;
}

A630Status A630HfiSend(const A630Io *io, const A630GmuMemory *m, A630Hfi *h, unsigned id, uint32_t *words, unsigned dwords,
                       uint32_t *payload, unsigned payload_words)
{
    if (!dwords || dwords > 255) return A630ErrArgument;
    const uint32_t seq = (h->seq + 1) % 0xfff;   /* atomic_inc_return(&queue->seqnum) % 0xfff */
    h->seq = h->seq + 1;
    words[0] = (seq << 20) | (0u << 16) | (dwords << 8) | id;   /* HFI_MSG_CMD */
    h->last_id = id;
    h->last_header = words[0];
    A630Status s = queueWrite(io, m, words, dwords);
    if (s) return s;
    s = waitMsg(io);
    if (s) return s;
    for (;;) {
        uint32_t resp[19] = {0};   /* struct a6xx_hfi_msg_response */
        const unsigned n = queueRead(m, resp, 19);
        if (n == ~0u) return A630ErrHfiReply;
        if (!n) {
            s = waitMsg(io);
            if (s) return s;
            continue;
        }
        h->replies = h->replies + 1;
        if ((resp[0] & 0xff) == 100) {   /* HFI_F2H_MSG_ERROR: {header, code}; logged and skipped */
            h->fw_errors = h->fw_errors + 1;
            if (!h->first_fw_error) h->first_fw_error = resp[1] | 0x80000000u;
            continue;
        }
        if (((resp[1] >> 20) & 0xfff) != seq) {h->skipped = h->skipped + 1; continue;}
        h->last_ret = resp[1];
        h->last_error = resp[2];
        if (resp[2]) return A630ErrHfiReply;
        for (unsigned i = 0; payload && i < payload_words && i < 16; ++i) payload[i] = resp[3 + i];
        return A630Ok;
    }
}

/* ---- GMU cold start (a6xx_gmu_resume -> a6xx_gmu_fw_start -> a6xx_hfi_start_v1) -------------------------------------- */
static A630Status hfiStartV1(const A630Io *io, const A630GmuMemory *m, A630Hfi *h, const A630GmuConfig *c, A630GmuReport *r)
{
    uint32_t msg[160];
    /* a6xx_hfi_send_gmu_init: {header, seg_id, dbg_buffer_addr, dbg_buffer_size, boot_state} (GMU_COLD_BOOT = 0). */
    r->step = A630StepHfiInit;
    msg[1] = 0; msg[2] = m->debug_iova; msg[3] = m->debug_bytes; msg[4] = 0;
    A630Status s = A630HfiSend(io, m, h, 0, msg, 5, 0, 0);
    if (s) return s;
    /* a6xx_hfi_get_fw_version: supported version 1.10. */
    r->step = A630StepHfiVersion;
    msg[1] = (1u << 28) | (1u << 19) | (1u << 17);
    uint32_t version[16] = {0};
    s = A630HfiSend(io, m, h, 1, msg, 2, version, 16);
    if (s) return s;
    h->fw_version = version[0];
    r->fw_version = version[0];
    /* a6xx_hfi_send_perf_table_v1: {header, num_gpu_levels, num_gmu_levels, gx_votes[16]{vote, freq}, cx_votes[4]{...}}. */
    r->step = A630StepHfiPerf;
    for (unsigned i = 0; i < 43; ++i) msg[i] = 0;
    msg[1] = c->perf->gpu_count;
    msg[2] = c->perf->gmu_count;
    for (unsigned i = 0; i < c->perf->gpu_count; ++i) {msg[3 + 2 * i] = c->perf->gx_votes[i]; msg[4 + 2 * i] = c->perf->gpu_khz[i];}
    for (unsigned i = 0; i < c->perf->gmu_count; ++i) {msg[35 + 2 * i] = c->perf->cx_votes[i]; msg[36 + 2 * i] = c->perf->gmu_khz[i];}
    s = A630HfiSend(io, m, h, 4, msg, 43, 0, 0);
    if (s) return s;
    /* a6xx_build_bw_table: "a single 'off' entry since the 630 GMU doesn't do bus scaling". struct a6xx_hfi_msg_bw_table:
     * header, bw_level_num, cnoc_cmds_num, ddr_cmds_num, cnoc_wait_bitmask, ddr_wait_bitmask, cnoc_cmds_addrs[6],
     * cnoc_cmds_data[2][6], ddr_cmds_addrs[8], ddr_cmds_data[16][8] = 160 dwords. */
    r->step = A630StepHfiBw;
    for (unsigned i = 0; i < 160; ++i) msg[i] = 0;
    enum {bwLevels = 1, cnocNum, ddrNum, cnocWait, ddrWait, cnocAddr = 6, cnocData = 12, ddrAddr = 24, ddrData = 32};
    msg[bwLevels] = 1;
    msg[ddrNum] = 3;
    msg[ddrWait] = 0x07;
    msg[ddrAddr + 0] = 0x50000; msg[ddrAddr + 1] = 0x5005c; msg[ddrAddr + 2] = 0x5000c;
    msg[ddrData + 0] = 0x40000000; msg[ddrData + 1] = 0x40000000; msg[ddrData + 2] = 0x40000000;
    msg[cnocNum] = 3;
    msg[cnocWait] = 0x05;
    msg[cnocAddr + 0] = 0x50034; msg[cnocAddr + 1] = 0x5007c; msg[cnocAddr + 2] = 0x5004c;
    msg[cnocData + 0] = 0x40000000; msg[cnocData + 1] = 0x00000000; msg[cnocData + 2] = 0x40000000;
    msg[cnocData + 6] = 0x60000001; msg[cnocData + 7] = 0x20000001; msg[cnocData + 8] = 0x60000001;
    s = A630HfiSend(io, m, h, 3, msg, 160, 0, 0);
    if (s) return s;
    /* a6xx_hfi_send_test: "no more HFI messages until next boot"; Linux ignores its result. */
    r->step = A630StepHfiTest;
    (void)A630HfiSend(io, m, h, 5, msg, 1, 0, 0);
    return A630Ok;
}

A630Status A630SetFreq(const A630Io *io, unsigned perf_index, uint32_t *ret)
{
    gmuw(io, A630_GMU_DCVS_ACK_OPTION, 0);
    gmuw(io, A630_GMU_DCVS_PERF_SETTING, ((3u & 0xf) << 28) | perf_index);
    /* "Send an invalid index as a vote for the bus bandwidth and let the firmware decide on the right vote" */
    gmuw(io, A630_GMU_DCVS_BW_SETTING, 0xff);
    A630Status s = A630SetOob(io, A630OobDcvs, 0);
    A630ClearOob(io, A630OobDcvs);
    if (ret) *ret = gmu(io, A630_GMU_DCVS_RETURN);
    return s;
}

A630Status A630GmuColdStart(const A630Io *io, const A630GmuMemory *m, A630Hfi *h, const A630GmuConfig *c, A630GmuReport *r)
{
    if (!io || !m || !h || !c || !r || !c->perf || !c->firmware || !c->firmware_bytes || (c->firmware_bytes & 3) ||
        c->boot_level >= c->perf->gpu_count)
        return A630ErrArgument;
    A630Status s = A630Ok;
#define FAIL(x) do {s = (x); goto done;} while (0)
    /* a6xx_gmu_resume: enable the GMU interrupt (AO), all but WDOG_BITE, HOST_AHB_BUS_ERROR and FENCE_ERR masked. */
    gmuw(io, A630_GMU_AO_HOST_INTERRUPT_CLR, ~0u);
    gmuw(io, A630_GMU_AO_HOST_INTERRUPT_MASK, ~((1u << 0) | (1u << 5) | (1u << 3)));
    /* a6xx_gmu_fw_start(GMU_COLD_BOOT): TCM retention on (legacy: GMU_GENERAL_7). */
    gmuw(io, A630_GMU_GENERAL_7, 1);
    r->step = A630StepRpmhStart;
    if ((s = rpmhStart(io, r))) goto done;
    /* a6xx_gmu_fw_load, legacy: the flat image straight into the CM3 ITCM. */
    r->step = A630StepFwLoad;
    if (c->firmware_bytes > 0x8000) FAIL(A630ErrFirmware);
    for (size_t i = 0; i < c->firmware_bytes / 4; ++i) {
        const uint8_t *p = c->firmware + 4 * i;
        gmuw(io, A630_GMU_CM3_ITCM_START + (uint32_t)i, (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
             (uint32_t)p[3] << 24);
    }
    barrier(io);
    r->step = A630StepFwStart;
    /* Clear init result to make sure we are getting a fresh value */
    gmuw(io, A630_GMU_CM3_FW_INIT_RESULT, 0);
    gmuw(io, A630_GMU_CM3_BOOT_CONFIG, 0x02);
    /* Write the iova of the HFI table */
    gmuw(io, A630_GMU_HFI_QTBL_ADDR, m->hfi_iova);
    gmuw(io, A630_GMU_HFI_QTBL_INFO, 1);
    gmuw(io, A630_GMU_AHB_FENCE_RANGE_0, (1u << 31) | (0xau << 18) | 0xa0u);
    /* "Snapshots toggle the NMI bit ... Set the M3 config value to avoid that." */
    gmuw(io, A630_GMU_CM3_CFG, 0x4052);
    /* GMU chip id layout: upper 16 bits as is, minor and patch in four bits each, the low 8 bits unused. */
    const uint32_t chip = (c->chip_id & 0xffff0000u) | ((c->chip_id << 4) & 0xf000u) | ((c->chip_id << 8) & 0x0f00u);
    gmuw(io, A630_GMU_HFI_SFR_ADDR, chip);
    gmuw(io, A630_GPU_GMU_CX_GMU_PWR_COL_CP_MSG, m->log_iova | (m->log_bytes / 0x1000 - 1));
    /* a6xx_gmu_power_config, idle level ACTIVE: no IFPC or SPTP power collapse. */
    gmuw(io, A630_GMU_SYS_BUS_CONFIG, 0x1);
    gmuw(io, A630_GMU_ICACHE_CONFIG, 0x1);
    gmuw(io, A630_GMU_DCACHE_CONFIG, 0x1);
    gmuw(io, A630_GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
    gmurmw(io, A630_GMU_RPMH_CTRL, 0, (1u << 0) | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));
    /* a6xx_gmu_start */
    r->step = A630StepGmuStart;
    {
        const uint32_t ver = gmu(io, A630_GMU_CM3_DTCM_START + 0xff8);
        const uint32_t mask = ver <= 0x20010004 ? 0xffffffffu : 0x1ffu, reset_val = ver <= 0x20010004 ? 0xbabefaceu : 0x100u;
        gmuw(io, A630_GMU_CM3_SYSRESET, 1);
        gmuw(io, A630_GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0);   /* the log wptr index (pre-A7XX register) */
        gmuw(io, A630_GMU_CM3_SYSRESET, 0);
        barrier(io);
        const int ok = pollGmu(io, A630_GMU_CM3_FW_INIT_RESULT, mask, reset_val, 100, 10000, &r->fw_init_result, &r->polls);
        r->core_fw_version = gmu(io, A630_GMU_CORE_FW_VERSION);
        if (!ok) FAIL(A630ErrFwInit);
    }
    /* a6xx_gmu_gfx_rail_on (legacy) */
    r->step = A630StepRailOn;
    {
        gmuw(io, A630_GMU_BOOT_SLUMBER_OPTION, 0);
        const uint32_t vote = c->perf->gx_votes[c->boot_level];
        gmuw(io, A630_GMU_GX_VOTE_IDX, vote & 0xff);
        gmuw(io, A630_GMU_MX_VOTE_IDX, (vote >> 8) & 0xff);
        if ((s = A630SetOob(io, A630OobBootSlumber, &r->oob_info))) goto done;
    }
    /* a6xx_sptprac_enable (idle level ACTIVE < SPTP: the CPU owns it). Linux logs a timeout and goes on. */
    r->step = A630StepSptprac;
    gmuw(io, A630_GMU_GX_SPTPRAC_POWER_CONTROL, 0x778000);
    barrier(io);
    {
        unsigned i = 0;
        uint32_t v = 0;
        for (; i <= 100; ++i) {
            v = gmu(io, A630_GMU_SPTPRAC_PWR_CLK_STATUS);
            if ((v & 0x38) == 0x28) break;
            delay(io, 1);
        }
        r->sptprac_status = v;
    }
    /* a6xx_gmu_hfi_start */
    r->step = A630StepHfiStart;
    gmuw(io, A630_GMU_HFI_CTRL_INIT, 1);
    barrier(io);
    if (!pollGmu(io, A630_GMU_HFI_CTRL_STATUS, 1, 1, 100, 10000, &r->hfi_status, &r->polls)) FAIL(A630ErrHfiStart);
    /* a6xx_hfi_start(GMU_COLD_BOOT), legacy */
    if ((s = hfiStartV1(io, m, h, c, r))) goto done;
    /* "Turn on the GMU firmware fault interrupt after we know the boot sequence is successful" */
    gmuw(io, A630_GMU_GMU2HOST_INTR_CLR, ~0u);
    gmuw(io, A630_GMU_GMU2HOST_INTR_MASK, ~(1u << 23));
    /* a6xx_gmu_set_initial_freq: the current perf index (Linux: the highest) through the legacy DCVS OOB. */
    r->step = A630StepFreq;
    {
        uint32_t ret = 0;
        s = A630SetFreq(io, c->boot_level, &ret);
        if (s) goto done;
    }
    r->step = A630StepDone;
done:
#undef FAIL
    r->status = s;
    r->hfi_status = gmu(io, A630_GMU_HFI_CTRL_STATUS);
    r->sptprac_status = gmu(io, A630_GMU_SPTPRAC_PWR_CLK_STATUS);
    r->gx_status = r->sptprac_status;
    r->rpmh_power_state = gmu(io, A630_GPU_GMU_CX_GMU_RPMH_POWER_STATE);
    r->ao_status = gmu(io, A630_GMU_AO_HOST_INTERRUPT_STATUS);
    r->g2h_info = gmu(io, A630_GMU_GMU2HOST_INTR_INFO);
    return s;
}
