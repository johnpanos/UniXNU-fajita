// SPDX-License-Identifier: BSD-2-Clause-Patent
// QMI over QRTR: the message header, TLV encoding, and the messages fajita's Wi-Fi bring-up sends or answers.
// Wire format after drivers/soc/qcom/qmi_encdec.c and qmi_interface.c (Linux v6.16); message layouts from
// drivers/net/wireless/ath/ath10k/qmi_wlfw_v01.[ch] (wlfw), drivers/soc/qcom/qcom_pdr_msg.c (servreg locator) and
// linux-msm/rmtfs qmi_rmtfs.qmi. Freestanding C, host-tested.
#ifndef FAJITA_WIFI_QMI_H
#define FAJITA_WIFI_QMI_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WQMI_REQUEST 0u
#define WQMI_RESPONSE 2u
#define WQMI_INDICATION 4u
#define WQMI_HDR_BYTES 7u

// Services (QRTR NEW_SERVER service ids) and their instance words (version | instance << 8).
#define WQMI_SVC_WLFW 0x45u
#define WQMI_SVC_SERVREG_LOC 0x40u
#define WQMI_SVC_RMTFS 14u
#define WQMI_SVC_TFTP 4096u
#define WQMI_INSTANCE(version, instance) ((uint32_t)(version) | (uint32_t)(instance) << 8)

// wlfw message ids (qmi_wlfw_v01.h)
#define WLFW_IND_REGISTER 0x0020u
#define WLFW_FW_READY_IND 0x0021u
#define WLFW_WLAN_MODE 0x0022u
#define WLFW_WLAN_CFG 0x0023u
#define WLFW_CAP 0x0024u
#define WLFW_BDF_DOWNLOAD 0x0025u
#define WLFW_CAL_REPORT 0x0026u
#define WLFW_MSA_READY_IND 0x002Bu
#define WLFW_MSA_INFO 0x002Du
#define WLFW_MSA_READY 0x002Eu
#define WLFW_HOST_CAP 0x0034u
#define WLFW_MAX_DATA 6144u
#define WLFW_MAX_MEM_REG 2u
#define WLFW_MAX_CE 12u
#define WLFW_MAX_SVC 24u
#define WLFW_MAX_SHADOW 24u
#define WLFW_FW_READY_BIT 0x02ull
#define WLFW_CLIENT_ID 0x4b4e454cu         // ATH10K_QMI_CLIENT_ID ("LENK")
#define WLFW_MODE_MISSION 0u                // QMI_WLFW_MISSION_V01
#define WLFW_MODE_OFF 4u                    // QMI_WLFW_OFF_V01

// servreg locator (pd-mapper) and rmtfs message ids
#define SERVREG_GET_DOMAIN_LIST 0x21u
#define SERVREG_LOC_PFR 0x24u
#define RMTFS_OPEN 1u
#define RMTFS_CLOSE 2u
#define RMTFS_RW_IOVEC 3u
#define RMTFS_ALLOC_BUFF 4u
#define RMTFS_GET_DEV_ERROR 5u

#define WQMI_RESULT_SUCCESS 0u
#define WQMI_RESULT_FAILURE 1u
#define WQMI_ERR_NONE 0u
#define WQMI_ERR_MALFORMED 1u               // include/linux/soc/qcom/qmi.h QMI_ERR_*_V01
#define WQMI_ERR_INTERNAL 3u
#define WQMI_ERR_NOT_SUPPORTED 94u

typedef struct {
    uint8_t type;
    uint16_t txn, msg_id, len;
} wqmi_hdr;

// Writer: header first, TLVs after; wqmi_end patches the length. `err` latches overflow.
typedef struct {
    uint8_t *buf;
    size_t cap, at;
    int err;
} wqmi_writer;

void wqmi_begin(wqmi_writer *w, uint8_t *buf, size_t cap, uint8_t type, uint16_t txn, uint16_t msg_id);
void wqmi_tlv(wqmi_writer *w, uint8_t type, const void *value, size_t len);
void wqmi_u8(wqmi_writer *w, uint8_t type, uint8_t v);
void wqmi_u16(wqmi_writer *w, uint8_t type, uint16_t v);
void wqmi_u32(wqmi_writer *w, uint8_t type, uint32_t v);
void wqmi_u64(wqmi_writer *w, uint8_t type, uint64_t v);
void wqmi_result(wqmi_writer *w, uint16_t result, uint16_t error);   // TLV 0x02
long wqmi_end(wqmi_writer *w);   // total bytes, or -1 on overflow

// Reader over one message.
int wqmi_parse_hdr(const uint8_t *msg, size_t len, wqmi_hdr *h);   // 0, or -1 if short or the length disagrees
// Find TLV `type` in the message body; returns its value pointer and length, NULL if absent or malformed.
const uint8_t *wqmi_find(const uint8_t *msg, size_t len, uint8_t type, uint16_t *vlen);
int wqmi_get_result(const uint8_t *msg, size_t len, uint16_t *result, uint16_t *error);   // 0 if TLV 0x02 present

// ---- wlfw (ath10k client side) ----
long wlfw_ind_register_req(uint8_t *buf, size_t cap, uint16_t txn);
long wlfw_host_cap_8bit_req(uint8_t *buf, size_t cap, uint16_t txn);
long wlfw_msa_info_req(uint8_t *buf, size_t cap, uint16_t txn, uint64_t msa_addr, uint32_t size);
long wlfw_empty_req(uint8_t *buf, size_t cap, uint16_t txn, uint16_t msg_id);   // msa_ready, cap
long wlfw_bdf_req(uint8_t *buf, size_t cap, uint16_t txn, uint32_t total, uint32_t seg, const uint8_t *data, uint16_t len, int end);
long wlfw_cal_report_req(uint8_t *buf, size_t cap, uint16_t txn);
long wlfw_wlan_mode_req(uint8_t *buf, size_t cap, uint16_t txn, uint32_t mode);

typedef struct {uint32_t pipe_num, pipe_dir, nentries, nbytes_max, flags;} wlfw_ce_tgt;
typedef struct {uint32_t service_id, pipe_dir, pipe_num;} wlfw_ce_svc;
typedef struct {uint16_t id, offset;} wlfw_shadow;
long wlfw_wlan_cfg_req(uint8_t *buf, size_t cap, uint16_t txn, const wlfw_ce_tgt *tgt, unsigned ntgt, const wlfw_ce_svc *svc,
                       unsigned nsvc, const wlfw_shadow *shadow, unsigned nshadow);

typedef struct {
    uint16_t result, error;
    int fw_status_valid;
    uint64_t fw_status;
} wlfw_ind_register_resp;
int wlfw_parse_ind_register_resp(const uint8_t *msg, size_t len, wlfw_ind_register_resp *out);

typedef struct {uint64_t addr; uint32_t size; uint8_t secure;} wlfw_mem_region;
typedef struct {
    uint16_t result, error;
    unsigned count;
    wlfw_mem_region region[WLFW_MAX_MEM_REG];
} wlfw_msa_info_resp;
int wlfw_parse_msa_info_resp(const uint8_t *msg, size_t len, wlfw_msa_info_resp *out);

typedef struct {
    uint16_t result, error;
    int chip_valid, board_valid, soc_valid, fw_valid, build_valid;
    uint32_t chip_id, chip_family, board_id, soc_id, fw_version;
    char fw_timestamp[33], build_id[129];
} wlfw_cap_resp;
int wlfw_parse_cap_resp(const uint8_t *msg, size_t len, wlfw_cap_resp *out);

// ---- servreg locator server (pd-mapper) ----
typedef struct {const char *name; uint32_t instance;} wqmi_domain;
// Answer a GET_DOMAIN_LIST request for `service` (e.g. "wlan/fw") from the domains providing it. Returns the response size.
long servreg_answer(const uint8_t *req, size_t len, uint8_t *buf, size_t cap, const wqmi_domain *domains, unsigned ndomains);
// The sdm845 table of Linux's qcom_pd_mapper.c for the services the modem asks for: which domains provide `service`.
unsigned servreg_sdm845_lookup(const char *service, wqmi_domain *out, unsigned cap);

// ---- rmtfs server ----
typedef struct {uint32_t sector, offset, count;} rmtfs_iovec;
typedef struct {
    uint16_t msg_id, txn;
    char path[64];                  // OPEN
    uint32_t caller_id;             // CLOSE, RW_IOVEC, ALLOC_BUFF, GET_DEV_ERROR
    uint8_t direction;              // RW_IOVEC: 0 read (from storage to buffer), 1 write
    unsigned niov;
    rmtfs_iovec iov[32];
    uint8_t force_sync;
    uint32_t buff_size;             // ALLOC_BUFF
} rmtfs_req;
int rmtfs_parse_req(const uint8_t *msg, size_t len, rmtfs_req *out);
long rmtfs_resp(uint8_t *buf, size_t cap, const rmtfs_req *req, uint16_t result, uint16_t error, uint32_t caller_id,
                uint64_t buff_addr);

#ifdef __cplusplus
}
#endif
#endif
