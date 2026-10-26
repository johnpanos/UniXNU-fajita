// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm SMEM (shared memory heap + private partitions) and SMP2P (shared-memory point-to-point state bits), after
// drivers/soc/qcom/smem.c and smp2p.c of Linux v6.16. Freestanding C over a mapped SMEM window: every access is a
// naturally aligned 8/16/32-bit volatile load or store, so it is safe on Device/uncached memory. The hardware spinlock
// (TCSR mutex 3 on SDM845) and the IPC doorbell are callbacks. Host-tested against a synthetic SMEM image.
#ifndef FAJITA_WIFI_SMEM_H
#define FAJITA_WIFI_SMEM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WSMEM_ITEM_COUNT 512u
#define WSMEM_ITEM_LAST_FIXED 8u
#define WSMEM_HOST_APPS 0u
#define WSMEM_HOST_MODEM 1u
#define WSMEM_GLOBAL_HOST 0xfffeu
#define WSMEM_HOST_COUNT 25u
#define WSMEM_GLOBAL_HEAP_VERSION 11u
#define WSMEM_GLOBAL_PART_VERSION 12u
#define WSMEM_SBL_VERSION_INDEX 7u
#define WSMEM_HEADER_BYTES (16u * 4 + 32u * 4 + 16u + WSMEM_ITEM_COUNT * 16u)   // struct smem_header = 0x20d0
#define WSMEM_PTABLE_BYTES 4096u                                              // the ptable is the last 4 KiB
#define WSMEM_PART_HDR_BYTES 32u
#define WSMEM_PRIV_ENTRY_BYTES 16u
#define WSMEM_CANARY 0xa5a5u

enum {WSMEM_OK = 0, WSMEM_ERR_INVAL = -22, WSMEM_ERR_NOENT = -2, WSMEM_ERR_EXIST = -17, WSMEM_ERR_NOSPC = -28,
      WSMEM_ERR_NXIO = -6, WSMEM_ERR_LOCK = -110, WSMEM_ERR_NOTINIT = -19};

typedef struct {
    uint32_t offset, size, cacheline;   // offset from the SMEM base; 0 size = absent
} wsmem_part;

typedef struct {
    volatile uint8_t *base;     // the mapped SMEM window
    size_t size;
    // Hardware mutex around allocations (qcom_hwspinlock). NULL lock = no locking (host tests, single master).
    int (*lock)(void *ctx);     // 0 when taken
    void (*unlock)(void *ctx);
    void *lock_ctx;
    uint32_t version;           // SBL version word >> 16 (11 heap, 12 partitioned)
    uint32_t item_count;
    wsmem_part global;
    wsmem_part part[WSMEM_HOST_COUNT];
    unsigned nparts;
} wsmem;

// Validate the header (initialized == 1, reserved == 0), read the SBL version, and enumerate the global partition and
// the private partitions of `local_host` (0 = APPS). Returns 0 or a negative WSMEM_ERR.
int wsmem_init(wsmem *s, volatile void *base, size_t size, unsigned local_host);
// Look up `item` in the partition shared with `host` (else the global partition, else the legacy heap). Returns the item
// pointer and, if `size` is non-NULL, its usable size; NULL on failure with *err set (when non-NULL).
volatile void *wsmem_get(wsmem *s, unsigned host, unsigned item, size_t *size, int *err);
// Allocate `item` of `size` bytes in the partition shared with `host` (or global). -EEXIST if it already exists.
int wsmem_alloc(wsmem *s, unsigned host, unsigned item, size_t size);
// Free bytes in the partition shared with `host` (or the heap). Negative on error.
long wsmem_free_space(wsmem *s, unsigned host);
// Offset of p from the SMEM base (for logging and for computing physical addresses), or -1 if outside.
long wsmem_offset(const wsmem *s, const volatile void *p);

// ---- SMP2P ----
#define WSMP2P_MAGIC 0x504d5324u
#define WSMP2P_MAX_ENTRY 16u
#define WSMP2P_NAME 16u
#define WSMP2P_ITEM_BYTES (20u + WSMP2P_MAX_ENTRY * 20u)
#define WSMP2P_FEATURE_SSR_ACK 0x1u
#define WSMP2P_RESTART_DONE_BIT 0
#define WSMP2P_RESTART_ACK_BIT 1
// The modem's inbound bits (sdm845.dtsi mss_pil interrupts-extended) and the outbound stop bit (qcom,smem-states).
#define WSMP2P_MSS_WDOG 0
#define WSMP2P_MSS_FATAL 1
#define WSMP2P_MSS_READY 2
#define WSMP2P_MSS_HANDOVER 3
#define WSMP2P_MSS_STOP_ACK 7
#define WSMP2P_MSS_STOP 0

typedef struct {
    wsmem *smem;
    unsigned local_pid, remote_pid, out_item, in_item;
    volatile uint8_t *out, *in;
    void (*kick)(void *ctx);    // ring the remote's IPC doorbell (APCS IPC bit)
    void *kick_ctx;
    int negotiated, ssr_ack_enabled, ssr_ack;
    unsigned in_valid;          // inbound entries already seen
    uint32_t in_last[WSMP2P_MAX_ENTRY];
    uint32_t in_changed[WSMP2P_MAX_ENTRY];   // bits that changed since the last wsmp2p_take()
} wsmp2p;

// Allocate (or reuse) the outbound item in SMEM, write its header, publish version 1 last, and kick.
int wsmp2p_init(wsmp2p *p, wsmem *smem, unsigned local_pid, unsigned remote_pid, unsigned out_item, unsigned in_item,
                void (*kick)(void *), void *kick_ctx);
// Add an outbound entry ("master-kernel"); returns its index or a negative error. Kicks.
int wsmp2p_add_entry(wsmp2p *p, const char *name);
// Read-modify-write the outbound entry's bits; kicks if the value changed.
int wsmp2p_update(wsmp2p *p, int entry, uint32_t mask, uint32_t value);
// The interrupt body: find the inbound item, negotiate features, track entry values, ack an SSR restart. Call it on
// every SMP2P interrupt (or poll). Returns 1 once negotiated, 0 while the remote item is absent or not yet negotiated.
int wsmp2p_irq(wsmp2p *p);
// The inbound entry `name`: its current value, and the bits changed since the last take (cleared). -2 if not present.
int wsmp2p_take(wsmp2p *p, const char *name, uint32_t *value, uint32_t *changed);

#ifdef __cplusplus
}
#endif
#endif
