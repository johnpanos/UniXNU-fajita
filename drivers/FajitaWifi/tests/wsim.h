// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 target simulator for the host tests: a register file at the WLAN membase offsets and the copy-engine side
// of the hardware, over a host DMA arena (IOVA -> host memory). Source rings are consumed when the host publishes a
// write index (register or shadow), gather fragments concatenated, and handed to the firmware model (fw callback).
// The firmware model answers with wsim_send(), which fills the next posted destination buffer, sets nbytes, advances
// the read index (register + the DDR RRI word when ctrl1 bit 19 is set) and raises the copy-complete status.
#ifndef FAJITA_WIFI_WSIM_H
#define FAJITA_WIFI_WSIM_H
#include <stdint.h>
#include "../WifiCe.h"

#define WSIM_REG_BYTES 0x250000u
#define WSIM_DMA_IOVA 0x40000000ull

typedef struct wsim wsim;
typedef void (*wsim_fw_fn)(wsim *s, unsigned ce, const uint8_t *data, uint32_t len, uint32_t transfer_id, void *arg);

struct wsim {
    uint32_t *reg;                   // WSIM_REG_BYTES / 4
    uint8_t *mem;                    // DMA arena (host side of the IOVA window)
    uint64_t iova;
    uint32_t size;
    uint32_t srri[WSNOC_CE_COUNT], drri[WSNOC_CE_COUNT];
    uint8_t gather[WSNOC_CE_COUNT][4096];
    uint32_t glen[WSNOC_CE_COUNT];
    wsim_fw_fn fw;
    void *fw_arg;
    uint32_t writes, reads, barriers, dma_faults, src_msgs[WSNOC_CE_COUNT], no_buffer;
    int stall_src[WSNOC_CE_COUNT];  // when set, the target does not consume that source ring
    uint32_t shadow_writes[WSNOC_CE_COUNT];
};

void wsim_init(wsim *s, uint32_t dma_bytes);
void wsim_free(wsim *s);
wce_io wsim_io(wsim *s);
uint32_t wsim_reg(wsim *s, uint32_t off);
// target -> host on a CE: 0 sent, -1 no posted buffer, -2 too long / bad descriptor
int wsim_send(wsim *s, unsigned ce, const void *data, uint32_t len);
// consume a stalled source ring now
void wsim_drain(wsim *s, unsigned ce);
void *wsim_dma(wsim *s, uint64_t iova, uint32_t len);
#endif
