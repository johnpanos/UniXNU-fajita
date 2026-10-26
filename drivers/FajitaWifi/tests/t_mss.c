// SPDX-License-Identifier: BSD-2-Clause-Patent
// Modem boot (WifiMss.c) against a simulated modem subsystem. The simulation is an executable list of the constraints the
// hardware and the hypervisor impose, so a wrong order fails here instead of hanging the phone:
//   - the Q6 only boots with load_state on, every rail at its top level, every clock on, both resets released, the MBA image
//     in place and owned by the MSS alone;
//   - the CPU may not write a region the hypervisor has taken from HLOS (an XPU violation on the phone) and a region must be
//     handed back before the OS reuses it;
//   - the MBA authenticates the metadata only while the MSS owns the buffer, and finishes only when every segment is in place
//     and the mpss carveout belongs to the MSS alone.
// Every failure the simulation can inject (an op failing at each call index, and a list of behavioural faults) must end with
// the memory back with HLOS, the clocks as found, the rails at their previous level, load_state off and the modem in reset.
#include "tcheck.h"
#include <stdlib.h>
#include <string.h>
#include "../WifiMss.h"
#include "../WifiFat.h"

#define HLOS (1ull << WMSS_VMID_HLOS)
#define MSS (1ull << WMSS_VMID_MSS_MSA)
#define NCLK 7

typedef struct {uint32_t en, mask, halt;} sclk;
// CLK_IFACE, SNOC_AXI, BUS, MEM, GPLL0_MSS, MNOC_AXI, PRNG as in WifiMss.c (enable register, mask, halt register)
static const sclk kSClk[NCLK] = {{0x8a000, 1, 0x8a000}, {0x8a150, 1, 0x8a150}, {0x8a154, 1, 0x8a154}, {0x52004, 1u << 10, 0x38004},
                                 {0x52004, 1u << 17, 0}, {0x8a004, 1, 0x8a004}, {0x52004, 1u << 13, 0x34004}};

enum {FL_NONE, FL_NO_FSM, FL_NO_PBL, FL_PBL_BAD, FL_NO_XPU, FL_XPU_BAD, FL_XPU_SCRIB, FL_META_FAIL, FL_META_NEVER, FL_SEG_FAIL,
      FL_AUTH_NEVER, FL_AUTH_FAIL, FL_NO_READY, FL_FATAL, FL_NO_HANDOVER, FL_SLEEP_STUCK, FL_CLK_STUCK, FL_NO_STOPACK, FL_N};
enum {EV_FSM = 1, EV_PBL, EV_XPU, EV_META, EV_AUTH, EV_READY, EV_FATAL, EV_HANDOVER, EV_STOPACK};

typedef struct {uint64_t t; int kind; unsigned epoch; uint32_t arg;} ev_t;
typedef struct {uint64_t addr, size, mask;} own_t;

typedef struct {
    uint64_t t;
    uint32_t gcc[0x8c000 / 4], rmb[0x50 / 4], q6[0x500 / 4], tcsr[0x6000 / 4];
    uint32_t aoss, pdc;
    int hwcg_all;                       // set bit 1 of the hwcg registers: halt checks are skipped
    int stuck_clk;                      // FL_CLK_STUCK: this clock's CLK_OFF never clears
    uint32_t arc_cur[WMSS_NARC], arc_max[WMSS_NARC];
    own_t own[8]; unsigned nown;
    // configuration
    wmss_cfg cfg;
    // memory
    uint8_t *mba, *mpss, mdata[0x4000];
    uint64_t mdata_phys; int mdata_alive; uint32_t mdata_len;
    // the files
    struct {char name[40]; uint8_t *data; uint32_t len;} file[40]; unsigned nfile;
    // modem model
    int fault;
    int load_state; unsigned epoch; int running, booted;
    ev_t ev[16]; unsigned nev;
    uint32_t pbl, mba_status, mss_status, smp2p_in; int smp2p_valid, smp2p_stop_out;
    uint32_t code_start, code_length, meta_addr, expect_total;
    unsigned writes_hw, halts, assigns, resets, reset_pulses;
    unsigned violations, precond;       // precond: bitmask of the booting constraint that failed
    char why[96];
    // fault injection of ops
    unsigned op_count, fail_at, failed_kind; int failed_op;
    unsigned unused;
    // expectations for the checks
    const uint8_t *expect_meta; uint32_t expect_meta_len;
    uint32_t mba_len;
    uint32_t init_gcc52004;
    unsigned segs_checked;
} sim;

static void ev_add(sim *s, uint64_t dt, int kind, uint32_t arg)
{
    if (s->nev < 16) {s->ev[s->nev].t = s->t + dt; s->ev[s->nev].kind = kind; s->ev[s->nev].epoch = s->epoch; s->ev[s->nev].arg = arg; ++s->nev;}
}
static own_t *own_of(sim *s, uint64_t addr, uint64_t size)
{
    for (unsigned i = 0; i < s->nown; ++i) if (s->own[i].addr == addr) return &s->own[i];
    if (s->nown >= 8) return NULL;
    s->own[s->nown] = (own_t){addr, size, HLOS};
    return &s->own[s->nown++];
}
static uint64_t owner_of_region(sim *s, unsigned region)
{
    const uint64_t a = region == WMSS_REGION_MBA ? s->cfg.mba_phys : region == WMSS_REGION_MPSS ? s->cfg.mpss_phys : s->mdata_phys;
    return own_of(s, a, 0)->mask;
}
static int clk_enabled(sim *s, unsigned i) {return (s->gcc[kSClk[i].en / 4] & kSClk[i].mask) != 0;}

static void modem_reset(sim *s)
{
    ++s->epoch; s->nev = 0; s->running = 0; s->booted = 0;
    s->pbl = 0; s->mba_status = 0; s->mss_status = 0; s->smp2p_valid = 0; s->smp2p_in = 0; s->code_length = 0; s->code_start = 0;
}

static int boot_preconditions(sim *s)
{
    unsigned bad = 0;
    if (!s->load_state) bad |= 1;
    for (unsigned a = 0; a < WMSS_NARC; ++a) if (s->arc_cur[a] != s->arc_max[a]) bad |= 2;
    for (unsigned c = 0; c < NCLK; ++c) if (!clk_enabled(s, c)) bad |= 4;
    if ((s->pdc >> 9 & 1) || s->aoss || s->rmb[WMSS_RMB_ALT_RESET / 4]) bad |= 8;
    if (s->rmb[WMSS_RMB_MBA_IMAGE / 4] != (uint32_t)s->cfg.mba_phys) bad |= 16;
    if (own_of(s, s->cfg.mba_phys, 0)->mask != MSS) bad |= 32;
    if (own_of(s, s->cfg.mpss_phys, 0)->mask != MSS) bad |= 64;
    if (s->mba_len > s->cfg.mba_size || memcmp(s->mba, s->file[0].data, s->mba_len)) bad |= 128;   // file[0] is the mba
    if (!(s->q6[WMSS_QDSP6SS_SLEEP / 4] & WMSS_CBCR_CLKEN) || s->q6[WMSS_QDSP6SS_BOOT_CORE_START / 4] != 1) bad |= 256;
    return bad;
}

// The events that happen as time passes.
static void run_events(sim *s)
{
    for (unsigned i = 0; i < s->nev;) {
        if (s->ev[i].t > s->t) {++i; continue;}
        ev_t e = s->ev[i];
        s->ev[i] = s->ev[--s->nev];
        if (e.epoch != s->epoch) continue;
        switch (e.kind) {
        case EV_FSM: s->mss_status = 1; break;
        case EV_PBL: s->pbl = s->fault == FL_PBL_BAD ? 0xffffffffu : 1; break;
        case EV_XPU: s->mba_status = s->fault == FL_XPU_BAD ? (uint32_t)-3 : s->fault == FL_XPU_SCRIB ? 2 : 1; s->booted = 1; break;
        case EV_META: s->mba_status = e.arg; break;
        case EV_AUTH: {
            // every segment is in place and the mpss carveout belongs to the MSS alone
            if (own_of(s, s->cfg.mpss_phys, 0)->mask != MSS || s->code_length != s->expect_total || s->fault == FL_AUTH_FAIL) {
                s->mba_status = (uint32_t)-5;
                if (own_of(s, s->cfg.mpss_phys, 0)->mask != MSS) snprintf(s->why, sizeof s->why, "auth: mpss not MSS-only");
            } else {
                s->mba_status = WMSS_MBA_AUTH_COMPLETE; s->running = 1; s->smp2p_valid = 1;
                if (s->fault != FL_NO_READY) ev_add(s, 300000, s->fault == FL_FATAL ? EV_FATAL : EV_READY, 0);
                if (s->fault != FL_NO_HANDOVER && s->fault != FL_NO_READY && s->fault != FL_FATAL) ev_add(s, 600000, EV_HANDOVER, 0);
            }
            break;
        }
        case EV_READY: s->smp2p_in |= WMSS_SMP2P_READY; break;
        case EV_FATAL: s->smp2p_in |= WMSS_SMP2P_FATAL; break;
        case EV_HANDOVER: s->smp2p_in |= WMSS_SMP2P_HANDOVER; break;
        case EV_STOPACK: s->smp2p_in |= WMSS_SMP2P_STOP_ACK; break;
        }
    }
}

static uint32_t s_rd(void *c, unsigned blk, uint32_t off)
{
    sim *s = c;
    run_events(s);
    switch (blk) {
    case WMSS_BLK_RMB:
        if (off == WMSS_RMB_PBL_STATUS) return s->pbl;
        if (off == WMSS_RMB_MBA_STATUS) return s->mba_status;
        if (off == WMSS_RMB_MSS_STATUS) return s->mss_status;
        return s->rmb[off / 4];
    case WMSS_BLK_QDSP6: {
        uint32_t v = s->q6[off / 4];
        if (off == WMSS_QDSP6SS_SLEEP) v = (v & WMSS_CBCR_CLKEN) && s->fault != FL_SLEEP_STUCK ? v & ~WMSS_CBCR_CLKOFF : v | WMSS_CBCR_CLKOFF;
        return v;
    }
    case WMSS_BLK_GCC: {
        uint32_t v = s->gcc[off / 4];
        for (unsigned i = 0; i < NCLK; ++i) {
            if (kSClk[i].halt != off) continue;
            const int on = clk_enabled(s, i) && !(s->fault == FL_CLK_STUCK && s->stuck_clk == (int)i);
            v = on ? v & ~WMSS_CBCR_CLKOFF : v | WMSS_CBCR_CLKOFF;
        }
        return v;
    }
    case WMSS_BLK_AOSS_CC: return s->aoss;
    case WMSS_BLK_PDC: return s->pdc;
    case WMSS_BLK_TCSR: {
        const uint32_t base = off & ~0xfffu, reg = off & 0xfff;
        const int req = s->tcsr[(base + WMSS_AXI_HALTREQ) / 4] & 1;
        if (reg == WMSS_AXI_HALTACK) return req;
        if (reg == WMSS_AXI_IDLE) return (s->running || s->booted) && !req ? 0 : 1;
        return s->tcsr[off / 4];
    }
    }
    return 0;
}

static void s_wr(void *c, unsigned blk, uint32_t off, uint32_t v)
{
    sim *s = c;
    run_events(s);
    ++s->writes_hw;
    switch (blk) {
    case WMSS_BLK_GCC: s->gcc[off / 4] = v; break;
    case WMSS_BLK_AOSS_CC:
        if (v == 1 && !s->aoss) {++s->resets; modem_reset(s);}
        s->aoss = v;
        if (v == 1) ++s->reset_pulses;
        break;
    case WMSS_BLK_PDC: s->pdc = v; break;
    case WMSS_BLK_TCSR:
        s->tcsr[off / 4] = v;
        if ((off & 0xfff) == WMSS_AXI_HALTREQ && v) ++s->halts;
        break;
    case WMSS_BLK_QDSP6:
        s->q6[off / 4] = v;
        if (off == WMSS_QDSP6SS_BOOT_CMD && v == 1) {
            s->precond = boot_preconditions(s);
            if (s->precond) break;                      // the Q6 does not come up
            if (s->fault != FL_NO_FSM) ev_add(s, 200, EV_FSM, 0);
            if (s->fault != FL_NO_PBL && s->fault != FL_NO_FSM) ev_add(s, 300, EV_PBL, 0);
            if (s->fault != FL_NO_XPU && s->fault != FL_NO_PBL && s->fault != FL_NO_FSM && s->fault != FL_PBL_BAD) ev_add(s, 30000, EV_XPU, 0);
        }
        break;
    case WMSS_BLK_RMB:
        s->rmb[off / 4] = v;
        if (off == WMSS_RMB_MBA_COMMAND && v == WMSS_CMD_META_DATA_READY) {
            s->meta_addr = s->rmb[WMSS_RMB_PMI_META_DATA / 4];
            const int ok = s->mdata_alive && s->meta_addr == (uint32_t)s->mdata_phys && own_of(s, s->mdata_phys, 0)->mask == MSS &&
                           s->expect_meta && !memcmp(s->mdata, s->expect_meta, s->expect_meta_len) && s->fault != FL_META_FAIL;
            if (s->fault != FL_META_NEVER) ev_add(s, 2000, EV_META, ok ? WMSS_MBA_META_AUTH_SUCCESS : (uint32_t)-2);
            if (!ok) snprintf(s->why, sizeof s->why, "meta: addr %x alive %d owner %llx", s->meta_addr, s->mdata_alive,
                              (unsigned long long)own_of(s, s->mdata_phys, 0)->mask);
        } else if (off == WMSS_RMB_MBA_COMMAND && v == WMSS_CMD_LOAD_READY) {
            s->code_start = s->rmb[WMSS_RMB_PMI_CODE_START / 4];
        } else if (off == WMSS_RMB_PMI_CODE_LENGTH && v) {
            s->code_length = v;
            if (s->fault == FL_SEG_FAIL && v > 0x3000) s->mba_status = (uint32_t)-4;
            if (v == s->expect_total && s->fault != FL_AUTH_NEVER) ev_add(s, 50000, EV_AUTH, 0);
        }
        break;
    }
}

static uint64_t s_now(void *c) {sim *s = c; run_events(s); return s->t;}
static void s_udelay(void *c, uint32_t us) {sim *s = c; s->t += us ? us : 1; run_events(s);}
static void s_msleep(void *c, uint32_t ms) {sim *s = c; s->t += (uint64_t)ms * 1000; run_events(s);}

// Failure injection: every op that can fail counts; the fail_at-th one fails.
static int inject(sim *s, unsigned kind)
{
    if (++s->op_count == s->fail_at) {s->failed_kind = kind; s->failed_op = 1; return 1;}
    return 0;
}
enum {OP_ARC = 1, OP_ASSIGN, OP_LOADSTATE, OP_FWREAD, OP_WRITE, OP_ALLOC};

static int s_arc_get(void *c, unsigned a, uint32_t *cur, uint32_t *max)
{
    sim *s = c;
    if (inject(s, OP_ARC)) return -5;
    *cur = s->arc_cur[a]; *max = s->arc_max[a];
    return 0;
}
static int s_arc_set(void *c, unsigned a, uint32_t level)
{
    sim *s = c;
    if (inject(s, OP_ARC)) return -5;
    s->arc_cur[a] = level;
    return 0;
}
static int s_assign(void *c, uint64_t addr, uint64_t size, uint64_t *cur, const wmss_vmperm *next, unsigned n)
{
    sim *s = c;
    if (inject(s, OP_ASSIGN)) return -22;
    own_t *o = own_of(s, addr, size);
    if (!o) {++s->violations; return -22;}
    ++s->assigns;
    if (*cur != o->mask) {++s->violations; snprintf(s->why, sizeof s->why, "assign %llx: caller thinks %llx, hypervisor has %llx", (unsigned long long)addr, (unsigned long long)*cur, (unsigned long long)o->mask);}
    if (size & 0xfff) ++s->violations;
    uint64_t nm = 0;
    for (unsigned i = 0; i < n; ++i) nm |= 1ull << next[i].vmid;
    o->mask = nm; *cur = nm;
    return 0;
}
static int s_load_state(void *c, int on)
{
    sim *s = c;
    if (on && inject(s, OP_LOADSTATE)) return -110;
    s->load_state = on;
    return 0;
}
static int s_fw_size(void *c, const char *name, uint32_t *size)
{
    sim *s = c;
    for (unsigned i = 0; i < s->nfile; ++i) if (!strcmp(s->file[i].name, name)) {*size = s->file[i].len; return 0;}
    return -2;
}
static long s_fw_read(void *c, const char *name, uint32_t off, void *dst, uint32_t len)
{
    sim *s = c;
    if (inject(s, OP_FWREAD)) return -5;
    for (unsigned i = 0; i < s->nfile; ++i) {
        if (strcmp(s->file[i].name, name)) continue;
        if (off > s->file[i].len) return -22;
        const uint32_t n = len < s->file[i].len - off ? len : s->file[i].len - off;
        memcpy(dst, s->file[i].data + off, n);
        return n;
    }
    return -2;
}
static int s_region_write(void *c, unsigned region, uint64_t off, const void *src, uint32_t len)
{
    sim *s = c;
    if (inject(s, OP_WRITE)) return -5;
    // an XPU-protected region: the CPU may only write while HLOS still has access
    if (!(owner_of_region(s, region) & HLOS)) {++s->violations; snprintf(s->why, sizeof s->why, "CPU write to region %u owned by %llx", region, (unsigned long long)owner_of_region(s, region));}
    const uint64_t size = region == WMSS_REGION_MBA ? s->cfg.mba_size : region == WMSS_REGION_MPSS ? s->cfg.mpss_size : s->mdata_len;
    if (off + len > size) {++s->violations; return -22;}
    uint8_t *base = region == WMSS_REGION_MBA ? s->mba : region == WMSS_REGION_MPSS ? s->mpss : s->mdata;
    memcpy(base + off, src, len);
    return 0;
}
static int s_region_zero(void *c, unsigned region, uint64_t off, uint64_t len)
{
    sim *s = c;
    if (inject(s, OP_WRITE)) return -5;
    if (!(owner_of_region(s, region) & HLOS)) ++s->violations;
    if (region != WMSS_REGION_MPSS || off + len > s->cfg.mpss_size) {++s->violations; return -22;}
    memset(s->mpss + off, 0, len);
    return 0;
}
static int s_mdata_alloc(void *c, uint32_t size, uint64_t *phys)
{
    sim *s = c;
    if (inject(s, OP_ALLOC)) return -12;
    if (s->mdata_alive || size > sizeof s->mdata) {++s->violations; return -12;}
    s->mdata_alive = 1; s->mdata_len = size; s->mdata_phys = 0x80100000ull;
    own_t *o = own_of(s, s->mdata_phys, size);
    o->mask = HLOS;
    memset(s->mdata, 0, sizeof s->mdata);
    *phys = s->mdata_phys;
    return 0;
}
static void s_mdata_free(void *c)
{
    sim *s = c;
    if (!s->mdata_alive) {++s->violations; return;}
    if (own_of(s, s->mdata_phys, 0)->mask != HLOS) {++s->violations; snprintf(s->why, sizeof s->why, "metadata freed while owned by %llx", (unsigned long long)own_of(s, s->mdata_phys, 0)->mask);}
    s->mdata_alive = 0;
}
static int s_smp2p_bits(void *c, uint32_t *v)
{
    sim *s = c;
    run_events(s);
    if (!s->smp2p_valid) return -1;
    *v = s->smp2p_in;
    return 0;
}
static int s_smp2p_set(void *c, uint32_t mask, uint32_t v)
{
    sim *s = c;
    if (mask & WMSS_SMP2P_STOP_BIT) {
        s->smp2p_stop_out = (s->smp2p_stop_out & ~mask) | (v & mask);
        if ((v & mask) && s->running && s->fault != FL_NO_STOPACK) ev_add(s, 20000, EV_STOPACK, 0);
    }
    return 0;
}

static wmss_ops sim_ops(sim *s)
{
    wmss_ops o = {s, s_rd, s_wr, s_now, s_udelay, s_msleep, s_arc_get, s_arc_set, s_assign, s_load_state, s_fw_size, s_fw_read,
                  s_region_write, s_region_zero, s_mdata_alloc, s_mdata_free, s_smp2p_bits, s_smp2p_set};
    return o;
}

// ---- a synthetic modem image (the shape of the phone's: an ELF with a hash segment, split into modem.mdt + modem.bNN) ----
enum {V_SPLIT, V_HASH_OFFSET, V_HASH_FILE, V_NONSPLIT, V_ABSOLUTE, V_N};
#define SYN_MPSS_PHYS 0x8e000000ull
#define SYN_MPSS_SIZE 0x40000ull
#define SYN_MBA_PHYS 0x96500000ull
#define SYN_MBA_SIZE 0x20000ull

static void le32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static void add_file(sim *s, const char *name, const uint8_t *d, uint32_t len)
{
    strcpy(s->file[s->nfile].name, name);
    s->file[s->nfile].data = (uint8_t *)t_talloc(len);
    memcpy(s->file[s->nfile].data, d, len);
    s->file[s->nfile].len = len;
    ++s->nfile;
}
static void fill(uint8_t *p, uint32_t n, unsigned seed) {for (uint32_t i = 0; i < n; ++i) p[i] = (uint8_t)(seed * 131 + i * 7 + (i >> 8));}

typedef struct {uint32_t total_memsz; uint32_t boot_addr; unsigned nloads;} syn_info;

static void sim_init(sim *s, unsigned variant)
{
    memset(s, 0, sizeof *s);
    s->stuck_clk = -1;
    s->arc_cur[WMSS_ARC_MX] = 3; s->arc_cur[WMSS_ARC_CX] = 6; s->arc_cur[WMSS_ARC_MSS] = 0;
    s->arc_max[WMSS_ARC_MX] = 4; s->arc_max[WMSS_ARC_CX] = 7; s->arc_max[WMSS_ARC_MSS] = 7;
    s->cfg = (wmss_cfg){SYN_MBA_PHYS, SYN_MBA_SIZE, SYN_MPSS_PHYS, SYN_MPSS_SIZE, 0xf5b01000ull, 0x200000, "image/mba.mbn", "image/modem.mdt", 0, 0};
    s->mba = t_talloc(SYN_MBA_SIZE);
    s->mpss = t_talloc(SYN_MPSS_SIZE);
    s->gcc[0x52004 / 4] = 1u << 10;     // boot rom AHB already voted by someone else: must stay on
    s->init_gcc52004 = s->gcc[0x52004 / 4];
    s->gcc[0x34004 / 4] = s->gcc[0x38004 / 4] = s->gcc[0x8a000 / 4] = s->gcc[0x8a004 / 4] = 0;
    s->tcsr[0] = 0;
    // mba.mbn
    const uint32_t mbalen = 0x9000;
    uint8_t *mba = t_talloc(mbalen);
    fill(mba, mbalen, 1);
    add_file(s, "image/mba.mbn", mba, mbalen);
    s->mba_len = mbalen;
    // modem.mdt + segments
    enum {NPH = 6};
    const uint32_t ehdr = 52 + NPH * 32, hash = 0x300;
    struct {uint32_t type, flags, paddr, filesz, memsz;} ph[NPH] = {
        {0, 0, 0, ehdr, 0}, {0, 2u << 24, 0, hash, 0},
        {1, 7 | WMDT_RELOCATABLE, 0x0, 0x3000, 0x3000}, {1, 7 | WMDT_RELOCATABLE, 0x10000, 0x1800, 0x4000},
        {1, 6 | WMDT_RELOCATABLE, 0x20000, 0, 0x2000}, {1, 7 | WMDT_RELOCATABLE, 0x30000, 0x100, 0x100}};
    const uint32_t base = variant == V_ABSOLUTE ? (uint32_t)SYN_MPSS_PHYS : 0xc0000000u;
    uint8_t *seg[NPH] = {0};
    uint32_t file_off[NPH];
    uint32_t cursor = ehdr + hash;
    if (variant == V_HASH_OFFSET) cursor += 0x40;      // padding between the packed header and ...
    uint32_t mdt_len = ehdr + hash;
    if (variant == V_HASH_OFFSET) mdt_len = cursor;
    if (variant == V_HASH_FILE) mdt_len = ehdr;
    uint8_t *hashd = t_talloc(hash);
    fill(hashd, hash, 9);
    for (unsigned i = 2; i < NPH; ++i) {
        if (ph[i].filesz) {seg[i] = t_talloc(ph[i].filesz); fill(seg[i], ph[i].filesz, 20 + i);}
        file_off[i] = variant == V_NONSPLIT ? cursor : 0x100000u + i * 0x10000;   // beyond the .mdt when split
        if (variant == V_NONSPLIT) cursor += ph[i].filesz;
    }
    if (variant == V_NONSPLIT) mdt_len = cursor;
    uint8_t *mdt = t_talloc(mdt_len > 0x40000 ? mdt_len : 0x40000);
    le32(mdt, 0x464c457fu); mdt[4] = 1; mdt[5] = 1; mdt[6] = 1;
    le32(mdt + 24, base);
    le32(mdt + 28, 52);
    mdt[42] = 32; mdt[44] = NPH;
    for (unsigned i = 0; i < NPH; ++i) {
        uint8_t *p = mdt + 52 + i * 32;
        uint32_t off = i == 0 ? 0 : i == 1 ? (variant == V_HASH_OFFSET ? ehdr + hash + 0x20 : ehdr) : file_off[i];
        le32(p, ph[i].type); le32(p + 4, off); le32(p + 8, base + ph[i].paddr); le32(p + 12, base + ph[i].paddr);
        le32(p + 16, ph[i].filesz); le32(p + 20, ph[i].memsz); le32(p + 24, ph[i].flags); le32(p + 28, 4);
    }
    if (variant == V_HASH_OFFSET) {
        // the hash segment sits at p_offset, beyond a gap; the packed-after-header rule must not apply
        mdt_len = ehdr + hash + 0x20 + hash;
        memset(mdt + ehdr, 0xee, hash + 0x20);
        memcpy(mdt + ehdr + hash + 0x20, hashd, hash);
    } else if (variant == V_HASH_FILE) {
        add_file(s, "image/modem.b01", hashd, hash);
    } else {
        memcpy(mdt + ehdr, hashd, hash);
    }
    if (variant == V_NONSPLIT) for (unsigned i = 2; i < NPH; ++i) if (seg[i]) memcpy(mdt + file_off[i], seg[i], ph[i].filesz);
    add_file(s, "image/modem.mdt", mdt, mdt_len);
    if (variant != V_NONSPLIT)
        for (unsigned i = 2; i < NPH; ++i) {
            if (!seg[i]) continue;
            char nm[32];
            snprintf(nm, sizeof nm, "image/modem.b%02u", i);
            add_file(s, nm, seg[i], ph[i].filesz);
        }
    // what the MBA is expected to be given: the ELF header and program headers, then the hash segment
    uint8_t *meta = t_talloc(ehdr + hash);
    memcpy(meta, mdt, ehdr);
    memcpy(meta + ehdr, hashd, hash);
    s->expect_meta = meta; s->expect_meta_len = ehdr + hash;
    s->expect_total = 0;
    for (unsigned i = 2; i < NPH; ++i) s->expect_total += ph[i].memsz;
    // keep the segment data for the final check
    s->unused = 0;
}

static wmss *new_wmss(const wmss_ops *ops, const wmss_cfg *cfg)
{
    wmss *m = t_talloc(sizeof *m);
    wmss_init(m, ops, cfg);
    return m;
}

// The image's expected placement in the mpss carveout: segment i at paddr - min_addr (relocatable) or - mpss_phys.
static int check_mpss_contents(sim *s, unsigned variant)
{
    const uint32_t base = variant == V_ABSOLUTE ? (uint32_t)SYN_MPSS_PHYS : 0xc0000000u;
    const uint32_t reloc = variant == V_ABSOLUTE ? (uint32_t)SYN_MPSS_PHYS : base;
    static const uint32_t pa[4] = {0x0, 0x10000, 0x20000, 0x30000}, fsz[4] = {0x3000, 0x1800, 0, 0x100}, msz[4] = {0x3000, 0x4000, 0x2000, 0x100};
    for (unsigned k = 0; k < 4; ++k) {
        const unsigned i = k + 2;
        uint8_t *want = t_talloc(msz[k]);
        if (fsz[k]) fill(want, fsz[k], 20 + i);
        if (memcmp(s->mpss + (base + pa[k] - reloc), want, msz[k])) return 0;
    }
    return 1;
}

static void snapshot_clean(sim *s)
{
    // everything is as it was before the boot, except that the modem was reset
    CHECK(own_of(s, s->cfg.mba_phys, 0)->mask == HLOS);
    CHECK(own_of(s, s->cfg.mpss_phys, 0)->mask == HLOS);
    CHECK(!s->mdata_alive || own_of(s, s->mdata_phys, 0)->mask != HLOS);   // freed, or kept wired because it could not be given back
    CHECK(s->rmb[0] == s->rmb[0]);
    for (unsigned c = 0; c < NCLK; ++c) {
        const int was = kSClk[c].en == 0x52004 && kSClk[c].mask == (1u << 10);   // the one that was on before
        CHECK(clk_enabled(s, c) == was);
    }
    CHECK(s->gcc[0x52004 / 4] == s->init_gcc52004);
    CHECK(s->arc_cur[WMSS_ARC_MX] == 3 && s->arc_cur[WMSS_ARC_CX] == 6 && s->arc_cur[WMSS_ARC_MSS] == 0);
    CHECK(!s->load_state);
    CHECK(!s->running && !s->booted);
    CHECK(s->aoss == 0 && !(s->pdc >> 9 & 1));
    CHECK(!(s->tcsr[WMSS_HALT_Q6 / 4] & 1) && !(s->tcsr[WMSS_HALT_MODEM / 4] & 1) && !(s->tcsr[WMSS_HALT_NC / 4] & 1));
}

static void t_mss_boot_ok(void)
{
    for (unsigned v = 0; v < V_N; ++v) {
        sim *s = t_talloc(sizeof *s);
        sim_init(s, v);
        wmss_ops ops = sim_ops(s);
        wmss *m = new_wmss(&ops, &s->cfg);
        const int rc = wmss_boot(m);
        if (rc) printf("variant %u: rc %d failed step %s (%d), why: %s precond %x\n", v, rc, wmss_step_name((unsigned)m->failed_step), m->failed_step, s->why, s->precond);
        CHECK(rc == 0);
        CHECK(s->precond == 0 && s->violations == 0);
        CHECK(s->running && (s->smp2p_in & WMSS_SMP2P_READY));
        CHECK(own_of(s, s->cfg.mba_phys, 0)->mask == HLOS);         // the MBA image is back with HLOS
        CHECK(own_of(s, s->cfg.mpss_phys, 0)->mask == MSS);         // the modem's memory belongs to the modem
        CHECK(own_of(s, s->cfg.rmtfs_phys, 0)->mask == (HLOS | MSS));
        CHECK(!s->mdata_alive);                                      // metadata buffer given back and freed
        CHECK(s->load_state == 1);
        CHECK(s->code_start == (v == V_ABSOLUTE ? (uint32_t)SYN_MPSS_PHYS : (uint32_t)SYN_MPSS_PHYS) );
        CHECK(s->code_length == s->expect_total);
        CHECK(check_mpss_contents(s, v));
        // handover came: the proxy votes are back at their previous level and prng is off; the others are on
        CHECK(!m->proxy_held);
        CHECK(s->arc_cur[WMSS_ARC_MX] == 3 && s->arc_cur[WMSS_ARC_CX] == 6 && s->arc_cur[WMSS_ARC_MSS] == 0);
        CHECK(!clk_enabled(s, 6) && clk_enabled(s, 0) && clk_enabled(s, 1) && clk_enabled(s, 2) && clk_enabled(s, 3) && clk_enabled(s, 4) && clk_enabled(s, 5));
        // the order of the trace
        static const unsigned order[] = {WMSS_S_PREFLIGHT, WMSS_S_RMTFS, WMSS_S_MBA_COPY, WMSS_S_LOAD_STATE, WMSS_S_VOTE, WMSS_S_PROXY_CLK,
                                         WMSS_S_RESET_CLK, WMSS_S_RESET, WMSS_S_ACTIVE_CLK, WMSS_S_ASSIGN_MPSS, WMSS_S_ASSIGN_MBA,
                                         WMSS_S_Q6_RESET, WMSS_S_MBA_WAIT, WMSS_S_META_ALLOC, WMSS_S_META_ASSIGN, WMSS_S_META_AUTH,
                                         WMSS_S_META_RECLAIM, WMSS_S_MPSS_SHARE, WMSS_S_SEGMENTS, WMSS_S_MPSS_MSS, WMSS_S_AUTH,
                                         WMSS_S_READY, WMSS_S_MBA_RECLAIM, WMSS_S_HANDOVER};
        unsigned at = 0;
        for (unsigned i = 0; i < m->ntrace && at < sizeof order / sizeof order[0]; ++i)
            if (m->trace[i].step == order[at] && m->trace[i].rc == 0) ++at;
        if (at != sizeof order / sizeof order[0]) printf("variant %u: trace stops after %s\n", v, wmss_step_name(order[at ? at - 1 : 0]));
        CHECK(at == sizeof order / sizeof order[0]);
        // then stop: everything is given back and powered down
        CHECK(wmss_stop(m) == 0);
        CHECK(s->smp2p_stop_out == 0);
        snapshot_clean(s);
        CHECK(own_of(s, s->cfg.rmtfs_phys, 0)->mask == HLOS);
        CHECK(s->halts >= 1);
        CHECK(s->violations == 0);
        t_tfree_all();
    }
}

static void t_mss_handover_late(void)
{
    sim *s = t_talloc(sizeof *s);
    sim_init(s, V_SPLIT);
    s->fault = FL_NO_HANDOVER;
    wmss_ops ops = sim_ops(s);
    s->cfg.handover_ms = 500;
    wmss *m = new_wmss(&ops, &s->cfg);
    CHECK(wmss_boot(m) == 0);
    // not an error: the proxy votes stay until the handover bit shows up (poll)
    CHECK(m->proxy_held && s->arc_cur[WMSS_ARC_CX] == 7 && s->arc_cur[WMSS_ARC_MX] == 4 && s->arc_cur[WMSS_ARC_MSS] == 7 && clk_enabled(s, 6));
    CHECK(wmss_poll_handover(m) == 0);
    s->smp2p_in |= WMSS_SMP2P_HANDOVER;
    CHECK(wmss_poll_handover(m) == 1 && !m->proxy_held && s->arc_cur[WMSS_ARC_CX] == 6 && !clk_enabled(s, 6));
    CHECK(wmss_poll_handover(m) == 1);
    // stopping before the handover also releases the proxy resources
    sim *s2 = t_talloc(sizeof *s2);
    sim_init(s2, V_SPLIT);
    s2->fault = FL_NO_HANDOVER;
    wmss_ops o2 = sim_ops(s2);
    s2->cfg.handover_ms = 100;
    wmss *m2 = new_wmss(&o2, &s2->cfg);
    CHECK(wmss_boot(m2) == 0 && m2->proxy_held);
    s2->fault = FL_NONE;
    CHECK(wmss_stop(m2) == 0);
    snapshot_clean(s2);
    t_tfree_all();
}

// A failure of any kind must leave the machine clean.
static void check_failed_boot(sim *s, wmss *m, int rc, const char *what, unsigned a, int perms_known_good)
{
    ++tchecks;
    if (!rc) {++tfails; printf("FAIL %s %u: boot succeeded\n", what, a); return;}
    const int f0 = tfails;
    CHECK(m->rc == rc || m->rc != 0);
    CHECK(m->failed_step >= 0);
    CHECK(s->violations == 0);
    CHECK(!s->running);
    CHECK(s->load_state == 0);
    CHECK(s->arc_cur[WMSS_ARC_MX] == 3 && s->arc_cur[WMSS_ARC_CX] == 6 && s->arc_cur[WMSS_ARC_MSS] == 0);
    for (unsigned c = 0; c < NCLK; ++c) CHECK(clk_enabled(s, c) == (c == 3));
    CHECK(s->gcc[0x52004 / 4] == s->init_gcc52004);
    CHECK(s->aoss == 0 && !(s->pdc >> 9 & 1));
    if (perms_known_good) {
        CHECK(own_of(s, s->cfg.mba_phys, 0)->mask == HLOS);
        CHECK(own_of(s, s->cfg.mpss_phys, 0)->mask == HLOS);
        CHECK(own_of(s, s->cfg.rmtfs_phys, 0)->mask == HLOS);
        CHECK(!s->mdata_alive);
    } else {
        // a reclaim itself failed: nothing is freed that the MSS still owns
        CHECK(!s->mdata_alive || own_of(s, s->mdata_phys, 0)->mask != HLOS);
    }
    CHECK(!(s->tcsr[WMSS_HALT_Q6 / 4] & 1));
    if (tfails != f0) printf("   (%s %u: rc %d step %s, why %s, precond %x)\n", what, a, rc, wmss_step_name((unsigned)m->failed_step), s->why, s->precond);
}

static void t_mss_op_failures(void)
{
    for (unsigned v = 0; v < 2; ++v) {
        unsigned total = 0;
        for (unsigned k = 1; k < 200; ++k) {
            sim *s = t_talloc(sizeof *s);
            sim_init(s, v == 0 ? V_SPLIT : V_NONSPLIT);
            s->fail_at = k;
            wmss_ops ops = sim_ops(s);
            s->cfg.handover_ms = 50;
            wmss *m = new_wmss(&ops, &s->cfg);
            const int rc = wmss_boot(m);
            if (!s->failed_op) {total = k - 1; CHECK(rc == 0); t_tfree_all(); break;}
            if (!rc) {
                // a best-effort step failed after the modem was up (the MBA carveout reclaim, a rail restore): not fatal
                CHECK(s->failed_kind == OP_ASSIGN || s->failed_kind == OP_ARC);
                unsigned ready = 0;
                for (unsigned i = 0; i < m->ntrace; ++i) ready += m->trace[i].step == WMSS_S_READY;
                CHECK(ready == 1 && s->violations == 0);
                t_tfree_all();
                continue;
            }
            // The failing op was a reclaim when it is an assign after the modem was started; permissions can then stay.
            check_failed_boot(s, m, rc, "op", k, s->failed_kind != OP_ASSIGN);
            t_tfree_all();
        }
        CHECK(total > 30);
    }
}

static void t_mss_faults(void)
{
    static const char *const names[FL_N] = {"none", "no-fsm", "no-pbl", "pbl-bad", "no-xpu", "xpu-bad", "xpu-scribbled", "meta-fail",
                                            "meta-never", "seg-fail", "auth-never", "auth-fail", "no-ready", "fatal", "no-handover",
                                            "sleep-stuck", "clk-stuck", "no-stopack"};
    for (int f = 1; f < FL_N; ++f) {
        const unsigned nclk = f == FL_CLK_STUCK ? NCLK : 1;
        for (unsigned c = 0; c < nclk; ++c) {
            sim *s = t_talloc(sizeof *s);
            sim_init(s, V_SPLIT);
            s->fault = f; s->stuck_clk = (int)c;
            if (f == FL_CLK_STUCK && (c == 4)) {t_tfree_all(); continue;}   // gpll0_mss has no halt bit (a delay): cannot be stuck
            wmss_ops ops = sim_ops(s);
            s->cfg.handover_ms = 50;
            wmss *m = new_wmss(&ops, &s->cfg);
            const int rc = wmss_boot(m);
            if (f == FL_XPU_SCRIB || f == FL_NO_HANDOVER) {
                CHECK(rc == 0);          // "XPU unlocked, scribbled" is a success; a late handover is not an error
                CHECK(wmss_stop(m) == 0 || f == FL_NO_STOPACK);
                snapshot_clean(s);
            } else if (f == FL_NO_STOPACK) {
                CHECK(rc == 0);
                CHECK(wmss_stop(m) != 0);       // timed out, but everything is still torn down
                snapshot_clean(s);
            } else {
                check_failed_boot(s, m, rc, names[f], c, 1);
                // the failing step is where the fault is
                if (f == FL_NO_FSM || f == FL_NO_PBL || f == FL_PBL_BAD) CHECK(m->failed_step == WMSS_S_Q6_RESET);
                if (f == FL_NO_XPU || f == FL_XPU_BAD) CHECK(m->failed_step == WMSS_S_MBA_WAIT);
                if (f == FL_META_FAIL || f == FL_META_NEVER) CHECK(m->failed_step == WMSS_S_META_AUTH);
                if (f == FL_SEG_FAIL) CHECK(m->failed_step == WMSS_S_SEGMENTS);
                if (f == FL_AUTH_NEVER || f == FL_AUTH_FAIL) CHECK(m->failed_step == WMSS_S_AUTH);
                if (f == FL_NO_READY || f == FL_FATAL) CHECK(m->failed_step == WMSS_S_READY);
                if (f == FL_SLEEP_STUCK) CHECK(m->failed_step == WMSS_S_Q6_RESET);
                // after a failure the modem was put back in reset, after halting its AXI ports when it had been started
                if (f != FL_CLK_STUCK && f != FL_SLEEP_STUCK) CHECK(s->resets >= 1);
                if (f == FL_NO_READY || f == FL_FATAL || f == FL_AUTH_NEVER || f == FL_AUTH_FAIL || f == FL_META_FAIL || f == FL_SEG_FAIL) CHECK(s->halts >= 1);
            }
            t_tfree_all();
        }
    }
}

// What preflight refuses must be refused before anything is touched.
static void t_mss_preflight(void)
{
    enum {B_MBA_BIG, B_NO_MDT, B_NO_SEG, B_SEG_SIZE, B_OUTSIDE, B_NOHASH, B_NOMBA, B_N};
    for (unsigned b = 0; b < B_N; ++b) {
        sim *s = t_talloc(sizeof *s);
        sim_init(s, V_SPLIT);
        wmss_ops ops = sim_ops(s);
        switch (b) {
        case B_MBA_BIG: s->file[0].len = 0x100001; break;
        case B_NO_MDT: strcpy(s->file[1].name, "image/xxx.mdt"); break;
        case B_NO_SEG: strcpy(s->file[2].name, "image/modem.b99"); break;
        case B_SEG_SIZE: s->file[2].len -= 1; break;
        case B_OUTSIDE: s->cfg.mpss_size = 0x20000; break;
        case B_NOHASH: s->file[1].data[52 + 32 + 27] = 0; break;     // clear the hash type bits of phdr 1
        case B_NOMBA: strcpy(s->file[0].name, "image/none"); break;
        }
        wmss *m = new_wmss(&ops, &s->cfg);
        CHECK(wmss_boot(m) != 0);
        CHECK(m->failed_step == WMSS_S_PREFLIGHT);
        CHECK(s->writes_hw == 0 && s->assigns == 0 && s->load_state == 0 && s->nown == 0 && s->arc_cur[WMSS_ARC_CX] == 6);
        CHECK(s->violations == 0);
        t_tfree_all();
    }
}

// The phone's own modem partition: preflight on the real mba.mbn / modem.mdt / modem.bNN and a whole boot against the simulated
// modem, with the segments compared to the file bytes.
static int part_read(void *ctx, uint64_t block, uint32_t count, void *dst)
{
    const uint8_t *img = ctx;
    memcpy(dst, img + block * 4096, (size_t)count * 4096);
    return 0;
}
typedef struct {wfat fs; sim *s;} realctx;
static int r_size(void *c, const char *name, uint32_t *size)
{
    realctx *r = c;
    wfat_file f;
    if (wfat_open(&r->fs, name, &f) || f.dir) return -2;
    *size = f.size;
    return 0;
}
static long r_read(void *c, const char *name, uint32_t off, void *dst, uint32_t len)
{
    realctx *r = c;
    wfat_file f;
    if (wfat_open(&r->fs, name, &f) || f.dir) return -2;
    return wfat_read(&r->fs, &f, off, dst, len);
}
static void t_mss_real(void)
{
    if (!t_modem_img) {printf("  t_mss_real: skipped (no modem partition image)\n"); return;}
    size_t len = 0;
    uint8_t *img = t_slurp(t_modem_img, &len);
    CHECK(img != NULL);
    if (!img) return;
    realctx *rc = t_talloc(sizeof *rc);
    CHECK(wfat_mount(&rc->fs, part_read, img, len / 4096) == WFAT_OK);
    sim *s = t_talloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->stuck_clk = -1;
    s->arc_cur[WMSS_ARC_MX] = 3; s->arc_cur[WMSS_ARC_CX] = 6;
    s->arc_max[WMSS_ARC_MX] = 4; s->arc_max[WMSS_ARC_CX] = 7; s->arc_max[WMSS_ARC_MSS] = 7;
    s->cfg = (wmss_cfg){0x96500000ull, 0x200000, 0x8e000000ull, 0x7800000, 0xf5b01000ull, 0x200000, "image/mba.mbn", "image/modem.mdt", 0, 100};
    s->mba = calloc(1, 0x200000);
    s->mpss = calloc(1, 0x7800000);
    s->gcc[0x52004 / 4] = 1u << 10; s->init_gcc52004 = s->gcc[0x52004 / 4];
    // the simulated file table gets the phone's real files: file[0] is the mba, the rest follow
    static const char *const fixed[] = {"image/mba.mbn", "image/modem.mdt"};
    uint32_t mdtlen = 0;
    for (unsigned i = 0; i < 2 + 100; ++i) {
        char nm[40];
        if (i < 2) snprintf(nm, sizeof nm, "%s", fixed[i]);
        else snprintf(nm, sizeof nm, "image/modem.b%02u", i - 2);
        uint32_t sz = 0;
        if (r_size(rc, nm, &sz) || s->nfile >= 38) continue;
        s->file[s->nfile].data = malloc(sz ? sz : 1);
        s->file[s->nfile].len = sz;
        r_read(rc, nm, 0, s->file[s->nfile].data, sz);
        strcpy(s->file[s->nfile].name, nm);
        if (i == 1) mdtlen = sz;
        ++s->nfile;
    }
    CHECK(s->nfile == 2 + 27 && !strcmp(s->file[0].name, "image/mba.mbn"));
    s->mba_len = s->file[0].len;
    const uint8_t *mdt = s->file[1].data;
    wmdt_image im;
    CHECK(wmdt_parse(mdt, mdtlen, &im) == 0);
    CHECK(im.phnum == 30 && im.relocatable && im.hash_index == 1);
    const uint32_t ehdr = im.ph[0].filesz, hash = im.ph[1].filesz;
    CHECK(ehdr + hash == mdtlen);       // the phone's modem.mdt: header, then the hash segment
    s->expect_meta = mdt; s->expect_meta_len = mdtlen;
    unsigned loads = 0;
    for (unsigned i = 0; i < im.phnum; ++i) if (wmdt_segment_loads(&im.ph[i])) {s->expect_total += im.ph[i].memsz; ++loads;}
    wmss_ops ops = sim_ops(s);
    wmss *m = new_wmss(&ops, &s->cfg);
    const int r = wmss_boot(m);
    if (r) printf("  real image: rc %d step %s value %x why %s precond %x\n", r, wmss_step_name((unsigned)m->failed_step),
                  m->trace[m->ntrace - 1].value, s->why, s->precond);
    CHECK(r == 0);
    CHECK(s->violations == 0 && s->precond == 0);
    CHECK(s->code_length == s->expect_total && s->code_start == 0x8e000000u);
    CHECK(loads == 28);
    // every loadable segment is where Linux would put it, with the file's bytes
    unsigned good = 0;
    for (unsigned i = 0; i < im.phnum; ++i) {
        if (!wmdt_segment_loads(&im.ph[i])) continue;
        uint64_t off;
        CHECK(wmdt_segment_offset(&im, i, s->cfg.mpss_phys, s->cfg.mpss_size, &off) == 0);
        if (!im.ph[i].filesz) {++good; continue;}
        char nm[32];
        snprintf(nm, sizeof nm, "image/modem.b%02u", i);
        uint32_t sz = 0;
        r_size(rc, nm, &sz);
        uint8_t *want = malloc(sz);
        r_read(rc, nm, 0, want, sz);
        good += sz == im.ph[i].filesz && !memcmp(s->mpss + off, want, sz);
        free(want);
    }
    CHECK(good == loads);
    printf("  real modem_b: %u segments, %u bytes placed, boot rc %d in %llu virtual ms\n", loads, s->expect_total, r,
           (unsigned long long)(s->t / 1000));
    CHECK(wmss_stop(m) == 0);
    snapshot_clean(s);
    for (unsigned i = 0; i < s->nfile; ++i) free(s->file[i].data);
    free(s->mba); free(s->mpss); free(img);
}

static void t_mss_survey(void)
{
    sim *s = t_talloc(sizeof *s);
    sim_init(s, V_SPLIT);
    wmss_ops ops = sim_ops(s);
    wmss *m = new_wmss(&ops, &s->cfg);
    wmss_reg regs[64];
    const unsigned n = wmss_survey(m, regs, 64);
    CHECK(n >= 20 && s->writes_hw == 0);
    t_tfree_all();
}

void t_mss(void)
{
    RUN(t_mss_boot_ok);
    RUN(t_mss_handover_late);
    RUN(t_mss_op_failures);
    RUN(t_mss_faults);
    RUN(t_mss_preflight);
    RUN(t_mss_survey);
    RUN(t_mss_real);
}
