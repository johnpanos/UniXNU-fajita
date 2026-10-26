// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita Wi-Fi stage 2: the modem (MSS) boot, `fajita-wifi>=2`. The sequence is drivers/FajitaWifi/WifiMss.c (Linux's
// qcom_q6v5_mss.c for sdm845, host-tested against a simulated modem, every failure path unwinding); this file supplies the
// hardware under it:
//   - register windows: QDSP6SS 0x04080000, RMB 0x04180000, GCC (0x134000..0x18b000), AOSS CC MSS_RESTART 0x0c2b0000,
//     PDC sync reset 0x0b2e0000, TCSR halt regs 0x01f63000 (device memory, 32-bit accesses)
//   - the carveouts the MBA and the modem live in (write-combined mappings of mba 0x96500000 and mpss 0x8e000000, which
//     the OS does not manage), and a physically contiguous metadata buffer below 4 GiB
//   - RPMh ARC votes for mx/cx/mss through fajita_rpmh_send (FajitaBusVote.cpp) and the previous level read back first
//   - SCM memory assignment (qcom_scm_assign_mem: SIP service 0x0c command 0x16, the same SMC convention the zap shader
//     load uses), also behind fajita_wifi_msa_assign for the wlfw MSA regions (ath10k_qmi_setup_msa_permissions)
//   - the AOSS QMP load_state message (FajitaWifiLower.cpp), SMP2P ready/handover (FajitaWifiStack.cpp), and the firmware
//     files of the phone's modem partition (FajitaWifiStack.cpp's FAT reader)
// `fajita-wifi-mss-dry=1` only maps and reads the registers (a survey, no write). `kcmd wifimss` shows the rows, runs the
// survey and stops the modem. Nothing here runs without fajita-wifi>=2.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include "FajitaWifiBoard.h"
extern "C" {
#include "WifiMss.h"
#include "WifiWlfw.h"
int fajita_rpmh_lookup(const char *name, uint32_t *addr, uint8_t *aux, unsigned *auxLen) __attribute__((weak));
int fajita_rpmh_send(unsigned n, const uint32_t (*cmd)[3], uint32_t *resp) __attribute__((weak));
int fajita_wifi_smp2p_bits(uint32_t *value);
int fajita_wifi_smp2p_set(uint32_t mask, uint32_t value);
void fajita_wifi_smp2p_diag(char *buf, size_t cap);
int fajita_wifi_qmp_load_state(const char *name, int on);
int fajita_wifi_fw_size(const char *path, uint32_t *size);
long fajita_wifi_fw_read(const char *path, uint32_t off, void *dst, uint32_t len);
int fajita_wifi_modem(void);
void fajita_wifi_modem_poll(void);
int fajita_wifi_msa_assign(const wlfw_mem_region *regions, unsigned count);
void fajita_wifi_msa_release(void);
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
extern char fajita_wifi_rows[12][160];
extern volatile uint32_t fajita_wifi_state[8];
}

namespace {
struct Map {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapPhys(uint64_t phys, uint64_t size, Map &m, IOOptionBits cache)
{
    if (m.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, size);
    if (!memory) return false;
    m.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | cache), OSNoRetain);
    memory->release();
    m.va = m.keep ? static_cast<uintptr_t>(m.keep->getVirtualAddress()) : 0;
    return m.va != 0;
}
uint64_t nowUs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}

// ---- register blocks (offsets are the Linux driver's) ----
struct Block {uint64_t phys; uint32_t size, bias; Map map;};
Block gBlk[WMSS_NBLK] = {
    {FW_MSS_QDSP6_BASE, 0x1000, 0, {}},
    {FW_MSS_RMB_BASE, 0x1000, 0, {}},
    {FW_GCC_BASE + 0x34000, 0x57000, 0x34000, {}},          // gcc-sdm845: 0x34004 .. 0x8a154
    {FW_AOSS_CC_BASE + 0x10000, 0x1000, 0x10000, {}},       // MSS_RESTART
    {FW_PDC_GLOBAL_BASE, 0x1000, 0, {}},                    // RPMH_PDC_SYNC_RESET 0x100
    {0x01f60000ull + 0x3000, 0x3000, 0x3000, {}},           // tcsr_regs_1 halt regs 0x3000, 0x4000, 0x5000
};
unsigned gBadAccess;
volatile uint32_t *reg(unsigned blk, uint32_t off)
{
    Block &b = gBlk[blk];
    if (off < b.bias || off + 4 > b.bias + b.size || !b.map.va) {++gBadAccess; return nullptr;}
    return reinterpret_cast<volatile uint32_t *>(b.map.va + (off - b.bias));
}
uint32_t hwRd(void *, unsigned blk, uint32_t off)
{
    volatile uint32_t *r = reg(blk, off);
    if (!r) return 0xdeadbeef;
    const uint32_t v = *r;
    __asm__ volatile("dsb ld" ::: "memory");
    return v;
}
void hwWr(void *, unsigned blk, uint32_t off, uint32_t v)
{
    volatile uint32_t *r = reg(blk, off);
    if (!r) return;
    __asm__ volatile("dsb st" ::: "memory");
    *r = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
bool mapBlocks()
{
    for (Block &b : gBlk) if (!mapPhys(b.phys, b.size, b.map, kIOMapInhibitCache)) return false;
    return true;
}

// ---- time ----
uint64_t opNow(void *) {return nowUs();}
void opUdelay(void *, uint32_t us) {if (us >= 2000) IOSleep((us + 999) / 1000); else IODelay(us);}
void opMsleep(void *, uint32_t ms) {IOSleep(ms);}

// ---- RPMh ARC votes: cmd-db address + level table, the current vote by a READ, a vote by a WRITE with completion ----
constexpr uint32_t kMsgWriteResp = (1u << 16) | (1u << 8) | 8u, kMsgReadResp = (1u << 8) | 8u;
struct Arc {const char *name; uint32_t addr = 0, max = 0; bool ok = false;};
Arc gArc[WMSS_NARC] = {{"mx.lvl"}, {"cx.lvl"}, {"mss.lvl"}};
bool arcResolve(unsigned a)
{
    Arc &r = gArc[a];
    if (r.ok) return true;
    if (!fajita_rpmh_lookup || !fajita_rpmh_send) return false;
    uint8_t aux[64] = {};
    unsigned n = sizeof aux;
    if (fajita_rpmh_lookup(r.name, &r.addr, aux, &n) || !r.addr) return false;
    unsigned count = 0;   // rpmhpd_update_level_mapping: the table ends at the first zero after entry 0
    for (unsigned i = 0; i + 1 < n; i += 2, ++count)
        if (i > 0 && !(aux[i] | aux[i + 1])) break;
    if (count < 2) return false;
    r.max = count - 1;
    r.ok = true;
    return true;
}
int arcRead(unsigned a, uint32_t *v)
{
    const uint32_t cmd[1][3] = {{kMsgReadResp, gArc[a].addr, 0}};
    uint32_t resp[1] = {};
    const int t = fajita_rpmh_send(1, cmd, resp);
    if (t < 0) return t;
    *v = resp[0];
    return 0;
}
int opArcGet(void *, unsigned a, uint32_t *cur, uint32_t *max)
{
    if (a >= WMSS_NARC || !arcResolve(a)) return -19;
    const int r = arcRead(a, cur);
    if (r) return r;
    if (*cur > gArc[a].max) return -22;     // not a level index: do not trust it
    *max = gArc[a].max;
    return 0;
}
int opArcSet(void *, unsigned a, uint32_t level)
{
    if (a >= WMSS_NARC || !arcResolve(a)) return -19;
    const uint32_t cmd[1][3] = {{kMsgWriteResp, gArc[a].addr, level}};
    uint32_t resp[1] = {};
    const int t = fajita_rpmh_send(1, cmd, resp);
    if (t < 0) return t;
    uint32_t back = 0xffffffffu;
    if (arcRead(a, &back) || back != level) return -5;
    return 0;
}

// ---- DMA memory: physically contiguous, below 4 GiB, kernel-cached; flushed by hand around the hand-offs ----
struct Dma {
    OSSharedPtr<IOBufferMemoryDescriptor> desc;
    uint8_t *va = nullptr; uint64_t phys = 0; uint32_t size = 0;
    bool alloc(uint32_t bytes)
    {
        desc = OSSharedPtr<IOBufferMemoryDescriptor>(IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, bytes, 0x00000000fffff000ull), OSNoRetain);
        if (!desc) return false;
        va = static_cast<uint8_t *>(desc->getBytesNoCopy());
        IOByteCount seg = 0;
        phys = desc->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        size = bytes;
        if (!va || !phys || (phys & 0xfff) || seg < bytes || phys + bytes > 0x100000000ull) {desc.reset(); va = nullptr; return false;}
        bzero(va, bytes);
        flush(0, bytes);
        return true;
    }
    void flush(uint32_t off, uint32_t len)
    {
        for (uintptr_t p = (reinterpret_cast<uintptr_t>(va) + off) & ~uintptr_t(63); p < reinterpret_cast<uintptr_t>(va) + off + len; p += 64)
            __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void release() {desc.reset(); va = nullptr; phys = 0; size = 0;}
};
Dma gScm, gMdata;

// ---- SCM: qcom_scm_assign_mem (svc MP 0x0c, cmd 0x16, ARGS(7, RO VAL RO VAL RO VAL VAL)) ----
struct ScmRes {int64_t a0; uint64_t a1;};
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3)
{
    uint64_t first = fn, a6 = 0;
    for (unsigned resume = 0, busy = 0; resume < 10000;) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = p3, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}                       // QCOM_SCM_INTERRUPTED
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IOSleep(30); continue;}   // QCOM_SCM_V2_EBUSY
        return ScmRes{int64_t(x0), x1};
    }
    return ScmRes{-1000, 0};
}
int gLastScm;
int opAssign(void *, uint64_t addr, uint64_t size, uint64_t *cur, const wmss_vmperm *next, unsigned n)
{
    if (!gScm.va && !gScm.alloc(4096)) return -12;
    if (n > 8 || !n) return -22;
    // layout (qcom_scm_assign_mem): the extended-argument array, src vmids, mem_to_map, dest perms, each 64-byte aligned
    uint32_t src[64], ns = 0;
    for (unsigned b = 0; b < 64; ++b) if (*cur >> b & 1) src[ns++] = b;
    const uint32_t kExt = 0, kSrc = 128, kMap = 128 + ((ns * 4 + 63) & ~63u), kDest = kMap + 64, kEnd = kDest + n * 24;
    if (!ns || kEnd > 4096) return -22;
    uint8_t *p = gScm.va;
    bzero(p, 4096);
    memcpy(p + kSrc, src, ns * 4);
    uint64_t map[2] = {addr, size};
    memcpy(p + kMap, map, sizeof map);
    uint64_t nextMask = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t d[6] = {next[i].vmid, next[i].perm, 0, 0, 0, 0};   // vmid, perm, ctx (u64), ctx_size, unused
        memcpy(p + kDest + i * 24, d, 24);
        nextMask |= 1ull << next[i].vmid;
    }
    // args[3..9] go in the extended array: dest-adjacent args are (mem_region_sz, src, src_sz, dest, dest_sz, 0) -> see below
    // Linux: args[0]=mem_region(map) args[1]=map size args[2]=src args[3]=src_sz args[4]=dest args[5]=dest_sz args[6]=0
    uint64_t ext[7] = {ns * 4ull, gScm.phys + kDest, uint64_t(n) * 24, 0, 0, 0, 0};
    memcpy(p + kExt, ext, sizeof ext);
    gScm.flush(0, kEnd);
    const uint64_t arginfo = 7 | (1u << 4) | (0u << 6) | (1u << 8) | (0u << 10) | (1u << 12) | (0u << 14) | (0u << 16);   // RO VAL RO VAL RO VAL VAL
    const ScmRes r = scmCall(0x42000000u | (uint32_t(FW_SCM_SVC_MP) << 8) | FW_SCM_MP_ASSIGN, arginfo, gScm.phys + kMap, 16,
                             gScm.phys + kSrc, gScm.phys + kExt);
    gLastScm = int(r.a0);
    if (r.a0 || r.a1) {
        IOLog("fajita-wifi: FW2 scm assign %llx+%llx %llx->%llx: a0 %lld a1 %llx\n", (unsigned long long)addr, (unsigned long long)size,
              (unsigned long long)*cur, (unsigned long long)nextMask, (long long)r.a0, (unsigned long long)r.a1);
        return -22;
    }
    *cur = nextMask;
    return 0;
}

// ---- carveouts ----
Map gMba, gMpss;
int opRegionWrite(void *, unsigned region, uint64_t off, const void *src, uint32_t len)
{
    if (region == WMSS_REGION_MBA) {
        if (!gMba.va && !mapPhys(FW_MBA_REGION_PHYS, FW_MBA_REGION_SIZE, gMba, kIOMapWriteCombineCache)) return -12;
        if (off + len > FW_MBA_REGION_SIZE) return -22;
        memcpy(reinterpret_cast<void *>(gMba.va + off), src, len);
    } else if (region == WMSS_REGION_MPSS) {
        if (!gMpss.va && !mapPhys(FW_MPSS_REGION_PHYS, FW_MPSS_REGION_SIZE, gMpss, kIOMapWriteCombineCache)) return -12;
        if (off + len > FW_MPSS_REGION_SIZE) return -22;
        memcpy(reinterpret_cast<void *>(gMpss.va + off), src, len);
    } else if (region == WMSS_REGION_MDATA) {
        if (!gMdata.va || off + len > gMdata.size) return -22;
        memcpy(gMdata.va + off, src, len);
        gMdata.flush(uint32_t(off), len);
        return 0;
    } else return -22;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int opRegionZero(void *, unsigned region, uint64_t off, uint64_t len)
{
    if (region != WMSS_REGION_MPSS) return -22;
    if (!gMpss.va && !mapPhys(FW_MPSS_REGION_PHYS, FW_MPSS_REGION_SIZE, gMpss, kIOMapWriteCombineCache)) return -12;
    if (off + len > FW_MPSS_REGION_SIZE) return -22;
    bzero(reinterpret_cast<void *>(gMpss.va + off), size_t(len));
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int opMdataAlloc(void *, uint32_t size, uint64_t *phys)
{
    if (gMdata.va) return -16;
    if (!gMdata.alloc((size + 4095) & ~4095u)) return -12;
    *phys = gMdata.phys;
    return 0;
}
void opMdataFree(void *) {gMdata.release();}

// ---- the other links ----
int opLoadState(void *, int on) {return fajita_wifi_qmp_load_state("modem", on);}
int opFwSize(void *, const char *name, uint32_t *size) {return fajita_wifi_fw_size(name, size);}
long opFwRead(void *, const char *name, uint32_t off, void *dst, uint32_t len) {return fajita_wifi_fw_read(name, off, dst, len);}
void rowAdd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int opSmp2pBits(void *, uint32_t *v)
{
    const int rc = fajita_wifi_smp2p_bits(v);
    static uint64_t last;
    const uint64_t t = nowUs();
    if (t - last > 1000000) {   // once a second while waiting: what the modem has written to SMEM
        last = t;
        char d[150];
        fajita_wifi_smp2p_diag(d, sizeof d);
        IOLog("fajita-wifi: FWM smp2p rc %d bits %x | %s\n", rc, rc ? 0 : *v, d);
    }
    return rc;
}
int opSmp2pSet(void *, uint32_t mask, uint32_t v) {return fajita_wifi_smp2p_set(mask, v);}

void opStep(void *, unsigned index, unsigned step, int rc, uint32_t value, uint32_t us)
{
    IOLog("fajita-wifi: FWM live %u %s rc %d val %x %u us\n", index, wmss_step_name(step), rc, value, us);
}

const wmss_ops kOps = {nullptr, hwRd, hwWr, opNow, opUdelay, opMsleep, opArcGet, opArcSet, opAssign, opLoadState, opFwSize, opFwRead,
                       opRegionWrite, opRegionZero, opMdataAlloc, opMdataFree, opSmp2pBits, opSmp2pSet, opStep};

wmss *gM;
IOLock *gLock;
char gText[4096];
size_t gTextLen;
bool gUp;

void rowAdd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rowAdd(const char *fmt, ...)
{
    char row[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(row, sizeof row, fmt, ap);
    va_end(ap);
    IOLog("fajita-wifi: %s\n", row);
    const size_t n = strlen(row);
    if (gTextLen + n + 2 < sizeof gText) {memcpy(gText + gTextLen, row, n); gTextLen += n; gText[gTextLen++] = '\n'; gText[gTextLen] = 0;}
}

void trace(const wmss *m)
{
    for (unsigned i = 0; i < m->ntrace; ++i) {
        const wmss_step &s = m->trace[i];
        rowAdd("FWM %u %s rc %d val %x %u us", i, wmss_step_name(s.step), s.rc, s.value, s.us);
    }
}
uint32_t stepValue(const wmss *m, unsigned step, bool ok = true)
{
    for (unsigned i = m->ntrace; i--;) if (m->trace[i].step == step && (!ok || !m->trace[i].rc)) return m->trace[i].value;
    return 0;
}

void survey()
{
    wmss_reg regs[64];
    const unsigned n = wmss_survey(gM, regs, 64);
    static const char *const blk[] = {"q6", "rmb", "gcc", "aoss", "pdc", "tcsr"};
    char line[160];
    size_t at = 0;
    line[0] = 0;
    for (unsigned i = 0; i < n; ++i) {
        at += snprintf(line + at, sizeof line - at, " %s+%x=%08x", blk[regs[i].blk], regs[i].off, regs[i].val);
        if (at > 110 || i + 1 == n) {rowAdd("FWM survey%s", line); at = 0; line[0] = 0;}
    }
    rowAdd("FWM survey bad-access %u (reads of unmapped offsets)", gBadAccess);
}

// The WCN3990's rails and RF clock as RPMh resources (cmd-db names; the VRM registers of qcom-rpmh-regulator.c are voltage in mV +0,
// enable +4, mode +8; clk-rpmh.c's VRM clocks enable at +4 with the value 1; a read-back carries bit 31 as a completion flag): read-only here, so a log shows what the firmware left on before anything is voted.
void railsRow(const char *when)
{
    static const char *const kNames[] = {"ldoa5", "ldoa7", "ldoa17", "ldoa25", "ldoa23", "rfclka1", "rfclka2", "rfclka3"};
    if (!fajita_rpmh_lookup || !fajita_rpmh_send) {rowAdd("FWM rails (%s): no RPMh helpers", when); return;}
    char line[200] = "";
    for (const char *name : kNames) {
        uint32_t addr = 0;
        uint8_t aux[16] = {};
        unsigned n = sizeof aux;
        if (fajita_rpmh_lookup(name, &addr, aux, &n) || !addr) {snprintf(line + strlen(line), sizeof line - strlen(line), " %s:-", name); continue;}
        const uint32_t cmd[3][3] = {{kMsgReadResp, addr, 0}, {kMsgReadResp, addr + 4, 0}, {kMsgReadResp, addr + 8, 0}};
        uint32_t resp[3] = {};
        const int t = fajita_rpmh_send(3, cmd, resp);
        snprintf(line + strlen(line), sizeof line - strlen(line), " %s@%x:%s %umV en%u mode%u", name, addr, t < 0 ? "ERR" : "ok", resp[0] & 0x7fffffffu, resp[1] & 0x7fffffffu, resp[2] & 0x7fffffffu);
        if (strlen(line) > 120) {rowAdd("FWM rails (%s)%s", when, line); line[0] = 0;}
    }
    if (line[0]) rowAdd("FWM rails (%s)%s", when, line);
}

// fajita-wifi-rails=1 (experiment): vote the WCN3990's supplies and its RF reference clock on through RPMh, as ath10k_snoc's
// ath10k_hw_power_on does (regulators in the order of ath10k_regulators, then the clock), right after the modem is up. Never
// un-voted. VRM registers: enable +0, voltage in mV +4 (qcom-rpmh-regulator.c); the clock (clk-rpmh.c VRM) enable +0 = 1.
void railsOn()
{
    static const struct {const char *name; uint32_t mv;} kRails[] = {{"ldoa5", 800}, {"ldoa7", 1800}, {"ldoa17", 1304}, {"ldoa25", 3312}, {"ldoa23", 3312}};
    if (!fajita_rpmh_lookup || !fajita_rpmh_send) {rowAdd("FWM rails-on: no RPMh helpers"); return;}
    for (const auto &r : kRails) {
        uint32_t addr = 0;
        uint8_t aux[16] = {};
        unsigned n = sizeof aux;
        if (fajita_rpmh_lookup(r.name, &addr, aux, &n) || !addr) {rowAdd("FWM rails-on: %s not in cmd-db", r.name); continue;}
        const uint32_t cmd[3][3] = {{kMsgWriteResp, addr, r.mv}, {kMsgWriteResp, addr + 4, 1}, {kMsgWriteResp, addr + 8, 7}};
        const int t = fajita_rpmh_send(3, cmd, nullptr);
        rowAdd("FWM rails-on %s %u mV tcs%d", r.name, r.mv, t);
    }
    uint32_t addr = 0;
    uint8_t aux[16] = {};
    unsigned n = sizeof aux;
    if (!fajita_rpmh_lookup("rfclka2", &addr, aux, &n) && addr) {
        const uint32_t cmd[1][3] = {{kMsgWriteResp, addr + 4, 1}};
        rowAdd("FWM rails-on rfclka2 tcs%d", fajita_rpmh_send(1, cmd, nullptr));
    }
    railsRow("after vote");
}

bool setup()
{
    if (!gLock) gLock = IOLockAlloc();
    if (!gM) {
        gM = static_cast<wmss *>(IOMallocData(sizeof(wmss)));
        if (!gM) return false;
        wmss_cfg cfg = {FW_MBA_REGION_PHYS, FW_MBA_REGION_SIZE, FW_MPSS_REGION_PHYS, FW_MPSS_REGION_SIZE, FW_RMTFS_PHYS, FW_RMTFS_SIZE,
                        "image/mba.mbn", "image/modem.mdt", 5000, 10000, 0x100000};
        wmss_init(gM, &kOps, &cfg);
    }
    return mapBlocks();
}

size_t kcmdMss(const char *args, size_t len, char *out, size_t cap)
{
    char a[32] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    if (!gLock || !gM) return snprintf(out, cap, "wifimss: not ready\n");
    IOLockLock(gLock);
    size_t n;
    if (!strncmp(a, "stop", 4)) {
        const int rc = wmss_stop(gM);
        fajita_wifi_msa_release();
        rowAdd("FWM stop rc %d", rc);
        trace(gM);
        n = snprintf(out, cap, "wifimss stop rc %d\n", rc);
    } else if (!strncmp(a, "survey", 6)) {
        if (!gM->running) {gTextLen = 0; gText[0] = 0; survey();}
        n = snprintf(out, cap, "%s", gText);
    } else if (!strncmp(a, "poll", 4)) {
        n = snprintf(out, cap, "wifimss: handover %d running %d proxy %d\n", wmss_poll_handover(gM), gM->running, gM->proxy_held);
    } else {
        n = snprintf(out, cap, "%s%s", gText, gTextLen ? "" : "wifimss: no boot this run (fajita-wifi>=2)\nusage: wifimss [rows|survey|stop|poll]\n");
    }
    IOLockUnlock(gLock);
    return n < cap ? n : cap;
}
}  // namespace

// The fajita-wifi worker, fajita-wifi>=2, after the memory reservation check. 0: the modem is up (SMP2P ready).
extern "C" int fajita_wifi_modem(void)
{
    if (!setup()) {rowAdd("FW2 modem: register windows or state could not be set up"); return -12;}
    if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("wifimss", kcmdMss, "Wi-Fi modem boot: wifimss [rows|survey|stop|poll]");
    unsigned dry = 0;
    PE_parse_boot_argn("fajita-wifi-mss-dry", &dry, sizeof dry);
    gTextLen = 0; gText[0] = 0;
    IOLockLock(gLock);
    int rc;
    if (dry) {
        railsRow("dry");
        survey();
        rowAdd("FW2 modem: dry run (fajita-wifi-mss-dry=1): registers read, nothing written, modem not started");
        strlcpy(fajita_wifi_rows[2], "FW2 modem dry run: registers read only (see FWM survey rows)", sizeof fajita_wifi_rows[2]);
        IOLockUnlock(gLock);
        return -1;
    }
    railsRow("before boot");
    survey();           // the state before: whether a reset is held, which clocks are already on, and that every window answers
    const uint64_t t0 = nowUs();
    rc = wmss_boot(gM);
    const unsigned ms = unsigned((nowUs() - t0) / 1000);
    trace(gM);
    char row[160];
    if (!rc) {
        snprintf(row, sizeof row, "FW2 modem up: pbl%u mba%u meta%u seg%u auth%u ready1 handover%u (%u ms)", stepValue(gM, WMSS_S_Q6_RESET),
                 stepValue(gM, WMSS_S_MBA_WAIT), stepValue(gM, WMSS_S_META_AUTH), stepValue(gM, WMSS_S_SEGMENTS), stepValue(gM, WMSS_S_AUTH),
                 !gM->proxy_held, ms);
        gUp = true;
        fajita_wifi_state[2] = 1;
    } else {
        const wmss_step &f = gM->trace[gM->ntrace ? gM->ntrace - 1 : 0];
        snprintf(row, sizeof row, "FW2 modem FAILED at %s rc %d (last step %s rc %d val %x), unwound (%u ms)",
                 wmss_step_name(unsigned(gM->failed_step)), rc, wmss_step_name(f.step), f.rc, f.value, ms);
        fajita_wifi_state[3] = uint32_t(rc);
    }
    rowAdd("%s", row);
    strlcpy(fajita_wifi_rows[2], row, sizeof fajita_wifi_rows[2]);
    {
        unsigned rails = 0;
        PE_parse_boot_argn("fajita-wifi-rails", &rails, sizeof rails);
        if (rails && !rc) railsOn();
    }
    // No register survey while the modem runs: the QDSP6SS/RMB windows are not safe to read once the MBA has locked the XPUs
    // (the first successful boot ended in an unhandled SError right after this row). After a failure the modem is in reset.
    if (rc) survey();
    IOLockUnlock(gLock);
    return rc;
}

// Called from the stage-3 loop about once a second: the late SMP2P handover bit releases the proxy votes.
extern "C" void fajita_wifi_modem_poll(void)
{
    if (!gM || !gLock || !gUp) return;
    IOLockLock(gLock);
    if (gM->proxy_held && wmss_poll_handover(gM) == 1) rowAdd("FWM handover seen, proxy votes released");
    IOLockUnlock(gLock);
}

// ---- the wlfw MSA regions (ath10k_qmi_setup_msa_permissions / remove_msa_permission) ----
namespace {
struct Msa {uint64_t addr, size; bool secure; bool assigned;};
Msa gMsa[WLFW_MAX_MEM_REG];
unsigned gNMsa;
}

extern "C" void fajita_wifi_msa_release(void)
{
    for (unsigned i = gNMsa; i--;) {
        Msa &r = gMsa[i];
        if (!r.assigned) continue;
        uint64_t cur = (1ull << WMSS_VMID_MSS_MSA) | (1ull << FW_SCM_VMID_WLAN) | (r.secure ? 0 : 1ull << FW_SCM_VMID_WLAN_CE);
        const wmss_vmperm hlos = {WMSS_VMID_HLOS, WMSS_PERM_RW};
        if (!opAssign(nullptr, r.addr, r.size, &cur, &hlos, 1)) r.assigned = false;
        else IOLog("fajita-wifi: FW5 msa region %llx+%llx not returned to HLOS\n", (unsigned long long)r.addr, (unsigned long long)r.size);
    }
}

extern "C" int fajita_wifi_msa_assign(const wlfw_mem_region *regions, unsigned count)
{
    if (count > WLFW_MAX_MEM_REG) return -22;
    fajita_wifi_msa_release();      // a repeat of the handshake (modem restart): start from HLOS
    gNMsa = 0;
    for (unsigned i = 0; i < count; ++i) {
        gMsa[i] = {regions[i].addr, regions[i].size, regions[i].secure != 0, false};
        gNMsa = i + 1;
        uint64_t cur = 1ull << WMSS_VMID_HLOS;
        wmss_vmperm dst[3] = {{WMSS_VMID_MSS_MSA, WMSS_PERM_RW}, {FW_SCM_VMID_WLAN, WMSS_PERM_RW}, {FW_SCM_VMID_WLAN_CE, WMSS_PERM_RW}};
        const int rc = opAssign(nullptr, regions[i].addr, regions[i].size, &cur, dst, regions[i].secure ? 2 : 3);
        if (rc) {
            IOLog("fajita-wifi: FW5 msa assign region %u (%llx+%x) failed %d\n", i, (unsigned long long)regions[i].addr, regions[i].size, rc);
            fajita_wifi_msa_release();       // err_unmap: the ones done so far go back
            return rc;
        }
        gMsa[i].assigned = true;
    }
    return 0;
}
