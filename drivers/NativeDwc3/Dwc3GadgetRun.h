// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage B, part 2: the gadget object — bring-up orchestration, the ep0 control
// transfer state machine, the bulk endpoints, and the polled event loop. Included by
// Dwc3Gadget.h. Follows dwc3/{gadget,ep0}.c; the control/descriptor logic it calls
// (AcmEp0.h) and the wire protocol (UsbDbgProto.h/UsbDbgDevice.h) are host-tested.
#ifndef NATIVE_DWC3_GADGET_RUN_H
#define NATIVE_DWC3_GADGET_RUN_H

#include "AcmDescriptors.h"
#include "UsbDbgDevice.h"
// The generic ARM SMMUv2 stream planner (NativeUfs/SmmuTakeover.h) is included by the
// including TU (NativeDwc3.cpp) before this header; it maps the gadget DMA behind
// stream 0x740 (adopt the firmware bypass or plan a free bank).

// The kernel message buffer (bsd/sys/msgbuf.h), referenced to stream the log to the
// host. Declared weak + extern "C" so it resolves to the global symbol `_msgbufp` and
// the driver still links on a kernel without it. Only the leading fields are read.
extern "C" {
struct dwc3_msgbuf { int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc; };
extern struct dwc3_msgbuf *msgbufp __attribute__((weak_import));
void wakeup(void *chan);   // bsd/sys/systm.h
}

namespace dwc3 {

// Physical endpoint numbers: ep0 OUT = 0, ep0 IN = 1, bulk OUT = 4, bulk IN = 5.
enum { kPEp0Out = 0, kPEp0In = 1, kPBulkOut = 4, kPBulkIn = 5 };

enum Ep0Phase { kWaitSetup, kWaitDataIn, kWaitDataOut, kWaitStatus };

// Plain-old-data so it can live in a zeroed static without a C++ guard variable
// (the kernel forbids those). Maps and the DMA buffer are held raw for the driver's
// whole life and never released. Zero-init equals the correct initial state.
class Gadget {
public:
  Mmio m;
  GadgetDma dma;
  acmep0::State ep0;
  Ep0Phase phase;                  // kWaitSetup == 0
  acmep0::Ep0Action pending;
  bool bulkStarted;
  bool bulkOutArmed;
  usbdbg::Decoder rx;
  uint16_t txSeq;
  uint64_t logOffset;               // next msgbuf byte (monotonic) to send
  uint64_t logProduced;             // msgbuf bytes written since first look
  uint32_t logLastHead;
  bool logInit;
  // Device->host byte ring of encoded frames. Any thread enqueues (queueFrame, under
  // txLock); only the poll thread moves bytes to the bulk-IN TRB (kickTx), one
  // transfer in flight at a time, advanced on its XferComplete.
  static constexpr uint32_t kTxRing = 32768;   // power of two
  static constexpr uint32_t kTxChunk = 16384;  // max bytes per bulk-IN transfer
  IOSimpleLock *txLock;
  uint32_t txHead, txTail;          // free-running indices (mod kTxRing)
  bool inFlight;
  uint32_t inFlightLen;
  uint8_t encBuf[usbdbg::kMaxEncoded];
  uint8_t *txRing;                  // kTxRing bytes, IOMallocZeroData
  // Host->agent SHELL bytes for /dev/fjdbg (single producer: poll thread; single
  // consumer: the agent's read()).
  static constexpr uint32_t kShRing = 8192;   // power of two
  volatile uint32_t shHead, shTail;
  uint8_t *shRx;                    // kShRing bytes, IOMallocZeroData
  volatile uint32_t shOverflow;

  bool bringUp(unsigned mode);
  void poll();                     // drain the event buffer once
  void pumpLog();                  // mirror new kernel-log bytes to the host
  void kickTx();                   // poll thread: start the next bulk-IN transfer if idle
  bool queueFrame(uint8_t channel, const uint8_t *data, size_t len);   // any thread

private:
  void fail(GadgetStage st, const char *why) { native_dwc3_gadget_stage = st; native_dwc3_gadget_status = -int(st); native_dwc3_gadget_failure = why; }
  bool mapWindows();
  bool setupEventBuffer();
  bool smmuMap740();
  void gadgetStart();
  void ep0QueueSetup();
  void ep0StartTx(const uint8_t *data, uint16_t len);
  void ep0StartRx(uint16_t len);
  void ep0Status(bool threeStage);
  void onSetupComplete();
  void onEp0DataOrStatus(unsigned ep);
  void enableBulk();
  void armBulkOut();
  void onBulkOut(uint32_t remaining);
  void onBulkInDone();
};

// ---- bring-up ---------------------------------------------------------------------
inline bool Gadget::mapWindows()
{
  // Each mapping is kept for the driver's whole life (the IOMemoryMap is deliberately
  // not released, so the window stays mapped); this is a boot-time, never-stopped driver.
  auto map = [](uint64_t pa, uint64_t len) -> uintptr_t {
    IODeviceMemory *mem = IODeviceMemory::withRange(pa, len);
    if (!mem) return 0;
    IOMemoryMap *mm = mem->map(kIOMapAnywhere | kIOMapInhibitCache);
    mem->release();
    return mm ? uintptr_t(mm->getVirtualAddress()) : 0;
  };
  m.gcc = map(kGccBase, 0x100000);
  m.core = map(kDwc3CoreBase, kDwc3CoreSize);
  m.qscratch = map(kDwc3QscratchBase, 0x400);
  m.phy = map(kQusb2PhyBase, kQusb2PhySize);
  return m.gcc && m.core && m.qscratch && m.phy;
}

inline bool Gadget::setupEventBuffer()
{
  dma.buf = IOBufferMemoryDescriptor::inTaskWithOptions(
    kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) | IOOptionBits(kIOMemoryMapperNone),
    GadgetDma::kTotal, 4096);   // held for the driver's life; never released
  if (!dma.buf) return false;
  dma.virt = static_cast<uint8_t *>(dma.buf->getBytesNoCopy());
  IOByteCount contiguous = 0;
  dma.phys = dma.buf->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  if (!dma.virt || !dma.phys || (dma.phys & 4095) || contiguous < GadgetDma::kTotal) return false;
  bzero(dma.virt, GadgetDma::kTotal);
  cache_flush(dma.virt, GadgetDma::kTotal);
  // Event buffer registers: base, size, clear stale count.
  m.core_wr(kDwc3GEVNTADRLO0, uint32_t(dma.pa(GadgetDma::kEvent)));
  m.core_wr(kDwc3GEVNTADRHI0, uint32_t(dma.pa(GadgetDma::kEvent) >> 32));
  m.core_wr(kDwc3GEVNTSIZ0, GadgetDma::kEventBytes);
  m.core_wr(kDwc3GEVNTCOUNT0, m.core_rd(kDwc3GEVNTCOUNT0) & 0xffff);
  return true;
}

// Give the dwc3 stream (0x740) a VA64 bypass context bank so the controller's event
// buffer and TRBs DMA with physical addresses. The NativeUfs planner assumes the top
// banks are free and collides with UFS's stream-0x100 bank ("reserved context bank
// already used"); here we instead scan for a bank that NO valid stream points to and
// a free stream-match group, and install the same bypass recipe configureUfsSmmu uses
// (SCTLR=0, CBA2R VA64=1, CBAR S1-trans/S2-bypass, S2CR->bank, SMR valid|0x740). UFS's
// own bank and groups are left untouched. ARM SMMUv2 register layout.
inline bool Gadget::smmuMap740()
{
  IODeviceMemory *mem = IODeviceMemory::withRange(kAppsSmmuBase, kAppsSmmuSize);
  if (!mem) return false;
  IOMemoryMap *map = mem->map(kIOMapAnywhere | kIOMapInhibitCache);   // kept mapped for the driver's life
  mem->release();
  if (!map) return false;
  uintptr_t smmu = uintptr_t(map->getVirtualAddress());
  if (!smmu) return false;
  auto rd = [smmu](unsigned off) -> uint32_t { return *reinterpret_cast<volatile uint32_t *>(smmu + off); };
  auto wr = [smmu](unsigned off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(smmu + off) = v; __asm__ volatile("dsb sy" ::: "memory"); };

  uint32_t idr0 = rd(0x20), idr1 = rd(0x24), idr2 = rd(0x28);
  if (!(idr0 & (1u << 30)) || !(idr2 & (1u << 12))) { native_dwc3_gadget_failure = "smmu: no AArch64 s1"; return false; }
  unsigned numSmr = idr0 & 0xff, banks = idr1 & 0xff, pages = 1u << (((idr1 >> 28) & 7) + 1);
  if (numSmr > 128) numSmr = 128;
  if (!numSmr || banks < 2 || (pages + banks) * 4096ull > kAppsSmmuSize) { native_dwc3_gadget_failure = "smmu geometry"; return false; }
  native_dwc3_smmu_banks = banks;

  const uint32_t wantSmr = 0x80000000u | (kDwc3SmmuStream & 0xffff);   // valid, mask 0, id 0x740
  // Scan the stream-match table: note which banks are in use, whether 0x740 already has
  // a group, and the first free group.
  unsigned freeSlot = 0xffffffff, ourSlot = 0xffffffff;
  bool bankUsed[128] = {false};
  for (unsigned i = 0; i < numSmr; i++) {
    uint32_t smr = rd(0x800 + 4 * i);
    if (!(smr & 0x80000000u)) { if (freeSlot == 0xffffffff) freeSlot = i; continue; }
    uint32_t s2cr = rd(0xc00 + 4 * i);
    unsigned type = (s2cr >> 16) & 3, cbndx = s2cr & 0xff;
    if (type == 0 && cbndx < 128) bankUsed[cbndx] = true;   // a translation/bypass context bank in use
    if ((smr & 0xffff) == (kDwc3SmmuStream & 0xffff)) ourSlot = i;   // a prior group for our stream
  }
  unsigned slot = ourSlot != 0xffffffff ? ourSlot : freeSlot;
  if (slot == 0xffffffff) { native_dwc3_gadget_failure = "smmu: no free stream group"; return false; }
  // Pick a free context bank, scanning from the bottom (firmware tends to use the top).
  unsigned bank = 0xffffffff;
  for (unsigned b = 0; b < banks && b < 128; b++) {
    if (bankUsed[b]) continue;
    if (rd((pages + b) * 4096) & 1) continue;   // SCTLR.M set: a live translation context
    bank = b;
    break;
  }
  if (bank == 0xffffffff) { native_dwc3_gadget_failure = "smmu: no free context bank"; return false; }
  native_dwc3_smmu_slot_g = int32_t(slot);
  native_dwc3_smmu_bank_g = int32_t(bank);

  // Install the bypass bank, then point our stream at it.
  wr((pages + bank) * 4096, 0);          // SCTLR = 0 (translation disabled -> passthrough)
  wr(0x1800 + 4 * bank, 1);              // CBA2R.VA64 = 1 (inputs may exceed 32 bits)
  wr(0x1000 + 4 * bank, 0x10000);        // CBAR TYPE = S1_TRANS_S2_BYPASS
  wr(0xc00 + 4 * slot, bank);            // S2CR: TYPE=translation-context, CBNDX = bank
  wr(0x800 + 4 * slot, wantSmr);         // SMR: valid, id 0x740
  if (rd(0x800 + 4 * slot) != wantSmr || (rd(0xc00 + 4 * slot) & 0xff) != bank ||
      (rd((pages + bank) * 4096) & 1) || !(rd(0x1800 + 4 * bank) & 1)) {
    native_dwc3_gadget_failure = "smmu readback"; return false;
  }
  return true;
}

inline bool Gadget::bringUp(unsigned mode)
{
  if (!mapWindows()) { fail(kStClocks, "map windows"); return false; }
  bool weClk = false, wePhy = false;
  native_dwc3_gadget_stage = kStClocks;
  if (!clocksEnable(m, weClk)) { fail(kStClocks, "usb30 clocks"); return false; }
  native_dwc3_clocks_up = weClk;
  native_dwc3_gadget_stage = kStQscratch;
  qscratchSetup(m);
  native_dwc3_gadget_stage = kStPhy;
  if (!phyInit(m, wePhy)) { fail(kStPhy, "qusb2 pll lock"); return false; }
  native_dwc3_phy_up = wePhy;
  // GUSB2PHYCFG: clear SUSPHY so the PHY is reachable during setup.
  m.core_wr(kDwc3GUSB2PHYCFG0, m.core_rd(kDwc3GUSB2PHYCFG0) & ~(1u << 6));
  native_dwc3_gadget_stage = kStCoreReset;
  if (!coreSoftReset(m)) { fail(kStCoreReset, "core soft reset"); return false; }
  native_dwc3_gadget_stage = kStGlobal;
  // Global control: clear scaledown, clear hibernation; set PRTCAP = device.
  uint32_t gctl = m.core_rd(kDwc3GCTL);
  gctl &= ~(3u << 4);                           // SCALEDOWN
  gctl &= ~(3u << 12);                          // clear PRTCAPDIR (OTG)
  gctl |= (2u << 12);                           // device
  m.core_wr(kDwc3GCTL, gctl);
  native_dwc3_gadget_stage = kStEventBuf;
  if (!setupEventBuffer()) { fail(kStEventBuf, "event buffer dma"); return false; }
  native_dwc3_gadget_stage = kStSmmu;
  if (!smmuMap740()) { fail(kStSmmu, native_dwc3_gadget_failure ? native_dwc3_gadget_failure : "smmu"); return false; }

  if (mode < 2) { native_dwc3_gadget_stage = kStCoreReset; native_dwc3_gadget_status = 0; return true; }  // stop after reset

  gadgetStart();
  native_dwc3_gadget_stage = kStRunning;
  native_dwc3_gadget_status = 0;
  return true;
}

// ---- gadget start (dwc3_gadget_start) --------------------------------------------
inline void Gadget::gadgetStart()
{
  native_dwc3_gadget_stage = kStGadgetStart;
  // DCFG: high speed, ignore stream pp, device address 0.
  uint32_t dcfg = m.core_rd(kDwc3DCFG);
  dcfg &= ~7u;                                  // speed = high speed (0)
  dcfg |= kDwc3DcfgIgnStrmPp;
  dcfg &= ~(0x7fu << 3);                        // address 0
  m.core_wr(kDwc3DCFG, dcfg);
  // GRXTHRCFG: clear packet-count select (bit 29..). Leave default otherwise.
  // DEPSTARTCFG(0): reset endpoint resources for ep0.
  depCmd(m, kPEp0Out, kDwc3DepCmdStartConfig | dwc3DepCmdParam(0), 0, 0, 0);
  // Configure ep0 OUT and ep0 IN (control, max packet 64).
  auto depCfgControl = [&](unsigned ep) {
    uint32_t p0 = uint32_t(kDwc3EpTypeControl << kDwc3DepCfgTypeShift) | (64u << kDwc3DepCfgMaxPktShift) | kDwc3DepCfgActionInit;
    uint32_t p1 = kDwc3DepCfgXferCompleteEn | kDwc3DepCfgXferNotReadyEn | (uint32_t(ep) << kDwc3DepCfgEpNumShift);
    depCmd(m, ep, kDwc3DepCmdSetEpConfig, p0, p1, 0);
    depCmd(m, ep, kDwc3DepCmdSetXferResource, kDwc3DepXferCfgNumXferRes1, 0, 0);
  };
  depCfgControl(kPEp0Out);
  depCfgControl(kPEp0In);
  // Enable ep0 OUT/IN in DALEPENA.
  m.core_wr(kDwc3DALEPENA, m.core_rd(kDwc3DALEPENA) | (1u << kPEp0Out) | (1u << kPEp0In));
  // Device event enables.
  m.core_wr(kDwc3DEVTEN, kDwc3DevtenDisconn | kDwc3DevtenUsbReset | kDwc3DevtenConnectDone |
                         kDwc3DevtenWakeup | kDwc3DevtenCmdCmplt | kDwc3DevtenEvntOverflow | kDwc3DevtenSuspend);
  // Queue the first SETUP, then pull up (RUN_STOP).
  ep0QueueSetup();
  native_dwc3_gadget_stage = kStRunStop;
  uint32_t dctl = m.core_rd(kDwc3DCTL);
  dctl &= ~kDwc3DctlKeepConnect;
  dctl |= kDwc3DctlRunStop;
  m.core_wr(kDwc3DCTL, dctl);
}

// ---- ep0 control transfers --------------------------------------------------------
inline void Gadget::ep0QueueSetup()
{
  Trb *t = dma.trb(GadgetDma::kEp0Trb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kEp0Data));
  t->bph = uint32_t(dma.pa(GadgetDma::kEp0Data) >> 32);
  t->size = 8;
  t->ctrl = kDwc3TrbCtlControlSetup | kDwc3TrbHwo | kDwc3TrbIspImi | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  phase = kWaitSetup;
  depCmd(m, kPEp0Out, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kEp0Trb) >> 32), uint32_t(dma.pa(GadgetDma::kEp0Trb)), 0);
}

inline void Gadget::ep0StartTx(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++) dma.at(GadgetDma::kEp0Data)[i] = data[i];
  cache_flush(dma.at(GadgetDma::kEp0Data), len ? len : 1);
  Trb *t = dma.trb(GadgetDma::kEp0Trb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kEp0Data));
  t->bph = uint32_t(dma.pa(GadgetDma::kEp0Data) >> 32);
  t->size = len;
  t->ctrl = kDwc3TrbCtlControlData | kDwc3TrbHwo | kDwc3TrbIspImi | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  phase = kWaitDataIn;
  depCmd(m, kPEp0In, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kEp0Trb) >> 32), uint32_t(dma.pa(GadgetDma::kEp0Trb)), 0);
}

inline void Gadget::ep0StartRx(uint16_t len)
{
  Trb *t = dma.trb(GadgetDma::kEp0Trb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kEp0Data));
  t->bph = uint32_t(dma.pa(GadgetDma::kEp0Data) >> 32);
  t->size = len;
  t->ctrl = kDwc3TrbCtlControlData | kDwc3TrbHwo | kDwc3TrbIspImi | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  phase = kWaitDataOut;
  depCmd(m, kPEp0Out, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kEp0Trb) >> 32), uint32_t(dma.pa(GadgetDma::kEp0Trb)), 0);
}

inline void Gadget::ep0Status(bool threeStage)
{
  // Status direction is opposite the data stage. For our no-data (host->device)
  // requests, status is IN (ep0 IN). For a control-read, status is OUT (ep0 OUT).
  unsigned ep = (pending.kind == acmep0::kTxData) ? kPEp0Out : kPEp0In;
  Trb *t = dma.trb(GadgetDma::kEp0Trb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kEp0Data));
  t->bph = uint32_t(dma.pa(GadgetDma::kEp0Data) >> 32);
  t->size = 0;
  t->ctrl = (threeStage ? kDwc3TrbCtlControlStatus3 : kDwc3TrbCtlControlStatus2) | kDwc3TrbHwo | kDwc3TrbIspImi | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  phase = kWaitStatus;
  depCmd(m, ep, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kEp0Trb) >> 32), uint32_t(dma.pa(GadgetDma::kEp0Trb)), 0);
}

inline void Gadget::onSetupComplete()
{
  cache_flush(dma.at(GadgetDma::kEp0Data), 8);
  {
    unsigned k = native_dwc3_trace_setups;
    if (k < 8) for (unsigned b = 0; b < 8; b++) native_dwc3_setup_trace[k][b] = dma.at(GadgetDma::kEp0Data)[b];
    native_dwc3_trace_setups = native_dwc3_trace_setups + 1;
  }
  acmep0::Setup s = acmep0::parseSetup(dma.at(GadgetDma::kEp0Data));
  native_dwc3_setups = native_dwc3_setups + 1;
  pending = acmep0::handleSetup(ep0, s);
  switch (pending.kind) {
  case acmep0::kStall:
    // Protocol stall on ep0 (dwc3_ep0_stall_and_restart): SETSTALL on ep0 OUT, then a
    // fresh SETUP; the controller clears the ep0 stall on the next SETUP by itself.
    depCmd(m, kPEp0Out, 0x04 /* DEPCMD_SETSTALL */, 0, 0, 0);
    ep0QueueSetup();
    break;
  case acmep0::kTxData:
    if (pending.len) ep0StartTx(pending.buf, pending.len); else ep0Status(false);
    break;
  case acmep0::kRxData:
    if (pending.len) ep0StartRx(pending.len); else ep0Status(false);
    break;
  case acmep0::kStatusOnly:
  case acmep0::kSetAddressAck:
  case acmep0::kSetConfigAck:
    ep0Status(false);
    break;
  }
}

inline void Gadget::onEp0DataOrStatus(unsigned ep)
{
  (void)ep;
  if (phase == kWaitDataIn || phase == kWaitDataOut) {
    ep0Status(true);                       // data stage done -> status stage
    return;
  }
  if (phase == kWaitStatus) {              // status done -> apply post-action, next setup
    if (pending.kind == acmep0::kSetAddressAck) {
      uint32_t dcfg = m.core_rd(kDwc3DCFG);
      dcfg &= ~(0x7fu << 3);
      dcfg |= uint32_t(pending.value) << 3;
      m.core_wr(kDwc3DCFG, dcfg);
      native_dwc3_addr = pending.value;
    } else if (pending.kind == acmep0::kSetConfigAck) {
      ep0.config = pending.value;
      native_dwc3_configured = pending.value;
      if (pending.value) enableBulk();
    }
    ep0QueueSetup();
  }
}

// ---- bulk endpoints ---------------------------------------------------------------
inline void Gadget::enableBulk()
{
  if (bulkStarted) return;
  // DEPSTARTCFG with XferRscIdx 2 goes to physical ep 0 (dwc3_gadget_start_config(dwc, 2), issued by Linux on
  // SET_CONFIGURATION): it resets the transfer resources of every endpoint above ep0/ep1.
  depCmd(m, kPEp0Out, kDwc3DepCmdStartConfig | dwc3DepCmdParam(2), 0, 0, 0);
  auto cfgBulk = [&](unsigned ep) {
    uint32_t p0 = uint32_t(kDwc3EpTypeBulk << kDwc3DepCfgTypeShift) | (512u << kDwc3DepCfgMaxPktShift) | kDwc3DepCfgActionInit;
    uint32_t p1 = kDwc3DepCfgXferCompleteEn | (uint32_t(ep) << kDwc3DepCfgEpNumShift);
    if (ep & 1) p0 |= (uint32_t(ep >> 1) << kDwc3DepCfgFifoNumShift);
    depCmd(m, ep, kDwc3DepCmdSetEpConfig, p0, p1, 0);
    depCmd(m, ep, kDwc3DepCmdSetXferResource, kDwc3DepXferCfgNumXferRes1, 0, 0);
  };
  cfgBulk(kPBulkOut);
  cfgBulk(kPBulkIn);
  m.core_wr(kDwc3DALEPENA, m.core_rd(kDwc3DALEPENA) | (1u << kPBulkOut) | (1u << kPBulkIn));
  bulkStarted = true;
  armBulkOut();
  // Greet the host so a device->host frame is visible without any host command.
  static const char hello[] = "fajita usb-debug: gadget up\n";
  queueFrame(usbdbg::kChShell, reinterpret_cast<const uint8_t *>(hello), sizeof hello - 1);
}

enum : uint32_t { kBulkOutBytes = 4096 };   // a multiple of the 512-byte max packet

inline void Gadget::armBulkOut()
{
  Trb *t = dma.trb(GadgetDma::kBulkOutTrb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kBulkOutData));
  t->bph = uint32_t(dma.pa(GadgetDma::kBulkOutData) >> 32);
  t->size = kBulkOutBytes;
  t->ctrl = kDwc3TrbCtlNormal | kDwc3TrbHwo | kDwc3TrbIspImi | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  depCmd(m, kPBulkOut, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kBulkOutTrb) >> 32), uint32_t(dma.pa(GadgetDma::kBulkOutTrb)), 0);
  bulkOutArmed = true;
}

inline bool Gadget::queueFrame(uint8_t channel, const uint8_t *data, size_t len)
{
  if (!txLock || !txRing || !len) return false;
  IOSimpleLockLock(txLock);
  size_t n = usbdbg::encodeFrame(channel, txSeq++, data, len, encBuf, sizeof encBuf);
  bool ok = n && (kTxRing - (txHead - txTail)) >= n;
  if (ok) {
    for (size_t i = 0; i < n; i++) txRing[(txHead + i) & (kTxRing - 1)] = encBuf[i];
    txHead += uint32_t(n);
  }
  IOSimpleLockUnlock(txLock);
  return ok;
}

inline void Gadget::kickTx()
{
  if (!bulkStarted || inFlight || !txLock) return;
  IOSimpleLockLock(txLock);
  uint32_t avail = txHead - txTail;
  uint32_t chunk = avail < kTxChunk ? avail : kTxChunk;
  uint8_t *out = dma.at(GadgetDma::kBulkInData);
  for (uint32_t i = 0; i < chunk; i++) out[i] = txRing[(txTail + i) & (kTxRing - 1)];
  IOSimpleLockUnlock(txLock);
  if (!chunk) return;
  // A transfer that is an exact multiple of the max packet would leave the host's read
  // pending until more data (no short packet); send one byte less, the rest follows.
  if (chunk % 512 == 0) chunk -= 1;
  cache_flush(out, chunk);
  Trb *t = dma.trb(GadgetDma::kBulkInTrb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kBulkInData));
  t->bph = uint32_t(dma.pa(GadgetDma::kBulkInData) >> 32);
  t->size = chunk;
  t->ctrl = kDwc3TrbCtlNormal | kDwc3TrbHwo | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  inFlight = true;
  inFlightLen = chunk;
  if (depCmd(m, kPBulkIn, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kBulkInTrb) >> 32), uint32_t(dma.pa(GadgetDma::kBulkInTrb)), 0) != 0) {
    inFlight = false;   // command refused: retry on the next poll
  }
}

inline void Gadget::onBulkInDone()
{
  if (!inFlight) return;
  IOSimpleLockLock(txLock);
  txTail += inFlightLen;
  IOSimpleLockUnlock(txLock);
  native_dwc3_bulk_in = native_dwc3_bulk_in + inFlightLen;
  inFlight = false;
}

inline void Gadget::onBulkOut(uint32_t remaining)
{
  cache_flush(dma.at(GadgetDma::kBulkOutData), kBulkOutBytes);
  uint32_t got = remaining <= kBulkOutBytes ? kBulkOutBytes - remaining : 0;   // TRB size counts down
  native_dwc3_bulk_out = native_dwc3_bulk_out + got;
  const uint8_t *in = dma.at(GadgetDma::kBulkOutData);
  for (uint32_t i = 0; i < got; i++) {
    if (rx.feed(in[i]) != usbdbg::Decoder::kFrame) continue;
    if (rx.channel() == usbdbg::kChCmd) {
      usbdbg::Target t{};
      t.ident = "fajita-usb-debug";
      t.features = usbdbg::kFeatLog;
      alignas(4) static uint8_t rsp[usbdbg::kMaxData];
      size_t rn = usbdbg::handleCommand(t, rx.data(), rx.length(), rsp);
      queueFrame(usbdbg::kChRsp, rsp, rn);
    } else if (rx.channel() == usbdbg::kChShell) {
      const uint8_t *d = rx.data();
      for (size_t k = 0; k < rx.length(); k++) {
        if (shHead - shTail >= kShRing) { shOverflow = shOverflow + 1; break; }
        shRx[shHead & (kShRing - 1)] = d[k];
        __asm__ volatile("dmb ish" ::: "memory");
        shHead = shHead + 1;
      }
      wakeup(const_cast<uint32_t *>(&shHead));
    }
  }
  armBulkOut();
}

// ---- the polled event loop --------------------------------------------------------
inline void Gadget::poll()
{
  uint32_t count = m.core_rd(kDwc3GEVNTCOUNT0) & 0xffff;
  if (!count) return;
  cache_flush(dma.at(GadgetDma::kEvent), GadgetDma::kEventBytes);
  uint32_t *ev = reinterpret_cast<uint32_t *>(dma.at(GadgetDma::kEvent));
  unsigned n = count / 4;
  static unsigned pos = 0;
  for (unsigned i = 0; i < n; i++) {
    uint32_t e = ev[pos];
    pos = (pos + 1) % (GadgetDma::kEventBytes / 4);
    native_dwc3_events = native_dwc3_events + 1;
    if (native_dwc3_trace_events < 36) native_dwc3_event_trace[native_dwc3_trace_events] = e;
    native_dwc3_trace_events = native_dwc3_trace_events + 1;
    if (dwc3EvtIsDevSpec(e)) {
      if (!dwc3EvtIsDevice(e)) continue;   // carkit/i2c classes: not used
      unsigned type = dwc3DevEvtType(e);
      if (type == kDwc3DevEvtReset) {
        // USB reset: back to address 0. ep0 is left as it is: a host typically resets during the status stage of
        // its first GET_DESCRIPTOR, and a StartTransfer on ep0 while that transfer is still active fails and leaves
        // no SETUP armed (hardware usb-enum5: enumeration stopped after 6 SETUPs). Linux ends the active transfer
        // first (dwc3_ep0_reset_state); the pending status completes or is superseded by the next SETUP here.
        uint32_t dcfg = m.core_rd(kDwc3DCFG); dcfg &= ~(0x7fu << 3); m.core_wr(kDwc3DCFG, dcfg);
        native_dwc3_addr = 0;
        native_dwc3_resets = native_dwc3_resets + 1;
      } else if (type == kDwc3DevEvtConnectDone) {
        // dwc3_gadget_conndone_interrupt: the negotiated speed decides the bulk max packet the descriptors report.
        uint32_t speed = m.core_rd(kDwc3DSTS) & 7u;
        native_dwc3_link_speed = speed;
        ep0.highSpeed = (speed == 0);     // DWC3_DSTS_HIGHSPEED == 0
      }
    } else {
      unsigned ep = dwc3DepEvtEp(e), etype = dwc3DepEvtType(e);
      if (etype == kDwc3DepEvtXferComplete || etype == kDwc3DepEvtXferInProgress) {
        if (ep == kPEp0Out && phase == kWaitSetup) onSetupComplete();
        else if (ep == kPEp0Out || ep == kPEp0In) onEp0DataOrStatus(ep);
        else if (ep == kPBulkOut) {
          cache_flush(dma.trb(GadgetDma::kBulkOutTrb), sizeof(Trb));
          onBulkOut(dma.trb(GadgetDma::kBulkOutTrb)->size & kDwc3TrbSizeMask);
        } else if (ep == kPBulkIn) onBulkInDone();
      }
    }
  }
  m.core_wr(kDwc3GEVNTCOUNT0, count);   // acknowledge the bytes we consumed
}

inline void Gadget::pumpLog()
{
  // Mirror the kernel msgbuf (a ring of msg_size bytes; msg_bufx is its write index) to the host as LOG frames.
  // logProduced counts bytes written since we first looked (wraps of msg_bufx added in), so the LOG offsets the host
  // sees are monotonic; a backlog is streamed oldest first, one frame per poll, and an overrun skips to the oldest
  // bytes still in the ring.
  if (!&msgbufp || !msgbufp || !bulkStarted) return;
  int size = msgbufp->msg_size, head = msgbufp->msg_bufx;
  if (size <= 0 || head < 0 || head >= size) return;
  if (!logInit) {
    logInit = true;
    logLastHead = uint32_t(head);
    logProduced = uint64_t(head);      // assume no wrap before boot-time init: the ring index equals the count
  } else if (uint32_t(head) != logLastHead) {
    logProduced += uint32_t(head) >= logLastHead ? uint32_t(head) - logLastHead : uint32_t(size) - logLastHead + uint32_t(head);
    logLastHead = uint32_t(head);
  }
  if (logOffset + uint64_t(size) < logProduced) logOffset = logProduced - uint64_t(size);
  if (logOffset >= logProduced) return;
  static uint8_t chunk[usbdbg::kMaxData - 8];
  size_t take = size_t(logProduced - logOffset);
  if (take > sizeof chunk) take = sizeof chunk;
  for (size_t i = 0; i < take; i++) chunk[i] = uint8_t(msgbufp->msg_bufc[(logOffset + i) % uint64_t(size)]);
  static uint8_t data[usbdbg::kMaxData];
  size_t dn = usbdbg::buildLogData(logOffset, chunk, take, data);
  if (queueFrame(usbdbg::kChLog, data, dn)) logOffset += take;   // ring full: retry on the next poll
}

}  // namespace dwc3

#endif
