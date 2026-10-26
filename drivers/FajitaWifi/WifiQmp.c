// SPDX-License-Identifier: BSD-2-Clause-Patent
// AOSS QMP link-up, AP side (qcom_aoss.c qmp_open). See WifiQmp.h.
#include "WifiQmp.h"

void wqmp_read(const wqmp_io *io, wqmp_desc *d)
{
    uint32_t *w = &d->magic;
    for (unsigned i = 0; i < sizeof *d / 4; ++i) w[i] = io->rd(io->ctx, 4u * i);
}

int wqmp_is_up(const wqmp_desc *d)
{
    return d->magic == WQMP_MAGIC && d->mcore_link == WQMP_STATE_UP && d->mcore_link_ack == WQMP_STATE_UP
        && d->mcore_ch == WQMP_STATE_UP && d->mcore_ch_ack == WQMP_STATE_UP && d->ucore_ch == WQMP_STATE_UP
        && d->ucore_ch_ack == WQMP_STATE_UP;
}

const char *wqmp_state_name(uint32_t v)
{
    return v == WQMP_STATE_UP ? "up" : v == WQMP_STATE_DOWN ? "down" : v == 0 ? "0" : "?";
}

const char *wqmp_step_name(unsigned step)
{
    static const char *const n[WQMP_NSTEPS] = {"check", "ack-ucore-link", "link-up", "wait-link-ack", "ch-up",
                                                "wait-ucore-ch", "ack-ucore-ch", "wait-ch-ack", "close"};
    return step < WQMP_NSTEPS ? n[step] : "?";
}

static wqmp_step *record(wqmp_result *res, unsigned step, int rc, uint32_t value, uint32_t elapsed)
{
    if (res->nsteps >= WQMP_NSTEPS) return 0;
    wqmp_step *s = &res->step[res->nsteps++];
    s->step = step; s->rc = rc; s->value = value; s->elapsed_us = elapsed;
    return s;
}

static void kick(const wqmp_io *io, wqmp_result *res) {io->kick(io->ctx); ++res->kicks;}

// wait_event_timeout(event, rd(off) == UP, HZ): the condition is checked first, then after every relax().
static int wait_up(const wqmp_io *io, wqmp_result *res, unsigned step, uint32_t off, uint32_t timeout_us)
{
    const uint64_t t0 = io->now_us(io->ctx);
    uint32_t v;
    for (;;) {
        v = io->rd(io->ctx, off);
        const uint64_t dt = io->now_us(io->ctx) - t0;
        if (v == WQMP_STATE_UP) {record(res, step, WQMP_OK, v, (uint32_t)dt); return WQMP_OK;}
        if (dt >= timeout_us) {record(res, step, WQMP_ERR_TIMEDOUT, v, (uint32_t)dt); return WQMP_ERR_TIMEDOUT;}
        io->relax(io->ctx);
    }
}

int wqmp_open(const wqmp_io *io, uint32_t timeout_us, wqmp_result *res)
{
    if (!timeout_us) timeout_us = WQMP_TIMEOUT_US;
    res->nsteps = 0; res->kicks = 0; res->mbox_off = 0; res->mbox_size = 0;
    const uint32_t magic = io->rd(io->ctx, WQMP_MAGIC_OFF);
    if (magic != WQMP_MAGIC) {record(res, WQMP_STEP_CHECK, WQMP_ERR_INVAL, magic, 0); return res->rc = WQMP_ERR_INVAL;}
    const uint32_t ver = io->rd(io->ctx, WQMP_VERSION_OFF);
    if (ver != WQMP_VERSION) {record(res, WQMP_STEP_CHECK, WQMP_ERR_INVAL, ver, 0); return res->rc = WQMP_ERR_INVAL;}
    res->mbox_off = io->rd(io->ctx, WQMP_MCORE_MBOX_OFF);
    res->mbox_size = io->rd(io->ctx, WQMP_MCORE_MBOX_SIZE);
    if (!res->mbox_size) {record(res, WQMP_STEP_CHECK, WQMP_ERR_INVAL, 0, 0); return res->rc = WQMP_ERR_INVAL;}
    record(res, WQMP_STEP_CHECK, WQMP_OK, ver, 0);

    // Ack remote core's link state.
    const uint32_t ul = io->rd(io->ctx, WQMP_UCORE_LINK);
    io->wr(io->ctx, WQMP_UCORE_LINK_ACK, ul);
    record(res, WQMP_STEP_ACK_UCORE_LINK, WQMP_OK, ul, 0);
    // Set local core's link state to up.
    io->wr(io->ctx, WQMP_MCORE_LINK, WQMP_STATE_UP);
    kick(io, res);
    record(res, WQMP_STEP_LINK_UP, WQMP_OK, WQMP_STATE_UP, 0);
    if (wait_up(io, res, WQMP_STEP_WAIT_LINK_ACK, WQMP_MCORE_LINK_ACK, timeout_us)) goto close_link;

    io->wr(io->ctx, WQMP_MCORE_CH, WQMP_STATE_UP);
    kick(io, res);
    record(res, WQMP_STEP_CH_UP, WQMP_OK, WQMP_STATE_UP, 0);
    if (wait_up(io, res, WQMP_STEP_WAIT_UCORE_CH, WQMP_UCORE_CH, timeout_us)) goto close_channel;

    // Ack remote core's channel state.
    io->wr(io->ctx, WQMP_UCORE_CH_ACK, WQMP_STATE_UP);
    kick(io, res);
    record(res, WQMP_STEP_ACK_UCORE_CH, WQMP_OK, WQMP_STATE_UP, 0);
    if (wait_up(io, res, WQMP_STEP_WAIT_CH_ACK, WQMP_MCORE_CH_ACK, timeout_us)) goto close_channel;
    return res->rc = WQMP_OK;

close_channel:
    io->wr(io->ctx, WQMP_MCORE_CH, WQMP_STATE_DOWN);
close_link:
    io->wr(io->ctx, WQMP_MCORE_LINK, WQMP_STATE_DOWN);
    kick(io, res);
    record(res, WQMP_STEP_CLOSE, WQMP_ERR_TIMEDOUT, WQMP_STATE_DOWN, 0);
    return res->rc = WQMP_ERR_TIMEDOUT;
}
