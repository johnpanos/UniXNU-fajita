// SPDX-License-Identifier: BSD-2-Clause-Patent
// AOSS QMP mailbox link-up, the AP (mcore) side, as mainline Linux drivers/soc/qcom/qcom_aoss.c qmp_open() does it:
// check magic/version/mailbox size, ack the ucore (AOSS) link state, set the mcore link up, kick, wait for the link ack,
// set the mcore channel up, kick, wait for the ucore channel up, ack it, kick, wait for the mcore channel ack. A timeout
// closes the channel and/or link the way qmp_open's error path does. No message is ever written to the mailbox.
// Freestanding C: the caller supplies 32-bit register access to the message RAM, the doorbell and a clock. Host-tested
// against a simulated AOSS (tests/t_qmp.c).
#ifndef FAJITA_WIFI_QMP_H
#define FAJITA_WIFI_QMP_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// qcom_aoss.c QMP_DESC_* (message RAM offsets).
enum {
    WQMP_MAGIC_OFF = 0x0, WQMP_VERSION_OFF = 0x4, WQMP_FEATURES_OFF = 0x8,
    WQMP_UCORE_LINK = 0xc, WQMP_UCORE_LINK_ACK = 0x10, WQMP_UCORE_CH = 0x14, WQMP_UCORE_CH_ACK = 0x18,
    WQMP_UCORE_MBOX_SIZE = 0x1c, WQMP_UCORE_MBOX_OFF = 0x20,
    WQMP_MCORE_LINK = 0x24, WQMP_MCORE_LINK_ACK = 0x28, WQMP_MCORE_CH = 0x2c, WQMP_MCORE_CH_ACK = 0x30,
    WQMP_MCORE_MBOX_SIZE = 0x34, WQMP_MCORE_MBOX_OFF = 0x38,
};
#define WQMP_STATE_UP 0x0000ffffu     // GENMASK(15, 0)
#define WQMP_STATE_DOWN 0xffff0000u   // GENMASK(31, 16)
#define WQMP_MAGIC 0x4d41494cu        // "mail"
#define WQMP_VERSION 1u
#define WQMP_TIMEOUT_US 1000000u      // wait_event_timeout(..., HZ)

enum {WQMP_OK = 0, WQMP_ERR_INVAL = -22, WQMP_ERR_TIMEDOUT = -60};

typedef struct {
    uint32_t (*rd)(void *ctx, uint32_t off);            // 32-bit read of the message RAM (readl)
    void (*wr)(void *ctx, uint32_t off, uint32_t v);    // 32-bit write (writel: barrier before the store)
    void (*kick)(void *ctx);                            // ring the AOSS doorbell (apss_shared IPC bit 0)
    uint64_t (*now_us)(void *ctx);                      // a monotonic clock
    void (*relax)(void *ctx);                           // between polls (sleep or delay ~100 us .. 1 ms)
    void *ctx;
} wqmp_io;

// Both ends' descriptor words, as read.
typedef struct {
    uint32_t magic, version, features;
    uint32_t ucore_link, ucore_link_ack, ucore_ch, ucore_ch_ack, ucore_mbox_size, ucore_mbox_off;
    uint32_t mcore_link, mcore_link_ack, mcore_ch, mcore_ch_ack, mcore_mbox_size, mcore_mbox_off;
} wqmp_desc;
void wqmp_read(const wqmp_io *io, wqmp_desc *d);
// Link and both channels up and acknowledged both ways (what a finished qmp_open leaves).
int wqmp_is_up(const wqmp_desc *d);
const char *wqmp_state_name(uint32_t v);   // "up", "down", "0" or "?"

// The steps of wqmp_open, in order; each one recorded in the result.
enum {
    WQMP_STEP_CHECK,          // magic, version, mcore mailbox size/offset
    WQMP_STEP_ACK_UCORE_LINK, // UCORE_LINK_STATE_ACK = UCORE_LINK_STATE
    WQMP_STEP_LINK_UP,        // MCORE_LINK_STATE = UP, kick
    WQMP_STEP_WAIT_LINK_ACK,  // MCORE_LINK_STATE_ACK == UP
    WQMP_STEP_CH_UP,          // MCORE_CH_STATE = UP, kick
    WQMP_STEP_WAIT_UCORE_CH,  // UCORE_CH_STATE == UP
    WQMP_STEP_ACK_UCORE_CH,   // UCORE_CH_STATE_ACK = UP, kick
    WQMP_STEP_WAIT_CH_ACK,    // MCORE_CH_STATE_ACK == UP
    WQMP_STEP_CLOSE,          // the timeout path: MCORE_CH (if opened) and MCORE_LINK = DOWN, kick
    WQMP_NSTEPS
};
const char *wqmp_step_name(unsigned step);
typedef struct {
    unsigned step;      // WQMP_STEP_*
    int rc;             // 0, WQMP_ERR_INVAL or WQMP_ERR_TIMEDOUT
    uint32_t value;     // the word the step wrote or the last word it read
    uint32_t elapsed_us;
} wqmp_step;
typedef struct {
    wqmp_step step[WQMP_NSTEPS];
    unsigned nsteps, kicks;
    uint32_t mbox_off, mbox_size;   // the mcore (AP -> AOSS) mailbox, for qmp_send later
    int rc;
} wqmp_result;
// qmp_open. timeout_us per wait (0 = WQMP_TIMEOUT_US). Returns WQMP_OK when the link and both channels are up.
int wqmp_open(const wqmp_io *io, uint32_t timeout_us, wqmp_result *res);

#ifdef __cplusplus
}
#endif
#endif
