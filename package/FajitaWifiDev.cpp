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
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include "FajitaWifiBoard.h"
extern "C" {
#include "WifiDev.h"
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
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region)
{
    if (region.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
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
// After traffic: any fault recorded on our bank (CB_FSR at +0x58) or globally (sGFSR 0x48).
uint32_t smmuFaults()
{
    if (!gSmmu.va || gSmmuBank < 0) return 0;
    const uintptr_t smmu = gSmmu.va;
    const uint32_t idr1 = *reinterpret_cast<volatile uint32_t *>(smmu + 0x24);
    const unsigned pages = 1u << (((idr1 >> 28) & 7) + 1);
    return *reinterpret_cast<volatile uint32_t *>(smmu + (pages + unsigned(gSmmuBank)) * 4096 + 0x58);
}

// ---- wdev ops ----
uint32_t ioRd(void *, uint32_t off) {return *reinterpret_cast<volatile uint32_t *>(gMem.va + off);}
void ioWr(void *, uint32_t off, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(gMem.va + off) = v;}
void ioBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory");}
uint32_t devNow(void *) {return static_cast<uint32_t>(nowMs64());}
void devRandom(void *, uint8_t *out, uint32_t n) {read_random(out, n);}
void devRx(void *, const uint8_t *, uint32_t) {}
void devLink(void *, unsigned st, unsigned reason)
{
    gLastLink = st; gLinkReason = reason;
    note("FW7 mlme %s reason %u", wmlme_state_name(st), reason);
}
void devLog(void *, const char *step, int rc, uint32_t val) {note("FW6 %s rc %d t+%u ms", step, rc, val);}
void *devAlloc(size_t n, void *) {void *p = IOMalloc(n ? n : 1); if (p) bzero(p, n ? n : 1); return p;}
void devFree(void *p, size_t n, void *) {if (p) IOFree(p, n ? n : 1);}

void scanRow()
{
    const wmlme &m = gDev.mlme;
    char best[64] = "-";
    if (gReqSsid[0]) {
        const wmlme_bss *b = wmlme_find(&m, gReqSsid);
        if (b) snprintf(best, sizeof best, "%s %02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm%s", gReqSsid, b->bssid[0], b->bssid[1], b->bssid[2],
                        b->bssid[3], b->bssid[4], b->bssid[5], b->freq, b->rssi, b->rsn_ok ? " wpa2-psk" : " (rsn?)");
        else snprintf(best, sizeof best, "%s NOT SEEN", gReqSsid);
    }
    row(9, "FW7 scan #%u %s bss %u frames %u full %u: %s", gScans, wmlme_state_name(m.state), m.nbss, m.scan_frames, m.scan_full, best);
}
}  // namespace

extern "C" int fajita_wifi_dev_start(void)
{
    if (gStarted) return 0;
    gStarted = true;
    if (!gLock) gLock = IOLockAlloc();
    if (!arenaHeld()) {row(8, "FW6 arena %llx+%llx not held (phys-reserve)", FW_WLAN_DMA_PHYS, FW_WLAN_DMA_SIZE); return -1;}
    if (!mapRegion(FW_WLAN_MEMBASE, FW_WLAN_MEMBASE_SIZE, gMem) || !mapRegion(FW_WLAN_DMA_PHYS, FW_WLAN_DMA_SIZE, gArena)) {
        row(8, "FW6 map failed (membase %d arena %d)", gMem.va != 0, gArena.va != 0);
        return -2;
    }
    const int s = smmuBypassWlan();
    note("FW6 smmu sid %x/%x rc %d slot %d bank %d", FW_WLAN_SMMU_SID, FW_WLAN_SMMU_MASK, s, gSmmuSlot, gSmmuBank);
    if (s) {row(8, "FW6 smmu FAILED rc %d", s); return -3;}
    bzero(reinterpret_cast<void *>(gArena.va), FW_WLAN_DMA_SIZE);
    const wdev_ops ops = {nullptr, devNow, devRandom, devRx, devLink, devLog, devAlloc, devFree};
    const wce_io io = {nullptr, ioRd, ioWr, ioBarrier};
    IOLockLock(gLock);
    const int r = wdev_start(&gDev, &ops, &io, reinterpret_cast<void *>(gArena.va), FW_WLAN_DMA_PHYS, static_cast<uint32_t>(FW_WLAN_DMA_SIZE));
    IOLockUnlock(gLock);
    note("FW6 start rc %d ce intr %08x", r, wce_intr_summary(&gDev.ce));
    if (r) {row(8, "FW6 wdev_start FAILED rc %d", r); return -4;}
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
    if (gDev.state == WDEV_RUNNING && gReqScan && gDev.mlme.state != WMLME_SCANNING) {
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
        row(8, "FW6 dev %s%s%s mac %02x:%02x:%02x:%02x:%02x:%02x htc %d eid htt%u wmi%u wmi cmds%u ev%u bad%u unk%u(%x) chunks%u htt %u rx%u tx%u arena %u/%u ce rx%u/%u/%u tx%u smmu-fsr %x",
            wdev_state_name(gDev.state), gDev.state == WDEV_FAILED ? " in " : "", gDev.state == WDEV_FAILED ? wdev_state_name(gDev.failed_in) : "",
            gDev.mac[0], gDev.mac[1], gDev.mac[2], gDev.mac[3], gDev.mac[4], gDev.mac[5], gDev.htc.state, gDev.htt_eid, gDev.wmi_eid,
            gDev.wmi.cmds, gDev.wmi.events, gDev.wmi.bad_events, gDev.wmi.unknown_events, gDev.wmi.last_unknown_id, gDev.wmi.nchunks,
            gDev.htt.state, gDev.htt.rx_frames, gDev.htt.tx_frames, gDev.dma.used, gDev.dma.size, ce.pipe[1].rx, ce.pipe[2].rx, ce.pipe[5].rx,
            ce.pipe[0].tx + ce.pipe[3].tx + ce.pipe[4].tx, smmuFaults());
        if (gScans) scanRow();
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
        }
    } else {
        n = snprintf(out, cap, "%s%s\n%s\n", gText, fajita_wifi_rows[8], fajita_wifi_rows[9]);
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
    fajita_usbdbg_register_kcmd("wifidev", kcmdDev, "WCN3990 device: wifidev [state] | scan [SSID] | bss");
}
