// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita Wi-Fi stage 6/7: the WCN3990 device above wlfw FW_READY (drivers/FajitaWifi/WifiDev.c, host-tested), with the
// hardware glue only:
//   - the WLAN membase (0x18800000) mapped as device memory: the CE registers and shadow write indices;
//   - the DMA arena: FW_WLAN_DMA_PHYS, ordinary RAM held off XNU's free lists (phys-reserve), mapped uncached, IOVA = PA;
//   - apps_smmu: SIDs 0x40/0x41 (one SMR, id 0x40 mask 1) on a free context bank in bypass, the recipe NativeDwc3 uses for
//     its stream 0x740 (SCTLR 0, CBA2R VA64, CBAR S1-trans/S2-bypass, S2CR -> bank, then the SMR).
// Called from the fajita-wifi stack loop (FajitaWifiStack.cpp, one thread): fajita_wifi_dev_start once wlfw is ENABLED,
// fajita_wifi_dev_poll every iteration. Rows: FW6 (bring-up), FW7 (scan). kcmd wifidev [state|scan [SSID]|bss].
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/kauth.h>

#include "FajitaWifiBoard.h"
extern "C" {
#include "WifiDev.h"
#include "WifiSmmu.h"
extern char fajita_wifi_rows[12][160];
extern unsigned int phys_reserve_count __attribute__((weak_import));
extern uint64_t phys_reserve_base[8] __attribute__((weak_import)), phys_reserve_size[8] __attribute__((weak_import));
extern unsigned int phys_reserve_held[8] __attribute__((weak_import));
extern uint64_t phys_reserve_window[3] __attribute__((weak_import));
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
int fajita_wifi_dev_start(void);
void fajita_wifi_dev_poll(void);
void fajita_wifi_dev_kcmd(void);
void read_random(void *buffer, unsigned int numBytes);
// FajitaNet.cpp (fajita-net=wifi): the BSD en0 over this station until the IO80211 interface exists
int fajita_net_input(const void *frame, size_t len);
void fajita_net_set_backend(int (*send)(void *ctx, const void *frame, size_t len), void *ctx);
void fajita_net_set_mac(const uint8_t mac[6]);
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
// cache: kIOMapInhibitCache = Device-nGnRE (registers); kIOMapWriteCombineCache = Normal non-cacheable (the DMA arena: memcpy,
// bzero's DC ZVA and unaligned accesses fault on Device memory).
bool mapRegion(uint64_t phys, uint64_t length, Region &region, IOOptionBits cache = kIOMapInhibitCache)
{
    if (region.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | cache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint64_t nowMs64() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}

Region gMem, gArena, gSmmu;
wdev gDev;
bool gStarted;
IOLock *gLock;              // the kcmd side vs the poll loop
char gText[2048];           // FW6 step history (kcmd wifidev state)
size_t gTextLen;
int gSmmuSlot = -1, gSmmuBank = -1;
volatile int gReqScan;      // kcmd -> poll loop
char gReqSsid[33];
unsigned gScans, gLastLink, gLinkReason;
unsigned gStage, gConnects;   // fajita-wifi=N: 7 connects to fajita-wifi-ssid with the pushed passphrase
uint64_t gConnectAt;
int gConnectRc = 1;
uint64_t gScanAt;

void row(unsigned i, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void row(unsigned i, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strcmp(buf, fajita_wifi_rows[i])) {
        strlcpy(fajita_wifi_rows[i], buf, sizeof fajita_wifi_rows[i]);
        IOLog("fajita-wifi: %s\n", buf);
    }
}
void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    IOLog("fajita-wifi: %s\n", buf);
    if (gTextLen + strlen(buf) + 2 < sizeof gText) gTextLen += snprintf(gText + gTextLen, sizeof gText - gTextLen, "%s\n", buf);
}

// ---- the arena must be held by phys-reserve (or XNU could hand its pages out) ----
bool arenaHeld()
{
    if (!&phys_reserve_count || !&phys_reserve_base) return false;
    for (unsigned i = 0; i < phys_reserve_count && i < 8; ++i) {
        if (FW_WLAN_DMA_PHYS < phys_reserve_base[i] || FW_WLAN_DMA_PHYS + FW_WLAN_DMA_SIZE > phys_reserve_base[i] + phys_reserve_size[i]) continue;
        const uint64_t page = PAGE_SIZE, first = phys_reserve_base[i] & ~(page - 1);
        const uint64_t pages = ((phys_reserve_base[i] + phys_reserve_size[i] + page - 1) & ~(page - 1)) / page - first / page;
        return phys_reserve_held[i] == pages;
    }
    // entirely outside XNU's managed window is fine too
    return &phys_reserve_window && (FW_WLAN_DMA_PHYS >= phys_reserve_window[2] || FW_WLAN_DMA_PHYS + FW_WLAN_DMA_SIZE <= phys_reserve_window[0]);
}

// ---- apps_smmu: SIDs 0x40/0x41 to a bypass context bank (ARM SMMUv2 layout; Dwc3GadgetRun.h smmuMap740) ----
int smmuBypassWlan()
{
    if (!mapRegion(FW_APPS_SMMU_PHYS, FW_APPS_SMMU_SIZE, gSmmu)) return -1;
    const uintptr_t smmu = gSmmu.va;
    auto rd = [smmu](unsigned off) -> uint32_t {return *reinterpret_cast<volatile uint32_t *>(smmu + off);};
    auto wr = [smmu](unsigned off, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(smmu + off) = v; __asm__ volatile("dsb sy" ::: "memory");};
    const uint32_t idr0 = rd(0x20), idr1 = rd(0x24);
    unsigned numSmr = idr0 & 0xff, banks = idr1 & 0xff;
    const unsigned pages = 1u << (((idr1 >> 28) & 7) + 1);
    if (numSmr > 128) numSmr = 128;
    if (!numSmr || banks < 2 || (pages + banks) * 4096ull > FW_APPS_SMMU_SIZE) return -2;
    const uint32_t want = 0x80000000u | (FW_WLAN_SMMU_MASK << 16) | FW_WLAN_SMMU_SID;
    unsigned freeSlot = ~0u, ourSlot = ~0u, conflicts = 0;
    bool bankUsed[128] = {false};
    for (unsigned i = 0; i < numSmr; ++i) {
        const uint32_t smr = rd(0x800 + 4 * i);
        if (!(smr & 0x80000000u)) {if (freeSlot == ~0u) freeSlot = i; continue;}
        const uint32_t s2cr = rd(0xc00 + 4 * i);
        if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < 128) bankUsed[s2cr & 0xff] = true;
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        if (smr == want) {ourSlot = i; continue;}
        // another group matching 0x40 or 0x41 would be a stream-match conflict
        for (uint32_t sid = FW_WLAN_SMMU_SID; sid <= (FW_WLAN_SMMU_SID | FW_WLAN_SMMU_MASK); ++sid)
            if (((sid ^ id) & ~mask & 0x7fff) == 0) {++conflicts; note("FW6 smmu: SMR %u (%08x, s2cr %08x) already matches sid %x", i, smr, s2cr, sid);}
    }
    if (conflicts) return -3;
    const unsigned slot = ourSlot != ~0u ? ourSlot : freeSlot;
    if (slot == ~0u) return -4;
    unsigned bank = ~0u;
    if (ourSlot != ~0u) bank = rd(0xc00 + 4 * ourSlot) & 0xff;
    else
        for (unsigned b = 0; b < banks && b < 128; ++b) {
            if (bankUsed[b] || (rd((pages + b) * 4096) & 1)) continue;   // pointed to, or SCTLR.M set (live translation)
            bank = b;
            break;
        }
    if (bank == ~0u) return -5;
    wr((pages + bank) * 4096, 0);       // SCTLR = 0: translation off (passthrough)
    wr(0x1800 + 4 * bank, 1);           // CBA2R.VA64
    wr(0x1000 + 4 * bank, 0x10000);     // CBAR TYPE = S1 translation, S2 bypass
    wr(0xc00 + 4 * slot, bank);         // S2CR: translation context, CBNDX = bank
    wr(0x800 + 4 * slot, want);         // SMR: valid, id 0x40, mask 1
    if (rd(0x800 + 4 * slot) != want || (rd(0xc00 + 4 * slot) & 0xff) != bank || (rd((pages + bank) * 4096) & 1)) return -6;
    gSmmuSlot = int(slot); gSmmuBank = int(bank);
    return 0;
}
// fajita-wifi-smmu=xlate: SIDs 0x40 and 0x41 (two exact SMRs) on a free context bank that translates through WifiSmmu's stage-1
// tables, which map only the DMA arena (IOVA = PA); anything else faults (CB_FSR) instead of reaching memory. The tables sit in
// their own contiguous buffer outside the arena, cleaned to PoC (non-cacheable walks). Order: tables, TLB invalidate, bank
// registers with SCTLR.M last, S2CR, then the SMRs (NOTES-wifi.md, apps_smmu design).
OSSharedPtr<IOBufferMemoryDescriptor> gSmmuTables;
int gSmmuSlot2 = -1, gSmmuXlate;
uint32_t gSmmuFar, gSmmuFsrSeen;
int smmuTranslateWlan()
{
    if (!mapRegion(FW_APPS_SMMU_PHYS, FW_APPS_SMMU_SIZE, gSmmu)) return -1;
    const uintptr_t smmu = gSmmu.va;
    auto rd = [smmu](unsigned off) -> uint32_t {return *reinterpret_cast<volatile uint32_t *>(smmu + off);};
    auto wr = [smmu](unsigned off, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(smmu + off) = v; __asm__ volatile("dsb sy" ::: "memory");};
    const uint32_t idr0 = rd(0x20), idr1 = rd(0x24), idr2 = rd(0x28);
    if (!(idr0 & (1u << 30)) || !(idr2 & (1u << 12))) return -7;   // no stage 1 / no AArch64 4 KiB granule
    unsigned numSmr = idr0 & 0xff, banks = idr1 & 0xff;
    const unsigned pages = 1u << (((idr1 >> 28) & 7) + 1);
    if (numSmr > 128) numSmr = 128;
    if (!numSmr || banks < 2 || (pages + banks) * 4096ull > FW_APPS_SMMU_SIZE) return -2;
    unsigned freeSlot[2] = {~0u, ~0u}, nfree = 0;
    bool bankUsed[128] = {false};
    for (unsigned i = 0; i < numSmr; ++i) {
        const uint32_t smr = rd(0x800 + 4 * i);
        if (!(smr & 0x80000000u)) {if (nfree < 2) freeSlot[nfree++] = i; continue;}
        const uint32_t s2cr = rd(0xc00 + 4 * i);
        if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < 128) bankUsed[s2cr & 0xff] = true;
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        for (uint32_t sid = FW_WLAN_SMMU_SID; sid <= (FW_WLAN_SMMU_SID | FW_WLAN_SMMU_MASK); ++sid)
            if (((sid ^ id) & ~mask & 0x7fff) == 0) {note("FW6 smmu: SMR %u (%08x) already matches sid %x", i, smr, sid); return -3;}
    }
    if (nfree < 2) return -4;
    unsigned bank = ~0u;
    for (unsigned b = 0; b < banks && b < 128; ++b) {
        if (bankUsed[b] || (rd((pages + b) * 4096) & 1)) continue;
        bank = b;
        break;
    }
    if (bank == ~0u) return -5;
    // the tables: contiguous, below 4 GiB, outside the arena
    gSmmuTables = OSSharedPtr<IOBufferMemoryDescriptor>(IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, WSMMU_TABLE_BYTES, 0x00000000fffff000ull),
        OSNoRetain);
    if (!gSmmuTables) return -8;
    uint8_t *tva = static_cast<uint8_t *>(gSmmuTables->getBytesNoCopy());
    IOByteCount seg = 0;
    const uint64_t tpa = gSmmuTables->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!tva || !tpa || seg < WSMMU_TABLE_BYTES || (tpa & 0xfff)) return -8;
    wsmmu_ctx c;
    if (wsmmu_build(tva, tpa, FW_WLAN_DMA_PHYS, FW_WLAN_DMA_SIZE, &c)) return -9;
    for (uintptr_t p = reinterpret_cast<uintptr_t>(tva); p < reinterpret_cast<uintptr_t>(tva) + WSMMU_TABLE_BYTES; p += 64)
        __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");   // the SMMU walks non-cacheable: the tables must be in memory
    __asm__ volatile("dsb sy" ::: "memory");
    const unsigned cb = (pages + bank) * 4096;
    wr(cb + WSMMU_CB_SCTLR, 0);
    wr(0x1000 + 4 * bank, c.cbar);
    wr(0x1800 + 4 * bank, c.cba2r);
    wr(cb + WSMMU_CB_TCR2, c.tcr2);
    wr(cb + WSMMU_CB_TCR, c.tcr);
    wr(cb + WSMMU_CB_MAIR0, c.mair0);
    wr(cb + WSMMU_CB_MAIR1, c.mair1);
    wr(cb + WSMMU_CB_TTBR0, static_cast<uint32_t>(c.ttbr0));
    wr(cb + WSMMU_CB_TTBR0 + 4, static_cast<uint32_t>(c.ttbr0 >> 32));
    wr(cb + WSMMU_CB_FSR, rd(cb + WSMMU_CB_FSR));   // clear anything stale (write 1 to clear)
    wr(cb + WSMMU_CB_TLBIALL, 0);
    wr(cb + WSMMU_CB_TLBSYNC, 0);
    for (unsigned i = 0; i < 1000 && (rd(cb + WSMMU_CB_TLBSTATUS) & 1); ++i) IODelay(1);
    wr(cb + WSMMU_CB_SCTLR, c.sctlr);
    for (unsigned k = 0; k < 2; ++k) wr(0xc00 + 4 * freeSlot[k], bank);   // S2CR: translation context, CBNDX = bank
    wr(0x800 + 4 * freeSlot[0], 0x80000000u | FW_WLAN_SMMU_SID);           // exact SMRs, mask 0
    wr(0x800 + 4 * freeSlot[1], 0x80000000u | (FW_WLAN_SMMU_SID + 1));
    if (rd(0x800 + 4 * freeSlot[0]) != (0x80000000u | FW_WLAN_SMMU_SID) || (rd(0xc00 + 4 * freeSlot[0]) & 0xff) != bank ||
        !(rd(cb + WSMMU_CB_SCTLR) & 1) || rd(cb + WSMMU_CB_TTBR0) != static_cast<uint32_t>(c.ttbr0)) return -6;
    gSmmuSlot = int(freeSlot[0]); gSmmuSlot2 = int(freeSlot[1]); gSmmuBank = int(bank); gSmmuXlate = 1;
    note("FW6 smmu xlate: bank %u slots %u/%u tables %llx (%u x 2 MiB blocks at %llx) tcr %x tcr2 %x sctlr %x", bank, freeSlot[0],
         freeSlot[1], tpa, c.blocks, FW_WLAN_DMA_PHYS, c.tcr, c.tcr2, c.sctlr);
    return 0;
}

// After traffic: any fault recorded on our bank (CB_FSR at +0x58) or globally (sGFSR 0x48).
uint32_t smmuFaults()
{
    if (!gSmmu.va || gSmmuBank < 0) return 0;
    const uintptr_t smmu = gSmmu.va;
    const uint32_t idr1 = *reinterpret_cast<volatile uint32_t *>(smmu + 0x24);
    const unsigned pages = 1u << (((idr1 >> 28) & 7) + 1);
    const uintptr_t cb = smmu + (pages + unsigned(gSmmuBank)) * 4096;
    const uint32_t fsr = *reinterpret_cast<volatile uint32_t *>(cb + WSMMU_CB_FSR) & WSMMU_FSR_FAULTS;   // FORMAT (10:9) is not a fault
    if (fsr && fsr != gSmmuFsrSeen) {
        gSmmuFsrSeen = fsr;
        gSmmuFar = *reinterpret_cast<volatile uint32_t *>(cb + WSMMU_CB_FAR);
        note("FW6 SMMU FAULT fsr %x far %x:%08x fsynr0 %x", fsr, *reinterpret_cast<volatile uint32_t *>(cb + WSMMU_CB_FAR + 4), gSmmuFar,
             *reinterpret_cast<volatile uint32_t *>(cb + WSMMU_CB_FSYNR0));
    }
    return fsr;
}

uint32_t ioRdRaw(uint32_t off) {return *reinterpret_cast<volatile uint32_t *>(gMem.va + off);}
// The passphrase pushed by wifi-test.sh (never in a boot argument, never logged): /private/var/ngpu/wifi.psk, one line.
int readPsk(char *out, size_t cap)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    if (vnode_open("/private/var/ngpu/wifi.psk", FREAD, 0, 0, &vp, ctx) || !vp) return -2;
    int resid = 0;
    const int err = vn_rdwr(UIO_READ, vp, (caddr_t)out, (int)cap - 1, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                            vfs_context_proc(ctx));
    vnode_close(vp, FREAD, ctx);
    if (err) {bzero(out, cap); return -5;}
    size_t n = cap - 1 - (size_t)resid;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) --n;
    out[n] = 0;
    return n ? int(n) : -22;
}
// ---- wdev ops ----
uint32_t ioRd(void *, uint32_t off) {return *reinterpret_cast<volatile uint32_t *>(gMem.va + off);}
void ioWr(void *, uint32_t off, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(gMem.va + off) = v;}
void ioBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory");}
uint32_t devNow(void *) {return static_cast<uint32_t>(nowMs64());}
void devRandom(void *, uint8_t *out, uint32_t n) {read_random(out, n);}
// the last frames each way (dst, src, ethertype, length; for kcmd wifidev frames)
struct FrameNote {uint8_t dst[6], src[6]; uint16_t type; uint16_t len; uint8_t ip_proto; uint16_t udp_dport;};
FrameNote gRxNotes[8], gTxNotes[8];
unsigned gRxNoteN, gTxNoteN;
void noteFrame(FrameNote *ring, unsigned &n, const uint8_t *eth, uint32_t len)
{
    FrameNote &f = ring[n++ % 8];
    memcpy(f.dst, eth, 6); memcpy(f.src, eth + 6, 6);
    f.type = uint16_t(eth[12] << 8 | eth[13]); f.len = uint16_t(len);
    f.ip_proto = 0; f.udp_dport = 0;
    if (f.type == 0x0800 && len >= 14 + 20) {
        f.ip_proto = eth[14 + 9];
        const unsigned ihl = (eth[14] & 15u) * 4u;
        if (f.ip_proto == 17 && len >= 14 + ihl + 4) f.udp_dport = uint16_t(eth[14 + ihl + 2] << 8 | eth[14 + ihl + 3]);
    }
}
uint32_t gRxUnicast, gRxDhcp, gRxArp;
void devRx(void *, const uint8_t *eth, uint32_t len)   // poll thread, gLock held
{
    if (!memcmp(eth, gDev.mac, 6)) ++gRxUnicast;
    if (len >= 14 && eth[12] == 0x08 && eth[13] == 0x06) ++gRxArp;
    FrameNote tmp[1]; unsigned k = 0;
    noteFrame(tmp, k, eth, len);
    if (tmp[0].udp_dport == 68) ++gRxDhcp;
    if (!(eth[0] & 1) || tmp[0].udp_dport == 68 || gRxNoteN < 4) noteFrame(gRxNotes, gRxNoteN, eth, len);   // keep the interesting ones
    fajita_net_input(eth, len);
}
uint32_t gTxOk, gTxErr;
// en0's start routine (its own thread): one Ethernet frame to the AP
int wifiSend(void *, const void *frame, size_t len)
{
    if (!gLock) return -1;
    IOLockLock(gLock);
    if (len >= 14) noteFrame(gTxNotes, gTxNoteN, static_cast<const uint8_t *>(frame), static_cast<uint32_t>(len));
    const int r = wdev_tx(&gDev, static_cast<const uint8_t *>(frame), static_cast<uint32_t>(len));
    IOLockUnlock(gLock);
    if (r) ++gTxErr; else ++gTxOk;
    return r ? -3 : 0;
}
void devLink(void *, unsigned st, unsigned reason)
{
    gLastLink = st; gLinkReason = reason;
    note("FW7 mlme %s reason %u", wmlme_state_name(st), reason);
    if (st == WMLME_CONNECTED) {fajita_net_set_mac(gDev.mac); fajita_net_set_backend(wifiSend, nullptr);}   // fajita-net=wifi: en0 attaches
    else if (st == WMLME_FAILED || st == WMLME_IDLE) fajita_net_set_backend(nullptr, nullptr);
}
void devLog(void *, const char *step, int rc, uint32_t val) {note("FW6 %s rc %d t+%u ms", step, rc, val);}
void *devAlloc(size_t n, void *) {void *p = IOMalloc(n ? n : 1); if (p) bzero(p, n ? n : 1); return p;}
void devFree(void *p, size_t n, void *) {if (p) IOFree(p, n ? n : 1);}

void scanRow()
{
    const wmlme &m = gDev.mlme;
    char best[96] = "-";
    if (gReqSsid[0]) {
        const wmlme_bss *b = m.state >= WMLME_VDEV_START && m.state <= WMLME_CONNECTED ? &m.target : wmlme_find(&m, gReqSsid);   // joining: that BSS
        if (b) snprintf(best, sizeof best, "%s %02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm%s bcn %u prsp %u", gReqSsid, b->bssid[0], b->bssid[1],
                        b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5], b->freq, b->rssi,
                        !b->rsn_ok ? " (rsn?)" : b->akm == WWPA_AKM_SAE ? " wpa3-sae" : " wpa2-psk", b->n_beacons, b->n_probe_resps);
        else snprintf(best, sizeof best, "%s NOT SEEN", gReqSsid);
        static unsigned rsnShown;
        if (b && !b->rsn_ok && rsnShown != gScans && m.state == WMLME_SCAN_DONE) {   // once per scan: why the RSN element is not usable
            rsnShown = gScans;
            char hex[3 * WMLME_RSN_MAX + 1] = "";
            for (unsigned k = 0; k < b->rsn_len; ++k) snprintf(hex + 3 * k, sizeof hex - 3 * k, " %02x", b->rsn[k]);
            note("FW7 %s rsn (%u bytes):%s", gReqSsid, b->rsn_len, b->rsn_len ? hex : " none");
        }
    }
    unsigned n2 = 0, n5 = 0;
    if (gReqSsid[0]) wmlme_count(&m, gReqSsid, &n2, &n5);
    row(9, "FW7 scan #%u %s bss %u frames %u full %u: %s (bss 2g %u 5g %u)", gScans, wmlme_state_name(m.state), m.nbss, m.scan_frames, m.scan_full,
        best, n2, n5);
}
}  // namespace

extern "C" int fajita_wifi_dev_start(void)
{
    if (gStarted) return 0;
    gStarted = true;
    if (!gLock) gLock = IOLockAlloc();
    if (!arenaHeld()) {row(8, "FW6 arena %llx+%llx not held (phys-reserve)", FW_WLAN_DMA_PHYS, FW_WLAN_DMA_SIZE); return -1;}
    if (!mapRegion(FW_WLAN_MEMBASE, FW_WLAN_MEMBASE_SIZE, gMem) || !mapRegion(FW_WLAN_DMA_PHYS, FW_WLAN_DMA_SIZE, gArena, kIOMapWriteCombineCache)) {
        row(8, "FW6 map failed (membase %d arena %d)", gMem.va != 0, gArena.va != 0);
        return -2;
    }
    char mode[8] = "xlate";   // fajita-wifi-smmu=xlate (default: stage-1 tables over the arena only) | bypass
    PE_parse_boot_arg_str("fajita-wifi-smmu", mode, sizeof mode);
    const int s = !strcmp(mode, "xlate") ? smmuTranslateWlan() : smmuBypassWlan();
    note("FW6 smmu %s sid %x/%x rc %d slot %d/%d bank %d", gSmmuXlate ? "xlate" : "bypass", FW_WLAN_SMMU_SID, FW_WLAN_SMMU_MASK, s, gSmmuSlot,
         gSmmuSlot2, gSmmuBank);
    if (s) {row(8, "FW6 smmu FAILED rc %d", s); return -3;}
    note("FW6 arena %llx+%llx at %lx (normal non-cacheable), membase at %lx", FW_WLAN_DMA_PHYS, FW_WLAN_DMA_SIZE, (unsigned long)gArena.va, (unsigned long)gMem.va);
    bzero(reinterpret_cast<void *>(gArena.va), FW_WLAN_DMA_SIZE);
    __asm__ volatile("dsb sy" ::: "memory");
    note("FW6 arena zeroed; CE0 sr_size %x ctrl1 %x intr %x", ioRdRaw(0x240000 + 0x08), ioRdRaw(0x240000 + 0x18), ioRdRaw(0x24c000));
    // the firmware's READY carries no MAC on this phone: a locally administered one (FajitaNet's USB en0 is ...:02)
    wdev_ops ops = {nullptr, devNow, devRandom, devRx, devLink, devLog, devAlloc, devFree, {0x02, 0xfa, 0x6a, 0x17, 0x4e, 0x03}};
    {   // fajita-wifi-mac=<12 hex>: the phone's own MAC (persist:/wlan_mac.bin, tools/wifi/persist-wlan-mac.py)
        char m[20] = {0};
        uint8_t v[6];
        unsigned k = 0;
        if (PE_parse_boot_arg_str("fajita-wifi-mac", m, sizeof m)) {
            for (const char *q = m; *q && k < 12; ++q) {
                const int h = *q >= '0' && *q <= '9' ? *q - '0' : *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 : *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
                if (h < 0) continue;   // separators
                if (k & 1) v[k / 2] = uint8_t(v[k / 2] << 4 | h); else v[k / 2] = uint8_t(h);
                ++k;
            }
            if (k == 12 && !(v[0] & 1) && (v[0] | v[1] | v[2] | v[3] | v[4] | v[5])) {memcpy(ops.mac, v, 6); note("FW6 mac from fajita-wifi-mac (persist)");}
            else note("FW6 fajita-wifi-mac '%s' not a unicast MAC: using the LAA", m);
        }
    }
    const wce_io io = {nullptr, ioRd, ioWr, ioBarrier};
    IOLockLock(gLock);
    const int r = wdev_start(&gDev, &ops, &io, reinterpret_cast<void *>(gArena.va), FW_WLAN_DMA_PHYS, static_cast<uint32_t>(FW_WLAN_DMA_SIZE));
    IOLockUnlock(gLock);
    note("FW6 start rc %d ce intr %08x", r, wce_intr_summary(&gDev.ce));
    if (r) {row(8, "FW6 wdev_start FAILED rc %d", r); return -4;}
    fajita_net_set_mac(ops.mac);   // fajita-net=wifi: en0 attaches now with this MAC (link down until associated)
    PE_parse_boot_argn("fajita-wifi", &gStage, sizeof gStage);
    {unsigned prot = 1; PE_parse_boot_argn("fajita-wifi-txprot", &prot, sizeof prot); gDev.tx_unprotected = !prot;}
    {   // fajita-wifi-band=strongest|auto|2g|5g: which BSS of the SSID to join (WifiMlme.h WMLME_BAND_*)
        // Default auto: KittyCorner's 2.4 GHz BSS band-steers this phone (it ignores the first SAE commits once the phone has been on
        // 5 GHz; NOTES-wifi.md "Band steering"), so join 5 GHz when its signal is good, as a supplicant does.
        char band[16] = {0};
        gDev.band_pref = WMLME_BAND_AUTO;
        if (PE_parse_boot_arg_str("fajita-wifi-band", band, sizeof band)) {
            gDev.band_pref = !strcmp(band, "strongest") ? WMLME_BAND_STRONGEST : !strcmp(band, "2g") ? WMLME_BAND_2G
                           : !strcmp(band, "5g") ? WMLME_BAND_5G : WMLME_BAND_AUTO;
        }
        char sq[16] = {0};
        gDev.mgmt_seq_random = PE_parse_boot_arg_str("fajita-wifi-mgmt-seq", sq, sizeof sq) && !strcmp(sq, "random");
        note("FW7 management sequence numbers from %s", gDev.mgmt_seq_random ? "a random start" : "0");
        note("FW7 band preference %s", gDev.band_pref == WMLME_BAND_AUTO ? "auto (5 GHz when good)" : gDev.band_pref == WMLME_BAND_2G ? "2g"
                                      : gDev.band_pref == WMLME_BAND_5G ? "5g" : "strongest");
    }
    {   // fajita-wifi-ssid: scan for it as soon as the device runs
        char ssid[33] = {0};
        if (PE_parse_boot_arg_str("fajita-wifi-ssid", ssid, sizeof ssid) && ssid[0]) {strlcpy(gReqSsid, ssid, sizeof gReqSsid); gReqScan = 1;}
    }
    return 0;
}

extern "C" void fajita_wifi_dev_poll(void)
{
    if (!gStarted || !gLock) return;
    IOLockLock(gLock);
    wdev_poll(&gDev);
    // fajita-wifi>=7: join the target once the scan has seen it (3 tries, 10 s apart; a failed try rescans first)
    if (gStage >= 7 && gDev.state == WDEV_RUNNING && gReqSsid[0] && gConnects < 3 && nowMs64() - gConnectAt > 10000 &&
        (gDev.mlme.state == WMLME_SCAN_DONE || gDev.mlme.state == WMLME_FAILED)) {
        if (gDev.mlme.state == WMLME_FAILED && gConnects) {gReqScan = 1; gConnectAt = nowMs64() - 5000;}   // rescan, then retry
        else if (wmlme_find(&gDev.mlme, gReqSsid) && gDev.mlme.state == WMLME_SCAN_DONE && !gConnects && gScans == 1 &&
                 gDev.band_pref == WMLME_BAND_AUTO && [] {unsigned a, b; wmlme_count(&gDev.mlme, gReqSsid, &a, &b); return b == 0;}()) {
            note("FW7 %s only on 2.4 GHz in scan #1: scanning again for its 5 GHz BSS", gReqSsid);   // the 5 GHz beacon/probe was missed
            gReqScan = 1;
            gConnectAt = nowMs64() - 10000;
        } else if (wmlme_find(&gDev.mlme, gReqSsid)) {
            char psk[128];
            const int pl = readPsk(psk, sizeof psk);
            if (pl > 0) {
                ++gConnects;
                gConnectAt = nowMs64();
                gConnectRc = wdev_connect(&gDev, gReqSsid, psk);
                const wmlme_bss &tb = gDev.mlme.target;
                unsigned n2 = 0, n5 = 0;
                wmlme_count(&gDev.mlme, gReqSsid, &n2, &n5);
                note("FW7 connect #%u %s (psk %d chars, akm %u) rc %d: %02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm (bss 2g %u 5g %u) rsn tail %02x%02x rsnxe %u/%02x sel123 %u rates %u seq %u",
                     gConnects, gReqSsid, pl, tb.akm, gConnectRc, tb.bssid[0], tb.bssid[1], tb.bssid[2], tb.bssid[3], tb.bssid[4], tb.bssid[5], tb.freq,
                     tb.rssi, n2, n5, tb.rsn_len >= 2 ? tb.rsn[tb.rsn_len - 1] : 0, tb.rsn_len >= 2 ? tb.rsn[tb.rsn_len - 2] : 0, tb.rsnxe_len, tb.rsnxe0,
                     tb.h2e_only, tb.n_rates, unsigned(gDev.mlme.seq));
            } else if (nowMs64() - gConnectAt > 10000) {
                gConnectAt = nowMs64();
                note("FW7 no passphrase yet (/private/var/ngpu/wifi.psk: %d)", pl);
            }
            bzero(psk, sizeof psk);
        } else if (gDev.mlme.state == WMLME_SCAN_DONE && gScans < 8 && nowMs64() - gScanAt > 3000) {   // not seen: scan again (bounded)
            note("FW7 %s not in scan #%u: rescanning", gReqSsid, gScans);
            gReqScan = 1;
        }
    }
    if (gDev.state == WDEV_RUNNING && gReqScan && gDev.mlme.state != WMLME_SCANNING &&
        !(gDev.mlme.state >= WMLME_VDEV_START && gDev.mlme.state <= WMLME_CONNECTED)) {
        gReqScan = 0;
        ++gScans;
        gScanAt = nowMs64();
        const int r = wdev_scan(&gDev, gReqSsid[0] ? gReqSsid : nullptr);
        note("FW7 scan #%u start rc %d", gScans, r);
    }
    static uint64_t last;
    const uint64_t t = nowMs64();
    if (t - last >= 1000) {
        last = t;
        const wce &ce = gDev.ce;
        row(8, "FW6 dev %s%s%s mac %02x:%02x:%02x:%02x:%02x:%02x%s htc %d eid htt%u wmi%u wmi cmds%u ev%u bad%u unk%u(%x) chunks%u htt %u rx%u tx%u arena %u/%u ce rx%u/%u/%u tx%u smmu-fsr %x",
            wdev_state_name(gDev.state), gDev.state == WDEV_FAILED ? " in " : "", gDev.state == WDEV_FAILED ? wdev_state_name(gDev.failed_in) : "",
            gDev.mac[0], gDev.mac[1], gDev.mac[2], gDev.mac[3], gDev.mac[4], gDev.mac[5], gDev.mac_fallback ? (gDev.ops.mac[0] == 0x02 ? "(laa)" : "(persist)") : "", gDev.htc.state, gDev.htt_eid, gDev.wmi_eid,
            gDev.wmi.cmds, gDev.wmi.events, gDev.wmi.bad_events, gDev.wmi.unknown_events, gDev.wmi.last_unknown_id, gDev.wmi.nchunks,
            gDev.htt.state, gDev.htt.rx_frames, gDev.htt.tx_frames, gDev.dma.used, gDev.dma.size, ce.pipe[1].rx, ce.pipe[2].rx, ce.pipe[5].rx,
            ce.pipe[0].tx + ce.pipe[3].tx + ce.pipe[4].tx, smmuFaults());   // (the row says smmu-fsr; xlate faults are also logged)
        if (gScans) scanRow();
        if (gConnects) {
            const wmlme &m = gDev.mlme;
            row(11, "FW7 link %s reason %u akm %u pmf %u sae c%u/f%u/t%u st%u auth%u assoc%u mtx %u ok%u disc%u insp%u noack%u x%u last%u ap auth%u deauth%u disas%u act%u(cat%u) asr%u why%u eapol rx%u tx%u keys %u auth %d aid %u data rx%u tx%u/%u compl ok%u disc%u noack%u post%u x%u rxdrop a%u d%u f%u n%u bad%u%s off%u rx ucast%u dhcp%u arp%u%s",
                wmlme_state_name(m.state), m.reason, m.akm, m.pmf, m.sae_commits, m.sae_confirms, m.sae_tokens, m.sae_status, m.auth_tx,
                m.assoc_tx, m.mtx_sent, m.mtx_status[0], m.mtx_status[1], m.mtx_status[2], m.mtx_status[3], m.mtx_status[4], m.mtx_last, m.ap_rx[0xb], m.ap_rx[0xc], m.ap_rx[0xa], m.ap_rx[0xd], m.ap_last_cat, m.ap_rx[1], m.ap_last_reason, m.eapol_rx, m.eapol_tx, m.key_events, m.authorized, m.aid, gDev.rx_eth, gTxOk, gTxErr,
                gDev.tx_status[0], gDev.tx_status[1], gDev.tx_status[2], gDev.tx_status[3], gDev.tx_status[4],
                gDev.htt.rx_drop_attn, gDev.htt.rx_drop_decap, gDev.htt.rx_drop_fmt, gDev.htt.rx_drop_nondata, gDev.htt.rx_bad, gDev.htt.rx_confused ? "(CONFUSED)" : "", gDev.htt.rx_offload, gRxUnicast, gRxDhcp, gRxArp, gDev.tx_unprotected ? " UNPROT" : "");
        }
    }
    IOLockUnlock(gLock);
}

namespace {
size_t kcmdDev(const char *args, size_t, char *out, size_t cap)
{
    const char *a = args;
    while (*a == ' ') ++a;
    if (!strncmp(a, "wifidev", 7)) a += 7;
    while (*a == ' ') ++a;
    if (!gLock) return snprintf(out, cap, "wifidev: not started (fajita-wifi>=6, wlfw enabled)\n");
    IOLockLock(gLock);
    size_t n = 0;
    if (!strncmp(a, "scan", 4)) {
        a += 4;
        while (*a == ' ') ++a;
        if (*a) {size_t k = 0; while (a[k] && a[k] != '\n' && k < 32) {gReqSsid[k] = a[k]; ++k;} gReqSsid[k] = 0;}
        gReqScan = 1;
        n = snprintf(out, cap, "wifidev: scan requested (target '%s'), state %s\n", gReqSsid, wdev_state_name(gDev.state));
    } else if (!strncmp(a, "connect", 7)) {
        gConnects = 0; gConnectAt = 0;
        if (gStage < 7) gStage = 7;
        n = snprintf(out, cap, "wifidev: connect to '%s' requested (passphrase from /private/var/ngpu/wifi.psk)\n", gReqSsid);
    } else if (!strncmp(a, "txprot", 6)) {
        a += 6;
        while (*a == ' ') ++a;
        if (*a == '0' || *a == '1') gDev.tx_unprotected = *a == '0';
        n = snprintf(out, cap, "wifidev: data frames %s the protected bit\n", gDev.tx_unprotected ? "WITHOUT" : "with");
    } else if (!strncmp(a, "frames", 6)) {
        for (int dir = 0; dir < 2; ++dir) {
            const FrameNote *ring = dir ? gTxNotes : gRxNotes;
            const unsigned cnt = dir ? gTxNoteN : gRxNoteN;
            n += snprintf(out + n, cap - n, "%s %u:\n", dir ? "tx" : "rx", cnt);
            for (unsigned i = cnt > 8 ? cnt - 8 : 0; i < cnt && n + 100 < cap; ++i) {
                const FrameNote &f = ring[i % 8];
                n += snprintf(out + n, cap - n, "  %02x:%02x:%02x:%02x:%02x:%02x <- %02x:%02x:%02x:%02x:%02x:%02x type %04x len %u proto %u udp %u\n",
                              f.dst[0], f.dst[1], f.dst[2], f.dst[3], f.dst[4], f.dst[5], f.src[0], f.src[1], f.src[2], f.src[3], f.src[4],
                              f.src[5], f.type, f.len, f.ip_proto, f.udp_dport);
            }
        }
    } else if (!strncmp(a, "bss", 3)) {
        const wmlme &m = gDev.mlme;
        n = snprintf(out, cap, "wifidev: %u BSS (scan #%u, %s)\n", m.nbss, gScans, wmlme_state_name(m.state));
        for (unsigned i = 0; i < m.nbss && n + 120 < cap; ++i) {
            const wmlme_bss &b = m.bss[i];
            char ssid[33];
            unsigned k = 0;
            for (; k < b.ssid_len && k < 32; ++k) ssid[k] = b.ssid[k] >= 32 && b.ssid[k] < 127 ? char(b.ssid[k]) : '?';
            ssid[k] = 0;
            n += snprintf(out + n, cap - n, "  %02x:%02x:%02x:%02x:%02x:%02x %4u MHz %4d dBm %s%s '%s'\n", b.bssid[0], b.bssid[1], b.bssid[2],
                          b.bssid[3], b.bssid[4], b.bssid[5], b.freq, b.rssi, b.rsn_len ? (b.rsn_ok ? "wpa2" : "rsn?") : "open", b.ht ? " ht" : "", ssid);
            if (b.rsn_len && !b.rsn_ok && n + 3 * b.rsn_len + 12 < cap) {   // why the RSN element was not usable
                n += snprintf(out + n, cap - n, "    rsn");
                for (unsigned k = 0; k < b.rsn_len; ++k) n += snprintf(out + n, cap - n, " %02x", b.rsn[k]);
                n += snprintf(out + n, cap - n, "\n");
            }
        }
    } else {
        n = snprintf(out, cap, "%s%s\n%s\n%s\n", gText, fajita_wifi_rows[8], fajita_wifi_rows[9], fajita_wifi_rows[11]);
    }
    IOLockUnlock(gLock);
    return n < cap ? n : cap - 1;
}
}  // namespace

// kcmd registration happens at start (the USB debug channel is up by then)
extern "C" void fajita_wifi_dev_kcmd(void)
{
    static bool done;
    if (done || !fajita_usbdbg_register_kcmd) return;
    done = true;
    if (!gLock) gLock = IOLockAlloc();
    fajita_usbdbg_register_kcmd("wifidev", kcmdDev, "WCN3990 device: wifidev [state] | scan [SSID] | connect | bss | frames | txprot 0|1");
}
