// SPDX-License-Identifier: BSD-2-Clause-Patent
// ath10k board-data (board-2.bin) selection: pick the board data for this WCN3990 by the QMI board id, matching the
// board-name IE. After drivers/net/wireless/ath/ath10k/core.c (ath10k_core_search_bd, ath10k_core_create_board_name) of
// Linux v6.16. Freestanding C, host-tested.
#ifndef FAJITA_WIFI_BOARD_SELECT_H
#define FAJITA_WIFI_BOARD_SELECT_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WBOARD_MAGIC "QCA-ATH10K-BOARD"
#define WBOARD_IE_BOARD 0
#define WBOARD_IE_BOARD_NAME 0
#define WBOARD_IE_BOARD_DATA 1

// Compose the SNOC board name candidates ath10k tries, in order. `names` must hold at least 3 slots of >= 64 bytes.
// Returns how many were written. qmi_chip_id may be 0xff (unknown), in which case the chip-id candidate is skipped.
unsigned wboard_candidate_names(char names[][64], unsigned cap, uint32_t qmi_board_id, uint32_t qmi_chip_id);

// Search board-2.bin (whole file, with magic) for any candidate name; on a match, point *data/*len at the board data.
// Returns the index of the matched candidate, or -1 if none matched / the file is malformed.
int wboard_select(const uint8_t *file, size_t file_len, const char names[][64], unsigned nnames,
                  const uint8_t **data, uint32_t *len);

#ifdef __cplusplus
}
#endif
#endif
