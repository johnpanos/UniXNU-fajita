// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 SNOC copy-engine configuration, verbatim from drivers/net/wireless/ath/ath10k/snoc.c (Linux v6.16): the host
// CE attributes (host_ce_config_wlan), the target pipe configuration and service map sent in wlfw WLAN_CFG
// (target_ce_config_wlan, target_service_to_ce_map_wlan incl. its zero terminator, as ath10k sends it) and the shadow
// register map (target_shadow_reg_cfg_map). Shared by the wlfw client (WLAN_CFG) and the CE/HTC layer.
#ifndef FAJITA_WIFI_SNOC_H
#define FAJITA_WIFI_SNOC_H
#include <stdint.h>
#include "WifiQmi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WSNOC_CE_COUNT 12
#define WSNOC_CE_ATTR_FLAGS 0u
#define WSNOC_CE_ATTR_DIS_INTR 8u      // BIT(3)
#define WSNOC_PIPEDIR_NONE 0u
#define WSNOC_PIPEDIR_IN 1u            // target -> host
#define WSNOC_PIPEDIR_OUT 2u           // host -> target
#define WSNOC_PIPEDIR_INOUT 3u
#define WSNOC_SRC_WR_IDX_OFFSET 0x3cu
#define WSNOC_DST_WR_IDX_OFFSET 0x40u

// HTC service ids (htc.h SVC(group, idx) = group << 8 | idx)
#define WHTC_SVC_RSVD_CTRL 0x0001u
#define WHTC_SVC_WMI_CONTROL 0x0100u
#define WHTC_SVC_WMI_DATA_BE 0x0101u
#define WHTC_SVC_WMI_DATA_BK 0x0102u
#define WHTC_SVC_WMI_DATA_VI 0x0103u
#define WHTC_SVC_WMI_DATA_VO 0x0104u
#define WHTC_SVC_HTT_DATA_MSG 0x0300u
#define WHTC_SVC_HTT_DATA2_MSG 0x0301u
#define WHTC_SVC_HTT_DATA3_MSG 0x0302u
#define WHTC_SVC_HTT_LOG_MSG 0x0600u
#define WHTC_SVC_TEST_RAW_STREAMS 0xfe00u

typedef struct {uint32_t flags, src_nentries, src_sz_max, dest_nentries;} wsnoc_host_ce;

extern const wsnoc_host_ce wsnoc_host_ce_config[WSNOC_CE_COUNT];
extern const wlfw_ce_tgt wsnoc_target_ce_config[WSNOC_CE_COUNT];
extern const wlfw_ce_svc wsnoc_service_map[21];
extern const wlfw_shadow wsnoc_shadow_map[12];
#define WSNOC_N_SERVICE_MAP 21u
#define WSNOC_N_SHADOW 12u
// The pipes (ul = host->target, dl = target->host) an HTC service uses (ath10k_snoc_hif_map_service_to_pipe).
int wsnoc_service_pipes(uint32_t service_id, uint8_t *ul, uint8_t *dl);

#ifdef __cplusplus
}
#endif
#endif
