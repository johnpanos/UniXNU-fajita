// SPDX-License-Identifier: BSD-2-Clause-Patent
// Modem (MSS) boot, after mainline Linux drivers/remoteproc/qcom_q6v5_mss.c (sdm845) and qcom_q6v5.c. See WifiMss.h.
#include "WifiMss.h"

#define E_IO (-5)
#define E_AGAIN (-11)
#define E_BUSY (-16)
#define E_INVAL (-22)
#define E_TIMEDOUT (-110)
#define SZ_1M 0x100000u
#define SZ_4K 0x1000u
#define MDATA_MAX 0x4000u                 // mdata_mem is 0x4000 bytes
#define MAX_SEG_NAME 40

// ---- GCC branches the MSS needs (gcc-sdm845.c) ----
enum {WK_HALT, WK_VOTED, WK_DELAY};
typedef struct {
    const char *name;
    uint32_t en_reg, en_mask, halt_reg;
    unsigned kind;
    uint32_t hwcg_reg;      // 0: none; bit 1 of the register, when set, skips the halt check (clk_branch_in_hwcg_mode)
} wmss_clk;
enum {CLK_IFACE, CLK_SNOC_AXI, CLK_BUS, CLK_MEM, CLK_GPLL0_MSS, CLK_MNOC_AXI, CLK_PRNG, CLK_N};
static const wmss_clk kClk[CLK_N] = {
    [CLK_IFACE]     = {"iface",     0x8a000, 1u,       0x8a000, WK_HALT,  0x8a000},   // GCC_MSS_CFG_AHB_CLK
    [CLK_SNOC_AXI]  = {"snoc_axi",  0x8a150, 1u,       0x8a150, WK_HALT,  0},         // GCC_MSS_SNOC_AXI_CLK
    [CLK_BUS]       = {"bus",       0x8a154, 1u,       0x8a154, WK_VOTED, 0},         // GCC_MSS_Q6_MEMNOC_AXI_CLK
    [CLK_MEM]       = {"mem",       0x52004, 1u << 10, 0x38004, WK_VOTED, 0x38004},   // GCC_BOOT_ROM_AHB_CLK
    [CLK_GPLL0_MSS] = {"gpll0_mss", 0x52004, 1u << 17, 0,       WK_DELAY, 0},         // GCC_MSS_GPLL0_DIV_CLK_SRC
    [CLK_MNOC_AXI]  = {"mnoc_axi",  0x8a004, 1u,       0x8a004, WK_VOTED, 0x8a004},   // GCC_MSS_MFAB_AXIS_CLK
    [CLK_PRNG]      = {"prng",      0x52004, 1u << 13, 0x34004, WK_VOTED, 0x34004},   // GCC_PRNG_AHB_CLK
};
static const unsigned kResetClks[] = {CLK_IFACE, CLK_SNOC_AXI};
static const unsigned kActiveClks[] = {CLK_BUS, CLK_MEM, CLK_GPLL0_MSS, CLK_MNOC_AXI};
#define N_RESET (sizeof kResetClks / sizeof kResetClks[0])
#define N_ACTIVE (sizeof kActiveClks / sizeof kActiveClks[0])

static uint32_t RD(wmss *m, unsigned blk, uint32_t off) {return m->ops->rd(m->ops->ctx, blk, off);}
static void WR(wmss *m, unsigned blk, uint32_t off, uint32_t v) {m->ops->wr(m->ops->ctx, blk, off, v);}
static uint64_t now(wmss *m) {return m->ops->now_us(m->ops->ctx);}
static void udelay(wmss *m, uint32_t us) {m->ops->udelay(m->ops->ctx, us);}
static void msleep(wmss *m, uint32_t ms) {m->ops->msleep(m->ops->ctx, ms);}

const char *wmss_step_name(unsigned s)
{
    static const char *const n[WMSS_NSTEPS] = {
        "preflight", "rmtfs-assign", "mba-copy", "load-state", "rpmh-vote", "proxy-clk", "reset-clk", "mss-reset", "active-clk",
        "assign-mpss", "assign-mba", "q6-boot-fsm", "mba-wait", "meta-alloc", "meta-assign", "meta-auth", "meta-reclaim",
        "segments", "mpss-share", "mpss-to-mss", "mpss-auth", "ready", "mba-reclaim", "handover", "halt-axi", "stop", "unwind"};
    return s < WMSS_NSTEPS ? n[s] : "?";
}
static void note(wmss *m, unsigned step, int rc, uint32_t val)
{
    if (m->ntrace >= WMSS_MAX_TRACE) return;
    wmss_step *s = &m->trace[m->ntrace++];
    s->step = step; s->rc = rc; s->value = val;
    const uint64_t dt = now(m) - m->t0;
    s->us = dt > 0xffffffffu ? 0xffffffffu : (uint32_t)dt;
    if (m->ops->step) m->ops->step(m->ops->ctx, m->ntrace - 1, step, rc, val, s->us);
}
static int fail(wmss *m, unsigned step, int rc, uint32_t val)
{
    note(m, step, rc, val);
    if (!m->rc) {m->rc = rc; m->failed_step = (int)step;}
    return rc;
}

void wmss_init(wmss *m, const wmss_ops *ops, const wmss_cfg *cfg)
{
    unsigned char *p = (unsigned char *)m;
    for (size_t i = 0; i < sizeof *m; ++i) p[i] = 0;
    m->ops = ops;
    m->cfg = *cfg;
    if (!m->cfg.ready_ms) m->cfg.ready_ms = 5000;
    if (!m->cfg.handover_ms) m->cfg.handover_ms = 10000;
    m->mba_perm = m->mpss_perm = m->rmtfs_perm = 1ull << WMSS_VMID_HLOS;
    m->failed_step = -1;
}

// ---- clocks ----
static int clk_wait(wmss *m, const wmss_clk *c, int enabling)
{
    if (c->hwcg_reg && (RD(m, WMSS_BLK_GCC, c->hwcg_reg) & 2u)) return 0;   // hardware-gated: CLK_OFF means nothing
    if (c->kind == WK_DELAY || (!enabling && c->kind == WK_VOTED)) {udelay(m, 10); return 0;}
    for (unsigned n = 200; n; --n) {
        const int on = !(RD(m, WMSS_BLK_GCC, c->halt_reg) & WMSS_CBCR_CLKOFF);
        if (enabling ? on : !on) return 0;
        udelay(m, 1);
    }
    return E_BUSY;
}
static int clk_on(wmss *m, unsigned i)
{
    const wmss_clk *c = &kClk[i];
    if (m->clk_state >> i & 1) return 0;
    const uint32_t v = RD(m, WMSS_BLK_GCC, c->en_reg);
    m->clk_prior[i] = (v & c->en_mask) != 0;
    WR(m, WMSS_BLK_GCC, c->en_reg, v | c->en_mask);
    m->clk_state |= 1u << i;
    const int rc = clk_wait(m, c, 1);
    if (rc) {   // clk_branch2_enable's failure: the clock is switched off again, as clk_prepare_enable's caller does
        if (!m->clk_prior[i]) WR(m, WMSS_BLK_GCC, c->en_reg, RD(m, WMSS_BLK_GCC, c->en_reg) & ~c->en_mask);
        m->clk_state &= ~(1u << i);
    }
    return rc;
}
static void clk_off(wmss *m, unsigned i)
{
    const wmss_clk *c = &kClk[i];
    if (!(m->clk_state >> i & 1)) return;
    m->clk_state &= ~(1u << i);
    if (m->clk_prior[i]) return;     // somebody else had it on before us: leave it
    WR(m, WMSS_BLK_GCC, c->en_reg, RD(m, WMSS_BLK_GCC, c->en_reg) & ~c->en_mask);
    (void)clk_wait(m, c, 0);
}
static int clks_on(wmss *m, const unsigned *list, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        const int rc = clk_on(m, list[i]);
        if (rc) {while (i--) clk_off(m, list[i]); return rc;}
    }
    return 0;
}
static void clks_off(wmss *m, const unsigned *list, unsigned n) {for (unsigned i = 0; i < n; ++i) clk_off(m, list[i]);}

// ---- polling ----
// readl_poll_timeout: (reg & mask) == want, polled every sleep_us for timeout_us, one last look after the timeout.
static int poll_reg(wmss *m, unsigned blk, uint32_t off, uint32_t mask, uint32_t want, uint32_t sleep_us, uint32_t timeout_us)
{
    const uint64_t t0 = now(m);
    for (;;) {
        if ((RD(m, blk, off) & mask) == want) return 0;
        if (now(m) - t0 >= timeout_us) return (RD(m, blk, off) & mask) == want ? 0 : E_TIMEDOUT;
        udelay(m, sleep_us);
    }
}
static int rmb_pbl_wait(wmss *m, uint32_t ms)
{
    const uint64_t t0 = now(m);
    for (;;) {
        const int32_t val = (int32_t)RD(m, WMSS_BLK_RMB, WMSS_RMB_PBL_STATUS);
        if (val) return val;
        if (now(m) - t0 >= (uint64_t)ms * 1000) return E_TIMEDOUT;
        msleep(m, 1);
    }
}
static int rmb_mba_wait(wmss *m, uint32_t status, uint32_t ms)
{
    const uint64_t t0 = now(m);
    for (;;) {
        const int32_t val = (int32_t)RD(m, WMSS_BLK_RMB, WMSS_RMB_MBA_STATUS);
        if (val < 0) return val;
        if (!status && val) return val;
        if (status && (uint32_t)val == status) return val;
        if (now(m) - t0 >= (uint64_t)ms * 1000) return E_TIMEDOUT;
        msleep(m, 1);
    }
}

// ---- resets (reset-qcom-aoss.c, reset-qcom-pdc.c) ----
static void aoss_assert(wmss *m) {WR(m, WMSS_BLK_AOSS_CC, WMSS_AOSS_MSS_RESTART, 1); udelay(m, 250);}      // "6 sleep cycles"
static void aoss_deassert(wmss *m) {WR(m, WMSS_BLK_AOSS_CC, WMSS_AOSS_MSS_RESTART, 0); udelay(m, 250);}
static void pdc_set(wmss *m, int on)
{
    const uint32_t v = RD(m, WMSS_BLK_PDC, WMSS_PDC_SYNC_RESET), bit = 1u << WMSS_PDC_MODEM_BIT;
    WR(m, WMSS_BLK_PDC, WMSS_PDC_SYNC_RESET, on ? v | bit : v & ~bit);
}
static void reset_assert(wmss *m)    // q6v5_reset_assert, has_alt_reset
{
    pdc_set(m, 1);
    aoss_assert(m); aoss_deassert(m);    // reset_control_reset
    pdc_set(m, 0);
}
static void reset_deassert(wmss *m)  // q6v5_reset_deassert, has_alt_reset
{
    pdc_set(m, 1);
    WR(m, WMSS_BLK_RMB, WMSS_RMB_ALT_RESET, 1);
    aoss_assert(m); aoss_deassert(m);    // reset_control_reset
    WR(m, WMSS_BLK_RMB, WMSS_RMB_ALT_RESET, 0);
    pdc_set(m, 0);
}

// ---- AXI halt (q6v5proc_halt_axi_port) ----
static void halt_port(wmss *m, uint32_t base)
{
    if (RD(m, WMSS_BLK_TCSR, base + WMSS_AXI_IDLE)) return;
    WR(m, WMSS_BLK_TCSR, base + WMSS_AXI_HALTREQ, 1);
    {   // regmap_read_poll_timeout(val != 0, sleep 1000 us, timeout HALT_ACK_TIMEOUT_US)
        const uint64_t t0 = now(m);
        while (!RD(m, WMSS_BLK_TCSR, base + WMSS_AXI_HALTACK) && now(m) - t0 < 100000) udelay(m, 1000);
    }
    WR(m, WMSS_BLK_TCSR, base + WMSS_AXI_HALTREQ, 0);   // the port stays halted until reset
}
static void halt_ports(wmss *m)
{
    halt_port(m, WMSS_HALT_Q6);
    halt_port(m, WMSS_HALT_MODEM);
    halt_port(m, WMSS_HALT_NC);
    note(m, WMSS_S_HALT, 0, RD(m, WMSS_BLK_TCSR, WMSS_HALT_Q6 + WMSS_AXI_IDLE));
}

// ---- memory ownership (q6v5_xfer_mem_ownership) ----
static int xfer(wmss *m, unsigned step, uint64_t *cur, int local, int remote, uint64_t addr, uint64_t size)
{
    wmss_vmperm next[2];
    unsigned n = 0;
    if (local == !!(*cur & (1ull << WMSS_VMID_HLOS)) && remote == !!(*cur & (1ull << WMSS_VMID_MSS_MSA))) return 0;
    if (local) {next[n].vmid = WMSS_VMID_HLOS; next[n].perm = WMSS_PERM_RWX; ++n;}
    if (remote) {next[n].vmid = WMSS_VMID_MSS_MSA; next[n].perm = WMSS_PERM_RW; ++n;}
    const int rc = m->ops->assign(m->ops->ctx, addr, (size + SZ_4K - 1) & ~(uint64_t)(SZ_4K - 1), cur, next, n);
    if (rc) note(m, step, rc, (uint32_t)(local | remote << 1));
    return rc;
}
static int own(wmss *m, unsigned step, uint64_t *cur, uint64_t addr, uint64_t size, int to_mss, int to_hlos)
{
    return xfer(m, step, cur, to_hlos, to_mss, addr, size);
}

// ---- firmware ----
// "image/modem.mdt" -> "image/modem.b05"
static int seg_name(const char *mdt, unsigned i, char *out)
{
    unsigned len = 0;
    while (mdt[len]) ++len;
    if (len <= 4 || len + 1 > MAX_SEG_NAME || i > 99) return E_INVAL;
    for (unsigned k = 0; k < len; ++k) out[k] = mdt[k];
    out[len - 3] = 'b'; out[len - 2] = (char)('0' + i / 10); out[len - 1] = (char)('0' + i % 10); out[len] = 0;
    return 0;
}
// fw name[foff .. foff+len) -> region[roff ..), through m->buf.
static int fw_to_region(wmss *m, const char *name, uint32_t foff, unsigned region, uint64_t roff, uint32_t len)
{
    while (len) {
        const uint32_t n = len < sizeof m->buf ? len : (uint32_t)sizeof m->buf;
        if (m->ops->fw_read(m->ops->ctx, name, foff, m->buf, n) != (long)n) return E_IO;
        if (m->ops->region_write(m->ops->ctx, region, roff, m->buf, n)) return E_IO;
        foff += n; roff += n; len -= n;
    }
    return 0;
}

// Metadata = the ELF header and program headers (phdr[0].p_filesz bytes of modem.mdt), then the hash segment: packed after
// them in modem.mdt, or at its p_offset, or in its own modem.bNN (qcom_mdt_read_metadata). sizes only when `out` is null.
static int read_metadata(wmss *m, uint8_t *out, uint32_t *size_out)
{
    const wmdt_image *im = &m->img;
    if (im->phnum < 2 || im->ph[0].type == WMDT_PT_LOAD || im->hash_index < 1) return E_INVAL;
    const uint32_t ehdr = im->ph[0].filesz, hash = im->ph[im->hash_index].filesz;
    if (!ehdr || ehdr < im->ehdr_bytes || ehdr > 0x1000 || !hash || (uint64_t)ehdr + hash > MDATA_MAX) return E_INVAL;
    if (size_out) *size_out = ehdr + hash;
    if (!out) return 0;
    if (ehdr > m->mdt_bytes) return E_INVAL;
    if (m->ops->fw_read(m->ops->ctx, m->cfg.mdt_name, 0, out, ehdr) != (long)ehdr) return E_IO;
    if ((uint64_t)ehdr + hash == m->mdt_bytes) {
        if (m->ops->fw_read(m->ops->ctx, m->cfg.mdt_name, ehdr, out + ehdr, hash) != (long)hash) return E_IO;
    } else if ((uint64_t)im->ph[im->hash_index].offset + hash <= m->mdt_bytes) {
        if (m->ops->fw_read(m->ops->ctx, m->cfg.mdt_name, im->ph[im->hash_index].offset, out + ehdr, hash) != (long)hash) return E_IO;
    } else {
        char nm[MAX_SEG_NAME];
        if (seg_name(m->cfg.mdt_name, (unsigned)im->hash_index, nm)) return E_INVAL;
        if (m->ops->fw_read(m->ops->ctx, nm, 0, out + ehdr, hash) != (long)hash) return E_IO;
    }
    return 0;
}

int wmss_preflight(wmss *m)
{
    uint32_t size = 0;
    if (m->ops->fw_size(m->ops->ctx, m->cfg.mba_name, &size) || !size || size > SZ_1M || size > m->cfg.mba_size)
        return fail(m, WMSS_S_PREFLIGHT, E_INVAL, size);       // "MBA is restricted to a maximum size of 1M"
    if (m->ops->fw_size(m->ops->ctx, m->cfg.mdt_name, &m->mdt_bytes) || m->mdt_bytes < 52)
        return fail(m, WMSS_S_PREFLIGHT, E_IO, m->mdt_bytes);
    uint8_t head[52 + 32 * WMDT_MAX_PHDRS];
    const uint32_t want = m->mdt_bytes < sizeof head ? m->mdt_bytes : (uint32_t)sizeof head;
    if (m->ops->fw_read(m->ops->ctx, m->cfg.mdt_name, 0, head, want) != (long)want) return fail(m, WMSS_S_PREFLIGHT, E_IO, 0);
    const int pr = wmdt_parse(head, want, &m->img);
    if (pr) return fail(m, WMSS_S_PREFLIGHT, E_INVAL, (uint32_t)-pr);
    uint32_t meta = 0;
    if (read_metadata(m, 0, &meta) || read_metadata(m, m->buf, &meta)) return fail(m, WMSS_S_PREFLIGHT, E_INVAL, 0x4d44);
    unsigned loads = 0;
    for (unsigned i = 0; i < m->img.phnum; ++i) {
        const wmdt_phdr *ph = &m->img.ph[i];
        if (!wmdt_segment_loads(ph)) continue;
        uint64_t off;
        if (wmdt_segment_offset(&m->img, i, m->cfg.mpss_phys, m->cfg.mpss_size, &off))
            return fail(m, WMSS_S_PREFLIGHT, E_INVAL, 0x5000 | i);     // "segment outside memory range", filesz > memsz
        ++loads;
        if (!ph->filesz) continue;
        if (ph->offset < m->mdt_bytes) {          // non-split: the file is large enough
            if ((uint64_t)ph->offset + ph->filesz > m->mdt_bytes) return fail(m, WMSS_S_PREFLIGHT, E_INVAL, 0x6000 | i);
        } else {
            char nm[MAX_SEG_NAME];
            uint32_t sz = 0;
            if (seg_name(m->cfg.mdt_name, i, nm)) return fail(m, WMSS_S_PREFLIGHT, E_INVAL, 0x7000 | i);
            if (m->ops->fw_size(m->ops->ctx, nm, &sz) || sz != ph->filesz) return fail(m, WMSS_S_PREFLIGHT, E_INVAL, 0x8000 | i);
        }
    }
    if (!loads) return fail(m, WMSS_S_PREFLIGHT, E_INVAL, 0x4c);
    m->boot_addr = (uint32_t)wmdt_boot_addr(&m->img, m->cfg.mpss_phys);
    note(m, WMSS_S_PREFLIGHT, 0, loads);
    return 0;
}

// ---- RPMh levels (the genpd votes of q6v5_pds_enable / q6v5_pds_disable) ----
static void arcs_restore(wmss *m)
{
    for (int a = WMSS_NARC - 1; a >= 0; --a) {
        if (!m->arc_voted[a]) continue;
        m->arc_voted[a] = 0;
        (void)m->ops->arc_set(m->ops->ctx, (unsigned)a, m->arc_prior[a]);
    }
}
static int arcs_vote(wmss *m)
{
    uint32_t cur[WMSS_NARC], max[WMSS_NARC];
    for (unsigned a = 0; a < WMSS_NARC; ++a) {
        const int rc = m->ops->arc_get(m->ops->ctx, a, &cur[a], &max[a]);
        if (rc || !max[a]) return rc ? rc : E_INVAL;
    }
    for (unsigned a = 0; a < WMSS_NARC; ++a) {
        m->arc_prior[a] = cur[a]; m->arc_max[a] = max[a];
        const int rc = m->ops->arc_set(m->ops->ctx, a, max[a]);
        m->arc_voted[a] = 1;     // restored even if this vote failed half-way
        if (rc) return rc;
    }
    return 0;
}
// Proxy resources (qcom_msa_handover): the xo/prng clocks and the rail votes. The xo clock (an RPMh vote) is left on.
static void proxy_release(wmss *m)
{
    if (!m->proxy_held) return;
    m->proxy_held = 0;
    clk_off(m, CLK_PRNG);
    arcs_restore(m);
}

// ---- unwind ----
// q6v5_mba_reclaim and the error labels of q6v5_mba_load, in one order that is safe for both: stop the AXI traffic, reset the
// Q6, switch the clocks off, take the memory back, drop the load state and the proxy resources.
static void unwind(wmss *m, int q6_was_started)
{
    note(m, WMSS_S_UNWIND, 0, (uint32_t)q6_was_started);
    if (q6_was_started) halt_ports(m);
    if (m->reset_asserted_by_us) {reset_assert(m); m->reset_asserted_by_us = 0;}
    if (m->reset_clks_on) {clks_off(m, kResetClks, N_RESET); m->reset_clks_on = 0;}
    if (m->active_clks_on) {clks_off(m, kActiveClks, N_ACTIVE); m->active_clks_on = 0;}
    m->q6_started = m->mba_loaded = m->running = 0;
    if (m->mba_perm != 1ull << WMSS_VMID_HLOS) {
        if (own(m, WMSS_S_MBA_RECLAIM, &m->mba_perm, m->cfg.mba_phys, m->cfg.mba_size, 0, 1)) note(m, WMSS_S_MBA_RECLAIM, E_IO, 0xbad);
    }
    if (m->mpss_perm != 1ull << WMSS_VMID_HLOS) (void)own(m, WMSS_S_MPSS_MSS, &m->mpss_perm, m->cfg.mpss_phys, m->cfg.mpss_size, 0, 1);
    if (m->mdata_held) {
        if (m->mdata_perm == 1ull << WMSS_VMID_HLOS) {m->ops->mdata_free(m->ops->ctx); m->mdata_held = 0;}
        else note(m, WMSS_S_META_RECLAIM, E_IO, 0xbad);   // not given back: keep the buffer wired rather than free MSS-owned memory
    }
    if (m->rmtfs_assigned) {
        if (!own(m, WMSS_S_RMTFS, &m->rmtfs_perm, m->cfg.rmtfs_phys, m->cfg.rmtfs_size, 0, 1)) m->rmtfs_assigned = 0;
    }
    if (m->load_state_on) {(void)m->ops->load_state(m->ops->ctx, 0); m->load_state_on = 0;}
    proxy_release(m);
}

// ---- q6v5proc_reset, MSS_SDM845 ----
static int q6_reset(wmss *m)
{
    uint32_t v = RD(m, WMSS_BLK_QDSP6, WMSS_QDSP6SS_SLEEP);
    WR(m, WMSS_BLK_QDSP6, WMSS_QDSP6SS_SLEEP, v | WMSS_CBCR_CLKEN);
    if (poll_reg(m, WMSS_BLK_QDSP6, WMSS_QDSP6SS_SLEEP, WMSS_CBCR_CLKOFF, 0, 1, 200)) return fail(m, WMSS_S_Q6_RESET, E_TIMEDOUT, 1);
    WR(m, WMSS_BLK_QDSP6, WMSS_QDSP6SS_BOOT_CORE_START, 1);       // de-assert QDSP6 stop core
    WR(m, WMSS_BLK_QDSP6, WMSS_QDSP6SS_BOOT_CMD, 1);              // trigger the boot FSM
    m->q6_started = 1;
    if (poll_reg(m, WMSS_BLK_RMB, WMSS_RMB_MSS_STATUS, 1, 1, 10, 10000)) {
        reset_deassert(m);        // "reset the modem so that the boot FSM is in reset state"
        return fail(m, WMSS_S_Q6_RESET, E_TIMEDOUT, 2);
    }
    const int pbl = rmb_pbl_wait(m, 1000);
    if (pbl != WMSS_PBL_SUCCESS) return fail(m, WMSS_S_Q6_RESET, pbl == E_TIMEDOUT ? E_TIMEDOUT : E_INVAL, (uint32_t)pbl);
    note(m, WMSS_S_Q6_RESET, 0, (uint32_t)pbl);
    return 0;
}

// ---- q6v5_mba_load ----
static int mba_load(wmss *m)
{
    int rc;
    rc = m->ops->load_state(m->ops->ctx, 1);                     // qcom_q6v5_prepare
    if (rc) {unwind(m, 0); return fail(m, WMSS_S_LOAD_STATE, rc, 1);}
    m->load_state_on = 1;
    note(m, WMSS_S_LOAD_STATE, 0, 1);

    m->proxy_held = 1;
    rc = arcs_vote(m);                                           // q6v5_pds_enable (and the shared rails)
    if (rc) {unwind(m, 0); return fail(m, WMSS_S_VOTE, rc, 0);}
    note(m, WMSS_S_VOTE, 0, m->arc_max[WMSS_ARC_CX]);
    rc = clk_on(m, CLK_PRNG);                                    // proxy clocks (xo is the RPMh CXO, already on)
    if (rc) {unwind(m, 0); return fail(m, WMSS_S_PROXY_CLK, rc, CLK_PRNG);}
    note(m, WMSS_S_PROXY_CLK, 0, 0);

    rc = clks_on(m, kResetClks, N_RESET);
    if (rc) {unwind(m, 0); return fail(m, WMSS_S_RESET_CLK, rc, 0);}
    m->reset_clks_on = 1;
    note(m, WMSS_S_RESET_CLK, 0, 0);

    reset_deassert(m);
    m->reset_asserted_by_us = 1;      // from here the Q6 is out of reset: the unwind puts it back
    note(m, WMSS_S_RESET, 0, 0);

    rc = clks_on(m, kActiveClks, N_ACTIVE);
    if (rc) {unwind(m, 0); return fail(m, WMSS_S_ACTIVE_CLK, rc, 0);}
    m->active_clks_on = 1;
    note(m, WMSS_S_ACTIVE_CLK, 0, 0);

    // Some versions of the MBA wipe the MPSS region as well: give the Q6 access to it, then to the MBA image.
    rc = own(m, WMSS_S_ASSIGN_MPSS, &m->mpss_perm, m->cfg.mpss_phys, m->cfg.mpss_size, 1, 0);
    if (rc) {unwind(m, 0); return fail(m, WMSS_S_ASSIGN_MPSS, rc, 0);}
    note(m, WMSS_S_ASSIGN_MPSS, 0, 0);
    rc = own(m, WMSS_S_ASSIGN_MBA, &m->mba_perm, m->cfg.mba_phys, m->cfg.mba_size, 1, 0);
    if (rc) {unwind(m, 0); return fail(m, WMSS_S_ASSIGN_MBA, rc, 0);}
    note(m, WMSS_S_ASSIGN_MBA, 0, 0);

    WR(m, WMSS_BLK_RMB, WMSS_RMB_MBA_IMAGE, (uint32_t)m->cfg.mba_phys);
    rc = q6_reset(m);
    if (rc) {unwind(m, m->q6_started); return rc;}

    rc = rmb_mba_wait(m, 0, 5000);
    if (rc != WMSS_MBA_XPU_UNLOCKED && rc != WMSS_MBA_XPU_UNLOCKED_SCRIBBLED) {
        const uint32_t st = (uint32_t)rc;
        unwind(m, 1);
        return fail(m, WMSS_S_MBA_WAIT, rc == E_TIMEDOUT ? E_TIMEDOUT : E_INVAL, st);
    }
    m->mba_loaded = 1;
    note(m, WMSS_S_MBA_WAIT, 0, (uint32_t)rc);
    return 0;
}

// ---- q6v5_mpss_init_image: the metadata, authenticated by the MBA ----
static int mpss_init_image(wmss *m)
{
    uint32_t size = 0;
    int rc = read_metadata(m, 0, &size);
    if (rc) return fail(m, WMSS_S_META_ALLOC, rc, 0);
    if (m->cfg.meta_off) {      // already in the MBA carveout (written before the MBA owned it): just hand over the address
        WR(m, WMSS_BLK_RMB, WMSS_RMB_PMI_META_DATA, (uint32_t)m->mdata_phys);
        WR(m, WMSS_BLK_RMB, WMSS_RMB_MBA_COMMAND, WMSS_CMD_META_DATA_READY);
        rc = rmb_mba_wait(m, WMSS_MBA_META_AUTH_SUCCESS, 1000);
        if (rc < 0) return fail(m, WMSS_S_META_AUTH, rc == E_TIMEDOUT ? E_TIMEDOUT : E_INVAL, (uint32_t)rc);
        note(m, WMSS_S_META_AUTH, 0, (uint32_t)rc);
        return 0;
    }
    rc = m->ops->mdata_alloc(m->ops->ctx, size, &m->mdata_phys);
    if (rc) return fail(m, WMSS_S_META_ALLOC, rc < 0 ? rc : E_IO, size);
    m->mdata_held = 1; m->mdata_size = size;
    m->mdata_perm = 1ull << WMSS_VMID_HLOS;
    // the metadata goes through m->buf (it is at most MDATA_MAX = 16 KiB)
    rc = read_metadata(m, m->buf, &size);
    if (!rc && m->ops->region_write(m->ops->ctx, WMSS_REGION_MDATA, 0, m->buf, size)) rc = E_IO;
    if (rc) return fail(m, WMSS_S_META_ALLOC, rc, 1);
    note(m, WMSS_S_META_ALLOC, 0, size);
    // Hypervisor mapping to access the metadata by the modem
    rc = xfer(m, WMSS_S_META_ASSIGN, &m->mdata_perm, 0, 1, m->mdata_phys, size);
    if (rc) return fail(m, WMSS_S_META_ASSIGN, E_AGAIN, (uint32_t)rc);
    note(m, WMSS_S_META_ASSIGN, 0, (uint32_t)m->mdata_phys);
    WR(m, WMSS_BLK_RMB, WMSS_RMB_PMI_META_DATA, (uint32_t)m->mdata_phys);
    WR(m, WMSS_BLK_RMB, WMSS_RMB_MBA_COMMAND, WMSS_CMD_META_DATA_READY);
    rc = rmb_mba_wait(m, WMSS_MBA_META_AUTH_SUCCESS, 1000);
    if (rc < 0) fail(m, WMSS_S_META_AUTH, rc == E_TIMEDOUT ? E_TIMEDOUT : E_INVAL, (uint32_t)rc);
    else note(m, WMSS_S_META_AUTH, 0, (uint32_t)rc);
    // Metadata authentication done, remove modem access.
    const int back = xfer(m, WMSS_S_META_RECLAIM, &m->mdata_perm, 1, 0, m->mdata_phys, size);
    if (!back) {m->ops->mdata_free(m->ops->ctx); m->mdata_held = 0; note(m, WMSS_S_META_RECLAIM, 0, 0);}
    else note(m, WMSS_S_META_RECLAIM, back, 0xbad);   // "mdt buffer not reclaimed system may become unstable": kept, never freed
    return rc < 0 ? (m->rc ? m->rc : E_INVAL) : 0;
}

// ---- q6v5_mpss_load ----
static int mpss_load(wmss *m)
{
    int rc;
    WR(m, WMSS_BLK_RMB, WMSS_RMB_PMI_CODE_LENGTH, 0);            // initialize the RMB validator
    rc = mpss_init_image(m);
    if (rc) return rc;
    // In case of a modem restart on secure devices the modem memory can be reclaimed only after the MBA is loaded; then share
    // ownership between HLOS and the MSS during segment loading.
    (void)own(m, WMSS_S_MPSS_SHARE, &m->mpss_perm, m->cfg.mpss_phys, m->cfg.mpss_size, 0, 1);
    rc = own(m, WMSS_S_MPSS_SHARE, &m->mpss_perm, m->cfg.mpss_phys, m->cfg.mpss_size, 1, 1);
    if (rc) return fail(m, WMSS_S_MPSS_SHARE, E_AGAIN, (uint32_t)rc);
    note(m, WMSS_S_MPSS_SHARE, 0, 0);

    uint32_t size = 0;
    unsigned nseg = 0;
    for (unsigned i = 0; i < m->img.phnum; ++i) {
        const wmdt_phdr *ph = &m->img.ph[i];
        if (!wmdt_segment_loads(ph)) continue;
        uint64_t off;
        if (wmdt_segment_offset(&m->img, i, m->cfg.mpss_phys, m->cfg.mpss_size, &off)) return fail(m, WMSS_S_SEGMENTS, E_INVAL, i);
        if (ph->filesz) {
            if (ph->offset < m->mdt_bytes) {
                if (fw_to_region(m, m->cfg.mdt_name, ph->offset, WMSS_REGION_MPSS, off, ph->filesz)) return fail(m, WMSS_S_SEGMENTS, E_IO, i);
            } else {
                char nm[MAX_SEG_NAME];
                if (seg_name(m->cfg.mdt_name, i, nm) || fw_to_region(m, nm, 0, WMSS_REGION_MPSS, off, ph->filesz))
                    return fail(m, WMSS_S_SEGMENTS, E_IO, i);
            }
        }
        if (ph->memsz > ph->filesz && m->ops->region_zero(m->ops->ctx, WMSS_REGION_MPSS, off + ph->filesz, ph->memsz - ph->filesz))
            return fail(m, WMSS_S_SEGMENTS, E_IO, i);
        size += ph->memsz;
        ++nseg;
        if (!RD(m, WMSS_BLK_RMB, WMSS_RMB_PMI_CODE_LENGTH)) {
            WR(m, WMSS_BLK_RMB, WMSS_RMB_PMI_CODE_START, m->boot_addr);
            WR(m, WMSS_BLK_RMB, WMSS_RMB_MBA_COMMAND, WMSS_CMD_LOAD_READY);
        }
        WR(m, WMSS_BLK_RMB, WMSS_RMB_PMI_CODE_LENGTH, size);
        const int st = (int32_t)RD(m, WMSS_BLK_RMB, WMSS_RMB_MBA_STATUS);
        if (st < 0) return fail(m, WMSS_S_SEGMENTS, E_INVAL, (uint32_t)st);    // "MPSS authentication failed"
    }
    note(m, WMSS_S_SEGMENTS, 0, nseg);

    // Transfer ownership of the modem DDR region to the Q6.
    rc = own(m, WMSS_S_MPSS_MSS, &m->mpss_perm, m->cfg.mpss_phys, m->cfg.mpss_size, 1, 0);
    if (rc) return fail(m, WMSS_S_MPSS_MSS, E_AGAIN, (uint32_t)rc);
    note(m, WMSS_S_MPSS_MSS, 0, 0);
    rc = rmb_mba_wait(m, WMSS_MBA_AUTH_COMPLETE, 10000);
    if (rc < 0) return fail(m, WMSS_S_AUTH, rc == E_TIMEDOUT ? E_TIMEDOUT : E_INVAL, (uint32_t)rc);
    note(m, WMSS_S_AUTH, 0, (uint32_t)rc);
    return 0;
}

// qcom_q6v5_wait_for_start: the SMP2P "ready" bit (the ready interrupt). The "fatal" bit ends the wait early.
static int wait_ready(wmss *m, uint32_t ms)
{
    const uint64_t t0 = now(m);
    uint32_t v = 0;
    for (;;) {
        if (!m->ops->smp2p_bits(m->ops->ctx, &v)) {
            if (v & WMSS_SMP2P_FATAL) return E_IO;
            if (v & WMSS_SMP2P_READY) return 0;
        }
        if (now(m) - t0 >= (uint64_t)ms * 1000) return E_TIMEDOUT;
        msleep(m, 10);
    }
}

int wmss_boot(wmss *m)
{
    m->t0 = now(m);
    m->ntrace = 0; m->rc = 0; m->failed_step = -1;
    int rc = wmss_preflight(m);
    if (rc) return rc;
    // The modem needs SMEM and its SMP2P item before it runs: bring the AP's end up first (a no-op write).
    (void)m->ops->smp2p_set(m->ops->ctx, 0, 0);
    if (m->cfg.rmtfs_size) {
        rc = own(m, WMSS_S_RMTFS, &m->rmtfs_perm, m->cfg.rmtfs_phys, m->cfg.rmtfs_size, 1, 1);    // HLOS RW + MSS RW
        if (rc) return fail(m, WMSS_S_RMTFS, rc, 0);
        m->rmtfs_assigned = 1;
        note(m, WMSS_S_RMTFS, 0, 0);
    }
    // q6v5_load: the MBA image into its carveout (HLOS-owned until mba_load assigns it)
    {
        uint32_t size = 0;
        (void)m->ops->fw_size(m->ops->ctx, m->cfg.mba_name, &size);
        if (fw_to_region(m, m->cfg.mba_name, 0, WMSS_REGION_MBA, 0, size)) {unwind(m, 0); return fail(m, WMSS_S_MBA_COPY, E_IO, size);}
        note(m, WMSS_S_MBA_COPY, 0, size);
        if (m->cfg.meta_off) {
            uint32_t msize = 0;
            if (m->cfg.meta_off < size || (uint64_t)m->cfg.meta_off + MDATA_MAX > m->cfg.mba_size) {unwind(m, 0); return fail(m, WMSS_S_META_ALLOC, E_INVAL, m->cfg.meta_off);}
            int mrc = read_metadata(m, m->buf, &msize);
            if (!mrc && m->ops->region_write(m->ops->ctx, WMSS_REGION_MBA, m->cfg.meta_off, m->buf, msize)) mrc = E_IO;
            if (mrc) {unwind(m, 0); return fail(m, WMSS_S_META_ALLOC, mrc, 2);}
            m->mdata_phys = m->cfg.mba_phys + m->cfg.meta_off;
            note(m, WMSS_S_META_ALLOC, 0, msize);
        }
    }
    rc = mba_load(m);
    if (rc) return rc;                       // mba_load unwound itself
    rc = mpss_load(m);
    if (rc) {unwind(m, 1); return rc;}
    m->running = 1;
    rc = wait_ready(m, m->cfg.ready_ms);
    if (rc) {m->running = 0; unwind(m, 1); return fail(m, WMSS_S_READY, rc, 0);}
    note(m, WMSS_S_READY, 0, 0);
    // The MBA image is no longer needed: back to HLOS.
    if (own(m, WMSS_S_MBA_RECLAIM, &m->mba_perm, m->cfg.mba_phys, m->cfg.mba_size, 0, 1)) note(m, WMSS_S_MBA_RECLAIM, E_IO, 0xbad);
    else note(m, WMSS_S_MBA_RECLAIM, 0, 0);
    // The handover (the modem no longer needs our votes) is waited for here, but a late one is not an error.
    const uint64_t t0 = now(m);
    while (wmss_poll_handover(m) == 0 && now(m) - t0 < (uint64_t)m->cfg.handover_ms * 1000) msleep(m, 10);
    return 0;
}

int wmss_poll_handover(wmss *m)
{
    if (!m->proxy_held) return 1;
    uint32_t v = 0;
    if (m->ops->smp2p_bits(m->ops->ctx, &v) || !(v & WMSS_SMP2P_HANDOVER)) return 0;
    proxy_release(m);
    note(m, WMSS_S_HANDOVER, 0, v);
    return 1;
}

int wmss_stop(wmss *m)
{
    if (!m->running) return 0;
    m->t0 = now(m);
    m->ops->smp2p_set(m->ops->ctx, WMSS_SMP2P_STOP_BIT, WMSS_SMP2P_STOP_BIT);
    const uint64_t t0 = now(m);
    int rc = E_TIMEDOUT;
    uint32_t v = 0;
    while (now(m) - t0 < 5000000) {
        if (!m->ops->smp2p_bits(m->ops->ctx, &v) && (v & WMSS_SMP2P_STOP_ACK)) {rc = 0; break;}
        msleep(m, 10);
    }
    m->ops->smp2p_set(m->ops->ctx, WMSS_SMP2P_STOP_BIT, 0);
    note(m, WMSS_S_STOP, rc, v);
    m->running = 0;
    unwind(m, 1);
    return rc;
}

unsigned wmss_survey(wmss *m, wmss_reg *out, unsigned cap)
{
    static const struct {unsigned blk; uint32_t off;} regs[] = {
        {WMSS_BLK_RMB, WMSS_RMB_MBA_IMAGE}, {WMSS_BLK_RMB, WMSS_RMB_PBL_STATUS}, {WMSS_BLK_RMB, WMSS_RMB_MBA_COMMAND},
        {WMSS_BLK_RMB, WMSS_RMB_MBA_STATUS}, {WMSS_BLK_RMB, WMSS_RMB_PMI_META_DATA}, {WMSS_BLK_RMB, WMSS_RMB_PMI_CODE_START},
        {WMSS_BLK_RMB, WMSS_RMB_PMI_CODE_LENGTH}, {WMSS_BLK_RMB, WMSS_RMB_MSS_STATUS}, {WMSS_BLK_RMB, WMSS_RMB_ALT_RESET},
        {WMSS_BLK_QDSP6, WMSS_QDSP6SS_SLEEP}, {WMSS_BLK_QDSP6, WMSS_QDSP6SS_BOOT_CORE_START}, {WMSS_BLK_QDSP6, WMSS_QDSP6SS_BOOT_CMD},
        {WMSS_BLK_AOSS_CC, WMSS_AOSS_MSS_RESTART}, {WMSS_BLK_PDC, WMSS_PDC_SYNC_RESET},
        {WMSS_BLK_TCSR, WMSS_HALT_Q6 + 4}, {WMSS_BLK_TCSR, WMSS_HALT_Q6 + 8}, {WMSS_BLK_TCSR, WMSS_HALT_MODEM + 4},
        {WMSS_BLK_TCSR, WMSS_HALT_MODEM + 8}, {WMSS_BLK_TCSR, WMSS_HALT_NC + 4}, {WMSS_BLK_TCSR, WMSS_HALT_NC + 8},
    };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0] && n < cap; ++i)
        {out[n].blk = regs[i].blk; out[n].off = regs[i].off; out[n].val = RD(m, regs[i].blk, regs[i].off); ++n;}
    for (unsigned c = 0; c < CLK_N && n + 2 <= cap; ++c) {
        out[n].blk = WMSS_BLK_GCC; out[n].off = kClk[c].en_reg; out[n].val = RD(m, WMSS_BLK_GCC, kClk[c].en_reg); ++n;
        if (kClk[c].halt_reg) {out[n].blk = WMSS_BLK_GCC; out[n].off = kClk[c].halt_reg; out[n].val = RD(m, WMSS_BLK_GCC, kClk[c].halt_reg); ++n;}
    }
    return n;
}
