// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WifiQrtr.h"
#include <string.h>

static void wr32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint16_t rd16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}

long qrtr_build(uint8_t *buf, size_t cap, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node,
                uint32_t dst_port, uint32_t confirm_rx, const void *payload, size_t payload_len)
{
    // struct qrtr_hdr_v1: version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port (af_qrtr.c).
    if (payload_len & 3) return -1;   // QRTR requires 4-byte-aligned payloads
    if (QRTR_HDR_V1_BYTES + payload_len > cap) return -1;
    wr32(buf + 0, QRTR_PROTO_VER_1);
    wr32(buf + 4, type);
    wr32(buf + 8, src_node);
    wr32(buf + 12, src_port);
    wr32(buf + 16, confirm_rx);
    wr32(buf + 20, (uint32_t)payload_len);
    wr32(buf + 24, dst_node);
    wr32(buf + 28, dst_port);
    if (payload_len) memcpy(buf + 32, payload, payload_len);
    return (long)(QRTR_HDR_V1_BYTES + payload_len);
}

int qrtr_parse(const uint8_t *buf, size_t len, qrtr_hdr_v1 *h, const uint8_t **payload, size_t *payload_len)
{
    if (len < 32) return -1;
    h->version = rd32(buf + 0);
    h->type = rd32(buf + 4);
    h->src_node = rd32(buf + 8);
    h->src_port = rd32(buf + 12);
    h->confirm_rx = rd32(buf + 16);
    h->size = rd32(buf + 20);
    h->dst_node = rd32(buf + 24);
    h->dst_port = rd32(buf + 28);
    if (h->version != QRTR_PROTO_VER_1) return -1;
    if ((size_t)32 + h->size > len) return -1;
    if (payload) *payload = buf + 32;
    if (payload_len) *payload_len = h->size;
    return 0;
}

void qrtr_ctrl_build(uint8_t out[20], uint32_t cmd, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    wr32(out + 0, cmd);
    wr32(out + 4, service);
    wr32(out + 8, instance);
    wr32(out + 12, node);
    wr32(out + 16, port);
}

int qrtr_ctrl_parse(const uint8_t *body, size_t len, qrtr_ctrl_pkt *out)
{
    if (len < 4) return -1;
    memset(out, 0, sizeof *out);
    out->cmd = rd32(body);
    if (len >= 20) {
        out->service = rd32(body + 4);
        out->instance = rd32(body + 8);
        out->node = rd32(body + 12);
        out->port = rd32(body + 16);
    } else if (len >= 12) {
        // client / resume_tx form: node, port after cmd
        out->node = rd32(body + 4);
        out->port = rd32(body + 8);
    }
    return 0;
}

// ---- GLINK ----
static void wr16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}

void glink_cmd(uint8_t out[8], uint16_t cmd, uint16_t param1, uint32_t param2)
{
    wr16(out, cmd); wr16(out + 2, param1); wr32(out + 4, param2);
}

long glink_open(uint8_t *buf, size_t cap, uint16_t lcid, const char *name)
{
    size_t name_len = strlen(name) + 1;
    if (name_len > GLINK_NAME_SIZE) return -1;
    size_t total = (8 + name_len + 7) & ~(size_t)7;
    if (total > cap) return -1;
    memset(buf, 0, total);
    glink_cmd(buf, GLINK_CMD_OPEN, lcid, (uint32_t)name_len);
    memcpy(buf + 8, name, name_len);
    return (long)total;
}

void glink_intent(uint8_t out[20], uint16_t lcid, uint32_t size, uint32_t liid)
{
    wr16(out, GLINK_CMD_INTENT); wr16(out + 2, lcid);
    wr32(out + 4, 1);          // count
    wr32(out + 8, size);
    wr32(out + 12, liid);
    wr32(out + 16, 0);         // pad to 20 (count struct: size,iid pairs; one here)
}

void glink_tx_data_hdr(uint8_t out[16], int cont, uint16_t lcid, uint32_t iid, uint32_t chunk_size, uint32_t left_size)
{
    wr16(out, cont ? GLINK_CMD_TX_DATA_CONT : GLINK_CMD_TX_DATA);
    wr16(out + 2, lcid);
    wr32(out + 4, iid);
    wr32(out + 8, chunk_size);
    wr32(out + 12, left_size);
}

int glink_parse_hdr(const uint8_t *buf, size_t len, glink_msg_hdr *h)
{
    if (len < 8) return -1;
    h->cmd = rd16(buf);
    h->param1 = rd16(buf + 2);
    h->param2 = rd32(buf + 4);
    return 0;
}
