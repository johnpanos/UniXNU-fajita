// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synopsys DesignWare USB3 DRD ("DWC3") register map and decode helpers for the
// SDM845 primary USB controller (usb_1_dwc3 @ 0x0a600000, core window 0xcd00).
//
// Only register OFFSETS and field positions are used here. They are architectural
// facts of the Synopsys DWC3 IP, the same numbers the mainline Linux dwc3 driver
// records in drivers/usb/dwc3/core.h (GPL-2.0); no DWC3 code is copied. Everything
// below is either a plain offset/bit constant or our own decode logic. The driver
// is a read-only probe first (see NativeDwc3.cpp); nothing here writes MMIO.
//
// This header is also compiled on the host by tests/ (NATIVE_DWC3_HOST), so it uses
// only fixed-width integer types and no kernel headers.
#ifndef NATIVE_DWC3_REGISTERS_H
#define NATIVE_DWC3_REGISTERS_H

#include <stdint.h>

// ---- Physical layout left by the firmware (SDM845 / fajita) -------------------
// From the upstream device tree (arch/arm64/boot/dts/qcom/sdm845.dtsi,
// sdm845-oneplus-common.dtsi) and Mu-Silicium's MemoryMapLib ("USB30_PRIM",
// 0x0A600000, 0x0011B000):
//   usb_1       wrapper   0x0a6f8800 (QSCRATCH etc., not touched by the probe)
//   usb_1_dwc3  core      0x0a600000  size 0xcd00
//   usb_1_hsphy QUSB2 v2  0x088e2000  size 0x400
//   GCC         clocks    0x00100000
//   apps_smmu             0x15000000  size 0x80000,  dwc3 stream <&apps_smmu 0x740 0>
enum : uint64_t {
  kDwc3CoreBase   = 0x0a600000ull,
  kDwc3CoreSize   = 0xcd00ull,
  kDwc3QscratchBase = 0x0a6f8800ull,   // usb_1 wrapper (GGPIO/QSCRATCH); probe maps 0x1000
  kQusb2PhyBase   = 0x088e2000ull,
  kQusb2PhySize   = 0x400ull,
  kGccBase        = 0x00100000ull,
  kAppsSmmuBase   = 0x15000000ull,
  kAppsSmmuSize   = 0x80000ull,
};
enum : uint16_t { kDwc3SmmuStream = 0x740, kDwc3SmmuMask = 0x0 };

// ---- GCC branch/GDSC offsets for USB30_PRIM ----------------------------------
// From the SDM845 GCC driver (drivers/clk/qcom/gcc-sdm845.c): halt_reg of each
// clk_branch and the GDSC gdscr. CLK_OFF is bit31 of a CBCR; PWR_ON is bit31 of a
// GDSCR. Read-only: the probe only reports whether UEFI left these running.
enum : uint32_t {
  kGccUsb30PrimGdscr        = 0xf004,   // usb30_prim_gdsc
  kGccUsb30PrimMasterCbcr   = 0xf00c,   // gcc_usb30_prim_master_clk
  kGccUsb30PrimSleepCbcr    = 0xf010,   // gcc_usb30_prim_sleep_clk
  kGccUsb30PrimMockUtmiCbcr = 0xf014,   // gcc_usb30_prim_mock_utmi_clk
  kGccCfgNocUsb3PrimCbcr    = 0x502c,   // gcc_cfg_noc_usb3_prim_axi_clk
  kGccAggreUsb3PrimCbcr     = 0x8201c,  // gcc_aggre_usb3_prim_axi_clk
  kGccUsbPhyCfgAhb2PhyCbcr  = 0x6a004,  // gcc_usb_phy_cfg_ahb2phy_clk
  // Block reset controls (gcc-sdm845.c gcc_resets): asserted/deasserted during
  // bring-up (stage B). Reset bit is CBCR-style bit0 (BLK_ARES) in Linux's
  // regmap_update_bits on the BCR word.
  kGccUsb30PrimBcr          = 0xf000,   // GCC_USB30_PRIM_BCR
  kGccQusb2PhyPrimBcr       = 0x12000,  // GCC_QUSB2PHY_PRIM_BCR
  kGccUsbPhyCfgAhb2PhyBcr   = 0x6a000,  // GCC_USB_PHY_CFG_AHB2PHY_BCR
  kGccBlkAres               = 1u << 0,
  kGccGdscrPwrOn            = 1u << 31,  // GDSCR PWR_ON status (read); SW_COLLAPSE is bit0
  kGccCbcrClkEnable         = 1u << 0,   // CBCR CLK_ENABLE (write); CLK_OFF is bit31 (read)
};

// ---- QSCRATCH / wrapper registers (wrapper base 0x0a6f8800 + offset) ----------
// SDM845 dwc3-qcom glue (drivers/usb/dwc3/dwc3-qcom.c): the USB2-only board selects
// the UTMI clock as the pipe clock and overrides VBUS/session for device mode, since
// there is no Type-C/PMIC driver to assert it. Stage B only; the probe never writes.
enum : uint32_t {
  kQscratchGeneralCfg       = 0x08,
  kQscratchHsPhyCtrl        = 0x10,
  kQscratchSsPhyCtrl        = 0x30,
  kQscratchPipeUtmiClkSel   = 1u << 0,   // PIPE_UTMI_CLK_SEL
  kQscratchPipe3PhystatusSw = 1u << 3,   // PIPE3_PHYSTATUS_SW
  kQscratchPipeUtmiClkDis   = 1u << 8,   // PIPE_UTMI_CLK_DIS
  kQscratchUtmiOtgVbusValid = 1u << 20,  // HS_PHY_CTRL: UTMI_OTG_VBUS_VALID
  kQscratchSwSessvldSel     = 1u << 28,  // HS_PHY_CTRL: SW_SESSVLD_SEL
  kQscratchLane0PwrPresent  = 1u << 24,  // SS_PHY_CTRL: LANE0_PWR_PRESENT (unused HS-only)
};

// ---- DWC3 global registers (core base + offset) ------------------------------
enum : uint32_t {
  kDwc3GSBUSCFG0   = 0xc100,
  kDwc3GSBUSCFG1   = 0xc104,
  kDwc3GCTL        = 0xc110,
  kDwc3GUCTL1      = 0xc11c,
  kDwc3GSTS        = 0xc118,
  kDwc3GSNPSID     = 0xc120,
  kDwc3GGPIO       = 0xc124,
  kDwc3GUID        = 0xc128,
  kDwc3GUCTL       = 0xc12c,
  kDwc3GBUSERRADDR0 = 0xc130,
  kDwc3GBUSERRADDR1 = 0xc134,
  kDwc3GHWPARAMS0  = 0xc140,
  kDwc3GHWPARAMS1  = 0xc144,
  kDwc3GHWPARAMS2  = 0xc148,
  kDwc3GHWPARAMS3  = 0xc14c,
  kDwc3GHWPARAMS4  = 0xc150,
  kDwc3GHWPARAMS5  = 0xc154,
  kDwc3GHWPARAMS6  = 0xc158,
  kDwc3GHWPARAMS7  = 0xc15c,
  kDwc3GHWPARAMS8  = 0xc600,
  kDwc3GUSB2PHYCFG0 = 0xc200,
  kDwc3GUSB3PIPECTL0 = 0xc2c0,
  // Event buffer 0 (we use a single event buffer for the gadget).
  kDwc3GEVNTADRLO0 = 0xc400,
  kDwc3GEVNTADRHI0 = 0xc404,
  kDwc3GEVNTSIZ0   = 0xc408,
  kDwc3GEVNTCOUNT0 = 0xc40c,
};

// ---- DWC3 device (gadget) registers ------------------------------------------
enum : uint32_t {
  kDwc3DCFG    = 0xc700,
  kDwc3DCTL    = 0xc704,
  kDwc3DEVTEN  = 0xc708,
  kDwc3DSTS    = 0xc70c,
  kDwc3DGCMDPAR = 0xc710,
  kDwc3DGCMD   = 0xc714,
  kDwc3DALEPENA = 0xc720,
  // Per-endpoint command: base + ep*0x10.
  kDwc3DEPCMDPAR2Base = 0xc800,
  kDwc3DEPCMDPAR1Base = 0xc804,
  kDwc3DEPCMDPAR0Base = 0xc808,
  kDwc3DEPCMDBase     = 0xc80c,
};
static inline uint32_t dwc3DepCmd(unsigned ep)     { return kDwc3DEPCMDBase + ep * 0x10u; }
static inline uint32_t dwc3DepCmdPar0(unsigned ep) { return kDwc3DEPCMDPAR0Base + ep * 0x10u; }
static inline uint32_t dwc3DepCmdPar1(unsigned ep) { return kDwc3DEPCMDPAR1Base + ep * 0x10u; }
static inline uint32_t dwc3DepCmdPar2(unsigned ep) { return kDwc3DEPCMDPAR2Base + ep * 0x10u; }

// ---- Gadget bit fields (facts of the DWC3 IP; from dwc3/core.h, gadget.h) -----
// Endpoint command word (DEPCMD): command in [3:0], ACT/IOC, param in [31:16].
enum : uint32_t {
  kDwc3DepCmdSetEpConfig = 0x01, kDwc3DepCmdSetXferResource = 0x02, kDwc3DepCmdStartXfer = 0x06,
  kDwc3DepCmdUpdateXfer = 0x07, kDwc3DepCmdEndXfer = 0x08, kDwc3DepCmdStartConfig = 0x09,
  kDwc3DepCmdAct = 1u << 10, kDwc3DepCmdIoc = 1u << 8,
};
static inline uint32_t dwc3DepCmdParam(uint32_t p) { return p << 16; }
static inline unsigned dwc3DepCmdStatus(uint32_t reg) { return (reg >> 12) & 0xf; }
// DEPCFG param0/param1 (SETEPCONFIG): type [2:1], max packet [13:3], fifo num [21:17],
// ep number [29:25]; param1 enables: xfer-complete bit8, xfer-not-ready bit10,
// xfer-in-progress bit9; action in param0 [31:30] (0 = init).
enum : uint32_t {
  kDwc3DepCfgTypeShift = 1, kDwc3DepCfgMaxPktShift = 3, kDwc3DepCfgFifoNumShift = 17, kDwc3DepCfgEpNumShift = 25,
  kDwc3DepCfgXferCompleteEn = 1u << 8, kDwc3DepCfgXferInProgressEn = 1u << 9, kDwc3DepCfgXferNotReadyEn = 1u << 10,
  kDwc3DepCfgActionInit = 0u << 30,
  kDwc3DepXferCfgNumXferRes1 = 1,          // DEPXFERCFG param0 = 1 resource
  kDwc3EpTypeControl = 0, kDwc3EpTypeIsoc = 1, kDwc3EpTypeBulk = 2, kDwc3EpTypeIntr = 3,
};
// TRB control [5:0] TRBCTL (<<4), HWO bit0, LST bit1, CHN bit2, CSP bit3, ISP/IMI bit10, IOC bit11.
enum : uint32_t {
  kDwc3TrbHwo = 1u << 0, kDwc3TrbLst = 1u << 1, kDwc3TrbChn = 1u << 2, kDwc3TrbCsp = 1u << 3,
  kDwc3TrbIspImi = 1u << 10, kDwc3TrbIoc = 1u << 11,
  kDwc3TrbCtlNormal = 1u << 4, kDwc3TrbCtlControlSetup = 2u << 4, kDwc3TrbCtlControlStatus2 = 3u << 4,
  kDwc3TrbCtlControlStatus3 = 4u << 4, kDwc3TrbCtlControlData = 5u << 4, kDwc3TrbCtlLink = 8u << 4,
  kDwc3TrbSizeMask = 0x00ffffff,
};
// Device event enables (DEVTEN) and device-event / endpoint-event types.
enum : uint32_t {
  kDwc3DevtenDisconn = 1u << 0, kDwc3DevtenUsbReset = 1u << 1, kDwc3DevtenConnectDone = 1u << 2,
  kDwc3DevtenUlstChng = 1u << 3, kDwc3DevtenWakeup = 1u << 4, kDwc3DevtenEvntOverflow = 1u << 11,
  kDwc3DevtenCmdCmplt = 1u << 10, kDwc3DevtenErrTicErr = 1u << 9, kDwc3DevtenSuspend = 1u << 6,
};
enum : unsigned {
  kDwc3DevEvtDisconnect = 0, kDwc3DevEvtReset = 1, kDwc3DevEvtConnectDone = 2, kDwc3DevEvtLinkStsChange = 3,
  kDwc3DevEvtWakeup = 4, kDwc3DevEvtSuspend = 6, kDwc3DevEvtCmdCmpl = 10,
  kDwc3DepEvtXferComplete = 1, kDwc3DepEvtXferInProgress = 2, kDwc3DepEvtXferNotReady = 3, kDwc3DepEvtEpCmdCmplt = 7,
};
// DCFG: IGNSTRMPP bit23, NUMP in [21:17]. GEVNTCOUNT EHB bit31 ack. DCTL KEEP_CONNECT bit19.
enum : uint32_t {
  kDwc3DcfgIgnStrmPp = 1u << 23, kDwc3DcfgNumpShift = 17, kDwc3DcfgLpmCap = 1u << 22,
  kDwc3GevntCountEhb = 1u << 31, kDwc3GevntSizIntMask = 1u << 31,
  kDwc3DctlKeepConnect = 1u << 19,
  kDwc3DstsDevCtrlHlt = 1u << 22,
};
// An event-buffer entry: bit0 clear = endpoint event (ep in [5:1], type in [9:6]);
// bit0 set = non-endpoint; a device event has device-event type in [7:1].
static inline bool dwc3EvtIsDevice(uint32_t e) { return (e & 1u) != 0; }
static inline unsigned dwc3DepEvtEp(uint32_t e) { return (e >> 1) & 0x1f; }
static inline unsigned dwc3DepEvtType(uint32_t e) { return (e >> 6) & 0xf; }
static inline unsigned dwc3DepEvtStatus(uint32_t e) { return (e >> 12) & 0xf; }
static inline unsigned dwc3DevEvtType(uint32_t e) { return (e >> 1) & 0x7f; }

// ---- Field helpers ------------------------------------------------------------
// GSNPSID: the top 16 bits are the IP class, the low 16 the revision. DWC_usb3
// reports 0x5533xxxx; DWC_usb31 reports 0x3331xxxx / 0x3330xxxx.
static inline uint16_t dwc3SnpsClass(uint32_t id) { return uint16_t(id >> 16); }
static inline uint16_t dwc3SnpsRevision(uint32_t id) { return uint16_t(id & 0xffff); }
static inline bool dwc3SnpsIsUsb3(uint32_t id) {
  uint16_t c = dwc3SnpsClass(id);
  return c == 0x5533 || c == 0x3331 || c == 0x3330;
}

// GCTL: core soft reset is bit11; prtcap direction (current mode) is bits [13:12]
// (1 = host, 2 = device, 3 = OTG).
enum : uint32_t { kDwc3GctlCoreSoftReset = 1u << 11 };
static inline unsigned dwc3GctlPrtCap(uint32_t gctl) { return (gctl >> 12) & 3u; }

// DCFG: device speed is bits [2:0] (0 hs, 1 fs, 4 ss, ...); device address bits [9:3].
static inline unsigned dwc3DcfgSpeed(uint32_t dcfg) { return dcfg & 7u; }
static inline unsigned dwc3DcfgDevAddr(uint32_t dcfg) { return (dcfg >> 3) & 0x7fu; }

// DCTL run/stop is bit31; core soft reset is bit30.
enum : uint32_t { kDwc3DctlRunStop = 1u << 31, kDwc3DctlCoreSoftReset = 1u << 30 };

// DSTS: connected link speed bits [2:0]; USBLNKST (link state) bits [21:18];
// DEVCTRLHLT bit22 is set while the device controller is halted.
static inline unsigned dwc3DstsConnectSpeed(uint32_t dsts) { return dsts & 7u; }
static inline unsigned dwc3DstsLinkState(uint32_t dsts) { return (dsts >> 18) & 0xfu; }
static inline bool dwc3DstsHalted(uint32_t dsts) { return (dsts & (1u << 22)) != 0; }

// GEVNTSIZ: event-buffer size in bytes is bits [15:0]; bit31 masks the event
// interrupt. GEVNTCOUNT: pending byte count is bits [15:0].
static inline uint32_t dwc3EvntSizBytes(uint32_t siz) { return siz & 0xffffu; }
static inline bool dwc3EvntSizMasked(uint32_t siz) { return (siz & (1u << 31)) != 0; }
static inline uint32_t dwc3EvntCountBytes(uint32_t cnt) { return cnt & 0xffffu; }

// ---- A snapshot of the controller state, for the boot log --------------------
struct Dwc3Snapshot {
  uint32_t gctl, gsts, gsnpsid, ghwparams0, ghwparams1, ghwparams3;
  uint32_t dcfg, dctl, dsts, dalepena, devten;
  uint32_t gevntsiz0, gevntcount0;
  uint32_t gusb2phycfg0;
  uint32_t qusb2_pll_status;   // QUSB2 PHY PLL/common status word (see Dwc3Phy.h)
  uint32_t gcc_gdscr, gcc_master, gcc_mock_utmi, gcc_sleep, gcc_noc_axi, gcc_aggre_axi, gcc_ahb2phy;
};

// Does the GCC state the firmware left mean the DWC3 core window can be read
// safely? PWR_ON (GDSCR bit31) must be set and the master + AHB2PHY branches must
// not be CLK_OFF (CBCR bit31). Mirrors NativeUfs's read-only clock gate.
static inline bool dwc3GccCoreReadable(const Dwc3Snapshot &s) {
  bool powered = (s.gcc_gdscr & 0x80000000u) != 0;
  bool master = !(s.gcc_master & 0x80000000u);
  bool ahb2phy = !(s.gcc_ahb2phy & 0x80000000u);
  return powered && master && ahb2phy;
}

#endif
