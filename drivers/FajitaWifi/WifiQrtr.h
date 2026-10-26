// SPDX-License-Identifier: BSD-2-Clause-Patent
// QRTR (IPC Router) over GLINK, and the GLINK native transport framing. QRTR header v1 from
// include/uapi/linux/qrtr.h + net/qrtr/af_qrtr.c; control packets from net/qrtr/ns.c; GLINK commands from
// drivers/rpmsg/qcom_glink_native.c (Linux v6.16). Freestanding C, host-tested.
#ifndef FAJITA_WIFI_QRTR_H
#define FAJITA_WIFI_QRTR_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- QRTR ----
#define QRTR_PROTO_VER_1 1u
#define QRTR_TYPE_DATA 1u
#define QRTR_TYPE_HELLO 2u
#define QRTR_TYPE_BYE 3u
#define QRTR_TYPE_NEW_SERVER 4u
#define QRTR_TYPE_DEL_SERVER 5u
#define QRTR_TYPE_DEL_CLIENT 6u
#define QRTR_TYPE_RESUME_TX 7u
#define QRTR_TYPE_NEW_LOOKUP 10u
#define QRTR_TYPE_DEL_LOOKUP 11u
#define QRTR_PORT_CTRL 0xfffffffeu
#define QRTR_NODE_BCAST 0xffffffffu
#define QRTR_HDR_V1_BYTES 32u   // 8 __le32 words (struct qrtr_hdr_v1)

typedef struct {
    uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port;
} qrtr_hdr_v1;

typedef struct {
    uint32_t cmd, service, instance, node, port;   // server form; for client/resume use node/port only
} qrtr_ctrl_pkt;

// Build a v1 header + payload into buf. Returns total bytes, or -1 if it does not fit.
long qrtr_build(uint8_t *buf, size_t cap, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node,
                uint32_t dst_port, uint32_t confirm_rx, const void *payload, size_t payload_len);
// Parse a v1 header; sets *payload to the body and *payload_len to its length. Returns 0, or -1 if short or inconsistent.
int qrtr_parse(const uint8_t *buf, size_t len, qrtr_hdr_v1 *h, const uint8_t **payload, size_t *payload_len);
// A 20-byte control packet (cmd + service/instance/node/port). Build and parse.
void qrtr_ctrl_build(uint8_t out[20], uint32_t cmd, uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
int qrtr_ctrl_parse(const uint8_t *body, size_t len, qrtr_ctrl_pkt *out);

// ---- GLINK native commands (drivers/rpmsg/qcom_glink_native.c) ----
#define GLINK_CMD_VERSION 0u
#define GLINK_CMD_VERSION_ACK 1u
#define GLINK_CMD_OPEN 2u
#define GLINK_CMD_CLOSE 3u
#define GLINK_CMD_OPEN_ACK 4u
#define GLINK_CMD_INTENT 5u
#define GLINK_CMD_RX_DONE 6u
#define GLINK_CMD_RX_INTENT_REQ 7u
#define GLINK_CMD_RX_INTENT_REQ_ACK 8u
#define GLINK_CMD_TX_DATA 9u
#define GLINK_CMD_CLOSE_ACK 11u
#define GLINK_CMD_TX_DATA_CONT 12u
#define GLINK_CMD_READ_NOTIF 13u
#define GLINK_CMD_RX_DONE_W_REUSE 14u
#define GLINK_CMD_SIGNALS 15u
#define GLINK_VERSION_1 1u
#define GLINK_FEATURE_INTENT_REUSE (1u << 0)
#define GLINK_FEATURE_MIGRATION (1u << 1)
#define GLINK_FEATURE_INTENTLESS (1u << 1)
#define GLINK_NAME_SIZE 32u

typedef struct {uint16_t cmd, param1; uint32_t param2;} glink_msg_hdr;

// A bare 8-byte command (version, version_ack, open_ack, rx_done, close, close_ack, read_notif, intent_req_ack).
void glink_cmd(uint8_t out[8], uint16_t cmd, uint16_t param1, uint32_t param2);
// OPEN: header + name (NUL-terminated), padded to 8. param2 = name_len (incl NUL). Returns bytes or -1.
long glink_open(uint8_t *buf, size_t cap, uint16_t lcid, const char *name);
// INTENT advertise: id, lcid, count=1, size, liid (16 bytes).
void glink_intent(uint8_t out[16], uint16_t lcid, uint32_t size, uint32_t liid);
// TX_DATA header (16 bytes): cmd, lcid, iid, chunk_size, left_size. Caller appends the payload, padded to 8.
void glink_tx_data_hdr(uint8_t out[16], int cont, uint16_t lcid, uint32_t iid, uint32_t chunk_size, uint32_t left_size);
// Parse the 8-byte command header at the front of an rx buffer.
int glink_parse_hdr(const uint8_t *buf, size_t len, glink_msg_hdr *h);

#ifdef __cplusplus
}
#endif
#endif
