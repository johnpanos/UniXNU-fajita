#!/usr/bin/env python3
"""Generate the ath10k (WCN3990, WMI-TLV + HTT) ABI constants the fajita Wi-Fi layers use, from a Linux source tree.

  tools/wifi/gen-ath10k-abi.py LINUX_DIR [--check]

Extracts the named enums (WMI-TLV command/event ids, TLV tags, services, pdev/vdev/peer params, HTT message types) and
structs from drivers/net/wireless/ath/ath10k/*.h, compiles them with the host compiler (so every value is exactly what
Linux computes, enum chains and macros included), and writes:

  drivers/FajitaWifi/WifiAth10kIds.h    #define WA_<NAME> value   (the constants the driver code uses)
  drivers/FajitaWifi/tests/ath10k_layout.h   sizeof/offsetof of the Linux structs (the host tests compare the
                                             driver's encoders against them)

--check regenerates into memory and fails if the committed files differ. Linux v6.16 (/home/john/src/ref/linux here).
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_IDS = ROOT / 'drivers/FajitaWifi/WifiAth10kIds.h'
OUT_LAYOUT = ROOT / 'drivers/FajitaWifi/tests/ath10k_layout.h'
HEADERS = ['wmi-tlv.h', 'wmi.h', 'htt.h', 'htc.h', 'rx_desc.h', 'hw.h', 'core.h']

ENUMS = ['wmi_tlv_grp_id', 'wmi_tlv_cmd_id', 'wmi_tlv_event_id', 'wmi_tlv_tag', 'wmi_tlv_service', 'wmi_tlv_pdev_param',
         'wmi_tlv_vdev_param', 'wmi_tlv_peer_param', 'wmi_tlv_peer_flags', 'wmi_phy_mode', 'wmi_scan_event_type',
         'wmi_vdev_type', 'wmi_tlv_vdev_subtype', 'wmi_peer_param', 'htt_h2t_msg_type', 'htt_tlv_t2h_msg_type',
         'ath10k_hw_txrx_mode', 'htt_rx_mpdu_status', 'htt_data_tx_status', 'wmi_tlv_pdev_param']

NAMES = '''
WMI_TLV_INIT_CMDID WMI_TLV_START_SCAN_CMDID WMI_TLV_STOP_SCAN_CMDID WMI_TLV_SCAN_CHAN_LIST_CMDID
WMI_TLV_PDEV_SET_REGDOMAIN_CMDID WMI_TLV_PDEV_SET_PARAM_CMDID WMI_TLV_VDEV_CREATE_CMDID WMI_TLV_VDEV_DELETE_CMDID
WMI_TLV_VDEV_START_REQUEST_CMDID WMI_TLV_VDEV_STOP_CMDID WMI_TLV_VDEV_UP_CMDID WMI_TLV_VDEV_DOWN_CMDID
WMI_TLV_VDEV_SET_PARAM_CMDID WMI_TLV_VDEV_INSTALL_KEY_CMDID WMI_TLV_PEER_CREATE_CMDID WMI_TLV_PEER_DELETE_CMDID
WMI_TLV_PEER_SET_PARAM_CMDID WMI_TLV_PEER_ASSOC_CMDID WMI_TLV_MGMT_TX_SEND_CMD WMI_TLV_STA_POWERSAVE_MODE_CMDID
WMI_TLV_PDEV_SET_WMM_PARAMS_CMDID WMI_TLV_ECHO_CMDID
WMI_TLV_SERVICE_READY_EVENTID WMI_TLV_READY_EVENTID WMI_TLV_SERVICE_AVAILABLE_EVENTID WMI_TLV_SCAN_EVENTID
WMI_TLV_MGMT_RX_EVENTID WMI_TLV_VDEV_START_RESP_EVENTID WMI_TLV_VDEV_STOPPED_EVENTID WMI_TLV_PEER_STA_KICKOUT_EVENTID
WMI_TLV_MGMT_TX_COMPLETION_EVENTID WMI_TLV_MGMT_TX_BUNDLE_COMPLETION_EVENTID WMI_TLV_ROAM_EVENTID
WMI_TLV_PEER_DELETE_RESP_EVENTID WMI_TLV_DEBUG_MESG_EVENTID WMI_TLV_CHAN_INFO_EVENTID WMI_TLV_ECHO_EVENTID
WMI_TLV_OFFLOAD_BCN_TX_STATUS_EVENTID WMI_TLV_TX_PAUSE_EVENTID WMI_TLV_PDEV_TEMPERATURE_EVENTID
WMI_TLV_TAG_ARRAY_UINT32 WMI_TLV_TAG_ARRAY_BYTE WMI_TLV_TAG_ARRAY_STRUCT WMI_TLV_TAG_ARRAY_FIXED_STRUCT
WMI_TLV_TAG_STRUCT_SERVICE_READY_EVENT WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES WMI_TLV_TAG_STRUCT_WLAN_HOST_MEM_REQ
WMI_TLV_TAG_STRUCT_READY_EVENT WMI_TLV_TAG_STRUCT_SCAN_EVENT WMI_TLV_TAG_STRUCT_MGMT_RX_HDR
WMI_TLV_TAG_STRUCT_INIT_CMD WMI_TLV_TAG_STRUCT_RESOURCE_CONFIG WMI_TLV_TAG_STRUCT_WLAN_HOST_MEMORY_CHUNK
WMI_TLV_TAG_STRUCT_START_SCAN_CMD WMI_TLV_TAG_STRUCT_STOP_SCAN_CMD WMI_TLV_TAG_STRUCT_SCAN_CHAN_LIST_CMD
WMI_TLV_TAG_STRUCT_CHANNEL WMI_TLV_TAG_STRUCT_PDEV_SET_REGDOMAIN_CMD WMI_TLV_TAG_STRUCT_PDEV_SET_PARAM_CMD
WMI_TLV_TAG_STRUCT_VDEV_CREATE_CMD WMI_TLV_TAG_STRUCT_VDEV_DELETE_CMD WMI_TLV_TAG_STRUCT_VDEV_START_REQUEST_CMD
WMI_TLV_TAG_STRUCT_VDEV_STOP_CMD WMI_TLV_TAG_STRUCT_VDEV_UP_CMD WMI_TLV_TAG_STRUCT_VDEV_DOWN_CMD
WMI_TLV_TAG_STRUCT_VDEV_SET_PARAM_CMD WMI_TLV_TAG_STRUCT_VDEV_INSTALL_KEY_CMD WMI_TLV_TAG_STRUCT_PEER_CREATE_CMD
WMI_TLV_TAG_STRUCT_PEER_DELETE_CMD WMI_TLV_TAG_STRUCT_PEER_SET_PARAM_CMD WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD
WMI_TLV_TAG_STRUCT_VHT_RATE_SET WMI_TLV_TAG_STRUCT_MGMT_TX_CMD WMI_TLV_TAG_STRUCT_VDEV_START_RESPONSE_EVENT
WMI_TLV_TAG_STRUCT_VDEV_STOPPED_EVENT WMI_TLV_TAG_STRUCT_PEER_STA_KICKOUT_EVENT WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT
WMI_TLV_TAG_STRUCT_SERVICE_AVAILABLE_EVENT WMI_TLV_TAG_STRUCT_P2P_NOA_DESCRIPTOR WMI_TLV_TAG_STRUCT_STA_POWERSAVE_MODE_CMD
WMI_TLV_TAG_STRUCT_ROAM_EVENT WMI_TLV_TAG_STRUCT_PEER_DELETE_RESP_EVENT WMI_TLV_TAG_STRUCT_ECHO_CMD
WMI_TLV_TAG_STRUCT_ECHO_EVENT WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_BUNDLE_EVENT
WMI_TLV_SERVICE_RX_FULL_REORDER WMI_TLV_SERVICE_MGMT_TX_WMI WMI_TLV_SERVICE_SUPPORT_EXTEND_ADDRESS
WMI_TLV_SERVICE_TX_DATA_MGMT_ACK_RSSI WMI_TLV_MAX_SERVICE WMI_TLV_MAX_EXT_SERVICE
WMI_TLV_PEER_AUTHORIZE WMI_TLV_PEER_USE_4ADDR
WMI_TLV_PEER_AUTH WMI_TLV_PEER_QOS WMI_TLV_PEER_NEED_PTK_4_WAY WMI_TLV_PEER_NEED_GTK_2_WAY WMI_TLV_PEER_HT WMI_TLV_PEER_PMF
WMI_TLV_PDEV_PARAM_PMF_QOS WMI_TLV_PDEV_PARAM_DYNAMIC_BW WMI_TLV_PDEV_PARAM_IDLE_PS_CONFIG
WMI_TLV_VDEV_PARAM_DEF_KEYID WMI_TLV_VDEV_PARAM_TX_ENCAP_TYPE WMI_TLV_VDEV_PARAM_BEACON_INTERVAL
WMI_TLV_VDEV_PARAM_DTIM_PERIOD WMI_TLV_VDEV_PARAM_SLOT_TIME WMI_TLV_VDEV_PARAM_PREAMBLE WMI_TLV_VDEV_PARAM_PROTECTION_MODE
WMI_TLV_VDEV_PARAM_FIXED_RATE WMI_TLV_VDEV_PARAM_MGMT_TX_RATE
MODE_11A MODE_11G MODE_11NA_HT20 MODE_11NG_HT20 MODE_11AC_VHT20
WMI_VDEV_TYPE_STA WMI_TLV_VDEV_SUBTYPE_NONE
WMI_SCAN_EVENT_STARTED WMI_SCAN_EVENT_COMPLETED WMI_SCAN_EVENT_BSS_CHANNEL WMI_SCAN_EVENT_FOREIGN_CHANNEL
WMI_SCAN_EVENT_DEQUEUED WMI_SCAN_EVENT_PREEMPTED WMI_SCAN_EVENT_START_FAILED WMI_SCAN_EVENT_RESTARTED
WMI_SCAN_EVENT_FOREIGN_CHANNEL_EXIT
HTT_H2T_MSG_TYPE_VERSION_REQ HTT_H2T_MSG_TYPE_TX_FRM HTT_H2T_MSG_TYPE_RX_RING_CFG HTT_H2T_MSG_TYPE_AGGR_CFG
HTT_H2T_MSG_TYPE_FRAG_DESC_BANK_CFG
HTT_TLV_T2H_MSG_TYPE_VERSION_CONF HTT_TLV_T2H_MSG_TYPE_RX_IND HTT_TLV_T2H_MSG_TYPE_RX_FLUSH
HTT_TLV_T2H_MSG_TYPE_PEER_MAP HTT_TLV_T2H_MSG_TYPE_PEER_UNMAP HTT_TLV_T2H_MSG_TYPE_RX_ADDBA HTT_TLV_T2H_MSG_TYPE_RX_DELBA
HTT_TLV_T2H_MSG_TYPE_TX_COMPL_IND HTT_TLV_T2H_MSG_TYPE_SEC_IND HTT_TLV_T2H_MSG_TYPE_MGMT_TX_COMPL_IND
HTT_TLV_T2H_MSG_TYPE_TX_CREDIT_UPDATE_IND HTT_TLV_T2H_MSG_TYPE_RX_IN_ORD_PADDR_IND HTT_TLV_T2H_MSG_TYPE_RX_FRAG_IND
HTT_TLV_T2H_MSG_TYPE_RX_OFFLOAD_DELIVER_IND HTT_TLV_T2H_MSG_TYPE_RX_PN_IND HTT_TLV_T2H_MSG_TYPE_PKTLOG
HTT_TLV_T2H_MSG_TYPE_TX_INSPECT_IND HTT_TLV_T2H_MSG_TYPE_STATS_CONF
ATH10K_HW_TXRX_RAW ATH10K_HW_TXRX_NATIVE_WIFI ATH10K_HW_TXRX_ETHERNET ATH10K_HW_TXRX_MGMT
HTT_RX_IND_MPDU_STATUS_OK HTT_DATA_TX_STATUS_OK HTT_DATA_TX_STATUS_DISCARD HTT_DATA_TX_STATUS_NO_ACK
'''.split()

# Macros (#define) to evaluate too
MACROS = '''
WMI_TLV_ABI_VER0 WMI_TLV_ABI_VER1 WMI_TLV_ABI_VER_NS0 WMI_TLV_ABI_VER_NS1 WMI_TLV_ABI_VER_NS2 WMI_TLV_ABI_VER_NS3
WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL WMI_RSRC_CFG_FLAG_TX_ACK_RSSI
WMI_HOST_SCAN_REQ_ID_PREFIX WMI_HOST_SCAN_REQUESTOR_ID_PREFIX WMI_SCAN_FLAG_PASSIVE WMI_SCAN_ADD_BCAST_PROBE_REQ
WMI_SCAN_CHAN_STAT_EVENT WMI_SCAN_FILTER_PROBE_REQ WMI_SCAN_PRIORITY_LOW
WMI_CHAN_FLAG_PASSIVE WMI_CHAN_FLAG_ALLOW_HT WMI_CHAN_FLAG_ALLOW_VHT WMI_CHAN_FLAG_DFS WMI_CHAN_FLAG_ADHOC_ALLOWED
NUM_UNITS_IS_NUM_VDEVS NUM_UNITS_IS_NUM_PEERS NUM_UNITS_IS_NUM_ACTIVE_PEERS WMI_MAX_MEM_REQS
TARGET_TLV_NUM_VDEVS TARGET_TLV_NUM_PEERS TARGET_TLV_NUM_TDLS_VDEVS TARGET_TLV_NUM_MSDU_DESC TARGET_TLV_NUM_WOW_PATTERNS
TARGET_HL_TLV_NUM_PEERS TARGET_HL_TLV_AST_SKID_LIMIT TARGET_HL_TLV_NUM_WDS_ENTRIES
WMI_CMD_HDR_CMD_ID_MASK
WMI_RX_STATUS_OK WMI_RX_STATUS_ERR_CRC WMI_RX_STATUS_ERR_DECRYPT WMI_RX_STATUS_ERR_MIC WMI_RX_STATUS_ERR_KEY_CACHE_MISS
WMI_CIPHER_NONE WMI_CIPHER_WEP WMI_CIPHER_TKIP WMI_CIPHER_AES_OCB WMI_CIPHER_AES_CCM
WMI_KEY_PAIRWISE WMI_KEY_GROUP WMI_KEY_TX_USAGE
WMI_PEER_AUTHORIZE
WMI_RATE_PREAMBLE_OFDM
HTT_DATA_TX_DESC_FLAGS0_MAC_HDR_PRESENT HTT_DATA_TX_DESC_FLAGS0_NO_AGGR HTT_DATA_TX_DESC_FLAGS0_NO_ENCRYPT
HTT_DATA_TX_DESC_FLAGS0_NO_CLASSIFY HTT_DATA_TX_DESC_FLAGS0_RSVD0 HTT_DATA_TX_DESC_FLAGS0_PKT_TYPE_MASK
HTT_DATA_TX_DESC_FLAGS0_PKT_TYPE_LSB HTT_DATA_TX_DESC_FLAGS1_VDEV_ID_MASK HTT_DATA_TX_DESC_FLAGS1_VDEV_ID_LSB
HTT_DATA_TX_DESC_FLAGS1_EXT_TID_MASK HTT_DATA_TX_DESC_FLAGS1_EXT_TID_LSB HTT_DATA_TX_DESC_FLAGS1_POSTPONED
HTT_DATA_TX_EXT_TID_NON_QOS_MCAST_BCAST HTT_DATA_TX_EXT_TID_MGMT HTT_INVALID_PEERID
HTT_RX_RING_FLAGS_MAC80211_HDR HTT_RX_RING_FLAGS_MSDU_PAYLOAD HTT_RX_RING_FLAGS_PPDU_START HTT_RX_RING_FLAGS_PPDU_END
HTT_RX_RING_FLAGS_MPDU_START HTT_RX_RING_FLAGS_MPDU_END HTT_RX_RING_FLAGS_MSDU_START HTT_RX_RING_FLAGS_MSDU_END
HTT_RX_RING_FLAGS_RX_ATTENTION HTT_RX_RING_FLAGS_FRAG_INFO HTT_RX_RING_FLAGS_UNICAST_RX HTT_RX_RING_FLAGS_MULTICAST_RX
HTT_RX_RING_FLAGS_CTRL_RX HTT_RX_RING_FLAGS_MGMT_RX HTT_RX_RING_FLAGS_NULL_RX HTT_RX_RING_FLAGS_PHY_DATA_RX
HTT_RX_BUF_SIZE HTT_RX_RING_SIZE_MAX HTT_RX_RING_FILL_LEVEL_DUAL_MAC HTT_RX_RING_SIZE_MIN
HTT_FRAG_DESC_BANK_CFG_INFO_Q_STATE_VALID HTT_FRAG_DESC_BANK_CFG_INFO_Q_STATE_DEPTH_TYPE_LSB
HTT_TX_Q_STATE_ENTRY_SIZE HTT_TX_Q_STATE_ENTRY_MULTIPLIER HTT_TX_Q_STATE_NUM_PEERS HTT_TX_Q_STATE_NUM_TIDS
HTT_RX_IN_ORD_IND_INFO_OFFLOAD_MASK
HTT_RX_IN_ORD_IND_INFO_FRAG_MASK HTT_RX_IN_ORD_IND_INFO_TID_MASK HTT_RX_IN_ORD_IND_INFO_TID_LSB
HTT_RX_INDICATION_INFO0_EXT_TID_MASK HTT_RX_INDICATION_INFO0_EXT_TID_LSB HTT_RX_INDICATION_INFO1_NUM_MPDU_RANGES_MASK
HTT_RX_INDICATION_INFO1_NUM_MPDU_RANGES_LSB
HTT_TX_COMPL_INV_MSDU_ID
RX_ATTENTION_FLAGS_MSDU_DONE RX_ATTENTION_FLAGS_FCS_ERR RX_ATTENTION_FLAGS_MPDU_LENGTH_ERR
RX_ATTENTION_FLAGS_TKIP_MIC_ERR RX_ATTENTION_FLAGS_DECRYPT_ERR RX_ATTENTION_FLAGS_FIRST_MPDU RX_ATTENTION_FLAGS_LAST_MPDU
RX_MSDU_END_INFO0_FIRST_MSDU RX_MSDU_END_INFO0_LAST_MSDU
RX_MSDU_START_INFO0_MSDU_LENGTH_MASK RX_MSDU_START_INFO0_MSDU_LENGTH_LSB
RX_MSDU_START_INFO1_DECAP_FORMAT_MASK RX_MSDU_START_INFO1_DECAP_FORMAT_LSB
RX_MPDU_START_INFO0_ENCRYPTED
RX_HTT_HDR_STATUS_LEN
'''.split()

STRUCTS = '''
wmi_tlv_abi_version wmi_mac_addr wmi_tlv_init_cmd wmi_tlv_resource_config host_memory_chunk_tlv wmi_tlv_svc_rdy_ev
wmi_tlv_hw_bd_info wmi_tlv_rdy_ev hal_reg_capabilities wlan_host_mem_req wmi_start_scan_common wmi_tlv_start_scan_cmd
wmi_ssid wmi_scan_event wmi_tlv_mgmt_rx_ev wmi_channel wmi_tlv_scan_chan_list_cmd wmi_tlv_pdev_set_rd_cmd
wmi_tlv_pdev_set_param_cmd wmi_tlv_stop_scan_cmd
wmi_vdev_create_cmd wmi_vdev_delete_cmd wmi_tlv_vdev_start_cmd wmi_vdev_stop_cmd wmi_vdev_up_cmd wmi_vdev_down_cmd
wmi_vdev_set_param_cmd wmi_vdev_install_key_cmd wmi_tlv_peer_create_cmd wmi_peer_delete_cmd wmi_peer_set_param_cmd
wmi_tlv_peer_assoc_cmd wmi_vht_rate_set wmi_tlv_mgmt_tx_cmd wmi_vdev_start_response_event wmi_tlv_mgmt_tx_compl_ev
wmi_peer_sta_kickout_event wmi_tlv_sta_powersave_mode_cmd wmi_p2p_noa_descriptor wmi_tlv_vdev_start_cmd
htt_cmd_hdr htt_ver_req htt_frag_desc_bank_cfg64 htt_frag_desc_bank_id htt_q_state_conf htt_rx_ring_setup_hdr
htt_rx_ring_setup_ring64 htt_rx_ring_rx_desc_offsets htt_data_tx_desc_64 htt_data_tx_desc_frag htt_msdu_ext_desc_64
ath10k_htt_txbuf_64 htt_aggr_conf_v2 htt_resp_hdr htt_ver_resp htt_rx_in_ord_ind htt_rx_in_ord_msdu_desc_ext
htt_data_tx_completion htt_data_tx_completion_ext htt_peer_map_event htt_peer_unmap_event htt_rx_indication_hdr
htt_rx_indication_prefix htt_rx_indication_ppdu htt_rx_indication_mpdu_range htt_mgmt_tx_completion
ath10k_htc_hdr
rx_attention rx_frag_info_common rx_frag_info_wcn3990 rx_mpdu_start rx_mpdu_end rx_msdu_start_common
rx_msdu_start_wcn3990 rx_msdu_end_common rx_msdu_end_wcn3990 rx_ppdu_start rx_ppdu_end_common rx_ppdu_end_wcn3990
htt_rx_desc_v2 rx_msdu_start_qca99x0 rx_msdu_end_qca99x0
'''.split()
# Field offsets (struct.field) the tests check
OFFSETS = '''
wmi_tlv_resource_config.rx_decap_mode wmi_tlv_resource_config.num_msdu_desc wmi_tlv_resource_config.host_capab
wmi_tlv_svc_rdy_ev.num_mem_reqs wmi_tlv_svc_rdy_ev.abi wmi_tlv_rdy_ev.mac_addr wmi_tlv_rdy_ev.status
wmi_tlv_start_scan_cmd.num_channels wmi_tlv_start_scan_cmd.mac_addr wmi_tlv_start_scan_cmd.ie_len
wmi_start_scan_common.scan_ctrl_flags wmi_tlv_mgmt_rx_ev.buf_len wmi_tlv_mgmt_rx_ev.status
wmi_tlv_vdev_start_cmd.flags wmi_tlv_vdev_start_cmd.ssid wmi_tlv_peer_assoc_cmd.flags
wmi_tlv_peer_assoc_cmd.phy_mode wmi_tlv_peer_assoc_cmd.mac_addr wmi_vdev_install_key_cmd.key_cipher
wmi_vdev_install_key_cmd.key_len wmi_tlv_mgmt_tx_cmd.frame_len wmi_tlv_mgmt_tx_cmd.paddr
htt_data_tx_desc_64.frags_paddr htt_data_tx_desc_64.peerid htt_rx_ring_setup_ring64.rx_ring_bufsize
htt_rx_ring_setup_ring64.offsets ath10k_htt_txbuf_64.htc_hdr ath10k_htt_txbuf_64.cmd_tx
htt_rx_desc_v2.msdu_payload htt_rx_desc_v2.attention htt_rx_desc_v2.msdu_start htt_rx_desc_v2.msdu_end
htt_rx_desc_v2.mpdu_start htt_rx_desc_v2.rx_hdr_status
'''.split()

PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;
typedef uint16_t __le16; typedef uint32_t __le32; typedef uint64_t __le64; typedef uint32_t __be32; typedef uint16_t __be16;
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (sizeof(long) * 8 - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define ETH_ALEN 6
#define DECLARE_FLEX_ARRAY(T, n) struct { struct {} __empty_##n; T n[]; }
#define __counted_by(x)
#define WMI_TLV_CMD(grp_id) (((grp_id) << 12) | 0x1)
#define WMI_TLV_EV(grp_id) (((grp_id) << 12) | 0x1)
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define SZ_2K 2048
#define L1_CACHE_BYTES 64
#define SKB_DATA_ALIGN(x) ALIGN(x, 64)
'''


def blocks(text, kind, name):
    m = re.search(r'\b' + kind + r'\s+' + re.escape(name) + r'\s*\{', text)
    if not m:
        return None
    i, depth = m.end(), 1
    while depth:
        c = text[i]
        depth += c == '{'
        depth -= c == '}'
        i += 1
    j = text.index(';', i)
    return text[m.start():j + 1]


def macro(text, name):
    m = re.search(r'^#define\s+' + re.escape(name) + r'\b[ \t]+(.*(?:\\\n.*)*)$', text, re.M)
    return m.group(0) if m else None


def generate(linux):
    src = Path(linux) / 'drivers/net/wireless/ath/ath10k'
    text = '\n'.join((src / h).read_text() for h in HEADERS)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    # the WMI-TLV ABI version lives in wmi-tlv.c: its #defines only
    ctext = re.sub(r'/\*.*?\*/', '', (src / 'wmi-tlv.c').read_text(), flags=re.S)
    text += '\n' + '\n'.join(m.group(0) for m in re.finditer(r'^#define[ \t]+WMI_TLV_ABI\w*(?:.*\\\n)*.*$', ctext, re.M))
    parts, missing = [PRELUDE], []
    # every #define and enum of the headers, in source order (an enum may reuse a name a later #define takes)
    prelude_names = set(re.findall(r'#define\s+(\w+)', PRELUDE))
    pat = re.compile(r'^#define[ \t]+(\w+)(?:\(|[ \t]|$)(?:.*\\\n)*.*$|\benum\s*(\w*)\s*\{', re.M)
    for m in pat.finditer(text):
        if m.group(0).startswith('#'):
            if m.group(1) not in prelude_names and not m.group(1).startswith('_'):
                parts.append(m.group(0))
            continue
        b = blocks(text[m.start():], 'enum', m.group(2)) if m.group(2) else text[m.start():text.index('};', m.end()) + 2]
        if b and b not in parts:
            parts.append(b)
    have, emitted = [], []

    def emit(name, stack=()):
        if name in emitted or name in stack:
            return name in emitted
        b = blocks(text, 'struct', name)
        if b is None:
            return False
        inner = b[b.index('{'):]
        for dep in re.findall(r'\bstruct\s+(\w+)\b(?!\s*\*)', inner):
            if not re.search(r'\bstruct\s+' + dep + r'\s*\{', inner):   # defined inside this one
                emit(dep, stack + (name,))
        parts.append(b)
        emitted.append(name)
        return True

    for st in STRUCTS:
        if emit(st):
            have.append(st)
        else:
            missing.append('struct ' + st)
    main = ['int main(void) {']
    for n in NAMES + MACROS:
        main.append(f'  printf("I %s %llu\\n", "{n}", (unsigned long long)({n}));')
    for s in have:
        main.append(f'  printf("S %s %zu\\n", "{s}", sizeof(struct {s}));')
    for o in OFFSETS:
        s, f = o.split('.')
        if s in have:
            main.append(f'  printf("O %s %s %zu\\n", "{s}", "{f}", offsetof(struct {s}, {f}));')
    main.append('  return 0; }')
    parts.append('\n'.join(main))
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / 'abi.c'
        c.write_text('\n'.join(parts) + '\n')
        r = subprocess.run(['cc', '-std=gnu11', '-w', str(c), '-o', str(Path(tmp) / 'abi')], capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write(r.stderr[-6000:])
            raise SystemExit('compile failed')
        out = subprocess.run([str(Path(tmp) / 'abi')], capture_output=True, text=True, check=True).stdout
    if missing:
        print('note: not found (skipped): ' + ', '.join(missing), file=sys.stderr)
    ids, layout = [], []
    for line in out.splitlines():
        k, *rest = line.split()
        if k == 'I':
            ids.append(f'#define WA_{rest[0]} {int(rest[1]):#x}u')
        elif k == 'S':
            layout.append(f'#define WL_SIZEOF_{rest[0]} {rest[1]}u')
        else:
            layout.append(f'#define WL_OFF_{rest[0]}__{rest[1]} {rest[2]}u')
    head = ('// SPDX-License-Identifier: BSD-2-Clause-Patent\n// GENERATED by tools/wifi/gen-ath10k-abi.py from Linux v6.16 '
            'drivers/net/wireless/ath/ath10k (do not edit).\n')
    ids_h = head + ('// The ath10k WMI-TLV / HTT constants of the fajita Wi-Fi layers, as Linux computes them.\n'
                    '#ifndef FAJITA_WIFI_ATH10K_IDS_H\n#define FAJITA_WIFI_ATH10K_IDS_H\n' + '\n'.join(ids) + '\n#endif\n')
    lay_h = head + ('// sizeof/offsetof of the Linux ath10k structs; the host tests compare the fajita encoders against them.\n'
                    '#ifndef FAJITA_WIFI_ATH10K_LAYOUT_H\n#define FAJITA_WIFI_ATH10K_LAYOUT_H\n' + '\n'.join(layout) + '\n#endif\n')
    return ids_h, lay_h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('linux')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    ids_h, lay_h = generate(a.linux)
    if a.check:
        bad = [p for p, t in ((OUT_IDS, ids_h), (OUT_LAYOUT, lay_h)) if not p.is_file() or p.read_text() != t]
        if bad:
            raise SystemExit('FAIL: out of date: ' + ', '.join(str(p.relative_to(ROOT)) for p in bad))
        print('PASS: ath10k ABI headers match Linux')
        return
    OUT_IDS.write_text(ids_h)
    OUT_LAYOUT.write_text(lay_h)
    print(f'wrote {OUT_IDS.relative_to(ROOT)} ({ids_h.count("#define WA_")} ids), '
          f'{OUT_LAYOUT.relative_to(ROOT)} ({lay_h.count("#define WL_")} layout values)')


if __name__ == '__main__':
    main()
