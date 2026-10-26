// SPDX-License-Identifier: BSD-2-Clause-Patent
// AOSS QMP link-up (WifiQmp.c) against a simulated AOSS. The message RAM starts in the state read on the phone (round 6:
// magic ok, version 1, ucore link up, every other link/channel word down, ucore mbox 0xc0+0x68, mcore 0x128+0x2d8). The
// simulated AOSS reacts only to a doorbell kick, after a programmable number of polls, one transition rule per kick
// round, like the AOP: link ack, then its channel, then the channel ack. Faults: dead, no link ack, no channel, no
// channel ack, bad magic/version, mailbox size 0.
#include "tcheck.h"
#include <string.h>
#include "../WifiQmp.h"

#define UP WQMP_STATE_UP
#define DOWN WQMP_STATE_DOWN
enum {F_NONE, F_DEAD, F_NO_LINK_ACK, F_NO_CH, F_NO_CH_ACK};

typedef struct {
    uint32_t w[0x400 / 4];
    int fault;
    unsigned latency;       // polls between a kick and the AOSS reaction
    unsigned pending;       // kicks not yet served
    unsigned countdown;
    uint64_t now;
    unsigned kicks, writes, bad_writes, mbox_writes;
    struct {char op; uint32_t off, v;} log[64];
    unsigned nlog;
} sim;

static uint32_t s_rd(void *c, uint32_t off) {sim *s = c; return s->w[off / 4];}
static void s_log(sim *s, char op, uint32_t off, uint32_t v)
{
    if (s->nlog < 64) {s->log[s->nlog].op = op; s->log[s->nlog].off = off; s->log[s->nlog].v = v; ++s->nlog;}
}
static void s_wr(void *c, uint32_t off, uint32_t v)
{
    sim *s = c;
    ++s->writes;
    // The AP may only write its own words and its acks of the AOSS words; never the mailboxes.
    if (off != WQMP_UCORE_LINK_ACK && off != WQMP_UCORE_CH_ACK && off != WQMP_MCORE_LINK && off != WQMP_MCORE_CH) ++s->bad_writes;
    if (off >= 0x3c) ++s->mbox_writes;
    s->w[off / 4] = v;
    s_log(s, 'w', off, v);
}
static void s_kick(void *c)
{
    sim *s = c;
    ++s->kicks;
    s_log(s, 'k', 0, 0);
    if (!s->pending++) s->countdown = s->latency;
}
static uint64_t s_now(void *c) {return ((sim *)c)->now;}

// One AOSS service round: the AOP's reaction to the AP's words.
static void aoss_serve(sim *s)
{
    uint32_t *w = s->w;
#define W(o) w[(o) / 4]
    if (s->fault == F_DEAD) return;
    if (W(WQMP_MCORE_LINK) == UP && W(WQMP_MCORE_LINK_ACK) != UP && s->fault != F_NO_LINK_ACK) W(WQMP_MCORE_LINK_ACK) = UP;
    else if (W(WQMP_MCORE_LINK) == DOWN && W(WQMP_MCORE_LINK_ACK) == UP) {
        W(WQMP_MCORE_LINK_ACK) = DOWN; W(WQMP_UCORE_CH) = DOWN; W(WQMP_MCORE_CH_ACK) = DOWN;
    }
    if (W(WQMP_MCORE_LINK_ACK) == UP && W(WQMP_MCORE_CH) == UP && W(WQMP_UCORE_CH) != UP && s->fault != F_NO_CH)
        W(WQMP_UCORE_CH) = UP;
    else if (W(WQMP_UCORE_CH_ACK) == UP && W(WQMP_UCORE_CH) == UP && W(WQMP_MCORE_CH) == UP && W(WQMP_MCORE_CH_ACK) != UP
             && s->fault != F_NO_CH_ACK)
        W(WQMP_MCORE_CH_ACK) = UP;
    if (W(WQMP_MCORE_CH) == DOWN && W(WQMP_MCORE_CH_ACK) == UP) {W(WQMP_MCORE_CH_ACK) = DOWN; W(WQMP_UCORE_CH) = DOWN;}
#undef W
}
static void s_relax(void *c)
{
    sim *s = c;
    s->now += 100;
    if (!s->pending) return;
    if (s->countdown) {--s->countdown; return;}
    s->pending = 0;
    aoss_serve(s);
}

static void sim_init(sim *s, int fault, unsigned latency)
{
    memset(s, 0, sizeof *s);
    s->fault = fault; s->latency = latency;
    s->w[0] = WQMP_MAGIC; s->w[1] = 1; s->w[2] = 0;
    s->w[WQMP_UCORE_LINK / 4] = UP;
    s->w[WQMP_UCORE_LINK_ACK / 4] = DOWN; s->w[WQMP_UCORE_CH / 4] = DOWN; s->w[WQMP_UCORE_CH_ACK / 4] = DOWN;
    s->w[WQMP_UCORE_MBOX_SIZE / 4] = 0x68; s->w[WQMP_UCORE_MBOX_OFF / 4] = 0xc0;
    s->w[WQMP_MCORE_LINK / 4] = DOWN; s->w[WQMP_MCORE_LINK_ACK / 4] = DOWN;
    s->w[WQMP_MCORE_CH / 4] = DOWN; s->w[WQMP_MCORE_CH_ACK / 4] = DOWN;
    s->w[WQMP_MCORE_MBOX_SIZE / 4] = 0x2d8; s->w[WQMP_MCORE_MBOX_OFF / 4] = 0x128;
    for (unsigned o = 0x3c; o < 0x400; o += 4) s->w[o / 4] = 0xa5a5a5a5u;   // mailboxes: must stay untouched
}
static wqmp_io sim_io(sim *s) {wqmp_io io = {s_rd, s_wr, s_kick, s_now, s_relax, s}; return io;}

static void t_qmp_phone_state(void)
{
    sim s; sim_init(&s, F_NONE, 0);
    wqmp_io io = sim_io(&s);
    wqmp_desc d; wqmp_read(&io, &d);
    CHECK(d.magic == WQMP_MAGIC && d.version == 1);
    CHECK(d.ucore_link == UP && d.mcore_link == DOWN && d.mcore_mbox_off == 0x128 && d.mcore_mbox_size == 0x2d8);
    CHECK(d.ucore_mbox_off == 0xc0 && d.ucore_mbox_size == 0x68);
    CHECK(!wqmp_is_up(&d));
    CHECK(!strcmp(wqmp_state_name(UP), "up") && !strcmp(wqmp_state_name(DOWN), "down") && !strcmp(wqmp_state_name(0), "0"));
    CHECK(!strcmp(wqmp_step_name(WQMP_STEP_WAIT_CH_ACK), "wait-ch-ack") && !strcmp(wqmp_step_name(99), "?"));
}

// The exact qcom_aoss.c qmp_open sequence of writes and kicks.
static void t_qmp_open_sequence(void)
{
    for (unsigned lat = 0; lat < 40; lat += 13) {
        sim s; sim_init(&s, F_NONE, lat);
        wqmp_io io = sim_io(&s);
        wqmp_result r;
        CHECK(wqmp_open(&io, 0, &r) == WQMP_OK && r.rc == WQMP_OK);
        static const struct {char op; uint32_t off, v;} want[] = {
            {'w', WQMP_UCORE_LINK_ACK, UP}, {'w', WQMP_MCORE_LINK, UP}, {'k', 0, 0},
            {'w', WQMP_MCORE_CH, UP}, {'k', 0, 0}, {'w', WQMP_UCORE_CH_ACK, UP}, {'k', 0, 0}};
        CHECK(s.nlog == sizeof want / sizeof want[0]);
        for (unsigned i = 0; i < s.nlog && i < sizeof want / sizeof want[0]; ++i)
            CHECK(s.log[i].op == want[i].op && s.log[i].off == want[i].off && s.log[i].v == want[i].v);
        CHECK(s.kicks == 3 && r.kicks == 3 && s.bad_writes == 0 && s.mbox_writes == 0);
        for (unsigned o = 0x3c; o < 0x400; o += 4) if (s.w[o / 4] != 0xa5a5a5a5u) {CHECK(!"mailbox touched"); break;}
        CHECK(r.mbox_off == 0x128 && r.mbox_size == 0x2d8);
        CHECK(r.nsteps == 8);
        for (unsigned i = 0; i < r.nsteps; ++i) CHECK(r.step[i].step == i && r.step[i].rc == 0);
        CHECK(r.step[WQMP_STEP_ACK_UCORE_LINK].value == UP);
        // Each wait took at least the AOSS latency.
        CHECK(r.step[WQMP_STEP_WAIT_LINK_ACK].elapsed_us >= 100u * lat);
        wqmp_desc d; wqmp_read(&io, &d);
        CHECK(wqmp_is_up(&d));
        CHECK(d.ucore_link_ack == UP && d.ucore_ch_ack == UP);
        // A second open on an up link (a warm restart) runs the same sequence and succeeds.
        CHECK(wqmp_open(&io, 0, &r) == WQMP_OK && r.nsteps == 8);
        wqmp_read(&io, &d);
        CHECK(wqmp_is_up(&d));
    }
}

static void t_qmp_timeouts(void)
{
    static const struct {int fault; unsigned fail_step; int ch_closed;} cases[] = {
        {F_DEAD, WQMP_STEP_WAIT_LINK_ACK, 0}, {F_NO_LINK_ACK, WQMP_STEP_WAIT_LINK_ACK, 0},
        {F_NO_CH, WQMP_STEP_WAIT_UCORE_CH, 1}, {F_NO_CH_ACK, WQMP_STEP_WAIT_CH_ACK, 1}};
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        sim s; sim_init(&s, cases[c].fault, 2);
        wqmp_io io = sim_io(&s);
        wqmp_result r;
        CHECK(wqmp_open(&io, 50000, &r) == WQMP_ERR_TIMEDOUT && r.rc == WQMP_ERR_TIMEDOUT);
        CHECK(r.nsteps >= 2);
        const wqmp_step *f = &r.step[r.nsteps - 2], *cl = &r.step[r.nsteps - 1];
        CHECK(f->step == cases[c].fail_step && f->rc == WQMP_ERR_TIMEDOUT && f->elapsed_us >= 50000);
        CHECK(cl->step == WQMP_STEP_CLOSE);
        // qmp_open's error path: channel down (only once it was opened), then link down, then a kick.
        CHECK(s.nlog >= 2 && s.log[s.nlog - 1].op == 'k');
        CHECK(s.log[s.nlog - 2].op == 'w' && s.log[s.nlog - 2].off == WQMP_MCORE_LINK && s.log[s.nlog - 2].v == DOWN);
        if (cases[c].ch_closed)
            CHECK(s.nlog >= 3 && s.log[s.nlog - 3].off == WQMP_MCORE_CH && s.log[s.nlog - 3].v == DOWN);
        else
            CHECK(s.w[WQMP_MCORE_CH / 4] == DOWN && s.log[s.nlog - 3].off != WQMP_MCORE_CH);
        CHECK(s.bad_writes == 0 && s.mbox_writes == 0);
        wqmp_desc d; wqmp_read(&io, &d);
        CHECK(!wqmp_is_up(&d) && d.mcore_link == DOWN && d.mcore_ch == DOWN);
    }
}

static void t_qmp_bad_desc(void)
{
    static const struct {uint32_t off, v;} cases[] = {{WQMP_MAGIC_OFF, 0}, {WQMP_MAGIC_OFF, 0x4d41494du},
                                                      {WQMP_VERSION_OFF, 2}, {WQMP_MCORE_MBOX_SIZE, 0}};
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        sim s; sim_init(&s, F_NONE, 0);
        s.w[cases[c].off / 4] = cases[c].v;
        wqmp_io io = sim_io(&s);
        wqmp_result r;
        CHECK(wqmp_open(&io, 0, &r) == WQMP_ERR_INVAL);
        CHECK(r.nsteps == 1 && r.step[0].step == WQMP_STEP_CHECK && r.step[0].rc == WQMP_ERR_INVAL);
        CHECK(s.writes == 0 && s.kicks == 0);   // nothing touched on a bad descriptor
    }
}

void t_qmp(void)
{
    RUN(t_qmp_phone_state);
    RUN(t_qmp_open_sequence);
    RUN(t_qmp_timeouts);
    RUN(t_qmp_bad_desc);
}
