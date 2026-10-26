// SPDX-License-Identifier: BSD-2-Clause-Patent
// HTT data path of the WCN3990 (layer 6), after ath10k htt.c / htt_tx.c / htt_rx.c (Linux v6.16, target_64bit, full-reorder
// in-order rx, native-wifi tx and rx decap, continuous fragment descriptors, no peer flow control):
//   setup   VERSION_REQ -> VERSION_CONF, then FRAG_DESC_BANK_CFG, RX_RING_CFG (64-bit ring, rx descriptor offsets of
//           htt_rx_desc_v2) and AGGR_CFG (v2) on the HTT endpoint (through HTC, creditless);
//   rx      a ring of 64-bit buffer addresses the target fills (alloc index word in host memory); RX_IN_ORD_PADDR_IND lists
//           the filled buffers, each [rx descriptor][native-wifi header][LLC/SNAP][payload]; they are converted to Ethernet II
//           frames and handed up, then posted again;
//   tx      an Ethernet frame becomes a native-wifi data frame in a per-id DMA buffer, described by a TX_FRM (HTC header, htt
//           command, tx descriptor) plus the first bytes of the frame (prefetch), sent as a two-fragment gather on the HTT
//           CE pipe without HTC; the target answers with TX_COMPL_IND carrying the ids.
// Freestanding and non-blocking; all device memory from ops.dma_alloc (one SMMU window). Host-tested on the CE simulator
// (tests/t_htt.c).
#ifndef FAJITA_WIFI_HTT_H
#define FAJITA_WIFI_HTT_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WHTT_RX_BUF 2048u              // HTT_RX_BUF_SIZE
#define WHTT_RX_DESC_SIZE 360u         // sizeof(struct htt_rx_desc_v2)
#define WHTT_TX_BUF 2048u              // one native-wifi frame per pending tx id
#define WHTT_PREFETCH 50u              // 36 (802.11 + qos + ht) + 4 (802.1q) + 8 (llc snap) + 2
#define WHTT_MAX_ETH 1514u
#define WHTT_MAX_TX 256u

enum {WHTT_OK = 0, WHTT_ERR_INVAL = -22, WHTT_ERR_NOMEM = -12, WHTT_ERR_BUSY = -16, WHTT_ERR_PROTO = -71, WHTT_ERR_IO = -5};
enum {WHTT_IDLE = 0, WHTT_VERSION_WAIT, WHTT_READY, WHTT_FAILED};

typedef struct {
    void *ctx;
    // a control message on the HTT endpoint (through HTC): the payload starts with the HTT message type
    int (*ctl_send)(void *ctx, const void *msg, uint32_t len);
    // TX_FRM: two gather fragments on the HTT upload pipe (HTC header + HTT command + tx descriptor; frame prefetch). 0 or -errno.
    int (*tx_sg)(void *ctx, uint64_t iova0, uint32_t len0, uint64_t iova1, uint32_t len1);
    void *(*dma_alloc)(void *ctx, uint32_t size, uint32_t align, uint64_t *iova);
    void (*barrier)(void *ctx);                     // normal memory -> device-visible, before an index is published
    // a received data frame as Ethernet II (14 byte header + payload); valid during the call
    void (*rx)(void *ctx, const uint8_t *eth, uint32_t len);
    // a transmit finished (status 0 acked, 1 discarded, 2 no ack) or was dropped
    void (*tx_done)(void *ctx, unsigned status);
    // HTT security indication (peer id, flags): the target has the key
    void (*sec_ind)(void *ctx, unsigned peer_id, unsigned flags);
} whtt_ops;

typedef struct {
    uint32_t rx_ring_size;        // power of two, 128..2048 (HTT_RX_RING_SIZE_MIN/MAX); buffers posted = size / 2 - 1
    uint32_t max_tx;              // pending tx ids; must not exceed the WMI resource config num_msdu_desc
    uint8_t eid;                  // HTC endpoint of the HTT service (the HTC header of TX_FRM)
    uint8_t max_ampdu, max_amsdu; // AGGR_CFG (64 and 3 are the ath10k defaults); 0 = defaults
    int nwifi_pad;                // the nwifi header is padded to 4 bytes (ath10k without NO_NWIFI_DECAP_4ADDR_PADDING)
} whtt_cfg;

typedef struct whtt {
    whtt_ops ops;
    whtt_cfg cfg;
    unsigned state;
    unsigned ver_major, ver_minor;
    // rx
    volatile uint64_t *ring; uint64_t ring_iova;
    volatile uint32_t *alloc_idx; uint64_t alloc_idx_iova;
    uint8_t *pool; uint64_t pool_iova; uint32_t pool_bufs;
    uint8_t *posted;              // pool_bufs flags: the buffer is in the ring
    uint32_t fill, fill_level, size_mask;
    int rx_confused;
    // tx
    uint8_t *txbuf; uint64_t txbuf_iova;       // max_tx * 44 bytes (ath10k_htt_txbuf_64)
    uint8_t *frag; uint64_t frag_iova;         // max_tx * 72 bytes (htt_msdu_ext_desc_64)
    uint8_t *data; uint64_t data_iova;         // max_tx * WHTT_TX_BUF
    uint16_t *free_ids; uint32_t nfree;
    uint8_t *busy;
    // counters
    uint32_t rx_offload;          // MSDUs of offloaded in-order indications (no rx descriptor)
    uint32_t rx_ind, rx_frames, rx_drop_attn, rx_drop_decap, rx_drop_fmt, rx_drop_nondata, rx_bad;
    uint32_t tx_frames, tx_done, tx_full, tx_err, sec_ind, other_msgs, peer_map;
} whtt;

// Allocate the rings and buffers and post the rx buffers. Memory comes from ops.dma_alloc and is never freed.
int whtt_init(whtt *h, const whtt_ops *ops, const whtt_cfg *cfg);
// Send VERSION_REQ; the rest of the setup follows the VERSION_CONF in whtt_rx. state READY when it has all been sent.
int whtt_start(whtt *h);
// A target->host HTT message (starting at the HTT message type; the HTC header already removed).
void whtt_rx(whtt *h, const uint8_t *msg, uint32_t len);
// Send an Ethernet frame (dst, src, ethertype, payload) as a native-wifi data frame of vdev `vdev` towards the AP `bssid`.
// protect: set the protected bit (the target encrypts with the peer's key); clear for frames that go in the clear (EAPOL before keys).
int whtt_tx(whtt *h, unsigned vdev, const uint8_t bssid[6], const uint8_t *eth, uint32_t len, int protect);
unsigned whtt_tx_free(const whtt *h);

#ifdef __cplusplus
}
#endif
#endif
