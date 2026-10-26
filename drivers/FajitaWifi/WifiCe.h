// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 copy engines (ath10k ce.c/ce.h with the wcn3990 register map of hw.c, Linux v6.16) and the SNOC HIF over them
// (snoc.c: pipe setup, rx post, rx/tx completion, service map). 64-bit descriptors (target_64bit), shadow source write
// indices (0x32000 + 4 * ce for CE 0/3/4/5/7), read indices in one host DMA word per CE (rri_on_ddr: ctrl1 bit 19).
// Freestanding and poll-driven: wce_poll() does the work of ath10k_ce_per_engine_service for every pipe. All device
// memory (rings, rx buffers, tx bounce buffers, the RRI word) comes from one DMA arena (wdma), so the SMMU window is a
// single IOVA range. The arena must be device-coherent from the CPU's point of view (uncached or write-combined);
// io.barrier orders normal-memory writes before a doorbell and a doorbell/index read before the descriptor reads.
// Host-tested against a register/DMA target simulator (tests/t_ce.c).
#ifndef FAJITA_WIFI_CE_H
#define FAJITA_WIFI_CE_H
#include <stddef.h>
#include <stdint.h>
#include "WifiSnoc.h"

#ifdef __cplusplus
extern "C" {
#endif

// wcn3990_regs / wcn3990_ce_regs (hw.c), offsets from the WLAN membase
#define WCE_CE0_BASE 0x240000u
#define WCE_CE_STRIDE 0x1000u
#define WCE_WRAPPER_BASE 0x24c000u
#define WCE_INTR_SUMMARY (WCE_WRAPPER_BASE + 0x0u)   // host_msi bits 12..23 = CE 0..11
#define WCE_RRI_LOW 0x24c004u
#define WCE_RRI_HIGH 0x24c008u
#define WCE_SHADOW_SRC_WR(ce) (0x32000u + 4u * (ce))   // shadow_sr_wr_ind_addr: CE 0, 3, 4, 5, 7
// per-CE register offsets
#define WCE_SR_BA_LO 0x00u
#define WCE_SR_BA_HI 0x04u
#define WCE_SR_SIZE 0x08u
#define WCE_DR_BA_LO 0x0cu
#define WCE_DR_BA_HI 0x10u
#define WCE_DR_SIZE 0x14u
#define WCE_CTRL1 0x18u                  // dmax 15:0, src byte swap 17, dst byte swap 18, rri update 19
#define WCE_CTRL1_DMAX_MASK 0xffffu
#define WCE_CTRL1_SRC_SWAP (1u << 17)
#define WCE_CTRL1_DST_SWAP (1u << 18)
#define WCE_CTRL1_RRI_UPD (1u << 19)
#define WCE_HOST_IE 0x2cu                // copy complete = bit 0, watermarks 0x1e
#define WCE_HOST_IS 0x30u                // write 1 to clear: cc 0x1, wm 0x1e
#define WCE_MISC_IE 0x34u                // error mask 0x3e0
#define WCE_MISC_IS 0x38u
#define WCE_SR_WR_INDEX 0x3cu
#define WCE_DST_WR_INDEX 0x40u
#define WCE_CURRENT_SRRI 0x44u
#define WCE_CURRENT_DRRI 0x48u
#define WCE_SRC_WM 0x4cu                 // low 31:16, high 15:0
#define WCE_DST_WM 0x50u
#define WCE_HOST_IE_CC 0x1u
#define WCE_HOST_WM_MASK 0x1eu
#define WCE_MISC_ERR_MASK 0x3e0u
// ce_desc_64
#define WCE_DESC_BYTES 16u
#define WCE_DESC_ADDR_MASK 0x7ffffffffull     // GENMASK_ULL(34, 0)
#define WCE_DESC_ADDR_HI_MASK 0x1fu
#define WCE_DESC_GATHER_HI (1u << 31)          // CE_WCN3990_DESC_FLAGS_GATHER, in the upper address word
#define WCE_DESC_FLAG_GATHER 0x1u              // CE_DESC_FLAGS_GATHER (desc flags)
#define WCE_DESC_META_MASK 0xfff0u             // ce_desc_meta_data_mask / lsb 4 (transfer id)
#define WCE_DESC_META_LSB 4u
#define WCE_SEND_FLAG_GATHER 0x10000u          // CE_SEND_FLAG_GATHER (upper address word of a gather fragment)

enum {WCE_OK = 0, WCE_ERR_NOSPC = -28, WCE_ERR_NOMEM = -12, WCE_ERR_INVAL = -22, WCE_ERR_IO = -5, WCE_ERR_NODEV = -19};

typedef struct {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t off);           // 32-bit read at membase + off
    void (*wr)(void *ctx, uint32_t off, uint32_t v);   // 32-bit write at membase + off
    void (*barrier)(void *ctx);                        // dsb: DMA memory <-> MMIO ordering
} wce_io;

// One contiguous DMA arena, CPU address <-> IOVA by offset. Bump-allocated, never freed.
typedef struct {
    uint8_t *cpu;
    uint64_t iova;
    uint32_t size, used;
} wdma;
void wdma_init(wdma *d, void *cpu, uint64_t iova, uint32_t size);
void *wdma_alloc(wdma *d, uint32_t size, uint32_t align, uint64_t *iova);   // zeroed; NULL when full
uint64_t wdma_iova(const wdma *d, const void *cpu);                         // 0 when outside
void *wdma_cpu(const wdma *d, uint64_t iova, uint32_t len);                 // NULL when outside

typedef struct {
    uint32_t nentries, mask, sw_index, write_index, hw_index;
    volatile uint8_t *desc;          // nentries * 16
    uint64_t desc_iova;
    uintptr_t *ctx;                  // per-transfer context
} wce_ring;

typedef struct {
    uint32_t id, attr, src_sz_max, buf_sz;
    wce_ring src, dst;
    uint8_t *rxbuf;                  // dst.nentries * buf_sz, slot i owned by ring context i
    uint64_t rxbuf_iova;
    uint8_t *txbuf;                  // src.nentries * src_sz_max: bounce buffer of the slot a copy send uses
    uint64_t txbuf_iova;
    uint32_t rx, tx, tx_done, rx_bad, full;
} wce_pipe;

struct wce;
// A received transfer on a pipe; data valid during the call only (the buffer is posted again afterwards).
typedef void (*wce_rx_fn)(struct wce *ce, unsigned pipe, uint8_t *data, uint32_t len, void *arg);
// A source descriptor the target has consumed (ctx as given to wce_send).
typedef void (*wce_tx_fn)(struct wce *ce, unsigned pipe, uintptr_t ctx, void *arg);

typedef struct wce {
    wce_io io;
    wdma *dma;
    volatile uint32_t *rri;          // WSNOC_CE_COUNT words: src rri 15:0, dst rri 31:16
    uint64_t rri_iova;
    wce_pipe pipe[WSNOC_CE_COUNT];
    wce_rx_fn rx;
    wce_tx_fn tx_done;
    void *arg;
    void *(*alloc)(size_t size, void *ctx);
    void *alloc_ctx;
    int started;
} wce;

// ath10k_snoc_hif_power_up after wlan_enable: allocate rings/buffers from the arena, ath10k_ce_alloc_rri, init every
// pipe (base, size, dmax, swap, watermarks), enable copy-complete interrupts where ath10k does. cfg NULL = the ath10k
// host_ce_config_wlan; dst_limit (0 = none) caps destination ring sizes, to shrink the arena.
int wce_init(wce *ce, const wce_io *io, wdma *dma, const wsnoc_host_ce *cfg, uint32_t dst_limit,
             void *(*alloc)(size_t, void *), void *alloc_ctx);
// ath10k_snoc_hif_start: post rx buffers on every pipe with a destination ring.
int wce_start(wce *ce);
// One descriptor (ath10k_ce_send_nolock_64). gather: more fragments follow (the write index is published with the last).
int wce_send(wce *ce, unsigned pipe, uint64_t iova, uint32_t len, uint32_t transfer_id, int gather, uintptr_t ctx);
// Copy hdr + payload into the bounce buffer of the next source slot and send it as one descriptor.
int wce_send_copy(wce *ce, unsigned pipe, const void *hdr, uint32_t hlen, const void *data, uint32_t len,
                  uint32_t transfer_id, uintptr_t ctx);
// Revert the last unpublished (gather) descriptor (__ath10k_ce_send_revert).
void wce_send_revert(wce *ce, unsigned pipe);
unsigned wce_src_free(wce *ce, unsigned pipe);
// ath10k_ce_per_engine_service on every pipe: clear status, rx completions (callback, repost), tx completions.
// Returns the number of transfers handled.
unsigned wce_poll(wce *ce);
uint32_t wce_intr_summary(wce *ce);

#ifdef __cplusplus
}
#endif
#endif
