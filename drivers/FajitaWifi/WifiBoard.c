// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WifiBoard.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static size_t align4(size_t n) {return (n + 3) & ~(size_t)3;}

static int fmt_hex(char *dst, size_t cap, const char *prefix, uint32_t board, int with_chip, uint32_t chip)
{
    // "bus=snoc,qmi-board-id=%x" [",qmi-chip-id=%x"]
    size_t n = 0;
    const char *p = prefix;
    while (*p && n + 1 < cap) dst[n++] = *p++;
    static const char hex[] = "0123456789abcdef";
    char tmp[9]; int ti = 0;
    uint32_t v = board;
    if (!v) tmp[ti++] = '0'; else while (v) {tmp[ti++] = hex[v & 0xf]; v >>= 4;}
    while (ti && n + 1 < cap) dst[n++] = tmp[--ti];
    if (with_chip) {
        const char *c = ",qmi-chip-id=";
        while (*c && n + 1 < cap) dst[n++] = *c++;
        ti = 0; v = chip;
        if (!v) tmp[ti++] = '0'; else while (v) {tmp[ti++] = hex[v & 0xf]; v >>= 4;}
        while (ti && n + 1 < cap) dst[n++] = tmp[--ti];
    }
    dst[n] = 0;
    return (int)n;
}

unsigned wboard_candidate_names(char names[][64], unsigned cap, uint32_t qmi_board_id, uint32_t qmi_chip_id)
{
    unsigned n = 0;
    if (qmi_chip_id != 0xff && n < cap) {
        fmt_hex(names[n], 64, "bus=snoc,qmi-board-id=", qmi_board_id, 1, qmi_chip_id); ++n;
    }
    if (n < cap) {fmt_hex(names[n], 64, "bus=snoc,qmi-board-id=", qmi_board_id, 0, 0); ++n;}
    return n;
}

// Parse the inner board IE list (board-name / board-data pairs) for a name match.
static int parse_board_ie(const uint8_t *data, size_t len, const char names[][64], unsigned nnames,
                          const uint8_t **out, uint32_t *outlen)
{
    int matched = -1;
    while (len > 8) {
        const uint32_t id = rd32(data), ielen = rd32(data + 4);
        data += 8; len -= 8;
        if (align4(ielen) > len) return -1;
        if (id == WBOARD_IE_BOARD_NAME) {
            matched = -1;
            for (unsigned i = 0; i < nnames; ++i) {
                if (strlen(names[i]) == ielen && !memcmp(data, names[i], ielen)) {matched = (int)i; break;}
            }
        } else if (id == WBOARD_IE_BOARD_DATA && matched >= 0) {
            *out = data; *outlen = ielen;
            return matched;
        }
        data += align4(ielen); len -= align4(ielen);
    }
    return -1;
}

int wboard_select(const uint8_t *file, size_t file_len, const char names[][64], unsigned nnames,
                  const uint8_t **data, uint32_t *len)
{
    const size_t magic_len = align4(sizeof(WBOARD_MAGIC));   // strlen+1, aligned to 4 (= 20)
    if (file_len < magic_len || memcmp(file, WBOARD_MAGIC, sizeof(WBOARD_MAGIC) - 1)) return -1;
    const uint8_t *p = file + magic_len;
    size_t rem = file_len - magic_len;
    while (rem > 8) {
        const uint32_t id = rd32(p), ielen = rd32(p + 4);
        p += 8; rem -= 8;
        if (align4(ielen) > rem) return -1;
        if (id == WBOARD_IE_BOARD) {
            int m = parse_board_ie(p, ielen, names, nnames, data, len);
            if (m >= 0) return m;
        }
        p += align4(ielen); rem -= align4(ielen);
    }
    return -1;
}
