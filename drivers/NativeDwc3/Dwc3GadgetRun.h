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
  uint64_t logOffset;

  bool bringUp(unsigned mode);
  void poll();                     // drain the event buffer once
  void pumpLog();                  // mirror new kernel-log bytes to the host

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
  void onBulkOut(uint32_t transferred);
  void sendFrame(uint8_t channel, const uint8_t *data, size_t len);
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

inline bool Gadget::smmuMap740()
{
  IODeviceMemory *mem = IODeviceMemory::withRange(kAppsSmmuBase, kAppsSmmuSize);
  if (!mem) return false;
  IOMemoryMap *map = mem->map(kIOMapAnywhere | kIOMapInhibitCache);   // kept mapped for the driver's life
  mem->release();
  if (!map) return false;
  uintptr_t smmu = uintptr_t(map->getVirtualAddress());
  if (!smmu) return false;
  auto rd = [smmu](unsigned off) { return *reinterpret_cast<volatile uint32_t *>(smmu + off); };
  auto wr = [smmu](unsigned off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(smmu + off) = v; __asm__ volatile("dsb sy" ::: "memory"); };
  UfsSmmuPlan plan = planUfsSmmu(kDwc3SmmuStream, kDwc3SmmuMask, kAppsSmmuSize, dma.phys, GadgetDma::kTotal, rd, true);
  if (plan.error) { native_dwc3_gadget_failure = plan.error; return false; }
  const char *err = configureUfsSmmu(plan, kDwc3SmmuStream, kDwc3SmmuMask, rd, wr);
  if (err) { native_dwc3_gadget_failure = err; return false; }
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
  acmep0::Setup s = acmep0::parseSetup(dma.at(GadgetDma::kEp0Data));
  native_dwc3_setups = native_dwc3_setups + 1;
  pending = acmep0::handleSetup(ep0, s);
  switch (pending.kind) {
  case acmep0::kStall:
    depCmd(m, kPEp0In, kDwc3DepCmdStartXfer, 0, 0, 0);   // (a real stall uses SETSTALL; re-arm setup instead)
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
  depCmd(m, kPBulkOut, kDwc3DepCmdStartConfig | dwc3DepCmdParam(2), 0, 0, 0);
  auto cfgBulk = [&](unsigned ep) {
    uint32_t p0 = uint32_t(kDwc3EpTypeBulk << kDwc3DepCfgTypeShift) | (512u << kDwc3DepCfgMaxPktShift) | kDwc3DepCfgActionInit;
    uint32_t p1 = kDwc3DepCfgXferCompleteEn | kDwc3DepCfgXferInProgressEn | (uint32_t(ep) << kDwc3DepCfgEpNumShift);
    if (ep & 1) p0 |= (uint32_t(ep >> 1) << kDwc3DepCfgFifoNumShift);
    depCmd(m, ep, kDwc3DepCmdSetEpConfig, p0, p1, 0);
    depCmd(m, ep, kDwc3DepCmdSetXferResource, kDwc3DepXferCfgNumXferRes1, 0, 0);
  };
  cfgBulk(kPBulkOut);
  cfgBulk(kPBulkIn);
  m.core_wr(kDwc3DALEPENA, m.core_rd(kDwc3DALEPENA) | (1u << kPBulkOut) | (1u << kPBulkIn));
  bulkStarted = true;
  armBulkOut();
  // Greet the host so a round-trip is visible without any host command.
  const char hello[] = "fajita usb-debug: gadget up\r\n";
  sendFrame(usbdbg::kChLog, nullptr, 0);   // no-op guard; real log via pumpLog
  (void)hello;
}

inline void Gadget::armBulkOut()
{
  Trb *t = dma.trb(GadgetDma::kBulkOutTrb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kBulkOutData));
  t->bph = uint32_t(dma.pa(GadgetDma::kBulkOutData) >> 32);
  t->size = 512;
  t->ctrl = kDwc3TrbCtlNormal | kDwc3TrbHwo | kDwc3TrbIspImi | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  depCmd(m, kPBulkOut, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kBulkOutTrb) >> 32), uint32_t(dma.pa(GadgetDma::kBulkOutTrb)), 0);
  bulkOutArmed = true;
}

inline void Gadget::sendFrame(uint8_t channel, const uint8_t *data, size_t len)
{
  if (!bulkStarted || len == 0) return;
  uint8_t *out = dma.at(GadgetDma::kBulkInData);
  size_t n = usbdbg::encodeFrame(channel, txSeq++, data, len, out, 4096);
  if (!n) return;
  cache_flush(out, n);
  Trb *t = dma.trb(GadgetDma::kBulkInTrb);
  t->bpl = uint32_t(dma.pa(GadgetDma::kBulkInData));
  t->bph = uint32_t(dma.pa(GadgetDma::kBulkInData) >> 32);
  t->size = uint32_t(n);
  t->ctrl = kDwc3TrbCtlNormal | kDwc3TrbHwo | kDwc3TrbIspImi | kDwc3TrbIoc | kDwc3TrbLst;
  cache_flush(t, sizeof *t);
  depCmd(m, kPBulkIn, kDwc3DepCmdStartXfer, uint32_t(dma.pa(GadgetDma::kBulkInTrb) >> 32), uint32_t(dma.pa(GadgetDma::kBulkInTrb)), 0);
  native_dwc3_bulk_in = native_dwc3_bulk_in + n;
}

inline void Gadget::onBulkOut(uint32_t transferred)
{
  cache_flush(dma.at(GadgetDma::kBulkOutData), 512);
  uint32_t got = transferred <= 512 ? 512 - transferred : 512;   // TRB size counts down
  native_dwc3_bulk_out = native_dwc3_bulk_out + got;
  const uint8_t *in = dma.at(GadgetDma::kBulkOutData);
  for (uint32_t i = 0; i < got; i++) {
    if (rx.feed(in[i]) == usbdbg::Decoder::kFrame && rx.channel() == usbdbg::kChCmd) {
      usbdbg::Target t{};
      t.ident = "fajita-usb-debug";
      t.features = usbdbg::kFeatLog;
      alignas(4) uint8_t rsp[usbdbg::kMaxData];
      size_t rn = usbdbg::handleCommand(t, rx.data(), rx.length(), rsp);
      sendFrame(usbdbg::kChRsp, rsp, rn);
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
    if (dwc3EvtIsDevice(e)) {
      unsigned type = dwc3DevEvtType(e);
      if (type == kDwc3DevEvtReset) {
        phase = kWaitSetup;
        uint32_t dcfg = m.core_rd(kDwc3DCFG); dcfg &= ~(0x7fu << 3); m.core_wr(kDwc3DCFG, dcfg);
        native_dwc3_addr = 0;
      } else if (type == kDwc3DevEvtConnectDone) {
        uint32_t speed = m.core_rd(kDwc3DSTS) & 7u;
        native_dwc3_link_speed = speed;
        ep0.highSpeed = (speed == 0);     // DWC3_DSTS_HIGHSPEED == 0
        ep0QueueSetup();
      }
    } else {
      unsigned ep = dwc3DepEvtEp(e), etype = dwc3DepEvtType(e);
      if (etype == kDwc3DepEvtXferComplete || etype == kDwc3DepEvtXferInProgress) {
        if (ep == kPEp0Out && phase == kWaitSetup) onSetupComplete();
        else if (ep == kPEp0Out || ep == kPEp0In) onEp0DataOrStatus(ep);
        else if (ep == kPBulkOut) onBulkOut(dma.trb(GadgetDma::kBulkOutTrb)->size & kDwc3TrbSizeMask);
      }
    }
  }
  m.core_wr(kDwc3GEVNTCOUNT0, count);   // acknowledge the bytes we consumed
}

inline void Gadget::pumpLog()
{
  // Mirror new kernel msgbuf bytes to the host as LOG frames (device->host).
  extern struct { int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc; } *msgbufp __attribute__((weak_import));
  if (!&msgbufp || !msgbufp || !bulkStarted) return;
  int size = msgbufp->msg_size, head = msgbufp->msg_bufx;
  if (size <= 0 || head < 0 || head > size) return;
  uint64_t total = uint64_t(head);
  if (total <= logOffset) return;
  uint8_t chunk[usbdbg::kMaxData - 8];
  size_t take = size_t(total - logOffset);
  if (take > sizeof chunk) { logOffset = total - sizeof chunk; take = sizeof chunk; }
  for (size_t i = 0; i < take; i++) chunk[i] = uint8_t(msgbufp->msg_bufc[(logOffset + i) % size]);
  uint8_t data[usbdbg::kMaxData];
  size_t dn = usbdbg::buildLogData(logOffset, chunk, take, data);
  sendFrame(usbdbg::kChLog, data, dn);
  logOffset = total;
}

}  // namespace dwc3

#endif
