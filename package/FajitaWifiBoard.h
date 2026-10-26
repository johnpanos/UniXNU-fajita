// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita (OnePlus 6T, SDM845 / WCN3990) Wi-Fi board constants, with sources. Addresses from mainline Linux v6.16
// (/home/john/src/ref/linux): arch/arm64/boot/dts/qcom/sdm845.dtsi + sdm845-oneplus-common.dtsi, the mss-pil and wifi
// nodes, the GCC/AOSS/reset drivers. These feed the staged bring-up (package/FajitaWifiProbe.cpp, fajita-wifi=<stage>,
// default off) and are documented in NOTES-wifi.md. Nothing here touches hardware by itself.
#ifndef FAJITA_WIFI_BOARD_H
#define FAJITA_WIFI_BOARD_H
#include <stdint.h>

// ---- modem (MSS) remoteproc: qcom,sdm845-mss-pil (qcom_q6v5_mss.c, version MSS_SDM845) ----
#define FW_MSS_QDSP6_BASE 0x04080000ull     // reg "qdsp6", 0x408 bytes
#define FW_MSS_RMB_BASE   0x04180000ull     // reg "rmb", 0x48 bytes
#define FW_MSS_QDSP6_SIZE 0x1000u
#define FW_MSS_RMB_SIZE   0x1000u
// RMB register offsets (RMB_* in qcom_q6v5_mss.c)
#define FW_RMB_MBA_IMAGE       0x00
#define FW_RMB_PBL_STATUS      0x04
#define FW_RMB_MBA_COMMAND     0x08
#define FW_RMB_MBA_STATUS      0x0C
#define FW_RMB_PMI_META_DATA   0x10
#define FW_RMB_PMI_CODE_START  0x14
#define FW_RMB_PMI_CODE_LENGTH 0x18
#define FW_RMB_MBA_MSS_STATUS  0x40
#define FW_RMB_MBA_ALT_RESET   0x44
#define FW_RMB_PBL_SUCCESS            0x1
#define FW_RMB_MBA_XPU_UNLOCKED       0x1
#define FW_RMB_MBA_XPU_UNLOCKED_SCRIB 0x2
#define FW_RMB_MBA_META_AUTH_SUCCESS  0x3
#define FW_RMB_MBA_AUTH_COMPLETE      0x4
#define FW_RMB_CMD_META_DATA_READY    0x1
#define FW_RMB_CMD_LOAD_READY         0x2
// QDSP6SS registers (the MSS_SDM845 reset path)
#define FW_QDSP6SS_SLEEP           0x3C
#define FW_QDSP6SS_BOOT_CORE_START 0x400
#define FW_QDSP6SS_BOOT_CMD        0x404
#define FW_Q6SS_CBCR_CLKEN  (1u << 0)
#define FW_Q6SS_CBCR_CLKOFF (1u << 31)
#define FW_BOOT_FSM_TIMEOUT_US 10000

// Memory carveouts (sdm845.dtsi reserved-memory; "no-map" = never DRAM to the OS). Modem DMA targets.
#define FW_MBA_REGION_PHYS  0x96500000ull   // mba@96500000, 0x200000 (MBA copied here by q6v5_load)
#define FW_MBA_REGION_SIZE  0x200000ull
#define FW_MPSS_REGION_PHYS 0x8e000000ull   // mpss@8e000000, 0x7800000 (modem.mbn relocated here)
#define FW_MPSS_REGION_SIZE 0x7800000ull
#define FW_MDATA_REGION_SIZE 0x4000ull      // mdata_mem (metadata), alloc 0xa0000000..0xc0000000, 0x4000
#define FW_WLAN_MSA_PHYS    0x8df00000ull   // wlan-msa@8df00000, 0x100000 (WLAN MSA, wlfw msa_info)
#define FW_WLAN_MSA_SIZE    0x100000ull
// The WLAN device's DMA arena (CE rings, rx/tx buffers, HTT, WMI host memory): ordinary RAM held off XNU's free lists with
// phys-reserve (wifi-test.sh PHYS_RESERVE), mapped uncached; the target reaches it at IOVA = PA through the apps_smmu bypass
// bank of SIDs 0x40/0x41 (iommus = <&apps_smmu 0x0040 0x1>).
#define FW_WLAN_DMA_PHYS    0xf6000000ull
#define FW_WLAN_DMA_SIZE    0x800000ull
#define FW_WLAN_MEMBASE     0x18800000ull   // wifi@18800000, reg 0x800000 ("membase": CE registers, shadow write indices)
#define FW_WLAN_MEMBASE_SIZE 0x800000ull
#define FW_APPS_SMMU_PHYS   0x15000000ull   // apps_smmu, 0x80000
#define FW_APPS_SMMU_SIZE   0x80000ull
#define FW_WLAN_SMMU_SID    0x40u
#define FW_WLAN_SMMU_MASK   0x1u
#define FW_SMEM_PHYS        0x86000000ull   // smem@86000000, 0x200000
#define FW_SMEM_SIZE        0x200000ull
#define FW_RMTFS_PHYS       0xf5b01000ull   // fajita rmtfs-mem@f5b01000, 0x200000 (DT override of the SoC default)
#define FW_RMTFS_SIZE       0x200000ull

// ---- SCM (qcom_scm) memory-ownership assign (qcom_scm_assign_mem, SVC_MP 0x0c, cmd 0x16) ----
#define FW_SCM_SVC_MP   0x0c
#define FW_SCM_MP_ASSIGN 0x16
#define FW_SCM_VMID_HLOS    0x3
#define FW_SCM_VMID_MSS_MSA 0xF
#define FW_SCM_VMID_WLAN    0x18
#define FW_SCM_VMID_WLAN_CE 0x19
#define FW_SCM_PERM_READ 0x4
#define FW_SCM_PERM_WRITE 0x2
#define FW_SCM_PERM_EXEC 0x1
#define FW_SCM_PERM_RW (FW_SCM_PERM_READ | FW_SCM_PERM_WRITE)
#define FW_SCM_PERM_RWX (FW_SCM_PERM_RW | FW_SCM_PERM_EXEC)
// ARM_SMCCC STD / SMC64 / owner SIP (0x02), as FajitaGpuProbe's zap PAS calls. fn = 0x42000000 | (svc<<8) | cmd.

// ---- GCC branches the MSS needs (gcc-sdm845.c; base 0x100000, the enchilada UEFI "GCC CLK CTL") ----
#define FW_GCC_BASE 0x00100000ull
#define FW_GCC_MSS_CFG_AHB   0x8a000   // enable BIT(0), halt 0x8a000
#define FW_GCC_MSS_MFAB_AXIS 0x8a004   // enable BIT(0)
#define FW_GCC_MSS_SNOC_AXI  0x8a150   // enable BIT(0)
#define FW_GCC_MSS_Q6_MEMNOC 0x8a154   // enable BIT(0)
#define FW_GCC_BOOT_ROM_AHB_VOTE 0x52004   // BIT(10)
#define FW_GCC_PRNG_AHB_VOTE     0x52004   // BIT(13)
#define FW_GCC_MSS_GPLL0_DIV_VOTE 0x52004  // BIT(17)

// ---- AOSS QMP (qcom_aoss.c; mailbox apss_shared 0; load_state "{class: image, res: load_state, name: modem, val: on}") ----
#define FW_AOSS_QMP_PHYS 0x0c300000ull
#define FW_AOSS_QMP_SIZE 0x400u
#define FW_APSS_SHARED_PHYS 0x17990000ull   // mailbox@17990000, IPC trigger at +12 (apps_shared_apcs_data.offset)
#define FW_APSS_IPC_OFFSET  12
#define FW_APSS_IPC_MODEM   12              // apss_shared 12 = modem glink-edge mbox
#define FW_APSS_IPC_AOSS    0               // apss_shared 0  = aoss_qmp mbox

// ---- resets (reset-qcom-aoss.c, reset-qcom-pdc.c) ----
#define FW_AOSS_CC_BASE  0x0c2a0000ull      // aoss_reset: MSS_RESTART at +0x10000, assert=1 deassert=0, 200us
#define FW_AOSS_CC_MSS_RESTART 0x10000
#define FW_PDC_GLOBAL_BASE 0x0b2e0000ull    // pdc_reset: PDC_MODEM_SYNC_RESET bit 9 in RPMH_PDC_SYNC_RESET 0x100
#define FW_PDC_SYNC_RESET 0x100
#define FW_PDC_MODEM_SYNC_BIT 9

// ---- GLINK over SMEM (qcom_glink_smem.c): the modem edge, remote pid 1 ----
#define FW_GLINK_MODEM_PID 1
#define FW_SMEM_GLINK_DESC 478              // SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR, 32 bytes (tx tail/head, rx tail/head)
#define FW_SMEM_GLINK_FIFO_0 479            // TX fifo (SZ_16K)
#define FW_SMEM_GLINK_FIFO_1 480            // RX fifo
#define FW_GLINK_FIFO_BYTES (16u << 10)

// ---- SMP2P modem (smp2p.c): smem items, SPI 451, apss_shared 14 ----
// qcom,smem = <435>, <428>: smp2p.c takes smem_items[SMP2P_INBOUND] first, so 435 is the modem's item (inbound for the AP) and 428
// the AP's own (outbound). Round 3 had these two swapped: the AP allocated the modem's item and the modem never found its peer.
#define FW_SMP2P_MPSS_OUT_ITEM 428
#define FW_SMP2P_MPSS_IN_ITEM  435
#define FW_SMP2P_SPI 451
#define FW_SMP2P_APCS_MBOX 14
#define FW_SMP2P_MAGIC 0x504d5324u          // "$PMS" little-endian

// ---- WLAN / CE (wifi@18800000) ----
#define FW_WIFI_MEMBASE 0x18800000ull       // reg, 0x800000
#define FW_WIFI_MEMSIZE 0x800000ull
#define FW_WIFI_CE_IRQ_FIRST_SPI 414        // 12 CE IRQs, SPI 414..425 (INTID = SPI + 32 = 446..457)
#define FW_WIFI_CE_COUNT 12
#define FW_WIFI_SMMU_SID 0x40               // iommus = <&apps_smmu 0x40 0x1>
#define FW_WIFI_SMMU_MASK 0x1

// The modem's QMI instance for all services on SDM845 is 1 (version 1). Firmware comes from .cache (never git):
//   mba.mbn, modem.mbn (one ELF, no split .bNN), wlanmdsp.mbn, board-2.bin (ath10k WCN3990 hw1.0).

#endif
