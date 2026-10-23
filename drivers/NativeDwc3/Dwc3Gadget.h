// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage B: the SDM845 DWC3 CDC-ACM device controller, driven polled (no IRQ), so it
// can run from the earliest boot and, later, from the debugger path. Only this file
// writes device-mode registers. It follows the mainline Linux sequence
// (drivers/usb/dwc3/{core,gadget,ep0,dwc3-qcom}.c, phy-qcom-qusb2.c; reference clone
// /home/john/src/ref/linux) step for step; no driver code is copied, only the
// documented register order and field values.
//
// It is #included by NativeDwc3.cpp (the build compiles that one TU) and uses IOKit,
// so it is not a host-testable header; the hardware-independent logic it calls
// (AcmEp0.h, AcmDescriptors.h, UsbDbgProto.h) is tested on the host.
#ifndef NATIVE_DWC3_GADGET_H
#define NATIVE_DWC3_GADGET_H

#include <IOKit/IOBufferMemoryDescriptor.h>
#include "Dwc3Registers.h"
#include "Dwc3Phy.h"
#include "AcmEp0.h"
#include "UsbDbgProto.h"

extern "C" {
extern volatile uint32_t native_dwc3_gadget_stage, native_dwc3_clocks_up, native_dwc3_phy_up, native_dwc3_link_speed;
extern volatile uint32_t native_dwc3_setups, native_dwc3_configured, native_dwc3_events, native_dwc3_addr;
extern volatile int32_t native_dwc3_gadget_status;
extern volatile uint64_t native_dwc3_bulk_out, native_dwc3_bulk_in;
extern const char *volatile native_dwc3_gadget_failure;
extern volatile int32_t native_dwc3_smmu_slot_g, native_dwc3_smmu_bank_g;
extern volatile uint32_t native_dwc3_smmu_banks;
}

namespace dwc3 {

enum GadgetStage : uint32_t {
  kStClocks = 1, kStQscratch = 2, kStPhy = 3, kStCoreReset = 4, kStGlobal = 5,
  kStEventBuf = 6, kStSmmu = 7, kStGadgetStart = 8, kStRunStop = 9, kStRunning = 10,
};

// One DWC3 TRB (16 bytes): buffer ptr low/high, size, control.
struct Trb { uint32_t bpl, bph, size, ctrl; };

// MMIO windows.
struct Mmio {
  uintptr_t core = 0, qscratch = 0, phy = 0, gcc = 0;
  uint32_t rd(uintptr_t base, unsigned off) const { return *reinterpret_cast<volatile uint32_t *>(base + off); }
  void wr(uintptr_t base, unsigned off, uint32_t v) const {
    *reinterpret_cast<volatile uint32_t *>(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
  }
  uint32_t core_rd(unsigned o) const { return rd(core, o); }
  void core_wr(unsigned o, uint32_t v) const { wr(core, o, v); }
};

// The gadget's DMA region (physically contiguous, mapped through SMMU stream 0x740):
// a 4 KiB event buffer, the ep0/bulk TRBs, and the ep0/bulk data buffers.
struct GadgetDma {
  IOBufferMemoryDescriptor *buf;   // kept for the driver's life (boot-time; never freed)
  uint8_t *virt;
  uint64_t phys;
  // Offsets inside the region.
  static constexpr uint32_t kEvent = 0x0000, kEventBytes = 0x1000;
  static constexpr uint32_t kEp0Trb = 0x1000;     // one TRB (room for a few)
  static constexpr uint32_t kBulkOutTrb = 0x1040;
  static constexpr uint32_t kBulkInTrb = 0x1080;
  static constexpr uint32_t kEp0Data = 0x2000;    // ep0 IN/OUT staging (<=512)
  static constexpr uint32_t kBulkOutData = 0x3000;
  static constexpr uint32_t kBulkInData = 0x4000;
  static constexpr uint32_t kTotal = 0x10000;     // 64 KiB
  uint8_t *at(uint32_t off) { return virt + off; }
  uint64_t pa(uint32_t off) { return phys + off; }
  Trb *trb(uint32_t off) { return reinterpret_cast<Trb *>(virt + off); }
};

static void gdelay(unsigned us) { IODelay(us); }
static void cache_flush(const void *p, size_t n) {
  uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(63), e = reinterpret_cast<uintptr_t>(p) + n;
  for (; a < e; a += 64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

// Bring the USB30_PRIM clocks up if UEFI did not leave them running. Returns false on
// timeout. Mirrors clk-qcom branch/gdsc enable, but only touches what is off.
static bool clocksEnable(const Mmio &m, bool &weEnabled)
{
  weEnabled = false;
  uint32_t gdscr = m.rd(m.gcc, kGccUsb30PrimGdscr);
  if (!(gdscr & kGccGdscrPwrOn)) {
    m.wr(m.gcc, kGccUsb30PrimGdscr, gdscr & ~1u);   // clear SW_COLLAPSE
    weEnabled = true;
    for (int i = 0; i < 1000; i++) { if (m.rd(m.gcc, kGccUsb30PrimGdscr) & kGccGdscrPwrOn) break; gdelay(10); }
    if (!(m.rd(m.gcc, kGccUsb30PrimGdscr) & kGccGdscrPwrOn)) return false;
  }
  const unsigned cbcr[] = {kGccUsb30PrimMasterCbcr, kGccUsb30PrimMockUtmiCbcr, kGccUsb30PrimSleepCbcr,
                           kGccCfgNocUsb3PrimCbcr, kGccAggreUsb3PrimCbcr, kGccUsbPhyCfgAhb2PhyCbcr};
  for (unsigned off : cbcr) {
    uint32_t v = m.rd(m.gcc, off);
    if (v & 0x80000000u) {   // CLK_OFF set: enable and wait for it to clear
      m.wr(m.gcc, off, v | kGccCbcrClkEnable);
      weEnabled = true;
      for (int i = 0; i < 500; i++) { if (!(m.rd(m.gcc, off) & 0x80000000u)) break; gdelay(10); }
    }
  }
  return true;
}

// QSCRATCH: select UTMI as the pipe clock (USB2-only board) and force VBUS/session
// valid for device mode (no PMIC type-C driver asserts it).
static void qscratchSetup(const Mmio &m)
{
  uint32_t g = m.rd(m.qscratch, kQscratchGeneralCfg);
  m.wr(m.qscratch, kQscratchGeneralCfg, g | kQscratchPipeUtmiClkDis);
  gdelay(200);
  g = m.rd(m.qscratch, kQscratchGeneralCfg);
  m.wr(m.qscratch, kQscratchGeneralCfg, g | kQscratchPipeUtmiClkSel | kQscratchPipe3PhystatusSw);
  gdelay(200);
  g = m.rd(m.qscratch, kQscratchGeneralCfg);
  m.wr(m.qscratch, kQscratchGeneralCfg, g & ~kQscratchPipeUtmiClkDis);
  uint32_t h = m.rd(m.qscratch, kQscratchHsPhyCtrl);
  m.wr(m.qscratch, kQscratchHsPhyCtrl, h | kQscratchUtmiOtgVbusValid | kQscratchSwSessvldSel);
}

// QUSB2 v2 HS PHY init (phy-qcom-qusb2.c qusb2_v2 table). Skipped if the PLL is
// already locked (UEFI left it up). Returns false if the PLL does not lock.
static bool phyInit(const Mmio &m, bool &weInit)
{
  weInit = false;
  if (m.rd(m.phy, kQusb2PllCommonStatusOne) & kQusb2CoreReady) return true;   // already locked
  weInit = true;
  // Power down, program the init table, power up (offsets are QUSB2 v2 facts).
  m.wr(m.phy, 0x210, 0x23);                 // PORT_POWERDOWN: disable_ctrl bits (VREF|CLAMP_N|POWER_DOWN)
  const uint16_t tbl[][2] = {
    {0x04, 0x03}, {0x18c, 0x7c}, {0x2c, 0x80}, {0x184, 0x0a}, {0xb4, 0x19}, {0x194, 0x40}, {0x198, 0x20},
    {0x214, 0x21}, {0x220, 0x00}, {0x224, 0x58}, {0x240, 0x30}, {0x244, 0x29}, {0x248, 0xca}, {0x24c, 0x04},
    {0x250, 0x03}, {0x23c, 0x00},
  };
  for (auto &e : tbl) m.wr(m.phy, e[0], e[1]);
  uint32_t pd = m.rd(m.phy, 0x210);
  m.wr(m.phy, 0x210, pd & ~1u);             // clear POWER_DOWN
  gdelay(160);
  for (int i = 0; i < 20; i++) { if (m.rd(m.phy, kQusb2PllCommonStatusOne) & kQusb2CoreReady) return true; gdelay(50); }
  return (m.rd(m.phy, kQusb2PllCommonStatusOne) & kQusb2CoreReady) != 0;
}

// DWC3 core soft reset of the device side (DCTL.CSFTRST), poll until it clears.
static bool coreSoftReset(const Mmio &m)
{
  uint32_t dctl = m.core_rd(kDwc3DCTL);
  m.core_wr(kDwc3DCTL, (dctl | kDwc3DctlCoreSoftReset) & ~kDwc3DctlRunStop);
  for (int i = 0; i < 1000; i++) { if (!(m.core_rd(kDwc3DCTL) & kDwc3DctlCoreSoftReset)) return true; gdelay(1000); }
  return false;
}

// Run one endpoint command and wait for it to finish; returns the CMDSTATUS (0 = ok).
static int depCmd(const Mmio &m, unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
  m.core_wr(dwc3DepCmdPar0(ep), p0);
  m.core_wr(dwc3DepCmdPar1(ep), p1);
  m.core_wr(dwc3DepCmdPar2(ep), p2);
  m.core_wr(dwc3DepCmd(ep), cmd | kDwc3DepCmdAct);
  for (int i = 0; i < 5000; i++) {
    uint32_t r = m.core_rd(dwc3DepCmd(ep));
    if (!(r & kDwc3DepCmdAct)) return int(dwc3DepCmdStatus(r));
    gdelay(100);
  }
  return -1;
}

}  // namespace dwc3

#include "Dwc3GadgetRun.h"

#endif
