// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native SDM845 DWC3 USB device-mode bring-up for the OnePlus 6T (fajita).
//
// Default-off: no MMIO is touched unless native-dwc3-probe=1 is present. This is a
// read-only register probe, staged exactly like NativeUfs (drivers/NativeUfs):
//   native-dwc3-probe=1  map GCC + the dwc3 core window UEFI left up, read a
//                        snapshot (GSNPSID/GCTL/DCFG/DSTS/DALEPENA/event buffer)
//                        and the QUSB2 PHY PLL-ready bit. Nothing is written.
//   native-dwc3-probe=2  also compute (read-only) the apps_smmu plan for the dwc3
//                        stream 0x740, the way NativeUfs plans the UFS stream, to
//                        prove the DMA path before any gadget code runs.
// Every stage advances a counter that package/BootLog.c surfaces on the UFS log.
// The CDC-ACM gadget itself (TRB ring, DEPCMDs, enumeration) is stage B and is NOT
// built here; see NOTES-ssh.md. This driver never writes a device-mode register.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <kern/thread.h>
#include <libkern/OSAtomic.h>
extern "C" {
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>
}
#define NATIVE_DWC3_KERNEL 1
#include "Dwc3Registers.h"
#include "Dwc3Phy.h"
#include "../NativeUfs/SmmuTakeover.h"   // generic ARM SMMUv2 stream planner (read-only use here)
#include "Dwc3Gadget.h"                  // stage B: the DWC3 CDC-ACM device controller (usb-debug=N)
#include "AgentBlob.h"                   // generated: the CI-signed fjdbg-agent (empty without FAJITA_USBDBG_AGENT)

// The embedded userland agent, for the kernel's boot-time write into Data (kern_exec.c, usb-debug=3).
extern "C" int native_dwc3_agent_blob(const uint8_t **data, uint64_t *bytes)
{
  if (!data || !bytes || kAgentBlobBytes < 16) return 0;
  *data = kAgentBlob;
  *bytes = kAgentBlobBytes;
  return 1;
}

extern "C" {
// Counters the boot log reads (package/BootLog.c), all weak so a board without the
// log still links. Mirrors NativeUfs's native_ufs_* counter block.
volatile uint32_t native_dwc3_probe_calls = 0;
volatile uint32_t native_dwc3_matched = 0;
volatile uint32_t native_dwc3_mode = 0;      // native-dwc3-probe value actually seen
volatile uint32_t native_dwc3_stage = 0;     // 1 gcc, 2 core snapshot, 3 phy, 4 smmu plan, 5 done
volatile int32_t  native_dwc3_status = -1;    // 0 ok; negative = stage that soft-failed
// Snapshot, for the log.
volatile uint32_t native_dwc3_gsnpsid = 0, native_dwc3_gctl = 0, native_dwc3_gsts = 0;
volatile uint32_t native_dwc3_dcfg = 0, native_dwc3_dctl = 0, native_dwc3_dsts = 0;
volatile uint32_t native_dwc3_dalepena = 0, native_dwc3_devten = 0;
volatile uint32_t native_dwc3_ghwparams0 = 0, native_dwc3_ghwparams1 = 0, native_dwc3_ghwparams3 = 0;
volatile uint32_t native_dwc3_gevntsiz0 = 0, native_dwc3_gevntcount0 = 0;
volatile uint32_t native_dwc3_qusb2_status = 0, native_dwc3_phy_ready = 0;
volatile uint32_t native_dwc3_gcc_gdscr = 0, native_dwc3_gcc_master = 0, native_dwc3_gcc_mock = 0;
volatile uint32_t native_dwc3_gcc_ahb2phy = 0, native_dwc3_gcc_noc_axi = 0, native_dwc3_gcc_aggre_axi = 0;
volatile uint32_t native_dwc3_prtcap = 0;    // GCTL current mode (2 = device)
volatile int32_t  native_dwc3_smmu_slot = -1, native_dwc3_smmu_bank = -1;
volatile uint32_t native_dwc3_smmu_reuse = 0;
const char *volatile native_dwc3_smmu_error = nullptr;
const char *volatile native_dwc3_failure = nullptr;
// Stage B (gadget, usb-debug=N) state the boot log surfaces (row 'V').
volatile uint32_t native_dwc3_gadget_on = 0;      // usb-debug value seen
volatile uint32_t native_dwc3_gadget_stage = 0;   // see Dwc3Gadget.h GadgetStage
volatile int32_t  native_dwc3_gadget_status = -1;  // 0 ok, negative = failing stage
volatile uint32_t native_dwc3_clocks_up = 0, native_dwc3_phy_up = 0;   // 1 = we enabled it, 0 = reused UEFI
volatile uint32_t native_dwc3_link_speed = 0xff;   // DSTS connect speed after USB reset
volatile uint32_t native_dwc3_setups = 0, native_dwc3_configured = 0, native_dwc3_events = 0;
volatile uint32_t native_dwc3_addr = 0;            // address the host assigned
volatile uint64_t native_dwc3_bulk_out = 0, native_dwc3_bulk_in = 0;
const char *volatile native_dwc3_gadget_failure = nullptr;
volatile int32_t native_dwc3_smmu_slot_g = -1, native_dwc3_smmu_bank_g = -1;
volatile uint32_t native_dwc3_smmu_banks = 0, native_dwc3_resets = 0;
// Traces for the boot log: the first 8 SETUP packets (8 bytes each) and the first 36 raw events of the run.
volatile uint8_t native_dwc3_setup_trace[8][8];
volatile uint32_t native_dwc3_event_trace[36];
volatile uint32_t native_dwc3_trace_setups = 0, native_dwc3_trace_events = 0;
// Data-path health: poll-loop heartbeat, bulk transfer completions, the IN in-flight flag, endpoint-command failures.
volatile uint32_t native_dwc3_polls = 0, native_dwc3_out_xfers = 0, native_dwc3_in_xfers = 0, native_dwc3_in_flight = 0;
volatile uint32_t native_dwc3_cmd_fails = 0, native_dwc3_cmd_last_fail = 0;
}

static bool gProbeEnabled = false;
static unsigned gProbeMode = 0;
static unsigned gGadgetMode = 0;

static uint32_t dwc3Read32(uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static uintptr_t dwc3MapPhys(uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release();
  return keep ? static_cast<uintptr_t>(keep->getVirtualAddress()) : 0;
}

// The gadget instance (stage B). Allocated zeroed with IONewZero so no C++ constructor
// (and thus no static guard variable, which the kernel forbids) runs; zero is its
// correct initial state. It lives for the driver's life.
static dwc3::Gadget *gGadget = nullptr;

// ---- /dev/fjdbg: the userland end of the SHELL channel ---------------------------
// A userland agent opens /dev/fjdbg; read() returns bytes the host sent on the SHELL
// channel, write() sends bytes back to the host as SHELL frames. One opener at a time.
static volatile uint32_t gDevOpen = 0;
static bool gDevCreated = false;
extern "C" vnode_t rootvnode __attribute__((weak_import));
extern "C" volatile uint32_t native_dwc3_dev_major = 0, native_dwc3_dev_opens = 0;

static int fjdbgOpen(dev_t, int, int, struct proc *)
{
  if (!gGadget) return ENXIO;
  if (!OSCompareAndSwap(0, 1, &gDevOpen)) return EBUSY;
  native_dwc3_dev_opens = native_dwc3_dev_opens + 1;
  return 0;
}

static int fjdbgClose(dev_t, int, int, struct proc *)
{
  gDevOpen = 0;
  return 0;
}

static int fjdbgRead(dev_t, struct uio *uio, int)
{
  dwc3::Gadget *g = gGadget;
  while (g->shHead == g->shTail) {
    struct timespec ts = {1, 0};
    int err = msleep(const_cast<uint32_t *>(&g->shHead), nullptr, PZERO | PCATCH, "fjdbg", &ts);
    if (err && err != EWOULDBLOCK) return err;   // EINTR/ERESTART: a signal
  }
  int err = 0;
  while (!err && g->shHead != g->shTail && uio_resid(uio) > 0) {
    uint32_t tail = g->shTail, avail = g->shHead - tail;
    uint32_t off = tail & (dwc3::Gadget::kShRing - 1);
    uint32_t run = dwc3::Gadget::kShRing - off;   // contiguous bytes before the wrap
    uint32_t n = avail < run ? avail : run;
    if (n > uint32_t(uio_resid(uio))) n = uint32_t(uio_resid(uio));
    err = uiomove(reinterpret_cast<const char *>(g->shRx + off), int(n), uio);
    if (!err) g->shTail = tail + n;
  }
  return err;
}

static int fjdbgWrite(dev_t, struct uio *uio, int)
{
  dwc3::Gadget *g = gGadget;
  char buf[1024];
  while (uio_resid(uio) > 0) {
    int n = int(uio_resid(uio) < int(sizeof buf) ? uio_resid(uio) : sizeof buf);
    int err = uiomove(buf, n, uio);
    if (err) return err;
    // The TX ring may be full while the host is slow: wait up to ~2 s, then drop.
    for (int tries = 0; tries < 400 && !g->queueFrame(usbdbg::kChShell, reinterpret_cast<uint8_t *>(buf), size_t(n)); tries++) {
      IOSleep(5);
    }
  }
  return 0;
}

static struct cdevsw gFjdbgSw = {
  fjdbgOpen, fjdbgClose, fjdbgRead, fjdbgWrite, eno_ioctl, eno_stop, eno_reset, nullptr,
  eno_select, eno_mmap, eno_strat, eno_getc, eno_putc, 0,
};

static void fjdbgCreateNode()
{
  int major = cdevsw_add(-1, &gFjdbgSw);
  if (major < 0) return;
  native_dwc3_dev_major = uint32_t(major);
  devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "fjdbg");
}

// The polled service thread: drains the DWC3 event buffer, mirrors the kernel log to
// the host and moves queued frames to the bulk-IN endpoint. Only this thread touches
// the controller. Once the root (and with it devfs) is mounted it creates /dev/fjdbg.
#ifndef MAXPRI_KERNEL
#define MAXPRI_KERNEL 95   // osfmk/kern/sched.h: BASEPRI_REALTIME - 1, the highest non-realtime kernel priority
#endif
extern "C" {
struct processor;
struct processor *cpu_to_processor(int cpu);
struct processor *thread_bind(struct processor *processor);
// osfmk/kern/thread.h (behind MACH_KERNEL_PRIVATE in this TU's view; the kernel exports it)
kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
}

static void dwc3GadgetThread(void *, wait_result_t)
{
  thread_set_thread_name(current_thread(), "native_dwc3");
  // ep0 has tight host timeouts and Linux services it from the interrupt; this thread runs at MAXPRI_KERNEL and is
  // bound to CPU 1 (a little core: pcpu=0xf0 crowds the Gold cores, CPU 0 carries the monitor and watchdog pets), so
  // a busy userland boot cannot starve it. A bound thread runs even on a CPU that is not recommended.
  if (struct processor *cpu1 = cpu_to_processor(1)) {
    thread_bind(cpu1);
    thread_block(THREAD_CONTINUE_NULL);
  }
  for (;;) {
    if (gGadget) {
      native_dwc3_polls = native_dwc3_polls + 1;
      gGadget->poll();
      gGadget->pumpLog();
      gGadget->kickTx();
      if (!gDevCreated && &rootvnode && rootvnode) {
        gDevCreated = true;
        fjdbgCreateNode();
      }
    }
    // Control transfers have tight host timeouts: keep the poll period at 1 ms, and do not sleep at all while the
    // controller still has events queued.
    if (!gGadget || !gGadget->eventsPending()) IOSleep(1);
  }
}

class NativeDwc3 : public IOService {
  OSDeclareDefaultStructors(NativeDwc3)
  void inspect();
  void startGadget();
public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(NativeDwc3, IOService)

IOService *NativeDwc3::probe(IOService *provider, SInt32 *score)
{
  native_dwc3_probe_calls = native_dwc3_probe_calls + 1;
  // Attach to the root device, like NativeUfs's second personality: it starts
  // before the platform expert publishes IOResources, so the probe runs early and
  // before the original AMFI. The probe is a no-op unless the boot arg opts in.
  if (OSDynamicCast(IOPlatformExpertDevice, provider)) {
    native_dwc3_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  return nullptr;
}

bool NativeDwc3::start(IOService *provider)
{
  if (!IOService::start(provider)) {
    return false;
  }
  uint32_t value = 0;
  if (PE_parse_boot_argn("native-dwc3-probe", &value, sizeof(value))) {
    gProbeMode = value;
    gProbeEnabled = value != 0;
  }
  native_dwc3_mode = gProbeMode;
  uint32_t gadget = 0;
  if (PE_parse_boot_argn("usb-debug", &gadget, sizeof(gadget))) {
    gGadgetMode = gadget;
  }
  native_dwc3_gadget_on = gGadgetMode;
  if (gProbeEnabled) {
    inspect();
  }
  if (gGadgetMode) {
    startGadget();
  }
  return true;
}

// Stage B: bring the controller up as a CDC-ACM device and start the poll thread.
// usb-debug=1 stops after the core soft reset (no enumeration, no host visibility);
// usb-debug>=2 enumerates and runs the channel. Never panics: a failed stage is
// recorded in the 'V' boot-log row and the thread simply idles.
void NativeDwc3::startGadget()
{
  gGadget = IONewZero(dwc3::Gadget, 1);
  if (!gGadget) {
    native_dwc3_gadget_failure = "gadget alloc";
    native_dwc3_gadget_status = -1;
    return;
  }
  gGadget->txLock = IOSimpleLockAlloc();
  gGadget->txRing = static_cast<uint8_t *>(IOMallocZeroData(dwc3::Gadget::kTxRing));
  gGadget->shRx = static_cast<uint8_t *>(IOMallocZeroData(dwc3::Gadget::kShRing));
  if (!gGadget->txLock || !gGadget->txRing || !gGadget->shRx) {
    native_dwc3_gadget_failure = "gadget rings";
    native_dwc3_gadget_status = -1;
    return;
  }
  gGadget->bringUp(gGadgetMode);
  thread_t t;
  if (kernel_thread_start_priority(dwc3GadgetThread, nullptr, MAXPRI_KERNEL, &t) == KERN_SUCCESS) {
    thread_deallocate(t);
  }
}

// Read-only inspection. Each stage sets native_dwc3_stage before it touches MMIO,
// so a fault leaves the last safe stage recorded in the log. Nothing is written.
void NativeDwc3::inspect()
{
  OSSharedPtr<IOMemoryMap> gccMap, coreMap, phyMap, smmuMap;
  Dwc3Snapshot snap;
  bzero(&snap, sizeof(snap));

  native_dwc3_stage = 1;
  uintptr_t gcc = dwc3MapPhys(kGccBase, 0x100000, gccMap);
  if (!gcc) {
    native_dwc3_failure = "gcc map";
    native_dwc3_status = -1;
    return;
  }
  snap.gcc_gdscr    = dwc3Read32(gcc, kGccUsb30PrimGdscr);
  snap.gcc_master   = dwc3Read32(gcc, kGccUsb30PrimMasterCbcr);
  snap.gcc_mock_utmi = dwc3Read32(gcc, kGccUsb30PrimMockUtmiCbcr);
  snap.gcc_sleep    = dwc3Read32(gcc, kGccUsb30PrimSleepCbcr);
  snap.gcc_noc_axi  = dwc3Read32(gcc, kGccCfgNocUsb3PrimCbcr);
  snap.gcc_aggre_axi = dwc3Read32(gcc, kGccAggreUsb3PrimCbcr);
  snap.gcc_ahb2phy  = dwc3Read32(gcc, kGccUsbPhyCfgAhb2PhyCbcr);
  native_dwc3_gcc_gdscr = snap.gcc_gdscr;
  native_dwc3_gcc_master = snap.gcc_master;
  native_dwc3_gcc_mock = snap.gcc_mock_utmi;
  native_dwc3_gcc_ahb2phy = snap.gcc_ahb2phy;
  native_dwc3_gcc_noc_axi = snap.gcc_noc_axi;
  native_dwc3_gcc_aggre_axi = snap.gcc_aggre_axi;

  if (!dwc3GccCoreReadable(snap)) {
    // UEFI did not leave the controller clocked: report and stop, do not guess.
    native_dwc3_failure = "usb30 clocks off after hand-off";
    native_dwc3_status = -1;
    return;
  }

  native_dwc3_stage = 2;
  uintptr_t core = dwc3MapPhys(kDwc3CoreBase, kDwc3CoreSize, coreMap);
  if (!core) {
    native_dwc3_failure = "core map";
    native_dwc3_status = -2;
    return;
  }
  snap.gsnpsid    = dwc3Read32(core, kDwc3GSNPSID);
  snap.gctl       = dwc3Read32(core, kDwc3GCTL);
  snap.gsts       = dwc3Read32(core, kDwc3GSTS);
  snap.ghwparams0 = dwc3Read32(core, kDwc3GHWPARAMS0);
  snap.ghwparams1 = dwc3Read32(core, kDwc3GHWPARAMS1);
  snap.ghwparams3 = dwc3Read32(core, kDwc3GHWPARAMS3);
  snap.dcfg       = dwc3Read32(core, kDwc3DCFG);
  snap.dctl       = dwc3Read32(core, kDwc3DCTL);
  snap.dsts       = dwc3Read32(core, kDwc3DSTS);
  snap.dalepena   = dwc3Read32(core, kDwc3DALEPENA);
  snap.devten     = dwc3Read32(core, kDwc3DEVTEN);
  snap.gevntsiz0  = dwc3Read32(core, kDwc3GEVNTSIZ0);
  snap.gevntcount0 = dwc3Read32(core, kDwc3GEVNTCOUNT0);
  snap.gusb2phycfg0 = dwc3Read32(core, kDwc3GUSB2PHYCFG0);
  native_dwc3_gsnpsid = snap.gsnpsid;
  native_dwc3_gctl = snap.gctl;
  native_dwc3_gsts = snap.gsts;
  native_dwc3_ghwparams0 = snap.ghwparams0;
  native_dwc3_ghwparams1 = snap.ghwparams1;
  native_dwc3_ghwparams3 = snap.ghwparams3;
  native_dwc3_dcfg = snap.dcfg;
  native_dwc3_dctl = snap.dctl;
  native_dwc3_dsts = snap.dsts;
  native_dwc3_dalepena = snap.dalepena;
  native_dwc3_devten = snap.devten;
  native_dwc3_gevntsiz0 = snap.gevntsiz0;
  native_dwc3_gevntcount0 = snap.gevntcount0;
  native_dwc3_prtcap = dwc3GctlPrtCap(snap.gctl);
  if (!dwc3SnpsIsUsb3(snap.gsnpsid)) {
    native_dwc3_failure = "unexpected GSNPSID (not a DWC3 core)";
    native_dwc3_status = -2;
    return;
  }

  native_dwc3_stage = 3;
  uintptr_t phy = dwc3MapPhys(kQusb2PhyBase, kQusb2PhySize, phyMap);
  if (phy) {
    snap.qusb2_pll_status = dwc3Read32(phy, kQusb2PllCommonStatusOne);
    native_dwc3_qusb2_status = snap.qusb2_pll_status;
    native_dwc3_phy_ready = qusb2PhyReady(snap.qusb2_pll_status) ? 1 : 0;
  } else {
    native_dwc3_qusb2_status = 0;
    native_dwc3_phy_ready = 0;
  }

  if (gProbeMode >= 2) {
    native_dwc3_stage = 4;
    // Read-only SMMU plan for the dwc3 stream, exactly as NativeUfs plans the UFS
    // stream, but nothing is configured. A representative 64 KiB DMA window at a
    // plausible address only exercises the planner's address-width checks; the
    // real gadget would pass its own event-buffer/TRB allocation here.
    uintptr_t smmu = dwc3MapPhys(kAppsSmmuBase, kAppsSmmuSize, smmuMap);
    if (!smmu) {
      native_dwc3_failure = "smmu map";
      native_dwc3_status = -4;
      return;
    }
    const uint64_t probeDma = 0x80000000ull, probeBytes = 0x10000ull;
    UfsSmmuPlan plan = planUfsSmmu(kDwc3SmmuStream, kDwc3SmmuMask, kAppsSmmuSize, probeDma, probeBytes,
                                   [smmu](unsigned off) { return dwc3Read32(smmu, off); }, false);
    native_dwc3_smmu_slot = plan.error ? -1 : int32_t(plan.slot);
    native_dwc3_smmu_bank = plan.error ? -1 : int32_t(plan.bank);
    native_dwc3_smmu_reuse = plan.reuse ? 1 : 0;
    native_dwc3_smmu_error = plan.error;
  }

  native_dwc3_stage = 5;
  native_dwc3_status = 0;
  native_dwc3_failure = nullptr;
}
