// SPDX-License-Identifier: MIT
// fajita (SDM845) Adreno 630 bring-up, staged. `fajita-gpu=<n>` runs stages 1..n in a kernel thread once the boot progress
// monitor calls fajita_gpu_probe (after fajita-gpu-delay seconds, default 15); the USB debug channel's kernel command
// `gpu <n>` runs further stages later (`gpu` alone prints the state, `gpu log` the log, `gpu regs` a register snapshot).
// Each stage only runs when every earlier one passed; a failure stops the sequence and is reported, it never panics.
//
//   1 read-only: GCC GPU branches, GPUCC GDSCs/clocks, and (with CX on) the adreno SMMU IDs and the GMU state
//   2 A630PowerCx: GCC GPU clocks, gpu_cx_gdsc, gpu_cc_cxo (the writes Linux's gcc/gpucc/gdsc drivers make)
//   3 A630GmuClocks: gpu_cc_gmu_clk_src 200 MHz, gpu_cc_cx_gmu_clk
//   4 the GMU's SMMU context (adreno_smmu bank 1, SID 5) with its payloads at a6xx_gmu.c addresses; the command DB votes
//   5 A630RpmhInit + A630GmuColdStart: PDC/RSC, a630_gmu.bin into the ITCM, CM3 release, GX rail vote, SPTPRAC, HFI v1
//   6 GX on: OOB GPU_SET held, GX and RBBM_STATUS read
//   7 the GPU's SMMU context (bank 0, SID 0), the SQE, A630HwInit, ring 0, CP_ME_INIT, a CP_MEM_WRITE fence
//   8 zap shader (a630_zap.mbn through SCM PAS 13 into gpu_mem 0x8c415000) and CP_SET_SECURE_MODE 0
//   9 the GPU interrupt (SPI 300) and a fenced CP_MEMCPY
//  10 publish NativeAdrenoGpu (NativeAdrenoGpuPublished, the user client)
//  11 the staged AGXAccelerator (an IOAcceleratorES for Metal's discovery, MetalPluginName NativeAdrenoMetal), after checking the
//     bound IOAcceleratorES base (scripts/data/fajita-ioaccelerator-base-abi.json) against the live metaclass
// Linux v6.16 (msm a6xx) is the reference for every value; see drivers/FajitaAdreno/A630Gmu.h.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <stdarg.h>
extern "C" {
#include "A630Gmu.h"
#include "A630Gpu.h"
#include "A630Regs.h"
}
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoABI.h"
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t input_bits, uint32_t output_bits) __attribute__((weak));
extern "C" uint32_t native_adreno_publish_stats(uint32_t *closed) __attribute__((weak));
// The staged AGXAccelerator (drivers/NativeAdreno/NativeAdrenoAccelerator.cpp, built by scripts/build-fajita-gpu.py): its static
// initializer is out of __mod_init_func and exported under this name, its IOAcceleratorES imports are bound to the audited original
// addresses. All weak: a kernel without the staged class links and stage 11 reports "nolink".
extern "C" void fajita_agx_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp") __attribute__((weak));
extern "C" IOService *native_adreno_accelerator_allocate(void) __attribute__((weak));
extern "C" const uint8_t fajita_bound_ioacceleratores_meta[] __asm__("__ZN15IOAcceleratorES10gMetaClassE") __attribute__((weak));
extern "C" OSReturn OSKextLoadKextWithIdentifier(const char *kextIdentifier);

// Firmware, embedded by scripts/build-fajita-gpu.py from the ignored .cache copies (sha256-pinned); weak so a kernel built
// without them still links and stage 5 reports "nofw".
extern "C" const uint8_t fajita_gpu_gmu_fw[] __attribute__((weak));
extern "C" const uint64_t fajita_gpu_gmu_fw_size __attribute__((weak));
extern "C" const uint8_t fajita_gpu_sqe_fw[] __attribute__((weak));
extern "C" const uint64_t fajita_gpu_sqe_fw_size __attribute__((weak));
extern "C" const uint8_t fajita_gpu_zap_fw[] __attribute__((weak));
extern "C" const uint64_t fajita_gpu_zap_fw_size __attribute__((weak));
// USB debug kernel commands (drivers/NativeDwc3/UsbDbgKernelTarget.h); weak: NativeDwc3 may not be linked.
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
extern "C" int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                                      uint64_t *router) __attribute__((weak));

extern "C" {
volatile uint32_t fajita_gpu_stage = 0;      // last stage that passed
volatile uint32_t fajita_gpu_running = 0;    // a stage thread is running (target stage)
volatile int32_t fajita_gpu_failed = 0;      // stage that failed (0 none)
volatile uint32_t fajita_gpu_status = 0;     // its status code
char fajita_gpu_text[8192];                  // the log, newest last
volatile uint32_t fajita_gpu_text_len = 0;
}

namespace {

// ---- log ------------------------------------------------------------------------------------------------------------
IOSimpleLock *gLogLock;
void gpuLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gpuLog(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    IOLog("fajita-gpu: %s\n", line);
    line[n++] = '\n';
    if (gLogLock) IOSimpleLockLock(gLogLock);
    uint32_t len = fajita_gpu_text_len;
    if (len + (uint32_t)n > sizeof fajita_gpu_text) {   // drop the older half
        const uint32_t keep = sizeof fajita_gpu_text / 2;
        memmove(fajita_gpu_text, fajita_gpu_text + len - keep, keep);
        len = keep;
    }
    memcpy(fajita_gpu_text + len, line, (size_t)n);
    fajita_gpu_text_len = len + (uint32_t)n;
    if (gLogLock) IOSimpleLockUnlock(gLogLock);
}

uint64_t nowUs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}
unsigned bootArg(const char *name, unsigned fallback) {unsigned v = fallback; PE_parse_boot_argn(name, &v, sizeof v); return v;}

// ---- MMIO windows -----------------------------------------------------------------------------------------------------
struct Window {uint64_t phys, bytes; volatile uint8_t *va; OSSharedPtr<IOMemoryMap> map;};
Window gWin[A630BlockCount] = {
    {0x00100000, 0x90000, nullptr, nullptr},    // GCC: 0x44038 .. 0x8c010
    {0x05090000, 0x9000, nullptr, nullptr},     // GPUCC
    {0x0506a000, 0x30000, nullptr, nullptr},    // GMU
    {0, 0, nullptr, nullptr},                   // RSCC: GMU + 0x23000
    {0x0b280000, 0x10000, nullptr, nullptr},    // gmu_pdc
    {0x0b480000, 0x10000, nullptr, nullptr},    // gmu_pdc_seq
    {0x05000000, 0x40000, nullptr, nullptr},    // GPU
};
Window gSmmu = {0x05040000, 0x10000, nullptr, nullptr};
volatile uint32_t gIoFaults;

volatile uint8_t *mapWindow(Window &w)
{
    if (w.va) return w.va;
    IODeviceMemory *m = IODeviceMemory::withRange(w.phys, w.bytes);
    if (!m) return nullptr;
    w.map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    m->release();
    if (w.map) w.va = reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress());
    return w.va;
}
bool mapAll()
{
    for (unsigned b = 0; b < A630BlockCount; ++b) {
        if (b == A630BlockRscc) continue;
        if (!mapWindow(gWin[b])) {gpuLog("map %u (%llx) failed", b, (unsigned long long)gWin[b].phys); return false;}
    }
    gWin[A630BlockRscc].phys = gWin[A630BlockGmu].phys + A630GmuRsccOffset;
    gWin[A630BlockRscc].bytes = gWin[A630BlockGmu].bytes - A630GmuRsccOffset;
    gWin[A630BlockRscc].va = gWin[A630BlockGmu].va + A630GmuRsccOffset;
    return mapWindow(gSmmu) != nullptr;
}
uint32_t ioRead(void *, unsigned b, uint32_t off)
{
    if (b >= A630BlockCount || !gWin[b].va || off >= gWin[b].bytes || (off & 3)) {gIoFaults = gIoFaults + 1; return 0xdeadbeef;}
    return *reinterpret_cast<volatile uint32_t *>(gWin[b].va + off);
}
void ioWrite(void *, unsigned b, uint32_t off, uint32_t v)
{
    if (b >= A630BlockCount || !gWin[b].va || off >= gWin[b].bytes || (off & 3)) {gIoFaults = gIoFaults + 1; return;}
    *reinterpret_cast<volatile uint32_t *>(gWin[b].va + off) = v;
}
void ioBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory");}
void ioDelay(void *, unsigned us) {IODelay(us);}
const A630Io gIo = {nullptr, ioRead, ioWrite, ioBarrier, ioDelay};
uint32_t gpuRd(uint32_t reg) {return ioRead(nullptr, A630BlockGpu, reg * 4);}
void gpuWr(uint32_t reg, uint32_t v) {ioWrite(nullptr, A630BlockGpu, reg * 4, v);}
uint32_t gmuRd(uint32_t reg) {return ioRead(nullptr, A630BlockGmu, reg * 4);}
uint32_t smmuRd(uint32_t off) {return gSmmu.va && off < gSmmu.bytes ? *reinterpret_cast<volatile uint32_t *>(gSmmu.va + off) : 0xdeadbeef;}

// ---- stage state ------------------------------------------------------------------------------------------------------
A630ClockReport gClocks;
A630PerfTable gPerf;
A630GmuReport gGmu;
A630Hfi gHfi;
A630GmuMemory gGmuMem;
NativeAdrenoGmuContextReport gGmuCtx, gGpuCtx;
OSSharedPtr<IOMemoryMap> gPayloadMaps[5], gBufferMaps[5];
NativeAdrenoGpuBuffer gBuffers[5];
volatile uint8_t *gCpu[5];
NativeAdrenoRing gRing;
uint32_t gCmdDbCopy[0x20000 / 4];
bool gGxOn;

// GPU kernel buffers above the A630 GMEM window [1 MiB, 2 MiB) and below 4 GiB: sqe, ring, memptrs (rptr shadow +0, fence
// +4), pwrup (unused on A630), scratch.
const uint64_t kBufIova[5] = {0x01000000, 0x01100000, 0x01200000, 0x01201000, 0x01202000};
const size_t kBufBytes[5] = {0x13000, 0x8000, 0x1000, 0x1000, 0x1000};
enum {kSqe, kRing, kMemptrs, kPwrup, kScratch};

const char *statusName(A630Status s)
{
    static const char *n[] = {"ok", "argument", "clock", "gdsc", "rsc", "firmware", "fwinit", "oob", "hfistart", "hfisend",
                              "hfitimeout", "hfireply", "cmddb", "votes", "sptprac", "gx"};
    return (unsigned)s < sizeof n / sizeof *n ? n[s] : "?";
}

// ---- stages -------------------------------------------------------------------------------------------------------------
bool stage1()
{
    // Read-only. GCC is always readable; GPUCC needs gcc_gpu_cfg_ahb_clk (on at boot), the GMU and SMMU need CX.
    const uint32_t ahb = ioRead(nullptr, A630BlockGcc, 0x71004), memnoc = ioRead(nullptr, A630BlockGcc, 0x7100c),
                   axi = ioRead(nullptr, A630BlockGcc, 0x44038), vote = ioRead(nullptr, A630BlockGcc, 0x52004);
    gpuLog("S1 gcc ahb %08x memnoc %08x axi %08x gpll0 %08x misc %08x", ahb, memnoc, axi, vote,
           ioRead(nullptr, A630BlockGcc, 0x71028));
    if (ahb & 0x80000000u) {gpuLog("S1 gcc_gpu_cfg_ahb_clk is off: GPUCC not read"); return true;}
    const uint32_t cx = ioRead(nullptr, A630BlockGpuCc, 0x106c), gx = ioRead(nullptr, A630BlockGpuCc, 0x100c);
    gpuLog("S1 gpucc cx %08x gx %08x rcg %08x/%08x gmu %08x cxo %08x", cx, gx, ioRead(nullptr, A630BlockGpuCc, 0x1120),
           ioRead(nullptr, A630BlockGpuCc, 0x1124), ioRead(nullptr, A630BlockGpuCc, 0x1098), ioRead(nullptr, A630BlockGpuCc, 0x109c));
    if (cx & 0x80000000u) {
        gpuLog("S1 smmu cr0 %08x id0 %08x id1 %08x id2 %08x smr0 %08x s2cr0 %08x", smmuRd(0), smmuRd(0x20), smmuRd(0x24),
               smmuRd(0x28), smmuRd(0x800), smmuRd(0xc00));
        if (!(ioRead(nullptr, A630BlockGpuCc, 0x1098) & 0x80000000u))
            gpuLog("S1 gmu sysreset %x fwinit %x hfi %x sptprac %x general7 %x pstate %x", gmuRd(A630_GMU_CM3_SYSRESET),
                   gmuRd(A630_GMU_CM3_FW_INIT_RESULT), gmuRd(A630_GMU_HFI_CTRL_STATUS), gmuRd(A630_GMU_SPTPRAC_PWR_CLK_STATUS),
                   gmuRd(A630_GMU_GENERAL_7), gmuRd(A630_GPU_GMU_CX_GMU_RPMH_POWER_STATE));
    }
    return true;
}

bool stage2()
{
    const A630Status s = A630PowerCx(&gIo, &gClocks);
    gpuLog("S2 %s ahb %08x memnoc %08x axi %08x gpll0 %08x cx %08x>%08x cxo %08x gx %08x polls %u", statusName(s),
           gClocks.gcc_cfg_ahb, gClocks.gcc_memnoc, gClocks.gcc_axi, gClocks.gcc_gpll0, gClocks.cx_before, gClocks.cx_after,
           gClocks.cxo, gClocks.gx_gdscr, gClocks.polls);
    if (!s) gpuLog("S2 smmu cr0 %08x id0 %08x id1 %08x id2 %08x", smmuRd(0), smmuRd(0x20), smmuRd(0x24), smmuRd(0x28));
    fajita_gpu_status = s;
    return !s;
}

bool stage3()
{
    const A630Status s = A630GmuClocks(&gIo, &gClocks);
    gpuLog("S3 %s rcg %08x/%08x cx_gmu %08x; gmu sysreset %x pstate %x sptprac %x", statusName(s),
           ioRead(nullptr, A630BlockGpuCc, 0x1120), gClocks.rcg_cfg, gClocks.cx_gmu, gmuRd(A630_GMU_CM3_SYSRESET),
           gmuRd(A630_GPU_GMU_CX_GMU_RPMH_POWER_STATE), gmuRd(A630_GMU_SPTPRAC_PWR_CLK_STATUS));
    fajita_gpu_status = s;
    return !s;
}

volatile uint32_t gCtxStep;
void ctxProgress(uint32_t step) {gCtxStep = step;}
void ctxReport(const char *what, IOReturn st, const NativeAdrenoGmuContextReport &g)
{
    gpuLog("%s s%u st%x k%u/%u id1 %x id2 %x oas %u p%uw%ur%ue%u r%uw%uv%u spans %u", what, g.step, (unsigned)st,
           g.program_status, g.route_status, g.id1, g.id2, g.output_bits, g.program_phase, g.program_writes,
           g.program_readback_valid, g.program_enabled, g.route_phase, g.route_writes, g.routes_verified, g.spans);
    gpuLog("%s T%016llx S%08x C%08x R%08x cb%08x mismatch %x", what, (unsigned long long)g.ttbr0, g.sctlr, g.s2cr, g.smr,
           g.cbar, g.mismatch);
}

bool stage4()
{
    // a6xx_gmu_init (A630 branch): dummy 4 KiB at 0x60000000, debug 16 KiB, log 16 KiB, hfi 16 KiB; no icache (a page is
    // allocated anyway, the allocator wants one; nothing uses it). The GMU is arm-smmu-qcom's second client: bank 1.
    // The reset S2CRs read 0x000200ff (TYPE fault, CBNDX 0xff); the stream kit leases only a slot whose S2CR reads clean
    // FAULT with CBNDX 0 (gpu2/gpu3: attach refused Busy). The SDM845 SMMU firmware has the Qualcomm quirk that writes of a
    // FAULT-type S2CR are ignored and a BYPASS write lands as FAULT (arm-smmu-qcom qcom_smmu_cfg_probe; confirmed live: a
    // bypass 0x00010000 read back 0x00020000), so writing BYPASS with CBNDX 0 to an unused slot gives the clean FAULT the kit
    // wants. GMU uses slot 1, GPU slot 0; normalize both while no stream is valid.
    for (unsigned slot = 0; slot < 2; ++slot) {
        volatile uint32_t *smr = reinterpret_cast<volatile uint32_t *>(gSmmu.va + 0x800 + 4 * slot);
        volatile uint32_t *s2cr = reinterpret_cast<volatile uint32_t *>(gSmmu.va + 0xc00 + 4 * slot);
        if (!(*smr & 0x80000000u) && (*s2cr & 0x30000u) == 0x20000u && (*s2cr & 0xff) != 0) {
            *s2cr = 0x00010000u;   // BYPASS, CBNDX 0 -> the firmware stores FAULT, CBNDX 0
            __asm__ volatile("dsb sy" ::: "memory");
            gpuLog("S4 smmu slot %u s2cr normalized to %08x", slot, *s2cr);
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    IOReturn st = NativeAdrenoGmuContextSetSizes(0x1000, 0x1000, 0x4000, 0x4000, 0x4000);
    if (!st) st = NativeAdrenoGmuContextBringup(gSmmu.va, gSmmu.bytes, 1, 1, 1, 5, ctxProgress, &gGmuCtx);
    ctxReport("S4 gmuctx", st, gGmuCtx);
    if (st) {fajita_gpu_status = st; return false;}
    NativeAdrenoGmuPayload p[5];
    st = NativeAdrenoGmuContextPayloads(p);
    if (st) {gpuLog("S4 payloads %x", st); fajita_gpu_status = st; return false;}
    // CPU views of debug, log and hfi: write-combined (Normal non-cacheable), as the GMU maps them.
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(p[i].physical, p[i].bytes);
        if (m) {gPayloadMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
        if (!gPayloadMaps[i]) {gpuLog("S4 payload %u map failed", i); return false;}
        gpuLog("S4 payload %u pa %llx bytes %llx iova %llx", i, (unsigned long long)p[i].physical,
               (unsigned long long)p[i].bytes, (unsigned long long)p[i].iova);
    }
    gGmuMem.hfi = reinterpret_cast<volatile uint32_t *>(gPayloadMaps[4]->getVirtualAddress());
    gGmuMem.hfi_iova = uint32_t(p[4].iova);
    gGmuMem.debug_iova = uint32_t(p[2].iova);
    gGmuMem.debug_bytes = uint32_t(p[2].bytes);
    gGmuMem.log_iova = uint32_t(p[3].iova);
    gGmuMem.log_bytes = uint32_t(p[3].bytes);
    // The command DB (aop-cmd-db-mem 0x85fe0000, reserved in the UEFI map and outside XNU's memory): aligned word reads.
    {
        IODeviceMemory *m = IODeviceMemory::withRange(0x85fe0000, sizeof gCmdDbCopy);
        OSSharedPtr<IOMemoryMap> map;
        if (m) {map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain); m->release();}
        if (!map) {gpuLog("S4 cmd-db map failed"); return false;}
        const volatile uint32_t *db = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
        for (unsigned i = 0; i < sizeof gCmdDbCopy / 4; ++i) gCmdDbCopy[i] = db[i];
    }
    const A630Status s = A630BuildPerfTable(reinterpret_cast<const uint8_t *>(gCmdDbCopy), sizeof gCmdDbCopy, &gPerf);
    gpuLog("S4 votes %s cmd-db v%x magic %08x gx %08x %08x %08x %08x cx %08x %08x", statusName(s), gCmdDbCopy[0], gCmdDbCopy[1],
           gPerf.gx_votes[1], gPerf.gx_votes[2], gPerf.gx_votes[gPerf.gpu_count - 2], gPerf.gx_votes[gPerf.gpu_count - 1],
           gPerf.cx_votes[1], gPerf.cx_votes[2]);
    fajita_gpu_status = s;
    return !s;
}

bool stage5()
{
    if (!&fajita_gpu_gmu_fw_size || !fajita_gpu_gmu_fw || fajita_gpu_gmu_fw_size < 4) {gpuLog("S5 nofw"); return false;}
    A630Status s = A630RpmhInit(&gIo, &gGmu);
    gpuLog("S5 rpmh_init %s rsc %x", statusName(s), gGmu.seq_busy);
    if (s) {fajita_gpu_status = s; return false;}
    A630HfiInit(&gGmuMem, &gHfi);
    __asm__ volatile("dsb sy" ::: "memory");
    // fajita-gpu-level: the boot GX vote (index into the GPU levels; Linux: the highest, 7 = 710 MHz).
    const unsigned level = bootArg("fajita-gpu-level", gPerf.gpu_count - 1);
    const A630GmuConfig c = {0x06030002, fajita_gpu_gmu_fw, (size_t)fajita_gpu_gmu_fw_size, &gPerf,
                             level < gPerf.gpu_count ? level : gPerf.gpu_count - 1};
    const uint64_t t0 = nowUs();
    s = A630GmuColdStart(&gIo, &gGmuMem, &gHfi, &c, &gGmu);
    gpuLog("S5 coldstart %s step %u in %llu us: fwinit %x core %x hfi %x sptprac %x pstate %x ao %x g2h %x oob %x", statusName(s),
           gGmu.step, (unsigned long long)(nowUs() - t0), gGmu.fw_init_result, gGmu.core_fw_version, gGmu.hfi_status,
           gGmu.sptprac_status, gGmu.rpmh_power_state, gGmu.ao_status, gGmu.g2h_info, gGmu.oob_info);
    gpuLog("S5 hfi seq %u replies %u skipped %u fwerr %u/%x last id %u hdr %x ret %x err %x fwver %x", gHfi.seq, gHfi.replies,
           gHfi.skipped, gHfi.fw_errors, gHfi.first_fw_error, gHfi.last_id, gHfi.last_header, gHfi.last_ret, gHfi.last_error,
           gHfi.fw_version);
    fajita_gpu_status = s;
    return !s;
}

bool stage6()
{
    // hw_init's first step: hold OOB GPU_SET while the GPU is programmed (it is never cleared here: the GMU then keeps GX
    // on, as idle level ACTIVE asks anyway).
    uint32_t info = 0;
    const A630Status s = A630SetOob(&gIo, A630OobGpuSet, &info);
    const uint32_t st = gmuRd(A630_GMU_SPTPRAC_PWR_CLK_STATUS);
    gGxOn = A630GxOn(&gIo);
    gpuLog("S6 gpu_set %s info %x sptprac/gx %x gx %s gdscr %08x", statusName(s), info, st, gGxOn ? "on" : "OFF",
           ioRead(nullptr, A630BlockGpuCc, 0x100c));
    if (s || !gGxOn) {fajita_gpu_status = s ? s : A630ErrGx; return false;}
    gpuLog("S6 rbbm status %08x int %08x vbif halt %x/%x clock %08x", gpuRd(A630_RBBM_STATUS), gpuRd(A630_RBBM_INT_0_STATUS),
           gpuRd(A630_VBIF_XIN_HALT_CTRL0), gpuRd(A630_VBIF_XIN_HALT_CTRL1), gpuRd(A630_RBBM_CLOCK_CNTL));
    return true;
}

int ringWrite(void *, uint32_t reg, uint32_t value) {gpuWr(reg, value); return 0;}
int ringRead(void *, uint32_t reg, uint32_t *value) {*value = gpuRd(reg); return 0;}
int ringBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int ringPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int ringAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t ringNow(void *) {return nowUs();}
int ringIdle(void *) {IODelay(20); return 0;}
const NativeAdrenoRingIo kRingIo = {nullptr, ringWrite, ringRead, ringBarrier};
const NativeAdrenoRingCache kRingCache = {nullptr, ringPublish, ringAcquire};
const NativeAdrenoRingClock kClock = {nullptr, ringNow, ringIdle};

void cpRow(const char *what)
{
    const volatile uint32_t *mem = reinterpret_cast<const volatile uint32_t *>(gCpu[kMemptrs]);
    gpuLog("%s rbbm %08x int %08x hwfault %08x rptr %x wptr %x shadow %x fence %x sqe %x cp1 %x protect %x fsr0 %x",
           what, gpuRd(A630_RBBM_STATUS), gpuRd(A630_RBBM_INT_0_STATUS), gpuRd(A630_CP_HW_FAULT), gpuRd(A630_CP_RB_RPTR),
           gpuRd(A630_CP_RB_WPTR), mem ? mem[0] : 0, mem ? mem[1] : 0, gpuRd(A630_CP_SQE_CNTL), gpuRd(A630_CP_STATUS_1),
           gpuRd(A630_CP_PROTECT_STATUS), smmuRd(0x8000 + 0x58));
}

// Submit kernel words with WHERE_AM_I (the RPTR shadow) and the a6xx fence; wait for it.
bool cpRun(const uint32_t *words, size_t count, const char *what, uint32_t timeout_us = 200000)
{
    uint32_t buf[64];
    if (count > 48) return false;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) buf[n++] = words[i];
    n += A630WhereAmIWords(gBuffers[kMemptrs].iova, buf + n);
    uint32_t fence = 0, seen = 0;
    const IOReturn st = gRing.submit(buf, n, &fence);
    if (st) {gpuLog("%s submit %x", what, st); return false;}
    const NativeAdrenoRingFence f = gRing.wait(fence, &kClock, timeout_us, &seen);
    if (f) {cpRow(what); gpuLog("%s fence %u not reached (%u, result %d)", what, fence, seen, (int)f); return false;}
    return true;
}

extern "C" volatile uint32_t native_adreno_pool_mib, native_adreno_pool_stats[8], native_adreno_pool_status;
extern "C" volatile uint64_t native_adreno_submit_time[8] __attribute__((weak));
extern "C" volatile uint64_t native_adreno_maint_stats[4] __attribute__((weak));
extern "C" volatile uint64_t native_adreno_submit_ring[64][5] __attribute__((weak));
extern "C" volatile uint32_t native_adreno_skip_written_imports __attribute__((weak));
extern "C" volatile uint32_t native_adreno_skip_readonly_imports __attribute__((weak));
extern "C" volatile int native_adreno_trace __attribute__((weak));
extern "C" volatile uint32_t native_adreno_skip_written_publish __attribute__((weak));
extern "C" volatile uint32_t native_adreno_uncached_acquire_skip __attribute__((weak));
extern "C" volatile uint64_t native_adreno_acquire_stats[2] __attribute__((weak));
extern "C" volatile uint64_t native_adreno_import_check[3] __attribute__((weak));
extern "C" volatile uint32_t native_adreno_uche_invalidate __attribute__((weak));
extern "C" bool (*native_adreno_page_uncached)(uint64_t physical) __attribute__((weak));
extern "C" volatile uint64_t native_adreno_uncached_skipped __attribute__((weak));
extern "C" unsigned int pmap_cache_attributes(unsigned int pn);
// Every CPU mapping of the page is uncached (write-combined or device: VM_MEM_NOT_CACHEABLE in the pmap's attribute of the page).
static bool pageUncached(uint64_t physical) {return pmap_cache_attributes((unsigned int)(physical >> PAGE_SHIFT)) & 0x4u;}
extern "C" volatile uint64_t native_legacy_copy_n __attribute__((weak)), native_legacy_copy_sum __attribute__((weak)), native_legacy_copy_max __attribute__((weak));
extern "C" volatile uint64_t native_gop_gpu_offers __attribute__((weak)), native_gop_gpu_completions __attribute__((weak)),
    native_gop_gpu_failures __attribute__((weak)), native_gop_gpu_abandoned __attribute__((weak));
extern "C" volatile uint32_t native_legacy_gpu_fallbacks __attribute__((weak)), native_legacy_gpu_last_status __attribute__((weak));
bool stage7()
{
    // The user BO pool (sheng's sheng-gpu-pool): user BOs from one write-combined contiguous block skip the per-submission
    // write-combine re-assertion that walks every scattered BO page (25-40 ms per submission on fajita without it, session-213903).
    // fajita-gpu-pool=<MiB>, default 768; the context halves it down to 16 MiB when it cannot have it; 0 = scattered BOs only.
    native_adreno_pool_mib = bootArg("fajita-gpu-pool", 768);
    IOReturn st = NativeAdrenoGpuContextSetLayout(kBufIova, kBufBytes);
    if (!st) st = NativeAdrenoGpuContextBringup(gSmmu.va, gSmmu.bytes, 0, 2, 0, 0, ~0u, 0, ctxProgress, &gGpuCtx);
    ctxReport("S7 gpuctx", st, gGpuCtx);
    gpuLog("S7 pool %u MiB (asked %u) status %x", native_adreno_pool_stats[0], (unsigned)native_adreno_pool_mib, native_adreno_pool_status);
    if (st) {fajita_gpu_status = st; return false;}
    st = NativeAdrenoGpuContextBuffers(gBuffers);
    if (st) {gpuLog("S7 buffers %x", st); return false;}
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gBuffers[i].physical, gBuffers[i].bytes);
        if (m) {gBufferMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
        if (!gBufferMaps[i]) {gpuLog("S7 buffer %u map failed", i); return false;}
        gCpu[i] = reinterpret_cast<volatile uint8_t *>(gBufferMaps[i]->getVirtualAddress());
    }
    // The SQE without its first dword (adreno_fw_create_bo).
    if (!&fajita_gpu_sqe_fw_size || !fajita_gpu_sqe_fw || fajita_gpu_sqe_fw_size < 16 ||
        fajita_gpu_sqe_fw_size - 4 > gBuffers[kSqe].bytes) {gpuLog("S7 nosqe"); return false;}
    const size_t sqeBytes = (size_t)fajita_gpu_sqe_fw_size;
    for (size_t i = 4; i < sqeBytes; ++i) gCpu[kSqe][i - 4] = fajita_gpu_sqe_fw[i];
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t w[4];
    memcpy(w, fajita_gpu_sqe_fw, sizeof w);
    gpuLog("S7 sqe %zu bytes version %x whereami %d", sqeBytes - 4, w[1] & 0xfff, A630SqeHasWhereAmI(w, 4));
    const A630HwInitConfig hc = {gBuffers[kSqe].iova, gBuffers[kRing].iova, gBuffers[kMemptrs].iova, (uint32_t)gBuffers[kRing].bytes,
                                 0x1fffffffff000ull, 0x100000, 0x100000};
    A630HwInitReport hr = {};
    // The ring object programs RB_BASE/RB_CNTL/RPTR_ADDR itself (without NO_UPDATE); A630HwInit writes them again after, with
    // NO_UPDATE and the SQE start, as hw_init does.
    gRing.setA6xx(true);
    st = gRing.attach(&kRingIo, &kRingCache, const_cast<uint8_t *>(gCpu[kRing]), gBuffers[kRing].bytes, gBuffers[kRing].iova,
                      const_cast<uint8_t *>(gCpu[kMemptrs]), gBuffers[kMemptrs].bytes, gBuffers[kMemptrs].iova);
    if (st) {gpuLog("S7 ring attach %x", st); return false;}
    A630HwInit(&gIo, &hc, &hr);
    gpuLog("S7 hwinit step %u clock %08x rbbm %08x int %08x hwfault %08x sqe_cntl %x rb_cntl %08x", hr.step, hr.clock_cntl_before,
           hr.rbbm_status, hr.int_status, hr.cp_hw_fault, hr.sqe_cntl, gpuRd(A630_CP_RB_CNTL));
    uint32_t init[16];
    const unsigned n = A630CpInitWords(init);
    if (!cpRun(init, n, "S7 cp_init")) return false;
    cpRow("S7 cp_init done");
    // First real command: CP_MEM_WRITE of a pattern into the scratch page.
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gCpu[kScratch]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t mw[4] = {A630Pkt7(A630CpMemWrite, 3), (uint32_t)gBuffers[kScratch].iova, (uint32_t)(gBuffers[kScratch].iova >> 32),
                            0x5e9a630};
    if (!cpRun(mw, 4, "S7 mem_write")) return false;
    gpuLog("S7 mem_write scratch %x (want 5e9a630): %s", scratch[0], scratch[0] == 0x5e9a630 ? "CP OK" : "MISMATCH");
    cpRow("S7 end");
    return scratch[0] == 0x5e9a630;
}

// ---- zap shader (adreno_zap_shader_load / qcom_mdt_load / qcom_scm, after sheng's GpuPowerProbe) ------------------------
struct ScmRes {int64_t a0; uint64_t a1; uint32_t ms, resumes;};
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2)
{
    uint64_t first = fn, a6 = 0;
    const uint64_t start = nowUs();
    for (unsigned resume = 0, busy = 0; resume < 10000;) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = 0, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}               // QCOM_SCM_INTERRUPTED
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IOSleep(30); continue;}   // QCOM_SCM_V2_EBUSY
        return ScmRes{int64_t(x0), x1, uint32_t((nowUs() - start) / 1000), resume};
    }
    return ScmRes{-1000, 0, uint32_t((nowUs() - start) / 1000), 10000};
}
constexpr uint64_t kScmPil(uint32_t cmd) {return 0x42000000u | (0x02u << 8) | cmd;}   // ARM_SMCCC STD, SMC64, SIP, PIL svc
constexpr uint32_t kGpuPasId = 13;
constexpr uint64_t kZapPhys = 0x8c415000, kZapBytes = 0x2000;   // sdm845.dtsi gpu_mem (no-map), below XNU's memory
NativeAdrenoMemory gZapMetadata;
uint32_t rd32le(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}
uint16_t rd16le(const uint8_t *p) {return uint16_t(p[0] | p[1] << 8);}

bool stage8()
{
    // fajita-gpu-zap: 0 skip (stays in secure mode), 1 init_image only, 2 + mem_setup and copy, 3 + auth_and_reset, 4 (default)
    // + CP_SET_SECURE_MODE 0.
    const unsigned limit = bootArg("fajita-gpu-zap", 4);
    if (!limit) {gpuLog("S8 zap skipped (fajita-gpu-zap=0)"); return true;}
    const uint8_t *fw = &fajita_gpu_zap_fw_size ? fajita_gpu_zap_fw : nullptr;
    const uint64_t size = fw ? fajita_gpu_zap_fw_size : 0;
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) {gpuLog("S8 nozap"); return false;}
    const uint32_t phoff = rd32le(fw + 28);
    const unsigned phent = rd16le(fw + 42), phnum = rd16le(fw + 44);
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) {gpuLog("S8 bad phdrs"); return false;}
    auto ph = [&](unsigned i, unsigned field) {return rd32le(fw + phoff + i * 32 + field * 4);};   // type off va pa filesz memsz flags
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    if (ph(0, 0) == 1 || !hash) {gpuLog("S8 no hash segment"); return false;}
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    if (lo >= hi || hi - lo > kZapBytes) {gpuLog("S8 segments %llx..%llx do not fit", (unsigned long long)lo, (unsigned long long)hi); return false;}
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) {gpuLog("S8 metadata too large"); return false;}
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    if (gZapMetadata.allocate(metaBytes, 40, true, metaBytes) || gZapMetadata.spanCount() != 1) {gpuLog("S8 metadata alloc"); return false;}
    uint8_t *meta = static_cast<uint8_t *>(gZapMetadata.mapping());
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    if (gZapMetadata.publishForDevice(0, metaBytes, true)) {gpuLog("S8 metadata publish"); return false;}
    // qcom_scm_pas_init_image: ARGS(2, VAL, RW)
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, gZapMetadata.spans()[0].physical, 0);
    gpuLog("S8 init_image a0 %lld a1 %llx %u ms %u resumes (relocatable %d, %llx..%llx)", (long long)r.a0,
           (unsigned long long)r.a1, r.ms, r.resumes, relocate, (unsigned long long)lo, (unsigned long long)hi);
    if (r.a0 || r.a1 || limit < 2) return !r.a0 && !r.a1;
    if (relocate) {
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);   // qcom_scm_pas_mem_setup: ARGS(3)
        gpuLog("S8 mem_setup a0 %lld a1 %llx %u ms", (long long)r.a0, (unsigned long long)r.a1, r.ms);
        if (r.a0 || r.a1) return false;
    }
    {
        IODeviceMemory *m = IODeviceMemory::withRange(kZapPhys, kZapBytes);
        OSSharedPtr<IOMemoryMap> map;
        if (m) {map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
        if (!map) {gpuLog("S8 gpu_mem map failed"); return false;}
        volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        const uint64_t base = relocate ? lo : kZapPhys;
        for (unsigned i = 0; i < phnum; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t at = uint64_t(ph(i, 3)) - base, file = ph(i, 4), memsz = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || at + memsz > kZapBytes || file > memsz || off + file > size) {gpuLog("S8 segment %u out of range", i); return false;}
            for (uint64_t b = 0; b < file; ++b) region[at + b] = fw[off + b];
            for (uint64_t b = file; b < memsz; ++b) region[at + b] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    if (limit < 3) return true;
    r = scmCall(kScmPil(0x05), 1, kGpuPasId, 0, 0);   // qcom_scm_pas_auth_and_reset: ARGS(1)
    gpuLog("S8 auth_and_reset a0 %lld a1 %llx %u ms %u resumes", (long long)r.a0, (unsigned long long)r.a1, r.ms, r.resumes);
    if (r.a0 || r.a1 || limit < 4) return !r.a0 && !r.a1;
    const uint32_t secure[2] = {A630Pkt7(A630CpSetSecureMode, 1), 0};
    if (!cpRun(secure, 2, "S8 secure_mode")) return false;
    gpuLog("S8 CP left secure mode; trust cntl %08x", gpuRd(A630_RBBM_SECVID_TRUST_CNTL));
    return true;
}

// ---- interrupt --------------------------------------------------------------------------------------------------------
volatile uint32_t gIrqCount, gIrqSeen, gIrqEmpty;
volatile uint32_t gIrqError, gIrqHwFault, gIrqRptr;
void gpuIrq(void *)
{
    const uint32_t status = gpuRd(A630_RBBM_INT_0_STATUS);
    if (status) gpuWr(A630_RBBM_INT_CLEAR_CMD, status);
    gIrqSeen = gIrqSeen | status;
    // Anything but CP_CACHE_FLUSH_TS (bit 20) is an error source: keep the first one with the CP's fault registers (kcmd gpu).
    if ((status & ~0x100000u) && !gIrqError) {
        gIrqError = status;
        gIrqHwFault = gpuRd(A630_CP_HW_FAULT);
        gIrqRptr = gpuRd(A630_CP_RB_RPTR);
    }
    gIrqCount = gIrqCount + 1;
    gIrqEmpty = status ? 0 : gIrqEmpty + 1;
    if (gIrqEmpty > 100000) gpuWr(A630_RBBM_INT_0_MASK, 0);   // a storm: mask the GPU instead of hanging the CPU
    thread_wakeup(event_t(&gIrqCount));
}

bool stage9()
{
    // CP_MEMCPY of 1023 SQE dwords to scratch+4, then compare.
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gCpu[kScratch]);
    for (unsigned i = 1; i < 1024; ++i) scratch[i] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    const uint64_t src = gBuffers[kSqe].iova, dst = gBuffers[kScratch].iova + 4;
    const uint32_t copy[7] = {A630Pkt7(A630CpMemcpy, 5), 1023, (uint32_t)src, (uint32_t)(src >> 32), (uint32_t)dst,
                              (uint32_t)(dst >> 32), A630Pkt7(A630CpWaitMemWrites, 0)};
    int matched = -1;
    if (cpRun(copy, 7, "S9 memcpy")) {
        const volatile uint32_t *sqe = reinterpret_cast<const volatile uint32_t *>(gCpu[kSqe]);
        matched = 0;
        for (unsigned i = 0; i < 1023; ++i) matched += scratch[1 + i] == sqe[i];
    }
    // SPI 300 (sdm845.dtsi gpu interrupts, level high) = INTID 332.
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    const IOReturn a = armcpu_spi_attach ? armcpu_spi_attach(332, false, gpuIrq, nullptr, &readback, &router) : kIOReturnUnsupported;
    IOSleep(20);
    const uint32_t before = gIrqCount;
    const uint32_t nop = A630Pkt7(A630CpNop, 0);
    const bool fenced = cpRun(&nop, 1, "S9 nop");
    IOSleep(20);
    gpuLog("S9 memcpy matched %d/1023; irq attach %x readback %x router %llx count %u>%u seen %x status %x fence %d", matched, a,
           readback, (unsigned long long)router, before, gIrqCount, gIrqSeen, gpuRd(A630_RBBM_INT_0_STATUS), fenced);
    return matched == 1023;
}

// ---- stage 10: publish the user-client service (NativeAdrenoGpuPublished) --------------------------------------------
// The device info the userland agent's Turnip backend reads (NativeAdrenoABI.h v1): the A630 identity and the render
// server's GPU address window. chip_id is the msm/kgsl form 0x06030002 (not the GMU-massaged value); gmem is 1 MiB at
// 0x100000 (= UCHE_GMEM_RANGE_MIN); the UBWC fields match A630Gpu.c's a6xx_calc_ubwc_config defaults; flags are IOVA-only
// (A630 BOs are not IO-coherent). The window is 1 GiB at 0x20000000, inside the 2 GiB GPU context aperture and clear of the
// GMEM window at 1-2 MiB.
static NativeAdrenoSubmit gSubmit;
static NativeAdrenoTimer gTimer;
static NativeAdrenoTimeIo gTimerIo;
static int timerRead(void *, uint32_t reg, uint32_t *value) {*value = gpuRd(reg); return 0;}
static const NativeAdrenoAbiDeviceInfo gDeviceInfo = {
    0x06030002u,            // chip_id (msm/kgsl form)
    0x100000u,              // gmem_bytes (1 MiB)
    0x100000ull,            // gmem_base (= UCHE_GMEM_RANGE_MIN)
    0x1fffffffff000ull,     // uche_trap_base
    0x20000000ull,          // va_start (render server window)
    0x40000000ull,          // va_size (1 GiB, inside the 2 GiB aperture)
    15u,                    // highest_bank_bit
    6u,                     // ubwc_swizzle (levels 1+2)
    0u,                     // macrotile_mode (four channel)
    2u,                     // ubwc_version (UBWC 2.0)
    NativeAdrenoDeviceClientIova,
    0u};
bool stage10()
{
    if (!native_adreno_publish) {gpuLog("S10 nolink"); return false;}
    // fajita-gpu-import-skip (default 0; session-225259: 1 corrupted the lock-screen wallpaper): no publish or acquire of GPU-written imports.
    if (&native_adreno_skip_written_imports) native_adreno_skip_written_imports = bootArg("fajita-gpu-import-skip", 0);
    // fajita-gpu-ro-skip (default 1 since session-232253, screens identical to all-off): no per-submission maintenance of read-only imports either; the native Metal frontend cleans the
    // imports whose surfaces changed at commit (NativeBridgeMetalFrontend.mm flushImports). Only with a plugin that does.
    if (&native_adreno_skip_readonly_imports) native_adreno_skip_readonly_imports = bootArg("fajita-gpu-ro-skip", 1);
    // fajita-gpu-wpub-skip (default 1, session-232253: publish 22 -> 0.2 ms, screens identical): no publish of GPU-written imports either (the frontend cleans changed imports); acquire stays.
    if (&native_adreno_skip_written_publish) native_adreno_skip_written_publish = bootArg("fajita-gpu-wpub-skip", 1);
    // fajita-gpu-uncached-skip (default 1): publishing an import skips its pages that no CPU mapping caches (NativeAdrenoMemory.cpp).
    if (&native_adreno_page_uncached) native_adreno_page_uncached = bootArg("fajita-gpu-uncached-skip", 1) ? pageUncached : nullptr;
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    if (!vm) {gpuLog("S10 no vm"); return false;}
    const IOReturn attached = gSubmit.attach(vm, &gRing);
    if (attached) {gpuLog("S10 submit attach %x", attached); return false;}
    gTimerIo = NativeAdrenoTimeIo{nullptr, timerRead};
    const IOReturn timer = gTimer.attach(&gTimerIo, NativeAdrenoTimeCp);
    // input_bits 32 (the GPU SMMU context is a 32-bit input lower region over the 2 GiB aperture); output_bits from ID2.OAS
    // (36 on this SMMU, reported by the context bring-up).
    const uint32_t output_bits = gGpuCtx.output_bits ? gGpuCtx.output_bits : 40;
    const IOReturn published = native_adreno_publish(vm, &gSubmit, &gRing, &kClock, timer ? nullptr : &gTimer, &gDeviceInfo,
                                                     32, output_bits);
    uint32_t closed = 0, opened = native_adreno_publish_stats ? native_adreno_publish_stats(&closed) : 0;
    gpuLog("S10 publish %x timer %x output_bits %u opened %u closed %u", published, timer, output_bits, opened, closed);
    if (!published) gpuLog("S10 NativeAdrenoGpu service registered (open type 0); userland may probe it");
    return published == kIOReturnSuccess;
}

// Stage 11: Metal's discovery (Metal _MTLRegisterDevices: IOServiceMatching("IOAcceleratorES"), MetalPluginName) finds the staged
// AGXAccelerator. Its base is bound statically to the 20H392 IOGPUFamily addresses, so before its initializer runs the bound
// metaclass must be the live IOAcceleratorES one: same object, 136 bytes, IOService parent. Anything else refuses.
const OSMetaClass *metaNamed(const char *name)
{
    const OSSymbol *symbol = OSSymbol::withCString(name);
    const OSMetaClass *meta = symbol ? OSMetaClass::getMetaClassWithName(symbol) : nullptr;
    if (symbol) symbol->release();
    return meta;
}
bool stage11()
{
    if (!fajita_agx_static_init || !native_adreno_accelerator_allocate || !fajita_bound_ioacceleratores_meta) {gpuLog("S11 nolink"); return false;}
    const OSMetaClass *live = metaNamed("IOAcceleratorES");
    if (!live) {
        const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
        live = metaNamed("IOAcceleratorES");
        gpuLog("S11 IOGPUFamily load %x meta %p", loaded, live);
    }
    const void *bound = fajita_bound_ioacceleratores_meta;
    const unsigned size = live ? live->getClassSize() : 0;
    const OSMetaClass *parent = live ? live->getSuperClass() : nullptr;
    gpuLog("S11 IOAcceleratorES live %p bound %p size %u parent %s", live, bound, size,
           parent ? parent->getClassName() : "-");
    if (!live || (const void *)live != bound || size != 136 || parent != IOService::metaClass) {gpuLog("S11 base mismatch: refused"); return false;}
    // The initializer constructs AGXAccelerator's metaclass. Outside a kext load OSMetaClass would leave it unregistered (no
    // preModLoad: "wasn't called for class"), so run it as a load of the kernel's own classes, as OSKext does for a kext.
    static bool constructed;
    if (!constructed) {
        void *load = OSMetaClass::preModLoad("__kernel__");
        fajita_agx_static_init();
        const OSReturn registered = OSMetaClass::postModLoad(load);
        constructed = true;
        gpuLog("S11 staged initializer ran, metaclass registration %x", registered);
        if (registered) return false;
    }
    const OSMetaClass *agx = metaNamed("AGXAccelerator");
    if (!agx || agx->getSuperClass() != live) {gpuLog("S11 AGXAccelerator metaclass %p not under IOAcceleratorES", agx); return false;}
    IOService *accelerator = native_adreno_accelerator_allocate();
    OSDictionary *properties = OSDictionary::withCapacity(2);
    const OSString *plugin = OSString::withCString("NativeAdrenoMetal");
    if (properties && plugin) properties->setObject("MetalPluginName", plugin);
    const bool made = accelerator && properties && plugin && accelerator->init(properties) && accelerator->attach(IOService::getPlatform());
    if (plugin) plugin->release();
    if (properties) properties->release();
    if (!made) {gpuLog("S11 accelerator init/attach failed"); if (accelerator) accelerator->release(); return false;}
    accelerator->setName("AGXAccelerator");
    accelerator->registerService();
    gpuLog("S11 AGXAccelerator registered (IOAcceleratorES, MetalPluginName NativeAdrenoMetal, open type 0x4147)");
    return true;
}

// ---- runner -------------------------------------------------------------------------------------------------------------
bool (*const kStages[])() = {nullptr, stage1, stage2, stage3, stage4, stage5, stage6, stage7, stage8, stage9, stage10, stage11};
constexpr unsigned kLastStage = sizeof kStages / sizeof *kStages - 1;
volatile uint32_t gTarget;


// ---- Clock governor (fajita-gpu-gov, default 1): GPU DCVS level and the DDR/LLCC vote follow activity, tsens caps them ----
// Every 20 ms: GPU busy cycles (RBBM counter 1 = PERF_RBBM_STATUS_MASKED) and the GPU hand-off frames. Any activity: the top GPU
// level (Linux's sdm845 OPPs: index gpu_count-1 = 710 MHz) and the bus at Linux's A630 vote (7216 MB/s); 1 s without either: the
// lowest GPU OPP (index 1, 257 MHz) and the keepalive bus vote. Every second: tsens0/1 (sdm845.dtsi, v2 TM_Sn_STATUS at 0xa0, last
// temperature in deci-degC); at 80 C the level is capped at index 4 (520 MHz), at 84 C at index 1 (gpu-top-thermal is passive at 85 C,
// cpu0-thermal at 90 C). kcmd gpu gov shows the state; every change reads back (DCVS return, BCM echo).
extern "C" int fajita_bus_set(unsigned peakMBps) __attribute__((weak));
extern "C" char fajita_bus_row[256] __attribute__((weak));
struct Gov {
    uint32_t level, cap, bus, changes, busErrors, maxTempDeci, hotZone, ticks;
    uint64_t lastActiveUs, busyLast, framesLast;
    uint32_t dcvsRet;
    volatile uint32_t running, enabled;
};
Gov gGov;
volatile uint32_t *gTsens[2];
const unsigned kTsensCount[2] = {13, 8};
OSSharedPtr<IOMemoryMap> gTsensMap[2];
int32_t tsensMaxDeci(uint32_t *zone) {
    static const uint64_t base[2] = {0x0c263000, 0x0c265000};
    int32_t best = -1000;
    for (unsigned b = 0; b < 2; ++b) {
        if (!gTsens[b]) {
            IODeviceMemory *m = IODeviceMemory::withRange(base[b], 0x1000);
            if (m) {gTsensMap[b] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain); m->release();}
            if (!gTsensMap[b]) continue;
            gTsens[b] = reinterpret_cast<volatile uint32_t *>(gTsensMap[b]->getVirtualAddress());
        }
        for (unsigned n = 0; n < kTsensCount[b]; ++n) {
            const uint32_t v = gTsens[b][(0xa0 + 4 * n) / 4];
            if (!(v & (1u << 21))) continue;                                   // VALID
            const int32_t t = int32_t(v << 20) >> 20;                          // LAST_TEMP 11:0, signed deci-degC
            if (t > best) {best = t; if (zone) *zone = b * 16 + n;}
        }
    }
    return best;
}
void govSetLevel(unsigned level) {
    if (level == gGov.level) return;
    uint32_t ret = 0;
    if (A630SetFreq(&gIo, level, &ret) == A630Ok) {gGov.level = level; gGov.dcvsRet = ret; ++gGov.changes;}
}
void govSetBus(unsigned mbps) {
    if (!fajita_bus_set || mbps == gGov.bus) return;
    if (fajita_bus_set(mbps)) ++gGov.busErrors; else gGov.bus = mbps;
}
void govThread(void *, wait_result_t)
{
    const unsigned top = gPerf.gpu_count - 1, low = 1;
    gGov.level = top; gGov.cap = top; gGov.bus = 7216; gGov.lastActiveUs = nowUs();
    gpuWr(0x0507 + 1, 6);   // RBBM_PERFCTR_RBBM_SEL_1 = PERF_RBBM_STATUS_MASKED (kcmd gpu perf selects the same)
    auto busy = [] {return uint64_t(gpuRd(0x041c + 3)) << 32 | gpuRd(0x041c + 2);};
    gGov.busyLast = busy();
    gGov.framesLast = &native_gop_gpu_offers ? native_gop_gpu_offers : 0;
    for (;;) {
        IOSleep(20);
        if (!gGov.enabled) continue;
        ++gGov.ticks;
        const uint64_t now = nowUs(), b = busy(), f = &native_gop_gpu_offers ? native_gop_gpu_offers : 0;
        const bool active = b != gGov.busyLast || f != gGov.framesLast;
        gGov.busyLast = b; gGov.framesLast = f;
        if (!(gGov.ticks % 50)) {
            uint32_t zone = 0;
            const int32_t t = tsensMaxDeci(&zone);
            gGov.maxTempDeci = uint32_t(t); gGov.hotZone = zone;
            const unsigned cap = t >= 840 ? low : t >= 800 ? (top > 4 ? 4u : top) : top;
            if (cap != gGov.cap) {gGov.cap = cap; gpuLog("gov: %d.%d C (zone %u): GPU level capped at %u", t / 10, t % 10, zone, cap);}
            if (!(gGov.ticks % 500)) gpuLog("gov: level %u bus %u max %d.%d C zone %u changes %u bus-errors %u", gGov.level, gGov.bus, t / 10, t % 10,
                                            zone, gGov.changes, gGov.busErrors);
        }
        if (active) {
            gGov.lastActiveUs = now;
            govSetLevel(gGov.cap);
            govSetBus(7216);
        } else if (now - gGov.lastActiveUs > 1000000) {
            govSetLevel(low < gGov.cap ? low : gGov.cap);
            govSetBus(0);
        }
        if (gGov.level > gGov.cap) govSetLevel(gGov.cap);
    }
}
void govStart()
{
    if (gGov.running || !bootArg("fajita-gpu-gov", 1)) return;
    gGov.running = 1; gGov.enabled = 1;
    thread_t t = nullptr;
    if (kernel_thread_start(govThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t); else gGov.running = 0;
}

void runThread(void *, wait_result_t)
{
    const unsigned target = gTarget;
    if (!mapAll()) {fajita_gpu_failed = 1; fajita_gpu_running = 0; return;}
    while (fajita_gpu_stage < target && !fajita_gpu_failed) {
        const unsigned s = fajita_gpu_stage + 1;
        gpuLog("stage %u start", s);
        const uint64_t t0 = nowUs();
        const bool ok = kStages[s]();
        gpuLog("stage %u %s (%llu ms)", s, ok ? "passed" : "FAILED", (unsigned long long)((nowUs() - t0) / 1000));
        if (!ok) {fajita_gpu_failed = (int32_t)s; break;}
        fajita_gpu_stage = s;
    }
    if (fajita_gpu_stage >= 10) govStart();
    fajita_gpu_running = 0;
}

bool start(unsigned target)
{
    if (target > kLastStage) target = kLastStage;
    if (__atomic_exchange_n(&fajita_gpu_running, target, __ATOMIC_ACQ_REL)) return false;
    gTarget = target;
    thread_t t = nullptr;
    if (kernel_thread_start(runThread, nullptr, &t) != KERN_SUCCESS) {fajita_gpu_running = 0; return false;}
    thread_deallocate(t);
    return true;
}

size_t appendf(char *out, size_t cap, size_t at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
size_t appendf(char *out, size_t cap, size_t at, const char *fmt, ...)
{
    if (at >= cap) return at;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + at, cap - at, fmt, ap);
    va_end(ap);
    if (n < 0) return at;
    return at + ((size_t)n < cap - at ? (size_t)n : cap - at - 1);
}



// ---- kcmd gpu perf [MS]: a6xx performance counters around a window (fdperf's direct MMIO selection; a6xx.xml / a6xx_perfcntrs.xml) ----
// CP counter 0 stays PERF_CP_ALWAYS_COUNT (hw_init). Each entry: select register, countable, counter (LO; HI follows).
struct PerfSel {const char *name; uint32_t sel, countable, lo;};
const PerfSel kPerf[] = {
    {"cp_busy", 0x08d0 + 1, 2, 0x0400 + 2 * 1},          // PERF_CP_BUSY_CYCLES
    {"cp_busy_core_idle", 0x08d0 + 2, 1, 0x0400 + 2 * 2}, // PERF_CP_BUSY_GFX_CORE_IDLE
    {"rbbm_always", 0x0507 + 0, 0, 0x041c + 2 * 0},       // PERF_RBBM_ALWAYS_COUNT
    {"gpu_busy", 0x0507 + 1, 6, 0x041c + 2 * 1},          // PERF_RBBM_STATUS_MASKED
    {"sp_busy", 0xae10 + 0, 0, 0x04a6 + 2 * 0},           // PERF_SP_BUSY_CYCLES
    {"sp_alu", 0xae10 + 1, 1, 0x04a6 + 2 * 1},            // PERF_SP_ALU_WORKING_CYCLES
    {"sp_stall_tp", 0xae10 + 2, 4, 0x04a6 + 2 * 2},       // PERF_SP_STALL_CYCLES_TP
    {"sp_wave_idle", 0xae10 + 3, 22, 0x04a6 + 2 * 3},     // PERF_SP_WAVE_IDLE_CYCLES
    {"tp_busy", 0xb610 + 0, 0, 0x048e + 2 * 0},           // PERF_TP_BUSY_CYCLES
    {"tp_stall_uche", 0xb610 + 1, 1, 0x048e + 2 * 1},     // PERF_TP_STALL_CYCLES_UCHE
    {"tp_l1_req", 0xb610 + 2, 6, 0x048e + 2 * 2},         // PERF_TP_L1_CACHELINE_REQUESTS
    {"tp_l1_miss", 0xb610 + 3, 7, 0x048e + 2 * 3},        // PERF_TP_L1_CACHELINE_MISSES
    {"uche_busy", 0x0e1c + 0, 0, 0x0476 + 2 * 0},         // PERF_UCHE_BUSY_CYCLES
    {"uche_vbif_lat_cyc", 0x0e1c + 1, 2, 0x0476 + 2 * 1}, // PERF_UCHE_VBIF_LATENCY_CYCLES
    {"uche_vbif_lat_n", 0x0e1c + 2, 3, 0x0476 + 2 * 2},   // PERF_UCHE_VBIF_LATENCY_SAMPLES
    {"uche_stall_arb", 0x0e1c + 3, 1, 0x0476 + 2 * 3},    // PERF_UCHE_STALL_CYCLES_ARBITER
};
size_t perfCmd(const char *args, char *out, size_t cap)
{
    if (!gGxOn || !A630GxOn(&gIo)) return appendf(out, cap, 0, "gx off\n");
    unsigned ms = 1000;
    while (*args == ' ') ++args;
    if (*args >= '0' && *args <= '9') {ms = 0; while (*args >= '0' && *args <= '9') ms = ms * 10 + unsigned(*args++ - '0');}
    if (ms > 5000) ms = 5000;
    const unsigned n = sizeof kPerf / sizeof *kPerf;
    uint64_t a[n], b[n];
    for (unsigned i = 0; i < n; ++i) gpuWr(kPerf[i].sel, kPerf[i].countable);
    __asm__ volatile("dsb sy" ::: "memory");
    IODelay(10);
    auto rd64 = [](uint32_t lo) {uint32_t h0 = gpuRd(lo + 1), l = gpuRd(lo), h1 = gpuRd(lo + 1); if (h0 != h1) l = gpuRd(lo); return uint64_t(h1) << 32 | l;};
    for (unsigned i = 0; i < n; ++i) a[i] = rd64(kPerf[i].lo);
    const uint64_t t0 = nowUs();
    IOSleep(ms);
    for (unsigned i = 0; i < n; ++i) b[i] = rd64(kPerf[i].lo);
    const uint64_t t1 = nowUs();
    size_t w = appendf(out, cap, 0, "window %llu us\n", (unsigned long long)(t1 - t0));
    for (unsigned i = 0; i < n; ++i) w = appendf(out, cap, w, "%s %llu\n", kPerf[i].name, (unsigned long long)(b[i] - a[i]));
    return w;
}

size_t kcmd(const char *args, size_t len, char *out, size_t cap)
{
    char a[32] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    size_t w = 0;
    if (!strncmp(a, "log", 3)) {
        // the newest cap bytes of the log
        const uint32_t n = fajita_gpu_text_len;
        const uint32_t from = n > cap - 1 ? n - uint32_t(cap - 1) : 0;
        memcpy(out, fajita_gpu_text + from, n - from);
        return n - from;
    }
    if (!strncmp(a, "perf", 4)) return perfCmd(a + 4, out, cap);
    if (!strncmp(a, "gov", 3)) {   // gov [on|off]: the clock governor's state (off: it stops changing anything)
        if (!strncmp(a + 3, " on", 3)) gGov.enabled = 1; else if (!strncmp(a + 3, " off", 4)) gGov.enabled = 0;
        uint32_t z = 0;
        const int32_t t = tsensMaxDeci(&z);
        return appendf(out, cap, 0, "gov %s level %u (%u kHz) cap %u bus %u MB/s changes %u bus-errors %u dcvs-ret %x | max %d.%d C zone %u | idle %llu ms\n%s\n",
                       gGov.enabled ? "on" : "off", gGov.level, gPerf.gpu_khz[gGov.level < A630MaxGpuLevels ? gGov.level : 0], gGov.cap, gGov.bus,
                       gGov.changes, gGov.busErrors, gGov.dcvsRet, t / 10, t % 10, z,
                       (unsigned long long)((nowUs() - gGov.lastActiveUs) / 1000), &fajita_bus_row ? fajita_bus_row : "");
    }
    if (!strncmp(a, "freq ", 5)) {   // DCVS: the GPU perf index (0..gpu_count-1) through the GMU's legacy DCVS OOB, as Linux's set_freq
        unsigned idx = 0;
        for (const char *p = a + 5; *p >= '0' && *p <= '9'; ++p) idx = idx * 10 + unsigned(*p - '0');
        if (fajita_gpu_stage < 6 || idx >= gPerf.gpu_count) return appendf(out, cap, 0, "no (stage %u, levels %u)\n", (unsigned)fajita_gpu_stage, gPerf.gpu_count);
        uint32_t ret = 0;
        const A630Status st = A630SetFreq(&gIo, idx, &ret);
        return appendf(out, cap, 0, "freq index %u (%u kHz): %s ret %x\n", idx, gPerf.gpu_khz[idx], statusName(st), ret);
    }
    if (!strncmp(a, "membw", 5)) {   // CPU copy throughput (a DDR clock proxy): 4 x 16 MiB memcpy and a 16 MiB read sweep
        const size_t n = 16u << 20;
        void *x = IOMallocData(n), *y = IOMallocData(n);
        if (!x || !y) {if (x) IOFreeData(x, n); if (y) IOFreeData(y, n); return appendf(out, cap, 0, "no memory\n");}
        memset(x, 1, n); memset(y, 2, n);
        const uint64_t t0 = nowUs();
        for (int i = 0; i < 4; ++i) memcpy(i & 1 ? x : y, i & 1 ? y : x, n);
        const uint64_t t1 = nowUs();
        uint64_t sum = 0;
        for (size_t i = 0; i < n / 8; i += 8) sum += static_cast<volatile uint64_t *>(x)[i];
        const uint64_t t2 = nowUs();
        IOFreeData(x, n); IOFreeData(y, n);
        return appendf(out, cap, 0, "memcpy 64 MiB in %llu us = %llu MB/s; read 16 MiB in %llu us = %llu MB/s (%llx)\n",
                       (unsigned long long)(t1 - t0), (unsigned long long)(64ull * 1048576 / (t1 - t0 ? t1 - t0 : 1)),
                       (unsigned long long)(t2 - t1), (unsigned long long)(16ull * 1048576 / (t2 - t1 ? t2 - t1 : 1)), (unsigned long long)(sum & 0xf));
    }
    if (!strncmp(a, "set", 3)) {
        // A/B in one boot: "set import-skip|ro-skip|uncached-skip 0|1" changes the publish policy of the next submissions.
        const char *v = a + 3;
        while (*v == ' ') ++v;
        const char *name = v;
        while (*v && *v != ' ') ++v;
        const size_t nl = size_t(v - name);
        while (*v == ' ') ++v;
        const unsigned on = *v == '1';
        if (nl == 11 && !strncmp(name, "import-skip", nl) && &native_adreno_skip_written_imports) native_adreno_skip_written_imports = on;
        else if (nl == 7 && !strncmp(name, "ro-skip", nl) && &native_adreno_skip_readonly_imports) native_adreno_skip_readonly_imports = on;
        else if (nl == 13 && !strncmp(name, "uncached-skip", nl) && &native_adreno_page_uncached) native_adreno_page_uncached = on ? pageUncached : nullptr;
        else if (nl == 8 && !strncmp(name, "uche-inv", nl) && &native_adreno_uche_invalidate) native_adreno_uche_invalidate = on;
        else if (nl == 9 && !strncmp(name, "uacq-skip", nl) && &native_adreno_uncached_acquire_skip) native_adreno_uncached_acquire_skip = on;
        else if (nl == 9 && !strncmp(name, "wpub-skip", nl) && &native_adreno_skip_written_publish) native_adreno_skip_written_publish = on;
        else if (nl == 5 && !strncmp(name, "trace", nl) && &native_adreno_trace) native_adreno_trace = int(*v - '0');
        else if (nl) return appendf(out, cap, 0, "usage: gpu set import-skip|ro-skip|wpub-skip|uacq-skip|uncached-skip|trace 0|1\n");
        return appendf(out, cap, 0, "uche-inv %u trace %d uacq-skip %u wpub-skip %u import-skip %u ro-skip %u uncached-skip %u\n", &native_adreno_uche_invalidate ? (unsigned)native_adreno_uche_invalidate : 9u,
                       &native_adreno_trace ? (int)native_adreno_trace : 9,
                       &native_adreno_uncached_acquire_skip ? (unsigned)native_adreno_uncached_acquire_skip : 9u,
                       &native_adreno_skip_written_publish ? (unsigned)native_adreno_skip_written_publish : 9u,
                       &native_adreno_skip_written_imports ? (unsigned)native_adreno_skip_written_imports : 9u,
                       &native_adreno_skip_readonly_imports ? (unsigned)native_adreno_skip_readonly_imports : 9u,
                       &native_adreno_page_uncached ? (unsigned)(native_adreno_page_uncached != nullptr) : 9u);
    }
    if (!strncmp(a, "raw", 3)) {
        // For the host sampler: "raw [FROM]" = now and the cumulative counters in us (one line), then each submission after FROM
        // still in the 64-entry ring as "n start total publish done acquire" (us; done = submit end to the release that saw it complete). Counters: n retire wc publish tlb ring total max | imported owned
        // (bytes) | uncached-skipped bytes | handoff offers done | display copies.
        mach_timebase_info_data_t tb; clock_timebase_info(&tb);
        auto us = [&](uint64_t ticks) {return (unsigned long long)(ticks * tb.numer / tb.denom / 1000);};
        unsigned long long from = 0;
        for (const char *p = a + 3; *p; ++p) if (*p >= '0' && *p <= '9') from = from * 10 + unsigned(*p - '0');
        const uint64_t n = &native_adreno_submit_time ? native_adreno_submit_time[0] : 0;
        w = appendf(out, cap, w, "now %llu n %llu", us(mach_absolute_time()), (unsigned long long)n);
        for (unsigned i = 1; i < 8 && &native_adreno_submit_time; ++i) w = appendf(out, cap, w, " %llu", us(native_adreno_submit_time[i]));
        w = appendf(out, cap, w, " | %llu %llu | %llu | %llu %llu | %llu | %llu %llu\n",
                    (unsigned long long)(&native_adreno_maint_stats ? native_adreno_maint_stats[0] : 0),
                    (unsigned long long)(&native_adreno_maint_stats ? native_adreno_maint_stats[1] : 0),
                    (unsigned long long)(&native_adreno_uncached_skipped ? native_adreno_uncached_skipped : 0),
                    (unsigned long long)(&native_gop_gpu_offers ? native_gop_gpu_offers : 0),
                    (unsigned long long)(&native_gop_gpu_completions ? native_gop_gpu_completions : 0),
                    (unsigned long long)(&native_legacy_copy_n ? native_legacy_copy_n : 0),
                    (unsigned long long)(&native_adreno_acquire_stats ? native_adreno_acquire_stats[0] : 0),
                    (unsigned long long)(&native_adreno_acquire_stats ? native_adreno_acquire_stats[1] : 0));
        if (&native_adreno_submit_ring) {
            uint64_t k = n > 64 ? n - 64 : 0;
            if (from > k) k = from;
            for (; k < n && w + 64 < cap; ++k) {
                const volatile uint64_t *e = native_adreno_submit_ring[k & 63];
                w = appendf(out, cap, w, "%llu %llu %llu %llu %llu %llu\n", (unsigned long long)(k + 1), us(e[0]), us(e[1]), us(e[2]), us(e[3]),
                            us(e[4]));
            }
        }
        return w;
    }
    if (!strncmp(a, "retry", 5)) {   // run the failed stage again (every stage starts from its own register state)
        if (fajita_gpu_running) return appendf(out, cap, 0, "busy\n");
        const int f = fajita_gpu_failed;
        fajita_gpu_failed = 0;
        return appendf(out, cap, 0, "cleared failure of stage %d\n", f);
    }
    if (!strncmp(a, "regs", 4)) {
        if (!mapAll()) return appendf(out, cap, 0, "map failed\n");
        w = appendf(out, cap, w, "gcc ahb %08x memnoc %08x axi %08x; cx %08x gx %08x gmuclk %08x\n", ioRead(nullptr, A630BlockGcc, 0x71004),
                    ioRead(nullptr, A630BlockGcc, 0x7100c), ioRead(nullptr, A630BlockGcc, 0x44038), ioRead(nullptr, A630BlockGpuCc, 0x106c),
                    ioRead(nullptr, A630BlockGpuCc, 0x100c), ioRead(nullptr, A630BlockGpuCc, 0x1098));
        if (fajita_gpu_stage >= 3)
            w = appendf(out, cap, w, "gmu sysreset %x fwinit %x hfi %x sptprac %x pstate %x g2h %x ao %x\n", gmuRd(A630_GMU_CM3_SYSRESET),
                        gmuRd(A630_GMU_CM3_FW_INIT_RESULT), gmuRd(A630_GMU_HFI_CTRL_STATUS), gmuRd(A630_GMU_SPTPRAC_PWR_CLK_STATUS),
                        gmuRd(A630_GPU_GMU_CX_GMU_RPMH_POWER_STATE), gmuRd(A630_GMU_GMU2HOST_INTR_INFO), gmuRd(A630_GMU_AO_HOST_INTERRUPT_STATUS));
        if (gGxOn && A630GxOn(&gIo))
            w = appendf(out, cap, w, "gpu rbbm %08x int %08x hwfault %08x rptr %x wptr %x irqs %u seen %x\n", gpuRd(A630_RBBM_STATUS),
                        gpuRd(A630_RBBM_INT_0_STATUS), gpuRd(A630_CP_HW_FAULT), gpuRd(A630_CP_RB_RPTR), gpuRd(A630_CP_RB_WPTR), gIrqCount, gIrqSeen);
        return w;
    }
    if (a[0] >= '0' && a[0] <= '9') {
        unsigned n = 0;
        for (const char *p = a; *p >= '0' && *p <= '9'; ++p) n = n * 10 + unsigned(*p - '0');
        if (fajita_gpu_failed) return appendf(out, cap, 0, "stage %d failed earlier; reboot to retry\n", (int)fajita_gpu_failed);
        if (n <= fajita_gpu_stage) return appendf(out, cap, 0, "already at stage %u\n", (unsigned)fajita_gpu_stage);
        const unsigned from = fajita_gpu_stage + 1;
        if (!start(n)) return appendf(out, cap, 0, "busy (a stage thread runs to %u)\n", (unsigned)fajita_gpu_running);
        return appendf(out, cap, 0, "running stages %u..%u (gpu log)\n", from, n > kLastStage ? kLastStage : n);
    }
    w = appendf(out, cap, w, "stage %u running %u failed %d status %x iofaults %u\n", (unsigned)fajita_gpu_stage,
                (unsigned)fajita_gpu_running, (int)fajita_gpu_failed, (unsigned)fajita_gpu_status, (unsigned)gIoFaults);
    w = appendf(out, cap, w, "irq count %u seen %x first-error %x hwfault %x rptr %x\n", gIrqCount, gIrqSeen, gIrqError, gIrqHwFault, gIrqRptr);
    if (native_adreno_submit_time && native_adreno_submit_time[0]) {
        mach_timebase_info_data_t tb; clock_timebase_info(&tb);
        const uint64_t n = native_adreno_submit_time[0];
        auto us = [&](uint64_t ticks) {return (unsigned long long)(ticks * tb.numer / tb.denom / 1000 / n);};
        w = appendf(out, cap, w, "submit n%llu avg us: retire %llu wc %llu publish %llu tlb %llu ring %llu total %llu max %llu\n",
                    (unsigned long long)n, us(native_adreno_submit_time[1]), us(native_adreno_submit_time[2]), us(native_adreno_submit_time[3]),
                    us(native_adreno_submit_time[4]), us(native_adreno_submit_time[5]), us(native_adreno_submit_time[6]),
                    (unsigned long long)(native_adreno_submit_time[7] * tb.numer / tb.denom / 1000));
    }
    if (native_adreno_maint_stats && native_adreno_submit_time && native_adreno_submit_time[0]) {
        const uint64_t n = native_adreno_submit_time[0];
        w = appendf(out, cap, w, "publish per submit: imported %llu KiB in %llu, owned %llu KiB in %llu (x100 objects), uncached skipped %llu KiB\n",
                    (unsigned long long)(native_adreno_maint_stats[0] / n >> 10), (unsigned long long)(native_adreno_maint_stats[2] * 100 / n),
                    (unsigned long long)(native_adreno_maint_stats[1] / n >> 10), (unsigned long long)(native_adreno_maint_stats[3] * 100 / n),
                    (unsigned long long)((&native_adreno_uncached_skipped ? native_adreno_uncached_skipped : 0) / n >> 10));
    }
    if (&native_legacy_copy_n && native_legacy_copy_n) {
        mach_timebase_info_data_t tb; clock_timebase_info(&tb);
        w = appendf(out, cap, w, "display copy n%llu avg us %llu max us %llu\n", (unsigned long long)native_legacy_copy_n,
                    (unsigned long long)(native_legacy_copy_sum / native_legacy_copy_n * tb.numer / tb.denom / 1000),
                    (unsigned long long)(native_legacy_copy_max * tb.numer / tb.denom / 1000));
    }
    // The GPU hand-off (sheng-fb-gpu=1): frames offered to the render server's presenter, completed, failed, abandoned, CPU fallbacks.
    if (&native_gop_gpu_offers && native_gop_gpu_offers)
        w = appendf(out, cap, w, "handoff offers %llu done %llu failed %llu abandoned %llu fallback %u last %x\n",
                    (unsigned long long)native_gop_gpu_offers, (unsigned long long)(&native_gop_gpu_completions ? native_gop_gpu_completions : 0),
                    (unsigned long long)(&native_gop_gpu_failures ? native_gop_gpu_failures : 0),
                    (unsigned long long)(&native_gop_gpu_abandoned ? native_gop_gpu_abandoned : 0),
                    &native_legacy_gpu_fallbacks ? native_legacy_gpu_fallbacks : 0u, &native_legacy_gpu_last_status ? native_legacy_gpu_last_status : 0u);
    if (&native_adreno_import_check)
        w = appendf(out, cap, w, "import check %llu unmapped %llu replaced %llu\n", (unsigned long long)native_adreno_import_check[0],
                    (unsigned long long)native_adreno_import_check[1], (unsigned long long)native_adreno_import_check[2]);
    w = appendf(out, cap, w, "usage: gpu [N | log | regs | retry]  (stages 1..%u)\n", kLastStage);
    return w;
}

volatile uint32_t gProbed;
}  // namespace

// Called once a second by the boot progress monitor (package/BootProgress.c).
extern "C" void fajita_gpu_probe(void)
{
    static uint32_t calls;
    if (!gProbed) {
        gProbed = 1;
        gLogLock = IOSimpleLockAlloc();
        if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("gpu", kcmd, "Adreno 630 bring-up: gpu [N|log|regs]");
    }
    const unsigned target = bootArg("fajita-gpu", 0);
    if (!target || fajita_gpu_stage || fajita_gpu_running || fajita_gpu_failed) return;
    if (++calls < bootArg("fajita-gpu-delay", 15)) return;
    start(target);
}
