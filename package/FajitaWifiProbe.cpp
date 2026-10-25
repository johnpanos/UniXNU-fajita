// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita Wi-Fi (WCN3990 on the SDM845 modem DSP) staged bring-up, `fajita-wifi=<stage>`, default off. See NOTES-wifi.md.
// Nothing is mapped, voted or poked without the boot argument, so every other build is unchanged. This host-phase code
// implements stage 1 (read-only: map and read the modem/GCC/SMEM registers and report their state); stages 2..7 (modem
// remoteproc, SMEM/GLINK/QRTR, the AP QMI servers, wlfw, CE/WMI, connect) are logged as pending until the phone is
// available. The protocol and parsing they will use are the freestanding, host-tested drivers/FajitaWifi/* linked in.
// Driven from the boot progress monitor (fajita_wifi_poll, once a second) once IOKit is up.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include "FajitaWifiBoard.h"
extern "C" {
#include "WifiMdt.h"
#include "WifiQmi.h"
#include "WifiQrtr.h"
#include "WifiBoard.h"
}

extern "C" {
char fajita_wifi_rows[8][160];
volatile uint32_t fajita_wifi_state[8];   // [0] stage reached, [1] mapped ok, [2] pbl status, [3] last error
extern volatile uint32_t armcpu_wdog[6] __attribute__((weak_import));
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
}

namespace {
unsigned gStage, gDump;
bool gStarted;

uint64_t uptimeMs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
unsigned uptime() {return (unsigned)(uptimeMs() / 1000);}

struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    if (region.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}

struct Out {
    char *buf; size_t cap, at;
    Out(char *b, size_t c) : buf(b), cap(c), at(0) {buf[0] = 0;}
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(buf + at, at < cap ? cap - at : 0, fmt, ap);
        va_end(ap);
        if (n > 0) at += (size_t)n < cap - at ? (size_t)n : 0;
    }
};

// Stage 1: read-only. Map the modem RMB/QDSP6, the GCC MSS branch window, SMEM and the WLAN membase, and report what the
// firmware left (no writes). Confirms the addresses and whether the modem is already running (it should not be).
bool stage1() {
    Region rmb, gcc, smem;
    bool ok = mapRegion(FW_MSS_RMB_BASE, FW_MSS_RMB_SIZE, rmb)
           && mapRegion(FW_GCC_BASE + 0x8a000, 0x1000, gcc)
           && mapRegion(FW_SMEM_PHYS, 0x1000, smem);
    fajita_wifi_state[1] = ok;
    Out o(fajita_wifi_rows[1], sizeof fajita_wifi_rows[1]);
    if (!ok) {o.add("FW1 map FAILED"); fajita_wifi_state[3] = (uint32_t)-12; return false;}
    const uint32_t pbl = rd32(rmb.va, FW_RMB_PBL_STATUS);
    const uint32_t mba = rd32(rmb.va, FW_RMB_MBA_STATUS);
    const uint32_t mss_status = rd32(rmb.va, FW_RMB_MBA_MSS_STATUS);
    const uint32_t cfg_ahb = rd32(gcc.va, FW_GCC_MSS_CFG_AHB & 0xfff);
    const uint32_t smem_magic = rd32(smem.va, 0);   // smem header: proc_comm then version; nonzero when initialised
    o.add("FW1 t=%us rmb pbl%08x mba%08x mss%08x | gcc mss_cfg_ahb%08x(off%u) | smem[0]%08x",
          uptime(), pbl, mba, mss_status, cfg_ahb, (cfg_ahb >> 31) & 1, smem_magic);
    IOLog("fajita-wifi: %s\n", fajita_wifi_rows[1]);
    fajita_wifi_state[2] = pbl;
    return true;
}

void stagePending(unsigned stage, const char *what) {
    Out o(fajita_wifi_rows[2], sizeof fajita_wifi_rows[2]);
    o.add("FW%u %s: host phase, not yet implemented on hardware (see NOTES-wifi.md)", stage, what);
    IOLog("fajita-wifi: %s\n", fajita_wifi_rows[2]);
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-wifi");
    unsigned wdog = 0;
    if (PE_parse_boot_argn("fajita-wdog", &wdog, sizeof wdog) && wdog && &armcpu_wdog)
        for (unsigned s = 0; s < 120 && !armcpu_wdog[0]; ++s) IOSleep(1000);
    Out o(fajita_wifi_rows[0], sizeof fajita_wifi_rows[0]);
    o.add("FW0 stage %u (default off; read-only stage 1 on hardware)", gStage);
    IOLog("fajita-wifi: %s\n", fajita_wifi_rows[0]);
    fajita_wifi_state[0] = gStage;
    if (gStage >= 1 && !stage1()) return;
    // The protocol code is linked in and host-tested; the hardware steps follow once the phone is available.
    static const char *names[] = {"", "", "modem remoteproc", "SMEM/GLINK/QRTR", "AP QMI servers (pdr/tftp/rmtfs)",
                                  "wlfw handshake", "CE/HTC/WMI", "connect + data"};
    for (unsigned s = 2; s <= gStage && s <= 7; ++s) {stagePending(s, names[s]); break;}
    // Touch the freestanding entry points so the archive keeps them (and a bad prototype fails the kernel link, not a run).
    if (gDump) {
        wmdt_image img; (void)wmdt_parse(nullptr, 0, &img);
        uint8_t b[8]; glink_cmd(b, GLINK_CMD_VERSION, GLINK_VERSION_1, 0);
        (void)wlfw_empty_req(b, 0, 0, WLFW_CAP);
        char n[1][64]; (void)wboard_candidate_names(n, 0, 0, 0xff);
    }
}
}

// Called by the boot progress monitor once a second (BootProgress.c); the first call after IOKit is up starts the worker.
extern "C" void fajita_wifi_poll(void) {
    static bool absent;
    if (absent || gStarted) return;
    if (!IOService::getPlatform()) return;
    unsigned stage = 0;
    PE_parse_boot_argn("fajita-wifi", &stage, sizeof stage);
    if (!stage) {absent = true; return;}
    PE_parse_boot_argn("fajita-wifi-dump", &gDump, sizeof gDump);
    gStage = stage;
    gStarted = true;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
